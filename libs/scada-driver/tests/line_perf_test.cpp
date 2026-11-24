// How dead devices share a line with live ones, with the default failure
// keys (CS 4.6: time-out 1 s, retries 2, demote_after 3, back-off 10 s to
// 300 s): the figures of decision D-138 and the review of PR 137 (G5, G7).
// Driven on the simulated transport and an injected clock, so nothing
// sleeps; each case simulates up to 40 minutes. Only `ctest --preset perf`
// runs it.

#include "channel_builder.h"
#include "loopback.h"
#include "scada/driver/channel_engine.h"
#include "scada/driver/compiled.h"
#include "scada/driver/failure.h"
#include "scada/driver/plan.h"
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
constexpr std::uint32_t NORMAL = 1;  // the 1 s group of the builder
using Fault = SimBus::Fault;

// `units` units of one point each in the 1 s group, on a serial line at
// `bit_rate`, with the default failure keys.
CompiledChannel line_of(std::uint8_t units, std::uint32_t bit_rate) {
    std::vector<DeviceSpec> specs;
    for (std::uint8_t unit = 1; unit <= units; ++unit) {
        specs.push_back({.name = "d" + std::to_string(unit),
                         .unit_id = unit,
                         .points = adjacent(0, 1, NORMAL)});
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
    std::int64_t live_gap_ns = 0;       // worst live gap after 2 min
    std::vector<double> dead_share;     // per minute: the line held by the dead
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

// Runs `line` to `end_ns` with the `dead` units silent; `back` answers again
// from `back_ns`. Fills the times of `out`.
void run(Line& line, const std::vector<bool>& dead, std::uint8_t back,
         std::int64_t back_ns, std::int64_t end_ns, Figures& out) {
    for (std::size_t unit = 1; unit < dead.size(); ++unit) {
        if (dead[unit]) {
            line.bus.fault(static_cast<std::uint8_t>(unit), Fault::SILENT);
        }
    }
    for (std::int64_t now = SECOND; now <= end_ns; now += SECOND) {
        if (now == back_ns) {
            line.bus.fault(back, Fault::NONE);
        }
        (void)run_engine(line.engine, line.clock, now);
        if (out.all_demoted_ns < 0 && now < back_ns &&
            all_demoted(line, dead)) {
            out.all_demoted_ns = now;
        }
        if (now > back_ns && out.back_running_ns < 0 &&
            line.engine.failures().state(back - 1U) == DeviceStatus::RUNNING) {
            out.back_running_ns = now - back_ns;
        }
    }
}

// The line time of the dead per minute, and the worst live gaps, from the
// bus's log: each request holds the line until the next one is sent.
void analyse(const Line& line, const std::vector<bool>& dead, std::uint8_t back,
             std::int64_t back_ns, std::int64_t end_ns, Figures& out) {
    const std::vector<LoggedRequest>& log = line.bus.log();
    std::vector<std::int64_t> last(dead.size(), -1);
    out.dead_share.assign(static_cast<std::size_t>(end_ns / MINUTE), 0.0);
    for (std::size_t index = 0; index + 1 < log.size(); ++index) {
        const LoggedRequest& request = log[index];
        const auto minute = static_cast<std::size_t>(request.time_ns / MINUTE);
        if (dead[request.unit_id] &&
            (request.unit_id != back || request.time_ns < back_ns) &&
            minute < out.dead_share.size()) {
            out.dead_share[minute] +=
                static_cast<double>(log[index + 1].time_ns - request.time_ns) /
                static_cast<double>(MINUTE);
        }
        if (dead[request.unit_id]) {
            continue;
        }
        const std::int64_t previous = last[request.unit_id];
        last[request.unit_id] = request.time_ns;
        std::int64_t& worst =
            previous < 2 * MINUTE ? out.start_gap_ns : out.live_gap_ns;
        if (previous >= 10 * SECOND) {
            worst = std::max(worst, request.time_ns - previous);
        }
    }
}

Figures measure(std::uint8_t units, std::uint32_t bit_rate) {
    Line line(line_of(units, bit_rate), units);
    const std::vector<bool> dead = every_fifth(units);
    Figures out;
    run(line, dead, 5, 30 * MINUTE, 40 * MINUTE, out);
    analyse(line, dead, 5, 30 * MINUTE, 40 * MINUTE, out);
    return out;
}

double seconds(std::int64_t time_ns) {
    return static_cast<double>(time_ns) / 1e9;
}

void print(const char* name, const Figures& figures) {
    std::cout << std::format(
        "{}: all dead DEMOTED at {:.0f} s; the returned unit RUNNING {:.0f} s "
        "after its return; worst live gap {:.3f} s from 10 s to 2 min, "
        "{:.3f} s after\n",
        name, seconds(figures.all_demoted_ns), seconds(figures.back_running_ns),
        seconds(figures.start_gap_ns), seconds(figures.live_gap_ns));
    for (std::size_t minute = 0; minute < figures.dead_share.size();
         minute += 5) {
        std::cout << std::format(
            "  minute {}: the dead hold {:.0f} % of the "
            "line\n",
            minute, 100.0 * figures.dead_share[minute]);
    }
}

TEST(LinePerf, FiftyDeadAmongTwoHundredFiftyUnits) {
    // The case of the delta check: 250 units at 38,400 bit/s, every fifth
    // dead; unit 5 returns at 30 min. Live polls go first within their
    // priority (G5), so the live keep about their period while the dead are
    // still being counted, and the dead take the time the live leave.
    const Figures figures = measure(250, 38'400);
    print("250 units, 50 dead", figures);
    EXPECT_GT(figures.all_demoted_ns, 0);
    EXPECT_LE(figures.all_demoted_ns, 20 * MINUTE);
    EXPECT_LE(figures.start_gap_ns, 2 * SECOND);
    EXPECT_LE(figures.live_gap_ns, 2 * SECOND + 100 * MILLISECOND);
    EXPECT_GE(figures.back_running_ns, 0);
    EXPECT_LE(figures.back_running_ns, 5 * MINUTE + 30 * SECOND);
}

TEST(LinePerf, FiveDeadAmongTwentyFiveUnits) {
    // A line with idle time: 25 units at 19,200 bit/s, every fifth dead;
    // unit 5 returns at 30 min. Probes go in the windows before the next
    // due poll, their time-out ended there (G7); the live keep their 1 s
    // period.
    const Figures figures = measure(25, 19'200);
    print("25 units, 5 dead", figures);
    EXPECT_GT(figures.all_demoted_ns, 0);
    EXPECT_LE(figures.all_demoted_ns, MINUTE);
    EXPECT_LE(figures.live_gap_ns, SECOND + 50 * MILLISECOND);
    EXPECT_GE(figures.back_running_ns, 0);
    EXPECT_LE(figures.back_running_ns, 5 * MINUTE + 30 * SECOND);
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

}  // namespace
}  // namespace scada::driver::test
