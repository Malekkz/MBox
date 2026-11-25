// A seeded random run of the channel engine against the simulated bus, with
// the invariants the reviews of PR 137 found every blocking defect with:
//
// 1. A unit that stopped answering has no point GOOD past the verdict bound
//    (the lead's ruling J2, D-140), counted from the first request for that
//    point after the unit stopped.
// 2. Every enabled device is asked (polled or probed) within a bound.
// 3. A unit that answers again is RUNNING within its probe back-off and the
//    reconnect bounds.
// 4. A value whose companion's block stopped answering is not GOOD past the
//    same bound (DI 5.5, G1, H1, K1).
// 5. The engine never spins: it takes at most SPIN_STEPS steps in any 100 ms
//    of simulated time (review L1: a wait 1 ns ahead, nothing sent).
// 7. Every wake the engine returns is not before its step began, and a
//    failed chained part's own points are COMM_FAIL within the J2 bound
//    (review M2).
// 6. A unit that answers has its value asked within the bound of the
//    contract's order of service (DI 6.2 step 3): its period, plus each
//    request sent meanwhile that goes before it, an unanswered one at most
//    its time-out and an answered one of the same or a higher priority its
//    exchange, plus one request already on the line. The probes of demoted
//    units count at most one probe time-out in every PROBE_FLOOR + 1
//    (the lead's ruling N1).
//
// Each seed builds its own channel (transport, line, failure keys, chains,
// poll groups per unit, a second block in another group) and its own faults
// over time; the seeds are fixed, so every run is the
// same and a failure names its seed.

#include "channel_builder.h"
#include "loopback.h"
#include "scada/driver/channel_engine.h"
#include "scada/driver/compiled.h"
#include "scada/driver/failure.h"
#include "scada/driver/plan.h"
#include "scada/driver/point_rules.h"
#include "scada/driver/scheduler.h"
#include "scada/tag/types.h"
#include "sim_transport.h"
#include "test_protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace scada::driver::test {
namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): the ranges of the random channels and the bounds are the test data

using Fault = SimBus::Fault;
constexpr std::uint32_t VALUE_REGISTER = 0;
constexpr std::uint32_t COMPANION_REGISTER = 20;
constexpr std::uint32_t BLOCK_REGISTER = 40;
constexpr std::uint32_t THIRD_REGISTER = 30;   // the last part of a chain of 3
constexpr std::uint32_t SHARED_REGISTER = 60;  // a second device on unit 1
constexpr std::size_t SPIN_STEPS = 2'000;
constexpr std::int64_t SAMPLE = 100 * MILLISECOND;  // the run's sampling
constexpr std::int64_t NEVER_NS = std::numeric_limits<std::int64_t>::max();

// What a unit does from a time on.
enum class Mode : std::uint8_t {
    ANSWERS,
    SILENT,
    BUSY,
    COMPANION_SILENT,
    BLOCK_SILENT
};

// One random channel and its faults.
struct Scenario {
    CompiledChannel channel;
    std::uint8_t units = 0;
    std::vector<bool> chained;  // per device: a value with a companion
    // Per device: the chain has a third part (0 <- 20 <- 30), point 2.
    std::vector<bool> third;
    // A second device on unit 1 (the last device), reading register 60; with
    // a failure count of its own when `own_unit` (H5).
    bool shared = false;
    bool own_unit = false;
    // Per device: the index of a point in a second block, in another
    // group, or 0 for none.
    std::vector<std::uint32_t> block;
    std::vector<std::int64_t> latency_ns;
    struct Event {
        std::int64_t time_ns = 0;
        std::uint8_t unit = 0;
        Mode mode = Mode::ANSWERS;
    };
    std::vector<Event> events;
    // When the peer closes a stream, or noise arrives on a serial line.
    std::vector<std::int64_t> transport_faults;
};

// SplitMix64 with our own arithmetic: the standard distributions differ
// between standard libraries, so the same seed would build other channels
// on other builds (review M3).
class Random {
public:
    explicit Random(std::uint64_t seed) noexcept : state_(seed) {}
    std::uint64_t next() noexcept {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t value = state_;
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31U);
    }

private:
    std::uint64_t state_;
};

// A number from `low` to `high`, both included (`low` <= `high`, a small
// range).
std::int64_t pick(Random& random, std::int64_t low, std::int64_t high) {
    if (high <= low) {
        return low;
    }
    const auto span = static_cast<std::uint64_t>(high - low) + 1U;
    return low + static_cast<std::int64_t>(random.next() % span);
}

