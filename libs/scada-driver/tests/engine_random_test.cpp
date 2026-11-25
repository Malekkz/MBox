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
// 5. The engine never spins: every call of the run waits at some point.
//
// Each seed builds its own channel (transport, line, failure keys, chains)
// and its own faults over time; the seeds are fixed, so every run is the
// same and a failure names its seed.

#include "channel_builder.h"
#include "loopback.h"
#include "scada/driver/channel_engine.h"
#include "scada/driver/compiled.h"
#include "scada/driver/failure.h"
#include "scada/driver/plan.h"
#include "scada/driver/point_rules.h"
#include "scada/tag/types.h"
#include "sim_transport.h"
#include "test_protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace scada::driver::test {
namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): the ranges of the random channels and the bounds are the test data

using Fault = SimBus::Fault;
constexpr std::uint32_t VALUE_REGISTER = 0;
constexpr std::uint32_t COMPANION_REGISTER = 20;

// What a unit does from a time on.
enum class Mode : std::uint8_t { ANSWERS, SILENT, BUSY, COMPANION_SILENT };

// One random channel and its faults.
struct Scenario {
    CompiledChannel channel;
    std::uint8_t units = 0;
    std::vector<bool> chained;  // per device: a value with a companion
    std::vector<std::int64_t> latency_ns;
    struct Event {
        std::int64_t time_ns = 0;
        std::uint8_t unit = 0;
        Mode mode = Mode::ANSWERS;
    };
    std::vector<Event> events;
};

std::int64_t pick(std::mt19937_64& random, std::int64_t low,
                  std::int64_t high) {
    return std::uniform_int_distribution<std::int64_t>(low, high)(random);
}

Scenario scenario_of(std::uint64_t seed, std::int64_t end_ns) {
    std::mt19937_64 random(seed);
    Scenario out;
    out.units = static_cast<std::uint8_t>(pick(random, 2, 6));
    std::vector<DeviceSpec> specs;
    const auto group = static_cast<std::uint32_t>(pick(random, 0, 1));
    for (std::uint8_t unit = 1; unit <= out.units; ++unit) {
        const bool chain = pick(random, 0, 2) == 0;
        out.chained.push_back(chain);
        std::vector<PointSpec> points{
            {.first = VALUE_REGISTER, .group = group}};
        if (chain) {
            points[0].companions = {1};
            points.push_back({.first = COMPANION_REGISTER, .group = group});
        }
        specs.push_back({.name = "d" + std::to_string(unit),
                         .unit_id = unit,
                         .points = points});
        out.latency_ns.push_back(pick(random, 1, 40) * MILLISECOND);
    }
    out.channel = make_channel(specs, ReadRules{.max_units = 4});
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
    for (std::size_t device = 0; device < out.channel.devices.size();
         ++device) {
        FailureRules& rules = out.channel.devices[device].failure;
        rules.timeout_ns = pick(random, 50, 300) * MILLISECOND;
        rules.write_timeout_ns = rules.timeout_ns;
        rules.retries = static_cast<std::uint8_t>(pick(random, 0, 2));
        rules.demote_after = static_cast<std::uint8_t>(pick(random, 1, 3));
        rules.backoff_ns = pick(random, 2, 10) * 100 * MILLISECOND;
        rules.backoff_max_ns = pick(random, 2, 4) * SECOND;
        if (out.chained[device]) {
            // The value's quality comes from its companion (D-131).
            PointRules& value = out.channel.devices[device].points[0].rules;
            value.quality_rules.push_back(
                QualityRule{.companion = 0,
                            .entries = {},
                            .otherwise = {.quality = tag::Quality::GOOD}});
        }
    }
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
        }
        out.events.push_back({.time_ns = time, .unit = unit, .mode = mode});
    }
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
    } else {
        run.bus.silent_from(event.unit, 1000, 1000);  // nothing silent
    }
}

// The verdict bound of a device: one attempt, then one period or its
// retries' time-outs, plus one exchange of any device on the line, the
// quiet time and slack.
std::int64_t verdict_bound(const Harness& run, std::uint32_t device) {
    const CompiledChannel& channel = run.scenario.channel;
    const FailureRules& rules = channel.devices[device].failure;
    std::int64_t longest = 0;
    for (const CompiledDevice& other : channel.devices) {
        longest = std::max(longest, other.failure.timeout_ns);
    }
    const std::int64_t period =
        channel
            .poll_groups[channel.devices[device].points[0].poll_group.value_or(
                0)]
            .period_ns;
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
    return first_ns >= 0 && now_ns > first_ns + verdict_bound(run, device) &&
           run.tags.point(device, point).quality == tag::Quality::GOOD;
}

