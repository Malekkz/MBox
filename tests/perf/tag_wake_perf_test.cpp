// Performance test of the wake-up of a change ring consumer (issue P1-07,
// contract section 8.4): scada-sim writes one tag every 2 ms and
// calls scada_tag_notify; this process is blocked in scada_sub_next and
// measures, for each change, the time from the write (`update_mono_ns`,
// CLOCK_MONOTONIC of the same machine) to its return. Acceptance: the
// 99th percentile is below 1 ms, on x86-64 and ARM64 (decision D-57).
//
// With SCADA_PERF_REPORT_ONLY=1 in the environment it only prints the
// figures: the pipeline runs it so on shared runners, whose timing is not
// fit to gate on (decisions D-13, D-32).

#include "tag_processes_fixture.h"
#include "tagd_fixture.h"

#include "scada/sim/layout.h"
#include "scada/sim/pattern.h"
#include "scada/tag/scada_tag.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <time.h>  // NOLINT(modernize-deprecated-headers): clock_gettime and CLOCK_MONOTONIC are POSIX, declared here

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace scada::test::tagd;
using scada::test::ChildProcess;

// One write every 2 ms, so that the consumer is blocked when it comes.
constexpr std::uint64_t WRITE_RATE = 500;
constexpr std::uint64_t WRITES = 3000;
// Changes measured; the first ones, written before the subscription was
// open or while this process was still starting, are not.
constexpr std::size_t SAMPLES = 2500;
constexpr int WAIT_MS = 5000;
constexpr std::int64_t NANOSECONDS_PER_MICROSECOND = 1000;
// The target of issue P1-07 (decision D-32) for a consumer that keeps up:
// it receives within one producer cycle (contract section 15, test 24).
constexpr std::int64_t P99_LIMIT_NS = 1'000'000;
constexpr std::size_t PERCENT = 100;
constexpr std::size_t P99 = 99;

std::int64_t monotonic_now_ns() {
    timespec now{};
    EXPECT_EQ(::clock_gettime(CLOCK_MONOTONIC, &now), 0);
    constexpr std::int64_t NANOSECONDS_PER_SECOND = 1'000'000'000;
    return (std::int64_t{now.tv_sec} * NANOSECONDS_PER_SECOND) + now.tv_nsec;
}

// True if SCADA_PERF_REPORT_ONLY is 1.
bool report_only() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing changes the environment while the tests run
    const char* const text = std::getenv("SCADA_PERF_REPORT_ONLY");
    return text != nullptr && std::string_view(text) == "1";
}

// Takes SAMPLES changes from `sub` and returns, sorted, the time from each
// write to the return of scada_sub_next. Empty, with a failed expectation,
// if a change did not come.
std::vector<std::int64_t> measure(scada_sub* sub) {
    std::vector<std::int64_t> latencies;
    latencies.reserve(SAMPLES);
    // The first change may have been written before this process waited.
    scada_change change{};
    if (scada_sub_next(sub, &change, WAIT_MS) != SCADA_OK) {
        ADD_FAILURE() << "no first change";
        return {};
    }
    while (latencies.size() < SAMPLES) {
        const int code = scada_sub_next(sub, &change, WAIT_MS);
        const std::int64_t woke = monotonic_now_ns();
        if (code != SCADA_OK) {
            ADD_FAILURE() << "code " << code << " after " << latencies.size();
            return {};
        }
        latencies.push_back(woke - change.update_mono_ns);
    }
    std::ranges::sort(latencies);
    return latencies;
}

// Prints and records median, 99th percentile and maximum of the sorted
// `latencies`, and returns the 99th percentile.
std::int64_t report(std::span<const std::int64_t> latencies) {
    const std::int64_t median =
        latencies[latencies.size() / 2] / NANOSECONDS_PER_MICROSECOND;
    const std::int64_t p99 = latencies[(latencies.size() * P99) / PERCENT];
    const std::int64_t worst = latencies.back() / NANOSECONDS_PER_MICROSECOND;
    std::cout << "wake latency: samples=" << latencies.size()
              << " median_us=" << median
              << " p99_us=" << p99 / NANOSECONDS_PER_MICROSECOND
              << " max_us=" << worst << "\n";
    ::testing::Test::RecordProperty("median_us", std::to_string(median));
    ::testing::Test::RecordProperty(
        "p99_us", std::to_string(p99 / NANOSECONDS_PER_MICROSECOND));
    ::testing::Test::RecordProperty("max_us", std::to_string(worst));
    return p99;
}

// The 99th percentile `p99` is below 1 ms, on every architecture (decision
// D-57); not checked with SCADA_PERF_REPORT_ONLY.
::testing::AssertionResult meets_target(std::int64_t p99) {
    if (report_only() || p99 < P99_LIMIT_NS) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << "99th percentile " << p99 << " ns, limit " << P99_LIMIT_NS;
}