// The spec of unit `unit`: its points, chain, second block, keep-alive and
// pacing; notes them in `out`.
DeviceSpec unit_spec(Random& random, std::uint8_t unit, Scenario& out) {
    // fast, normal or slow (default groups: 200 ms, 1 s, 10 s, by
    // priority), mostly the first two.
    const auto group = static_cast<std::uint32_t>(
        std::min<std::int64_t>(pick(random, 0, 4), 2) % 3);
    const bool chain = pick(random, 0, 2) == 0;
    const bool third = chain && pick(random, 0, 1) == 0;
    out.chained.push_back(chain);
    out.third.push_back(third);
    std::vector<PointSpec> points{{.first = VALUE_REGISTER, .group = group}};
    if (chain) {
        points[0].companions = {1};
        points.push_back({.first = COMPANION_REGISTER, .group = group});
    }
    if (third) {
        points[1].companions = {2};
        points.push_back({.first = THIRD_REGISTER, .group = group});
    }
    std::uint32_t block = 0;
    if (pick(random, 0, 2) == 0) {
        block = static_cast<std::uint32_t>(points.size());
        points.push_back(
            {.first = BLOCK_REGISTER,
             .group =
                 (group + static_cast<std::uint32_t>(pick(random, 1, 2))) % 3});
    }
    out.block.push_back(block);
    DeviceSpec spec{
        .name = "d" + std::to_string(unit), .unit_id = unit, .points = points};
    if (pick(random, 0, 2) == 0) {
        spec.keepalive_ns = pick(random, 2, 20) * 100 * MILLISECOND;
    }
    if (pick(random, 0, 3) == 0) {
        spec.pacing.min_request_interval_ns =
            pick(random, 1, 10) * 10 * MILLISECOND;
    }
    out.latency_ns.push_back(pick(random, 1, 40) * MILLISECOND);
    return spec;
}

// The failure keys of every device, and the quality rules of the chains.
void set_keys(Random& random, Scenario& out) {
    for (std::size_t device = 0; device < out.channel.devices.size();
         ++device) {
        FailureRules& rules = out.channel.devices[device].failure;
        rules.timeout_ns = pick(random, 50, 300) * MILLISECOND;
        rules.write_timeout_ns = rules.timeout_ns;
        rules.retries = static_cast<std::uint8_t>(pick(random, 0, 2));
        rules.demote_after = static_cast<std::uint8_t>(pick(random, 1, 3));
        rules.backoff_ns = pick(random, 2, 10) * 100 * MILLISECOND;
        rules.backoff_max_ns = pick(random, 2, 4) * SECOND;
        if (device < out.units && out.third[device]) {
            // The companion's quality comes from the third part (D-131).
            PointRules& middle = out.channel.devices[device].points[1].rules;
            middle.quality_rules.push_back(
                QualityRule{.companion = 0,
                            .entries = {},
                            .otherwise = {.quality = tag::Quality::GOOD}});
        }
        if (device < out.units && out.chained[device]) {
            // The value's quality comes from its companion (D-131).
            PointRules& value = out.channel.devices[device].points[0].rules;
            value.quality_rules.push_back(
                QualityRule{.companion = 0,
                            .entries = {},
                            .otherwise = {.quality = tag::Quality::GOOD}});
        }
    }
}

// The units' modes over time, and the transport faults.
void add_events(Random& random, Scenario& out, std::int64_t end_ns) {
    for (std::int64_t time = 2 * SECOND; time < end_ns;
         time += pick(random, 1, 6) * SECOND) {
        const auto unit = static_cast<std::uint8_t>(pick(random, 1, out.units));
        const std::int64_t roll = pick(random, 0, 19);
        Mode mode = Mode::ANSWERS;
        if (roll < 5) {
            mode = Mode::SILENT;
        } else if (roll == 5) {
            mode = Mode::BUSY;
        } else if (roll == 6 && out.chained[unit - 1U]) {
            mode = Mode::COMPANION_SILENT;
        } else if (roll == 7 && out.block[unit - 1U] != 0) {
            mode = Mode::BLOCK_SILENT;
        }
        out.events.push_back({.time_ns = time, .unit = unit, .mode = mode});
    }
    // Now and then the peer closes a stream, or noise hits a serial line.
    for (std::int64_t time = pick(random, 5, 30) * SECOND; time < end_ns;
         time += pick(random, 20, 60) * SECOND) {
        out.transport_faults.push_back(time);
    }
}