struct UnitState {
    Mode mode = Mode::ANSWERS;
    std::int64_t since_ns = 0;
    // The first request for the value's and the companion's register since
    // `since_ns`.
    std::int64_t first_value_ns = -1;
    std::int64_t first_companion_ns = -1;
};

// Checks invariants 1, 3 and 4 at `now_ns`; returns the violations.
std::vector<std::string> check(const Harness& run,
                               const std::vector<UnitState>& units,
                               std::int64_t now_ns) {
    std::vector<std::string> out;
    for (std::uint8_t unit = 1; unit <= run.scenario.units; ++unit) {
        const std::uint32_t device = unit - 1U;
        const UnitState& state = units[unit];
        const FailureRules& rules =
            run.scenario.channel.devices[device].failure;
        const bool chain = run.scenario.chained[device];
        if (state.mode == Mode::SILENT &&
            (good_past_bound(run, device, 0, state.first_value_ns, now_ns) ||
             (chain && good_past_bound(run, device, 1, state.first_companion_ns,
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
        if (state.mode == Mode::ANSWERS) {
            const ConnectionRules& connection = run.scenario.channel.connection;
            const std::int64_t recover =
                rules.backoff_max_ns + rules.timeout_ns * 10 +
                connection.reconnect_max_ns + connection.connect_timeout_ns +
                3 * SECOND;
            if (now_ns > state.since_ns + recover &&
                run.engine.failures().state(device) != DeviceStatus::RUNNING) {
                out.push_back("unit " + std::to_string(unit) +
                              " answering and not RUNNING at " +
                              std::to_string(now_ns / MILLISECOND));
            }
        }
    }
    return out;
}

// Invariant 2 over the whole run: the longest silence of the bus towards
// any unit, from 2 s on.
std::vector<std::string> check_asked(const Harness& run, std::int64_t end_ns) {
    std::vector<std::string> out;
    const ConnectionRules& connection = run.scenario.channel.connection;
    for (std::uint8_t unit = 1; unit <= run.scenario.units; ++unit) {
        const FailureRules& rules =
            run.scenario.channel.devices[unit - 1U].failure;
        const std::int64_t bound = rules.backoff_max_ns +
                                   rules.timeout_ns * 10 +
                                   connection.reconnect_max_ns +
                                   connection.connect_timeout_ns + 5 * SECOND;
        std::int64_t last = 0;
        for (const LoggedRequest& request : run.bus.log()) {
            if (request.unit_id != unit) {
                continue;
            }
            if (request.time_ns - last > bound) {
                out.push_back("unit " + std::to_string(unit) +
                              " not asked from " +
                              std::to_string(last / MILLISECOND) + " to " +
                              std::to_string(request.time_ns / MILLISECOND));
            }
            last = request.time_ns;
        }
        if (end_ns - last > bound) {
            out.push_back("unit " + std::to_string(unit) + " not asked after " +
                          std::to_string(last / MILLISECOND));
        }
    }
    return out;
}

// One seed: the engine's steps run, and every violation.
std::pair<std::size_t, std::vector<std::string>> run_seed(std::uint64_t seed) {
    constexpr std::int64_t END = 120 * SECOND;
    constexpr std::int64_t SAMPLE = 100 * MILLISECOND;
    Harness run(scenario_of(seed, END));
    std::vector<UnitState> units(run.scenario.units + 1U);
    std::vector<std::string> violations;
    std::size_t steps = 0;
    std::size_t next_event = 0;
    std::size_t seen = 0;  // requests of the bus's log looked at
    for (std::int64_t now = SAMPLE; now <= END; now += SAMPLE) {
        while (next_event < run.scenario.events.size() &&
               run.scenario.events[next_event].time_ns <= now) {
            const Scenario::Event& event = run.scenario.events[next_event];
            apply(run, event);
            units[event.unit] = {.mode = event.mode,
                                 .since_ns = event.time_ns,
                                 .first_value_ns = -1,
                                 .first_companion_ns = -1};
            ++next_event;
        }
        const std::size_t taken = run_engine(run.engine, run.clock, now);
        steps += taken;
        if (taken > 100'000) {
            // An engine that never waits: a spin (invariant 5).
            violations.push_back("spin at " +
                                 std::to_string(now / MILLISECOND) + " ms");
            break;
        }
        const std::vector<LoggedRequest>& log = run.bus.log();
        for (; seen < log.size(); ++seen) {
            UnitState& state = units[log[seen].unit_id];
            std::int64_t& first = log[seen].first == COMPANION_REGISTER
                                      ? state.first_companion_ns
                                      : state.first_value_ns;
            if (first < 0 && log[seen].time_ns >= state.since_ns) {
                first = log[seen].time_ns;
            }
        }
        for (std::string& violation : check(run, units, now)) {
            violations.push_back(std::move(violation));
        }
    }
    for (std::string& violation : check_asked(run, END)) {
        violations.push_back(std::move(violation));
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
