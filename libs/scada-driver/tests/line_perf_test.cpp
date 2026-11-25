// How dead devices share a line with live ones, with the default failure
// keys (CS 4.6: time-out 1 s, retries 2, demote_after 3, back-off 10 s to
// 300 s): the figures of decision D-138, in the contract's order of service
// (DI 6.2), and the known limits it leaves (serving answering devices first
// is #147).
// Driven on the simulated transport and an injected clock, so nothing
// sleeps; each case simulates up to 40 minutes. Only `ctest --preset perf`
// runs it.

#include "channel_builder.h"
#include "loopback.h"
#include "scada/driver/channel_engine.h"
#include "scada/driver/compiled.h"
#include "scada/driver/failure.h"
#include "scada/driver/plan.h"
#include "scada/tag/types.h"
#include "sim_transport.h"
#include "test_protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace scada::driver::test {
namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): the line, the unit counts and the times are the test data

constexpr std::int64_t LATENCY = MILLISECOND;
constexpr std::int64_t MINUTE = 60 * SECOND;
constexpr std::uint32_t FAST = 0;    // the 200 ms group of the builder
constexpr std::uint32_t NORMAL = 1;  // the 1 s group of the builder
using Fault = SimBus::Fault;

// `units` units of one point each in the 1 s group, every fifth in
// `fifth_group`, on a serial line at `bit_rate`, with the default failure
// keys.
CompiledChannel line_of(std::uint8_t units, std::uint32_t bit_rate,
                        std::uint32_t fifth_group) {
    std::vector<DeviceSpec> specs;
    for (std::uint8_t unit = 1; unit <= units; ++unit) {
        specs.push_back(
            {.name = "d" + std::to_string(unit),
             .unit_id = unit,
             .points = adjacent(0, 1, unit % 5 == 0 ? fifth_group : NORMAL)});
    }
    CompiledChannel channel = make_channel(specs, ReadRules{.max_units = 64});
    channel.transport = TransportKind::SERIAL;
    channel.line.bit_rate = bit_rate;
    return channel;
}

// NOLINTBEGIN(misc-non-private-member-variables-in-classes): a test rig whose parts are its interface
struct Line {
    Line(CompiledChannel compiled, std::uint8_t units)
        : channel(std::move(compiled)),
          transport(bus, clock, LATENCY),
          engine(channel, protocol, transport, clock, tags) {
        for (std::uint8_t unit = 1; unit <= units; ++unit) {
            bus.map(unit, 0, 0, 0);
        }
    }

