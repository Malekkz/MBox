// Performance test of the simulator writer (issue P1-10): scada-sim sustains
// 50,000 updates per second across 10 owners on 50,000 tags, the reference
// load of the tag database contract (section 15, test 2), both as 10 owner
// threads of one process and as 10 processes of one owner each. It also
// reports the rate of one process writing as fast as it can.
//
// The simulator is paced and never writes ahead: if it keeps up, a run of
// ROUNDS writes of every tag takes ROUNDS seconds, plus the 1 ms tick of
// its last write. On x86-64, the machine of the acceptance of issue #18, a
// run passes if every update was written, it took at most ROUNDS seconds
// plus ELAPSED_MARGIN, and no update was more than BEHIND_LIMIT late; and
// one process writing as fast as it can must reach the reference rate. On
// ARM64, and with SCADA_PERF_REPORT_ONLY=1 in the environment, the figures
// are only printed; the count of updates is checked everywhere
// (decision D-41).

#include "tag_processes_fixture.h"
#include "tagd_fixture.h"

#include "scada/sim/layout.h"
#include "scada/sim/pattern.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace scada::test::tagd;
using scada::sim::Layout;
using scada::test::ChildProcess;

constexpr std::uint64_t REFERENCE_RATE = 50'000;
constexpr std::uint32_t OWNERS = 10;
constexpr std::uint64_t TAGS = 50'000;
// Every tag once a second: a run of ROUNDS seconds.
constexpr std::uint64_t ROUNDS = 10;
constexpr std::uint64_t MILLISECONDS_PER_SECOND = 1000;
// The limits of a paced run on x86-64.
constexpr std::uint64_t ELAPSED_MARGIN_MS = 100;
constexpr std::uint64_t BEHIND_LIMIT_MS = 250;
// Writes of every tag at the maximum rate.
constexpr std::uint64_t FAST_ROUNDS = 40;

#if defined(__x86_64__)
constexpr bool IS_X86_64 = true;
#else
constexpr bool IS_X86_64 = false;
#endif

// True if the figures are only reported: not on x86-64, or
// SCADA_PERF_REPORT_ONLY is 1.
bool report_only() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing changes the environment while the tests run
    const char* const text = std::getenv("SCADA_PERF_REPORT_ONLY");
    return !IS_X86_64 || (text != nullptr && std::string_view(text) == "1");
}

// The number after `key` in `text`, or nothing.
std::optional<std::uint64_t> number_after(std::string_view text,
                                          std::string_view key) {
    const std::size_t position = text.find(key);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view rest = text.substr(position + key.size());
    std::uint64_t value = 0;
    const std::from_chars_result parsed =
        std::from_chars(rest.data(), rest.data() + rest.size(), value);
    return parsed.ec == std::errc{} ? std::optional(value) : std::nullopt;
}

bool done(std::string_view out) {
    return out.find("done updates=") != std::string_view::npos;
}

// What the simulator processes of one run reported, together.
struct RunFigures {
    std::uint64_t updates = 0;
    std::uint64_t elapsed_ms = 0;     // of the slowest process
    std::uint64_t behind_max_ms = 0;  // of the latest update of any
};

Layout reference(std::uint32_t processes) {
    return Layout{.prefix = "load",
                  .processes = processes,
                  .owners = OWNERS,
                  .tags = TAGS,
                  .patterns = {scada::sim::Pattern::CHECK}};
}