Scenario scenario_of(std::uint64_t seed, std::int64_t end_ns) {
    Random random{seed};
    Scenario out;
    out.units = static_cast<std::uint8_t>(pick(random, 2, 6));
    std::vector<DeviceSpec> specs;
    for (std::uint8_t unit = 1; unit <= out.units; ++unit) {
        specs.push_back(unit_spec(random, unit, out));
    }
    out.shared = pick(random, 0, 3) == 0;
    if (out.shared) {
        specs.push_back({.name = "shared",
                         .unit_id = 1,
                         .points = {{.first = SHARED_REGISTER, .group = 1}}});
    }
    out.channel = make_channel(specs, ReadRules{.max_units = 4});
    if (out.shared && pick(random, 0, 1) == 0) {
        out.own_unit = true;
        out.channel.devices.back().unit = CompiledDevice::OWN_UNIT;
    }
    constexpr std::array<TransportKind, 3> KINDS{
        TransportKind::SERIAL, TransportKind::TCP, TransportKind::RTU_OVER_TCP};
    out.channel.transport =
        KINDS.at(static_cast<std::size_t>(pick(random, 0, 2)));
    if (out.channel.transport == TransportKind::SERIAL) {
        constexpr std::array<std::uint32_t, 3> RATES{9'600, 19'200, 38'400};
        out.channel.line.bit_rate =
            RATES.at(static_cast<std::size_t>(pick(random, 0, 2)));
    }
    out.channel.connection.reconnect_min_ns =
        pick(random, 2, 10) * 100 * MILLISECOND;
    out.channel.connection.reconnect_max_ns = 2 * SECOND;
    out.channel.connection.connect_timeout_ns = 500 * MILLISECOND;
    set_keys(random, out);
    add_events(random, out, end_ns);
    return out;
}

// NOLINTBEGIN(misc-non-private-member-variables-in-classes): a test rig whose parts are its interface
struct Harness {
    explicit Harness(Scenario given)
        : scenario(std::move(given)),
          transport(bus, clock, MILLISECOND),
          engine(scenario.channel, protocol, transport, clock, tags) {
        for (std::uint8_t unit = 1; unit <= scenario.units; ++unit) {
            bus.map(unit, 0, 0, 199);
            transport.latency(unit, scenario.latency_ns[unit - 1U]);
        }
    }

    Scenario scenario;
    SimBus bus;
    SimClock clock;
    SimTransport transport;
    RecordingTags tags;
    TestProtocol protocol;
    ChannelEngine engine;
    // Per unit id: the sample times at which its device became DEMOTED or
    // stopped being so, in order (a request to it then is a probe).
    std::vector<std::vector<std::int64_t>> demoted_flips;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

void apply(Harness& run, const Scenario::Event& event) {
    Fault fault = Fault::NONE;
    if (event.mode == Mode::SILENT) {
        fault = Fault::SILENT;
    } else if (event.mode == Mode::BUSY) {
        fault = Fault::BUSY;
    }
    run.bus.fault(event.unit, fault);
    if (event.mode == Mode::COMPANION_SILENT) {
        run.bus.silent_from(event.unit, COMPANION_REGISTER, COMPANION_REGISTER);
    } else if (event.mode == Mode::BLOCK_SILENT) {
        run.bus.silent_from(event.unit, BLOCK_REGISTER, BLOCK_REGISTER);
    } else {
        run.bus.silent_from(event.unit, 1000, 1000);  // nothing silent
    }
}

std::int64_t longest_timeout(const Harness& run) {
    std::int64_t longest = 0;
    for (const CompiledDevice& other : run.scenario.channel.devices) {
        longest = std::max(longest, other.failure.timeout_ns);
    }
    return longest;
}

std::int64_t period_of(const Harness& run, std::uint32_t device,
                       std::uint32_t point) {
    const CompiledChannel& channel = run.scenario.channel;
    return channel
        .poll_groups[channel.devices[device].points[point].poll_group.value_or(
            0)]
        .period_ns;
}

// The verdict bound of a point: one attempt, then one period or its
// retries' time-outs, plus one exchange of any device on the line, the
// quiet time and slack.
std::int64_t verdict_bound(const Harness& run, std::uint32_t device,
                           std::uint32_t point) {
    const FailureRules& rules = run.scenario.channel.devices[device].failure;
    const std::int64_t longest = longest_timeout(run);
    const std::int64_t period = period_of(run, device, point);
    return rules.timeout_ns +
           std::max(period, rules.timeout_ns * rules.retries) + longest +
           200 * MILLISECOND;
}

// The point of `device` (0: the value, 1: the companion) is GOOD past its
// bound, counted from `first_ns`, the first request for it since the unit
// stopped (-1: none yet).
bool good_past_bound(const Harness& run, std::uint32_t device,
                     std::uint32_t point, std::int64_t first_ns,
                     std::int64_t now_ns) {
    return first_ns >= 0 &&
           now_ns > first_ns + verdict_bound(run, device, point) &&
           run.tags.point(device, point).quality == tag::Quality::GOOD;
}

// How long a unit that answers again may take to be RUNNING: its probe
// back-off and the reconnect bounds (invariant 3).
std::int64_t recover_bound(const Harness& run, std::uint8_t unit) {
    const FailureRules& rules = run.scenario.channel.devices[unit - 1U].failure;
    const ConnectionRules& connection = run.scenario.channel.connection;
    return rules.backoff_max_ns + rules.timeout_ns * 10 +
           connection.reconnect_max_ns + connection.connect_timeout_ns +
           3 * SECOND;
}

struct UnitState {
    Mode mode = Mode::ANSWERS;
    std::int64_t since_ns = 0;
    // The first request for the value's and the companion's register since
    // `since_ns`.
    std::int64_t first_value_ns = -1;
    std::int64_t first_companion_ns = -1;
    std::int64_t first_block_ns = -1;
    std::int64_t first_third_ns = -1;
    std::int64_t first_shared_ns = -1;
};

std::int64_t ahead_ns(const Harness& run, std::uint8_t unit, std::uint8_t own,
                      std::int64_t from_ns, std::int64_t to_ns);
std::int64_t fault_slack(const Harness& run, std::int64_t from_ns,
                         std::int64_t to_ns);
std::uint8_t unit_priority(const Harness& run, std::uint8_t unit);

// Invariant 3 for a unit that answers since `state.since_ns`: RUNNING
// within its probe back-off and reconnect bounds plus the requests ahead.
bool recovered(const Harness& run, std::uint8_t unit, const UnitState& state,
               std::int64_t now_ns) {
    const std::uint32_t device = unit - 1U;
    // From its return, or from the last transport fault, after
    // which every device reconnects (7.3).
    std::int64_t since = state.since_ns;
    for (const std::int64_t fault : run.scenario.transport_faults) {
        if (fault <= now_ns) {
            since = std::max(since, fault);
        }
    }
    // Plus the requests ahead of it meanwhile (DI 6.2 step 3).
    // The cheap tests first: the sum is over the log.
    return run.engine.failures().state(device) == DeviceStatus::RUNNING ||
           now_ns <= since + recover_bound(run, unit) ||
           now_ns <= since + recover_bound(run, unit) +
                         ahead_ns(run, unit, unit_priority(run, unit), since,
                                  now_ns) +
                         fault_slack(run, since, now_ns);
}

// Checks invariants 1, 3 and 4 at `now_ns`; returns the violations.
std::vector<std::string> check(const Harness& run,
                               const std::vector<UnitState>& units,
                               std::int64_t now_ns) {
    std::vector<std::string> out;
    for (std::uint8_t unit = 1; unit <= run.scenario.units; ++unit) {
        const std::uint32_t device = unit - 1U;
        const UnitState& state = units[unit];
        const bool chain = run.scenario.chained[device];
        const std::uint32_t block = run.scenario.block[device];
        if (state.mode == Mode::SILENT &&
            (good_past_bound(run, device, 0, state.first_value_ns, now_ns) ||
             (chain && good_past_bound(run, device, 1, state.first_companion_ns,
                                       now_ns)) ||
             (block != 0 && good_past_bound(run, device, block,
                                            state.first_block_ns, now_ns)) ||
             (run.scenario.third[device] &&
              good_past_bound(run, device, 2, state.first_third_ns, now_ns)) ||
             (unit == 1 && run.scenario.shared &&
              good_past_bound(run, run.scenario.units, 0, state.first_shared_ns,
                              now_ns)))) {
            out.push_back("unit " + std::to_string(unit) +
                          " silent and GOOD at " +
                          std::to_string(now_ns / MILLISECOND));
        }
        // The value goes bad through its companion when the chain ends: the
        // companion's verdict bound, then the chain's last request.
        if (state.mode == Mode::COMPANION_SILENT &&
            good_past_bound(run, device, 0, state.first_companion_ns, now_ns)) {
            out.push_back("unit " + std::to_string(unit) +
                          " value GOOD with its companion silent at " +
                          std::to_string(now_ns / MILLISECOND));
        }
        // The failed chained part itself, written at once (J2; review M2).
        if (state.mode == Mode::COMPANION_SILENT &&
            good_past_bound(run, device, 1, state.first_companion_ns, now_ns)) {
            out.push_back("unit " + std::to_string(unit) +
                          " silent companion GOOD at " +
                          std::to_string(now_ns / MILLISECOND));
        }
        if (state.mode == Mode::BLOCK_SILENT &&
            good_past_bound(run, device, block, state.first_block_ns, now_ns)) {
            out.push_back("unit " + std::to_string(unit) +
                          " block silent and GOOD at " +
                          std::to_string(now_ns / MILLISECOND));
        }
        if (state.mode == Mode::ANSWERS &&
            !recovered(run, unit, state, now_ns)) {
            out.push_back("unit " + std::to_string(unit) +
                          " answering and not RUNNING at " +
                          std::to_string(now_ns / MILLISECOND));
        }
    }
    return out;
}

// The mode of `unit` at `time_ns`, or a sample later: a mode applies from
// the sample at its time, which the run reaches up to one sample late.
Mode mode_at(const Scenario& scenario, std::uint8_t unit, std::int64_t time_ns,
             std::int64_t sample_ns = SAMPLE) {
    Mode mode = Mode::ANSWERS;
    for (const Scenario::Event& event : scenario.events) {
        if (event.unit == unit && event.time_ns <= time_ns + sample_ns) {
            mode = event.mode;
        }
    }
    return mode;
}

// Whether the unit leaves `request` unanswered in `mode`.
bool unanswered(Mode mode, std::uint32_t first) {
    return mode == Mode::SILENT ||
           (mode == Mode::COMPANION_SILENT && first == COMPANION_REGISTER) ||
           (mode == Mode::BLOCK_SILENT && first == BLOCK_REGISTER);
}

// The priority of the group that reads register `first` of `unit`.
std::uint8_t priority_of(const Harness& run, std::uint8_t unit,
                         std::uint32_t first) {
    std::uint32_t device = unit - 1U;
    std::uint32_t point = 0;
    if (first == COMPANION_REGISTER) {
        point = 1;
    } else if (first == THIRD_REGISTER) {
        point = 2;
    } else if (first == BLOCK_REGISTER) {
        point = run.scenario.block[device];
    } else if (first == SHARED_REGISTER) {
        device = run.scenario.units;
    }
    const CompiledChannel& channel = run.scenario.channel;
    return channel
        .poll_groups[channel.devices[device].points[point].poll_group.value_or(
            0)]
        .priority;
}

// What one exchange of an answered request may hold the line: the latency,
// the frames and the quiet time at the slowest rate.
constexpr std::int64_t EXCHANGE_SLACK = 50 * MILLISECOND;

// The priority of the unit's group of the highest priority: the requests
// ahead of that one count (the contract's order, DI 6.2 step 3).
std::uint8_t unit_priority(const Harness& run, std::uint8_t unit) {
    std::uint8_t own = UINT8_MAX;
    for (const std::uint32_t first :
         {VALUE_REGISTER, COMPANION_REGISTER, THIRD_REGISTER, BLOCK_REGISTER}) {
        const std::uint32_t device = unit - 1U;
        if ((first != COMPANION_REGISTER || run.scenario.chained[device]) &&
            (first != THIRD_REGISTER || run.scenario.third[device]) &&
            (first != BLOCK_REGISTER || run.scenario.block[device] != 0)) {
            own = std::min(own, priority_of(run, unit, first));
        }
    }
    return own;
}

// Whether the device of `unit` was DEMOTED at the sample before `time_ns`.
bool demoted_at(const Harness& run, std::uint8_t unit, std::int64_t time_ns) {
    const std::vector<std::int64_t>& flips = run.demoted_flips[unit];
    const auto after = std::upper_bound(flips.begin(), flips.end(), time_ns);
    return (after - flips.begin()) % 2 == 1;
}

// A probe's time-out: the device's, at most 1 s (7.4).
std::int64_t probe_timeout(const Harness& run, std::uint32_t device) {
    return std::min(run.scenario.channel.devices[device].failure.timeout_ns,
                    SECOND);
}

// A transport fault between `from_ns` and `to_ns` (or just before) costs a
// reconnect, or the quiet time after noise: their bounds.
std::int64_t fault_slack(const Harness& run, std::int64_t from_ns,
                         std::int64_t to_ns) {
    const ConnectionRules& connection = run.scenario.channel.connection;
    std::int64_t slack = 0;
    for (const std::int64_t time : run.scenario.transport_faults) {
        if (time >= from_ns - SECOND && time <= to_ns) {
            slack += connection.reconnect_max_ns +
                     connection.connect_timeout_ns + SECOND;
        }
    }
    return slack;
}

// The line time that the requests sent between `from_ns` and `to_ns` may
// take before a request of `unit` at priority `own` in the contract's order
// of service (DI 6.2 step 3): an unanswered request at most its time-out,
// an answered one of the same or a higher priority, or of `unit` itself,
// its exchange; each with the quiet time.
std::int64_t ahead_ns(const Harness& run, std::uint8_t unit, std::uint8_t own,
                      std::int64_t from_ns, std::int64_t to_ns) {
    std::int64_t ahead = 0;
    std::int64_t probes = 0;  // line time of the probes of demoted units
    // An answered request of the same priority goes before it only when due
    // earlier: once per other unit and register in the gap.
    std::vector<std::pair<std::uint8_t, std::uint32_t>> same;
    // The log is in time order: start at the window.
    const std::vector<LoggedRequest>& log = run.bus.log();
    const auto begin =
        std::upper_bound(log.begin(), log.end(), from_ns,
                         [](std::int64_t time, const LoggedRequest& request) {
                             return time < request.time_ns;
                         });
    for (auto it = begin; it != log.end() && it->time_ns < to_ns; ++it) {
        const LoggedRequest& request = *it;
        if (request.time_ns <= from_ns || request.time_ns >= to_ns) {
            continue;
        }
        const std::uint32_t device = request.first == SHARED_REGISTER
                                         ? run.scenario.units
                                         : request.unit_id - 1U;
        const Mode mode =
            mode_at(run.scenario, request.unit_id, request.time_ns, SAMPLE);
        const std::uint8_t priority =
            priority_of(run, request.unit_id, request.first);
        const std::pair<std::uint8_t, std::uint32_t> key{request.unit_id,
                                                         request.first};
        if (unanswered(mode, request.first) &&
            demoted_at(run, request.unit_id, request.time_ns)) {
            probes += probe_timeout(run, device) + EXCHANGE_SLACK;
        } else if (unanswered(mode, request.first)) {
            ahead += run.scenario.channel.devices[device].failure.timeout_ns +
                     EXCHANGE_SLACK;
        } else if (request.unit_id == unit || priority < own ||
                   (priority == own &&
                    std::find(same.begin(), same.end(), key) == same.end())) {
            ahead +=
                run.scenario.latency_ns[request.unit_id - 1U] + EXCHANGE_SLACK;
            if (priority == own) {
                same.push_back(key);
            }
        }
    }
    // Probes of demoted units take at most one probe time-out in every
    // PROBE_FLOOR + 1 (the lead's ruling N1), and one already on the line.
    std::int64_t shortest = NEVER_NS;
    std::int64_t longest = 0;
    for (std::uint32_t device = 0; device < run.scenario.channel.devices.size();
         ++device) {
        shortest = std::min(shortest, probe_timeout(run, device));
        longest = std::max(longest, probe_timeout(run, device));
    }
    const std::int64_t rounds =
        2 + (to_ns - from_ns) / ((Scheduler::PROBE_FLOOR + 1) * shortest);
    return ahead + std::min(probes, rounds * (longest + EXCHANGE_SLACK));
}

// Invariant 6 for the gap from `from_ns` to `to_ns` between two requests
// for the value of `unit`: its period, the requests ahead of it, and one
// request already on the line.
std::int64_t live_gap_bound(const Harness& run, std::uint8_t unit,
                            std::int64_t from_ns, std::int64_t to_ns) {
    return period_of(run, unit - 1U, 0) + longest_timeout(run) +
           EXCHANGE_SLACK + SECOND +
           ahead_ns(run, unit, priority_of(run, unit, VALUE_REGISTER), from_ns,
                    to_ns) +
           fault_slack(run, from_ns, to_ns);
}

// Invariant 2 over the whole run: the longest silence of the bus towards
// any unit, from 2 s on: its back-off, period and reconnect bounds, and
// the requests ahead of it in the contract's order.
std::vector<std::string> check_asked(const Harness& run, std::int64_t end_ns) {
    std::vector<std::string> out;
    const ConnectionRules& connection = run.scenario.channel.connection;
    for (std::uint8_t unit = 1; unit <= run.scenario.units; ++unit) {
        const FailureRules& rules =
            run.scenario.channel.devices[unit - 1U].failure;
        std::int64_t period = 0;
        for (std::uint32_t point = 0;
             point < run.scenario.channel.devices[unit - 1U].points.size();
             ++point) {
            period = std::max(period, period_of(run, unit - 1U, point));
        }
        const std::int64_t bound = rules.backoff_max_ns + period +
                                   rules.timeout_ns * 10 +
                                   connection.reconnect_max_ns +
                                   connection.connect_timeout_ns + 5 * SECOND;
        const std::uint8_t own = unit_priority(run, unit);
        std::int64_t last = 0;
        for (const LoggedRequest& request : run.bus.log()) {
            if (request.unit_id != unit) {
                continue;
            }
            if (request.time_ns - last >
                bound + ahead_ns(run, unit, own, last, request.time_ns) +
                    fault_slack(run, last, request.time_ns)) {
                out.push_back("unit " + std::to_string(unit) +
                              " not asked from " +
                              std::to_string(last / MILLISECOND) + " to " +
                              std::to_string(request.time_ns / MILLISECOND));
            }
            last = request.time_ns;
        }
        if (end_ns - last > bound + ahead_ns(run, unit, own, last, end_ns) +
                                fault_slack(run, last, end_ns)) {
            out.push_back("unit " + std::to_string(unit) + " not asked after " +
                          std::to_string(last / MILLISECOND));
        }
    }
    return out;
}

// Notes the first request for each register of a unit since its mode
// changed, from the bus's log from `seen` on, and checks invariant 6 on the
// requests for a value; `last_value` is per unit the last such request.
void scan(const Harness& run, std::vector<UnitState>& units,
          std::vector<std::int64_t>& last_value, std::size_t& seen,
          std::vector<std::string>& violations) {
    const std::vector<LoggedRequest>& log = run.bus.log();
    for (; seen < log.size(); ++seen) {
        const LoggedRequest& request = log[seen];
        UnitState& state = units[request.unit_id];
        std::int64_t* first = &state.first_value_ns;
        if (request.first == COMPANION_REGISTER) {
            first = &state.first_companion_ns;
        } else if (request.first == BLOCK_REGISTER) {
            first = &state.first_block_ns;
        } else if (request.first == THIRD_REGISTER) {
            first = &state.first_third_ns;
        } else if (request.first == SHARED_REGISTER) {
            first = &state.first_shared_ns;
        }
        if (*first < 0 && request.time_ns >= state.since_ns) {
            *first = request.time_ns;
        }
        if (request.first != VALUE_REGISTER) {
            continue;
        }
        std::int64_t& last = last_value[request.unit_id];
        if (state.mode == Mode::ANSWERS &&
            last >= state.since_ns + recover_bound(run, request.unit_id) &&
            request.time_ns - last >
                live_gap_bound(run, request.unit_id, last, request.time_ns)) {
            violations.push_back("unit " + std::to_string(request.unit_id) +
                                 " answering and its value not asked from " +
                                 std::to_string(last / MILLISECOND) + " to " +
                                 std::to_string(request.time_ns / MILLISECOND) +
                                 " ms");
        }
        last = request.time_ns;
    }
}

// Steps the engine until the clock passes `end_ns`, as `run_engine` does,
// and checks that every wake it returns is not before the step began
// (invariant 7; review M2). Returns the steps taken.
std::size_t run_checked(Harness& run, std::int64_t end_ns,
                        std::vector<std::string>& violations) {
    constexpr std::size_t MAX_STEPS = 1'000'000;
    std::size_t steps = 0;
    while (run.clock.monotonic_ns() <= end_ns && steps < MAX_STEPS) {
        const std::int64_t start = run.clock.monotonic_ns();
        const std::int64_t next = run.engine.step();
        ++steps;
        if (next < start) {
            violations.push_back("a wake " +
                                 std::to_string((start - next) / MILLISECOND) +
                                 " ms before its step at " +
                                 std::to_string(start / MILLISECOND) + " ms");
        }
        if (next > end_ns) {
            run.clock.advance_to(end_ns + 1);
            break;
        }
        run.clock.advance_to(next);  // never back
    }
    return steps;
}

// One seed: the engine's steps run, and every violation.
std::pair<std::size_t, std::vector<std::string>> run_seed(std::uint64_t seed) {
    constexpr std::int64_t END = 120 * SECOND;
    Harness run(scenario_of(seed, END));
    run.demoted_flips.resize(run.scenario.units + 1U);
    std::vector<UnitState> units(run.scenario.units + 1U);
    std::vector<std::string> violations;
    std::size_t steps = 0;
    std::size_t next_event = 0;
    std::size_t next_fault = 0;
    std::size_t seen = 0;     // requests of the bus's log looked at
    std::size_t busiest = 0;  // the most steps in one sample
    // Per unit: the last request for its value (invariant 6).
    std::vector<std::int64_t> last_value(run.scenario.units + 1U, 0);
    for (std::int64_t now = SAMPLE; now <= END; now += SAMPLE) {
        while (next_event < run.scenario.events.size() &&
               run.scenario.events[next_event].time_ns <= now) {
            const Scenario::Event& event = run.scenario.events[next_event];
            apply(run, event);
            // From when it applies by the simulated clock (review P2).
            units[event.unit] = {
                .mode = event.mode,
                .since_ns = std::max(event.time_ns, run.clock.monotonic_ns()),
                .first_value_ns = -1,
                .first_companion_ns = -1,
                .first_block_ns = -1,
                .first_third_ns = -1,
                .first_shared_ns = -1};
            ++next_event;
        }
        while (next_fault < run.scenario.transport_faults.size() &&
               run.scenario.transport_faults[next_fault] <= now) {
            if (run.scenario.channel.transport == TransportKind::SERIAL) {
                run.transport.noise(now, now + 50 * MILLISECOND);
            } else {
                run.transport.close_by_peer();
            }
            ++next_fault;
        }
        const std::size_t taken = run_checked(run, now, violations);
        steps += taken;
        busiest = std::max(busiest, taken);
        if (taken > SPIN_STEPS) {
            // An engine that wakes without sending: a spin (invariant 5).
            violations.push_back("spin at " +
                                 std::to_string(now / MILLISECOND) +
                                 " ms: " + std::to_string(taken) + " steps");
            break;
        }
        // Checked at the simulated clock, which a step leaves at the end of
        // the exchange it ran, past the loop's time (review P2).
        const std::int64_t clock_ns = run.clock.monotonic_ns();
        for (std::uint8_t unit = 1; unit <= run.scenario.units; ++unit) {
            std::vector<std::int64_t>& flips = run.demoted_flips[unit];
            const bool demoted =
                run.engine.failures().state(unit - 1U) == DeviceStatus::DEMOTED;
            if (demoted != (flips.size() % 2 == 1)) {
                flips.push_back(clock_ns);
            }
        }
        scan(run, units, last_value, seen, violations);
        for (std::string& violation : check(run, units, clock_ns)) {
            violations.push_back(std::move(violation));
        }
    }
    for (std::string& violation : check_asked(run, END)) {
        violations.push_back(std::move(violation));
    }
    if (busiest > SPIN_STEPS / 2) {
        violations.push_back("busiest sample: " + std::to_string(busiest) +
                             " steps");
    }
    return {steps, violations};
}

TEST(EngineRandom, TheInvariantsHoldForEverySeed) {
    // 100 seeds of 120 s each; a few hundred thousand engine steps in all.
    constexpr std::uint64_t SEEDS = 100;
    std::size_t steps = 0;
    for (std::uint64_t seed = 1; seed <= SEEDS; ++seed) {
        const auto [seed_steps, violations] = run_seed(seed);
        steps += seed_steps;
        EXPECT_TRUE(violations.empty())
            << "seed " << seed << ": " << violations.size()
            << " violations, the first: " << violations.front();
    }
    EXPECT_GT(steps, 200'000U);
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

}  // namespace
}  // namespace scada::driver::test