    CompiledChannel channel;
    SimBus bus;
    SimClock clock;
    SimTransport transport;
    RecordingTags tags;
    TestProtocol protocol;
    ChannelEngine engine;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

struct Figures {
    std::int64_t all_demoted_ns = -1;   // every dead unit DEMOTED
    std::int64_t back_running_ns = -1;  // the returned unit RUNNING again
    std::int64_t start_gap_ns = 0;      // worst live gap, 10 s to 2 min
    std::int64_t death_gap_ns = 0;      // worst live gap, 2 min after a death
    std::int64_t live_gap_ns = 0;       // worst live gap after that
    std::int64_t demoted_gap_ns = 0;    // worst live gap once all are DEMOTED
    std::int64_t bad_ns = -1;  // death to COMM_FAIL, the slowest dead unit
    std::vector<double> dead_share;  // per minute: the line held by the dead
};

// The units that never answer, by unit id: every fifth.
std::vector<bool> every_fifth(std::uint8_t units) {
    std::vector<bool> dead(units + 1U, false);
    for (std::uint8_t unit = 5; unit <= units; unit += 5) {
        dead[unit] = true;
    }
    return dead;
}

// The dead units are all DEMOTED.
bool all_demoted(const Line& line, const std::vector<bool>& dead) {
    for (std::size_t unit = 1; unit < dead.size(); ++unit) {
        if (dead[unit] && line.engine.failures().state(
                              static_cast<std::uint32_t>(unit - 1)) !=
                              DeviceStatus::DEMOTED) {
            return false;
        }
    }
    return true;
}

// The dead units' points are all COMM_FAIL.
bool all_bad(const Line& line, const std::vector<bool>& dead) {
    for (std::size_t unit = 1; unit < dead.size(); ++unit) {
        if (dead[unit] &&
            !line.tags.has_flag(static_cast<std::uint32_t>(unit - 1), 0,
                                tag::QualityFlag::COMM_FAIL)) {
            return false;
        }
    }
    return true;
}

// When the units die, when one of them returns, when the run ends.
struct Times {
    std::int64_t death_ns = 0;  // 0: dead from the start
    std::int64_t back_ns = 30 * MINUTE;
    std::int64_t end_ns = 40 * MINUTE;
};

// Runs `line` with the `dead` units silent from `times.death_ns`; `back`
// answers again from `times.back_ns`. Fills the times of `out`.
void run(Line& line, const std::vector<bool>& dead, std::uint8_t back,
         const Times& times, Figures& out) {
    const auto die = [&] {
        for (std::size_t unit = 1; unit < dead.size(); ++unit) {
            if (dead[unit]) {
                line.bus.fault(static_cast<std::uint8_t>(unit), Fault::SILENT);
            }
        }
    };
    if (times.death_ns == 0) {
        die();
    }
    const std::int64_t back_ns = times.back_ns;
    // When the units died by the simulated clock: a step runs the exchange
    // it started to its end, past the loop's time (review P2).
    std::int64_t died_ns = 0;
    for (std::int64_t now = SECOND; now <= times.end_ns; now += SECOND) {
        if (now == back_ns) {
            line.bus.fault(back, Fault::NONE);
        }
        (void)run_engine(line.engine, line.clock, now);
        if (now == times.death_ns) {
            die();
            died_ns = line.clock.monotonic_ns();
        }
        if (out.bad_ns < 0 && now > times.death_ns && all_bad(line, dead)) {
            out.bad_ns = line.clock.monotonic_ns() - died_ns;
        }
        if (out.all_demoted_ns < 0 && now < back_ns &&
            all_demoted(line, dead)) {
            out.all_demoted_ns = line.clock.monotonic_ns();
        }
        if (now > back_ns && out.back_running_ns < 0 &&
            line.engine.failures().state(back - 1U) == DeviceStatus::RUNNING) {
            out.back_running_ns = line.clock.monotonic_ns() - back_ns;
        }
    }
}

// The line time of the dead per minute, and the worst live gaps, from the
// bus's log: each request holds the line until the next one is sent.
void analyse(const Line& line, const std::vector<bool>& dead, std::uint8_t back,
             const Times& times, Figures& out) {
    const std::int64_t back_ns = times.back_ns;
    const std::int64_t end_ns = times.end_ns;
    const std::int64_t death_ns = times.death_ns;
    const std::vector<LoggedRequest>& log = line.bus.log();
    std::vector<std::int64_t> last(dead.size(), -1);
    out.dead_share.assign(static_cast<std::size_t>(end_ns / MINUTE), 0.0);
    for (std::size_t index = 0; index + 1 < log.size(); ++index) {
        const LoggedRequest& request = log[index];
        const auto minute = static_cast<std::size_t>(request.time_ns / MINUTE);
        if (dead[request.unit_id] && request.time_ns >= death_ns &&
            (request.unit_id != back || request.time_ns < back_ns) &&
            minute < out.dead_share.size()) {
            out.dead_share[minute] +=
                static_cast<double>(log[index + 1].time_ns - request.time_ns) /
                static_cast<double>(MINUTE);
        }
        if (dead[request.unit_id] && request.time_ns >= death_ns) {
            continue;
        }
        const std::int64_t previous = last[request.unit_id];
        last[request.unit_id] = request.time_ns;
        if (previous < 10 * SECOND) {
            continue;
        }
        // A gap belongs to the window its end falls in.
        const std::int64_t gap = request.time_ns - previous;
        if (out.all_demoted_ns > 0 && previous >= out.all_demoted_ns) {
            out.demoted_gap_ns = std::max(out.demoted_gap_ns, gap);
        }
        const std::int64_t settled =
            std::max(2 * MINUTE, death_ns + 2 * MINUTE);
        if (death_ns > 0 && request.time_ns >= death_ns &&
            previous < death_ns + 2 * MINUTE) {
            out.death_gap_ns = std::max(out.death_gap_ns, gap);
        } else if (previous < 2 * MINUTE) {
            out.start_gap_ns = std::max(out.start_gap_ns, gap);
        } else if (previous >= settled) {
            out.live_gap_ns = std::max(out.live_gap_ns, gap);
        }
    }
}

Figures measure(std::uint8_t units, std::uint32_t bit_rate,
                std::int64_t death_ns = 0, std::uint32_t dead_group = NORMAL) {
    Line line(line_of(units, bit_rate, dead_group), units);
    const std::vector<bool> dead = every_fifth(units);
    const Times times{.death_ns = death_ns};
    Figures out;
    run(line, dead, 5, times, out);
    analyse(line, dead, 5, times, out);
    return out;
}

double seconds(std::int64_t time_ns) {
    return static_cast<double>(time_ns) / 1e9;
}

void print(const char* name, const Figures& figures) {
    std::cout << std::format(
        "{}: all dead COMM_FAIL {:.0f} s after their death, DEMOTED at "
        "{:.0f} s; the returned unit RUNNING {:.0f} s after its return; worst "
        "live gap {:.3f} s from 10 s to 2 min, {:.3f} s in the 2 min after "
        "the death, {:.3f} s after, {:.3f} s once all are DEMOTED\n",
        name, seconds(figures.bad_ns), seconds(figures.all_demoted_ns),
        seconds(figures.back_running_ns), seconds(figures.start_gap_ns),
        seconds(figures.death_gap_ns), seconds(figures.live_gap_ns),
        seconds(figures.demoted_gap_ns));
    for (std::size_t minute = 0; minute < figures.dead_share.size();
         minute += 5) {
        std::cout << std::format(
            "  minute {}: the dead hold {:.0f} % of the "
            "line\n",
            minute, 100.0 * figures.dead_share[minute]);
    }
}

TEST(LinePerf, FiftyDeadAmongTwoHundredFiftyUnits) {
    // 250 units at 38,400 bit/s, every fifth dead; unit 5 returns at 30 min.
    // In the contract's order the dead take their turns like any due
    // request until they are demoted: a known limit (D-138, #147). Measured:
    // COMM_FAIL by 54 s (J2), all DEMOTED at 464 s, a worst live gap of
    // 102.8 s, the returned unit RUNNING 267 s after its return.
    const Figures figures = measure(250, 38'400);
    print("250 units, 50 dead", figures);
    EXPECT_GT(figures.bad_ns, 0);
    EXPECT_LE(figures.bad_ns, 60 * SECOND);
    EXPECT_GT(figures.all_demoted_ns, 0);
    EXPECT_LE(figures.all_demoted_ns, 10 * MINUTE);
    EXPECT_LE(figures.start_gap_ns, 110 * SECOND);
    EXPECT_LE(figures.live_gap_ns, 110 * SECOND);
    EXPECT_GE(figures.back_running_ns, 0);
    EXPECT_LE(figures.back_running_ns, 5 * MINUTE + 30 * SECOND);
}

TEST(LinePerf, FiftyOfTwoHundredFiftyUnitsDieTogether) {
    // J2, J4: the same line, all 250 answering until 2 min; then every
    // fifth dies. Each dead unit is COMM_FAIL one period after its own first
    // time-out (J2); the dead hold the line until they are demoted, the
    // known limit (measured: COMM_FAIL by 54 s after the death, all DEMOTED
    // at 584 s, a worst live gap of 102.8 s after the death, 1.0 s before).
    const Figures figures = measure(250, 38'400, 2 * MINUTE);
    print("250 units, 50 die together at 2 min", figures);
    EXPECT_GT(figures.bad_ns, 0);
    EXPECT_LE(figures.bad_ns, 60 * SECOND);
    EXPECT_LE(figures.start_gap_ns, 1 * SECOND + 100 * MILLISECOND);
    EXPECT_LE(figures.death_gap_ns, 110 * SECOND);
    EXPECT_LE(figures.live_gap_ns, 110 * SECOND);
}

TEST(LinePerf, FiftyDeadOfAFasterGroupAmongTwoHundredFiftyUnits) {
    // The first case with the dead units in the 200 ms group, of a higher
    // priority than the live ones' 1 s group: by priority the dead go first
    // until they are demoted (measured: COMM_FAIL by 54 s, all DEMOTED at
    // 460 s, a worst live gap of 52.1 s, the returned unit RUNNING 260 s
    // after its return).
    const Figures figures = measure(250, 38'400, 0, FAST);
    print("250 units, 50 dead in the 200 ms group", figures);
    EXPECT_GT(figures.bad_ns, 0);
    EXPECT_LE(figures.bad_ns, 60 * SECOND);
    EXPECT_GT(figures.all_demoted_ns, 0);
    EXPECT_LE(figures.all_demoted_ns, 10 * MINUTE);
    EXPECT_LE(figures.live_gap_ns, 60 * SECOND);
    EXPECT_GE(figures.back_running_ns, 0);
    EXPECT_LE(figures.back_running_ns, 5 * MINUTE + 30 * SECOND);
}

TEST(LinePerf, FiveDeadAmongTwentyFiveUnits) {
    // A line with idle time: 25 units at 19,200 bit/s, every fifth dead;
    // unit 5 returns at 30 min. Probes go in the windows before the next
    // due poll, their time-out ended there (G7); a windowed probe that gets
    // no answer is no failure, so each back-off ends with a floor probe of
    // the whole 1 s, which holds one poll at most (J1). Until the dead are
    // demoted they take their turns like any due request (measured: a live
    // gap of 10.3 s at the start, 1.76 s after).
    const Figures figures = measure(25, 19'200);
    print("25 units, 5 dead", figures);
    EXPECT_LE(figures.start_gap_ns, 12 * SECOND);
    EXPECT_GT(figures.all_demoted_ns, 0);
    EXPECT_LE(figures.all_demoted_ns, 4 * MINUTE);
    EXPECT_LE(figures.bad_ns, 10 * SECOND);
    EXPECT_LE(figures.live_gap_ns, 2 * SECOND + 50 * MILLISECOND);
    EXPECT_GE(figures.back_running_ns, 0);
    EXPECT_LE(figures.back_running_ns, 5 * MINUTE + 30 * SECOND);
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

}  // namespace
}  // namespace scada::driver::test