class SimRate : public TagProcesses {
protected:
    // Runs every process of `layout` with `rate` and `rounds` and adds up
    // what they report. Nothing, with a failed expectation, if one did not
    // finish.
    [[nodiscard]] std::optional<RunFigures> run(const Layout& layout,
                                                std::uint64_t rate,
                                                std::uint64_t rounds) const {
        std::vector<ChildProcess> sims;
        for (std::uint32_t instance = 0; instance < layout.processes;
             ++instance) {
            std::optional<ChildProcess> sim =
                start_sim(layout,
                          {"--rate", std::to_string(rate), "--rounds",
                           std::to_string(rounds)},
                          instance);
            if (!sim.has_value()) {
                return std::nullopt;
            }
            sims.push_back(std::move(*sim));
        }
        RunFigures figures;
        bool all_done = true;
        for (ChildProcess& sim : sims) {
            if (!sim.wait_for_output(done, WAIT_LIMIT * 2)) {
                ADD_FAILURE() << "a simulator did not finish: " << sim.err();
                all_done = false;
                continue;
            }
            figures.updates +=
                number_after(sim.out(), "done updates=").value_or(0);
            figures.elapsed_ms =
                std::max(figures.elapsed_ms,
                         number_after(sim.out(), "elapsed_ms=").value_or(0));
            figures.behind_max_ms =
                std::max(figures.behind_max_ms,
                         number_after(sim.out(), "behind_max_ms=").value_or(0));
        }
        for (ChildProcess& sim : sims) {
            EXPECT_TRUE(stops_cleanly(sim));
        }
        if (!all_done) {
            return std::nullopt;
        }
        return figures;
    }

    // Runs the reference load with `processes` processes and checks the
    // rate.
    void expect_reference_rate(std::uint32_t processes) {
        const Layout layout = reference(processes);
        write_sim_tag_set(layout);
        std::optional<RunningTagd> tagd = ready_tagd();
        if (!tagd.has_value()) {
            return;
        }
        const std::optional<RunFigures> figures =
            run(layout, REFERENCE_RATE, ROUNDS);
        stop(*tagd);
        if (!figures.has_value() || figures->elapsed_ms == 0) {
            FAIL() << "no figures";
        }
        const std::uint64_t per_second =
            figures->updates * MILLISECONDS_PER_SECOND / figures->elapsed_ms;
        std::cout << "simulator rate: processes=" << processes
                  << " owners=" << OWNERS << " updates=" << figures->updates
                  << " elapsed_ms=" << figures->elapsed_ms
                  << " updates_per_second=" << per_second
                  << " behind_max_ms=" << figures->behind_max_ms << "\n";
        RecordProperty("updates_per_second", std::to_string(per_second));
        RecordProperty("behind_max_ms", std::to_string(figures->behind_max_ms));
        EXPECT_EQ(figures->updates, ROUNDS * TAGS);
        if (!report_only()) {
            EXPECT_LE(figures->elapsed_ms,
                      (ROUNDS * MILLISECONDS_PER_SECOND) + ELAPSED_MARGIN_MS)
                << figures->updates << " updates";
            EXPECT_LE(figures->behind_max_ms, BEHIND_LIMIT_MS);
        }
    }
};

TEST_F(SimRate, TenOwnerThreadsOfOneProcessSustainTheReferenceRate) {
    expect_reference_rate(1);
}

TEST_F(SimRate, TenProcessesSustainTheReferenceRate) {
    expect_reference_rate(OWNERS);
}

// The maximum rate of one process with 10 owner threads: at least the
// reference rate on x86-64.
TEST_F(SimRate, MaximumRateOfOneProcess) {
    const Layout layout = reference(1);
    write_sim_tag_set(layout);
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    const std::optional<RunFigures> figures = run(layout, 0, FAST_ROUNDS);
    stop(*tagd);
    if (!figures.has_value()) {
        FAIL() << "no figures";
    }
    const std::uint64_t per_second =
        figures->updates * MILLISECONDS_PER_SECOND /
        std::max<std::uint64_t>(figures->elapsed_ms, 1);
    std::cout << "simulator maximum rate: updates=" << figures->updates
              << " elapsed_ms=" << figures->elapsed_ms
              << " updates_per_second=" << per_second << "\n";
    RecordProperty("updates_per_second", std::to_string(per_second));
    EXPECT_EQ(figures->updates, FAST_ROUNDS * TAGS);
    if (!report_only()) {
        EXPECT_GE(per_second, REFERENCE_RATE);
    }
}

}  // namespace
