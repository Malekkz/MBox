// Performance test of the wake-up of a change ring consumer (issue P1-07,
// contract section 8.4): a writer process writes one tag every 2 ms and
// calls scada_tag_notify; this process is blocked in scada_sub_next and
// measures, for each change, the time from the write (`update_mono_ns`,
// CLOCK_MONOTONIC of the same machine) to its return. Acceptance: the
// 99th percentile is below 1 ms on x86-64; on ARM64 it is reported.
//
// With SCADA_PERF_REPORT_ONLY=1 in the environment it only prints the
// figures: the pipeline runs it so on shared runners, whose timing is not
// fit to gate on (decisions D-13, D-31).

#include "tag_processes_fixture.h"
#include "tagd_fixture.h"

#include "scada/tag/scada_tag.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <time.h>  // NOLINT(modernize-deprecated-headers): clock_gettime and CLOCK_MONOTONIC are POSIX, declared here

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
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

class WakeLatency : public TagProcesses {};

TEST_F(WakeLatency, ABlockedConsumerWakesWithinOneMillisecond) {
    write_load_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    std::optional<ChildProcess> writer =
        start_helper("SCADA_TAG_WRITER_COMMAND",
                     {base(), "load", "1", "1", std::to_string(WRITE_RATE),
                      std::to_string(WRITES), "notify"});
    if (!writer.has_value()) {
        return;
    }
    ASSERT_TRUE(writer->wait_for_output(attached, WAIT_LIMIT)) << writer->err();
    const Attached reader(base(), "reader");
    scada_sub* sub = nullptr;
    ASSERT_EQ(scada_sub_open(reader.ctx(), "load.w0", &sub), SCADA_OK);

    std::vector<std::int64_t> latencies;
    latencies.reserve(SAMPLES);
    // The first change may have been written before this process waited.
    scada_change change{};
    ASSERT_EQ(scada_sub_next(sub, &change, WAIT_MS), SCADA_OK);
    while (latencies.size() < SAMPLES) {
        const int code = scada_sub_next(sub, &change, WAIT_MS);
        const std::int64_t woke = monotonic_now_ns();
        ASSERT_EQ(code, SCADA_OK) << "after " << latencies.size();
        latencies.push_back(woke - change.update_mono_ns);
    }
    scada_sub_close(sub);

    std::ranges::sort(latencies);
    const std::int64_t median = latencies[latencies.size() / 2];
    const std::int64_t p99 = latencies[(latencies.size() * P99) / PERCENT];
    const std::int64_t worst = latencies.back();
    std::cout << "wake latency: samples=" << latencies.size()
              << " median_us=" << median / NANOSECONDS_PER_MICROSECOND
              << " p99_us=" << p99 / NANOSECONDS_PER_MICROSECOND
              << " max_us=" << worst / NANOSECONDS_PER_MICROSECOND << "\n";
    RecordProperty("median_us",
                   std::to_string(median / NANOSECONDS_PER_MICROSECOND));
    RecordProperty("p99_us", std::to_string(p99 / NANOSECONDS_PER_MICROSECOND));
    RecordProperty("max_us",
                   std::to_string(worst / NANOSECONDS_PER_MICROSECOND));
#if defined(__x86_64__)
    if (!report_only()) {
        EXPECT_LT(p99, P99_LIMIT_NS);
    }
#endif

    EXPECT_TRUE(stops_cleanly(*writer));
    stop(*tagd);
}

}  // namespace