// scada-sim with one owner `load` and one tag.
scada::sim::Layout one_tag() {
    return scada::sim::Layout{.prefix = "load",
                              .processes = 1,
                              .owners = 1,
                              .tags = 1,
                              .patterns = {scada::sim::Pattern::CHECK}};
}

class WakeLatency : public TagProcesses {};

TEST_F(WakeLatency, ABlockedConsumerWakesWithinOneMillisecond) {
    const scada::sim::Layout layout = one_tag();
    write_sim_tag_set(layout);
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        FAIL() << "no tagd";
    }
    std::optional<ChildProcess> writer =
        start_sim(layout, {"--rate", std::to_string(WRITE_RATE), "--rounds",
                           std::to_string(WRITES), "--notify", "each"});
    if (!writer.has_value()) {
        FAIL() << "no writer";
    }
    const Attached reader(base(), "reader");
    scada_sub* sub = nullptr;
    ASSERT_EQ(scada_sub_open(reader.ctx(), "load", &sub), SCADA_OK);
    const std::vector<std::int64_t> latencies = measure(sub);
    scada_sub_close(sub);
    ASSERT_EQ(latencies.size(), SAMPLES);
    EXPECT_TRUE(meets_target(report(latencies)));
    EXPECT_TRUE(stops_cleanly(*writer));
    stop(*tagd);
}

// Measures every subscription of `subs` at once, one thread each, and
// returns all samples together, sorted. Empty, with a failed expectation,
// if one of them did not get SAMPLES.
std::vector<std::int64_t> measure_all(std::span<scada_sub* const> subs) {
    std::vector<std::vector<std::int64_t>> each(subs.size());
    {
        std::vector<std::jthread> threads;
        for (std::size_t i = 0; i < subs.size(); ++i) {
            // By value: a thread captures nothing by reference (coding
            // standard, section 3). `each` is not resized while they run.
            threads.emplace_back(
                [out = &each[i], sub = subs[i]] { *out = measure(sub); });
        }
    }
    std::vector<std::int64_t> all;
    for (const std::vector<std::int64_t>& latencies : each) {
        if (latencies.size() != SAMPLES) {
            ADD_FAILURE() << "a subscription got " << latencies.size();
            return {};
        }
        all.insert(all.end(), latencies.begin(), latencies.end());
    }
    std::ranges::sort(all);
    return all;
}

// Observers of the test process, each with a subscription to load.w0;
// closed and detached when it goes out of scope.
class Observers {
public:
    Observers(const std::string& base, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            observers_.push_back(std::make_unique<Observed>(base));
            scada_sub* sub = nullptr;
            EXPECT_EQ(scada_sub_open(observers_.back()->ctx(), "load", &sub),
                      SCADA_OK);
            subs_.push_back(sub);
        }
    }
    ~Observers() {
        for (scada_sub* sub : subs_) {
            scada_sub_close(sub);
        }
    }
    Observers(const Observers&) = delete;
    Observers& operator=(const Observers&) = delete;
    Observers(Observers&&) = delete;
    Observers& operator=(Observers&&) = delete;

    [[nodiscard]] std::span<scada_sub* const> subs() const { return subs_; }

private:
    std::vector<std::unique_ptr<Observed>> observers_;
    std::vector<scada_sub*> subs_;
};

// Eight observers wait on the same ring (contract, section 11.4): one
// notify of the writer wakes them all with one system call. Report only,
// on every target: nine threads waking on four processors give a noisy
// 99th percentile (68 us to 449 us on idle or lightly loaded machines in
// the review of #55, and outliers of tens to hundreds of ms on a loaded
// one, where the single consumer above fails too), so a gate here would
// fail for the machine, not for the library.
TEST_F(WakeLatency, EightWaitingObserversEachWakeWithinOneMillisecond) {
    constexpr std::size_t OBSERVERS = 8;
    const scada::sim::Layout layout = one_tag();
    write_sim_tag_set(layout);
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        FAIL() << "no tagd";
    }
    std::optional<ChildProcess> writer =
        start_sim(layout, {"--rate", std::to_string(WRITE_RATE), "--rounds",
                           std::to_string(WRITES), "--notify", "each"});
    if (!writer.has_value()) {
        FAIL() << "no writer";
    }
    {
        // Subscribed once the writer has its ring, as in the case above, so
        // that no subscription waits for the ring to be looked for again.
        const Observers observers(base(), OBSERVERS);
        const std::vector<std::int64_t> all = measure_all(observers.subs());
        ASSERT_EQ(all.size(), OBSERVERS * SAMPLES);
        std::cout << "with " << OBSERVERS << " observers, all samples: ";
        static_cast<void>(report(all));  // report only, see above
    }
    EXPECT_TRUE(stops_cleanly(*writer));
    stop(*tagd);
}

}  // namespace
