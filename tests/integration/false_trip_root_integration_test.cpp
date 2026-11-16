// Conformance test 26 of the tag database contract (section 15, false
// trips): the reference load runs with real-time readers sharing the
// processors with the non-real-time owners; no process is reported down,
// and the count of SCADA_E_BUSY is reported. Contract section 16 settles
// the read retry limit with it: fewer than 1 SCADA_E_BUSY in 1,000,000
// reads.
//
// scada-sim writes the reference load (50,000 tags, 10 owners, 50,000
// updates per second) as a normal process with the default heartbeat
// timeout of 3,000 ms. scada-tag-rt-reader, a process the tag set marks
// REALTIME with the default timeout of 500 ms, runs 4 readers under
// SCHED_FIFO, one on each processor in turn, each a 10 ms control task
// that reads 5,000 tags per cycle: 2,000,000 reads per second in all. That
// load is fixed (decision D-57): the rate of SCADA_E_BUSY is per read, so a
// different number of reads per cycle would change it. The run lasts 10 s,
// or SCADA_CONFORMANCE_SECONDS (600 for the 10 minutes of the contract;
// the job `measurements` does that).
//
// Every build fails on a false trip: a read that gives OWNER_DOWN while
// the owner runs, a stale or late heartbeat, a torn, backwards or failed
// read. The criterion for SCADA_E_BUSY gates only in an optimised build
// without a sanitizer (SCADA_TAG_BUSY_GATE=1); the dev build prints the
// rate only. A real-time reader that preempts a non-real-time owner in the
// middle of a write on the same processor keeps that record odd until the
// reader's cycle ends, and the unoptimised write path is longer (D-57).
//
// SCHED_FIFO needs root, so the test has the CTest label `root` and the
// pipeline runs it with sudo (`ctest --preset root`). The helper limits
// its own real-time CPU time (RLIMIT_RTTIME) and its run time, and dies
// with the test binary, so a fault cannot hang the runner.

#include "tag_processes_fixture.h"
#include "tagd_fixture.h"

#include "scada/common/result.h"
#include "scada/sim/tag_set.h"
#include "scada/tag/tag_set.h"
#include "scada/test/child_process.h"
#include "scada/test/cpu_affinity.h"

#include <gtest/gtest.h>

#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal numbers are declared here, not in <csignal>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace scada::test::tagd;
using scada::test::ChildProcess;

constexpr std::uint64_t REFERENCE_RATE = 50'000;
constexpr std::uint64_t DEFAULT_SECONDS = 10;
constexpr std::uint64_t MAX_SECONDS = 3600;
constexpr std::size_t RT_READERS = 4;
constexpr const char* RT_PROCESS = "rtreader";
// The rate the simulator must reach, in percent of the rate asked for.
constexpr std::uint64_t RATE_PERCENT = 95;
constexpr std::uint64_t PERCENT = 100;
constexpr std::uint64_t MILLISECONDS_PER_SECOND = 1000;
// The criterion of contract section 16 for the read retry limit: fewer
// than 1 SCADA_E_BUSY in this many reads.
constexpr double READS_PER_ALLOWED_BUSY = 1'000'000.0;
// The run fails when its count of SCADA_E_BUSY is above this quantile of
// the count a true rate of exactly the criterion would give over its reads
// (decision D-57).
constexpr double BUSY_QUANTILE = 0.999;

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

// How often `text` holds `part`.
std::size_t occurrences(std::string_view text, std::string_view part) {
    std::size_t count = 0;
    for (std::size_t at = text.find(part); at != std::string_view::npos;
         at = text.find(part, at + part.size())) {
        ++count;
    }
    return count;
}

// SCADA_CONFORMANCE_SECONDS if it is set to 1 to MAX_SECONDS, otherwise
// DEFAULT_SECONDS.
std::uint64_t load_seconds() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing changes the environment while the tests run
    const char* const text = std::getenv("SCADA_CONFORMANCE_SECONDS");
    if (text == nullptr) {
        return DEFAULT_SECONDS;
    }
    const std::optional<std::uint64_t> seconds = number_after(text, "");
    if (!seconds.has_value() || *seconds == 0 || *seconds > MAX_SECONDS) {
        ADD_FAILURE() << "SCADA_CONFORMANCE_SECONDS is not 1 to "
                      << MAX_SECONDS;
        return DEFAULT_SECONDS;
    }
    return *seconds;
}

class FalseTrips : public TagProcesses {
protected:
    void SetUp() override {
        ASSERT_EQ(::geteuid(), 0U)
            << "this test runs threads under SCHED_FIFO; run it as root "
               "(sudo ctest --preset root)";
        TagProcesses::SetUp();
    }

    // The reference load with the default heartbeat timeouts, and the
    // reader process RT_PROCESS marked REALTIME.
    void write_tag_set() const {
        scada::sim::TagSetOptions options;
        options.layout = load_layout();
        options.uid = ::geteuid();
        options.tagd_uid = ::geteuid();
        options.readers = {RT_PROCESS};
        std::string detail;
        scada::Result<scada::tag::TagSet> tag_set =
            scada::sim::make_tag_set(options, detail);
        if (!tag_set.ok()) {
            ADD_FAILURE() << "no tag set: " << detail;
            return;
        }
        scada::tag::TagSet set = std::move(tag_set).value();
        bool found = false;
        for (scada::tag::ProcessDefinition& process : set.processes) {
            if (process.name == RT_PROCESS) {
                process.realtime = true;
                process.heartbeat_timeout_ms =
                    scada::tag::DEFAULT_REALTIME_HEARTBEAT_TIMEOUT_MS;
                found = true;
            }
        }
        EXPECT_TRUE(found);
        std::ofstream file(tag_set_path());
        file << scada::sim::tag_set_json(set);
    }

    // Starts scada-tag-rt-reader on the reference load, for at most
    // `limit`.
    [[nodiscard]] std::optional<ChildProcess> start_rt_readers(
        std::chrono::seconds limit) const {
        const scada::sim::Layout layout = load_layout();
        return start_helper(
            "SCADA_TAG_RT_READER_COMMAND",
            {base(), RT_PROCESS, layout.prefix, std::to_string(layout.owners),
             std::to_string(layout.tags), std::to_string(RT_READERS),
             std::to_string(limit.count())});
    }
};

// The counts line of scada-tag-rt-reader in `out`, with a space in front,
// so that every key can be looked for with the space before it.
std::string counts_line(const std::string& out) {
    const std::size_t line = out.rfind("\nreads=");
    return " " + (line == std::string::npos ? out : out.substr(line + 1));
}

// The number after ` <key>=` in `counts`, recorded as a property of the
// test; 0 with a failed expectation if it is missing.
std::uint64_t field(const std::string& counts, const std::string& key) {
    const std::optional<std::uint64_t> value =
        number_after(counts, " " + key + "=");
    EXPECT_TRUE(value.has_value()) << key << " in: " << counts;
    ::testing::Test::RecordProperty(key, std::to_string(value.value_or(0)));
    return value.value_or(0);
}

// The real-time readers saw only whole writes of a running owner and ran
// as a REALTIME process with the default timeout.
void expect_reader_counts(const std::string& counts) {
    EXPECT_EQ(field(counts, "torn"), 0U);
    EXPECT_EQ(field(counts, "backwards"), 0U);
    EXPECT_EQ(field(counts, "errors"), 0U);
    // A read that gave OWNER_DOWN while the owner ran: a false trip.
    EXPECT_EQ(field(counts, "down"), 0U);
    static_cast<void>(field(counts, "overruns"));
    static_cast<void>(field(counts, "longest_cycle_us"));
    EXPECT_EQ(field(counts, "realtime"), 1U);
    EXPECT_EQ(field(counts, "timeout_ms"),
              scada::tag::DEFAULT_REALTIME_HEARTBEAT_TIMEOUT_MS);
}

// The smallest count k with P(X <= k) >= `quantile` for X Poisson with mean
// `mean`. Summed in logarithms, so that a mean of thousands does not
// underflow; the terms far below the mean are 0 and do not matter.
std::uint64_t poisson_quantile(double mean, double quantile) {
    double log_term = -mean;  // log P(X = 0)
    double sum = 0.0;
    // Far above the quantile of any mean: the loop always ends.
    const auto last =
        static_cast<std::uint64_t>(mean + (20.0 * std::sqrt(mean)) + 100.0);
    for (std::uint64_t k = 0; k < last; ++k) {
        if (k > 0) {
            log_term += std::log(mean) - std::log(static_cast<double>(k));
        }
        sum += std::exp(log_term);
        if (sum >= quantile) {
            return k;
        }
    }
    return last;
}

// True if CTest says that this build gates on the criterion for
// SCADA_E_BUSY: an optimised build without a sanitizer (decision D-57).
bool busy_rate_gates() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing changes the environment while the tests run
    const char* const gate = std::getenv("SCADA_TAG_BUSY_GATE");
    return gate != nullptr && std::string_view(gate) == "1";
}

// The count of SCADA_E_BUSY of the real-time readers is consistent with
// the criterion of contract section 16 (fewer than 1 in 1,000,000 reads),
// in a build that gates on it; other builds only report it. The test is
// statistical (decision D-57): it fails when the count is above the 99.9 %
// quantile of a Poisson count with the mean that a rate of exactly 1 in
// 1,000,000 gives over the reads of this run (35 for 20 million reads). A
// change that doubles the rate fails it most of the time; a build at the
// criterion fails it about once in a thousand runs. The 600 s run of the
// job `measurements` decides the criterion itself.
void expect_busy_rate(const std::string& counts) {
    const std::uint64_t reads = field(counts, "reads");
    const std::uint64_t busy = field(counts, "busy");
    EXPECT_GT(reads, 0U);
    const bool gates = busy_rate_gates();
    const std::uint64_t bound = poisson_quantile(
        static_cast<double>(reads) / READS_PER_ALLOWED_BUSY, BUSY_QUANTILE);
    const double per_million =
        static_cast<double>(busy) * READS_PER_ALLOWED_BUSY /
        static_cast<double>(std::max<std::uint64_t>(reads, 1));
    std::cout << "busy=" << busy << " reads=" << reads << " bound=" << bound
              << " per_million=" << per_million
              << (gates ? " (gated)" : " (reported only)") << "\n";
    ::testing::Test::RecordProperty("busy_bound", std::to_string(bound));
    if (gates) {
        EXPECT_LE(busy, bound) << busy << " SCADA_E_BUSY in " << reads
                               << " reads; bound " << bound;
    }
}

// scada-tagd reported no process down, nor a heartbeat late enough for
// readers to have seen a process down. The clean stops at the end give
// REASON="not running".
void expect_no_trip_in(const std::string& log) {
    const std::size_t stale = occurrences(log, R"(REASON="heartbeat stale")");
    const std::size_t late = occurrences(log, "heartbeat late");
    ::testing::Test::RecordProperty("stale", std::to_string(stale));
    ::testing::Test::RecordProperty("late", std::to_string(late));
    EXPECT_EQ(stale, 0U) << log;
    EXPECT_EQ(late, 0U) << log;
}

// The simulator, whose output is `out`, wrote `seconds` of the reference
// load at 95 % of its rate or more.
void expect_reference_rate(const std::string& out, std::uint64_t seconds) {
    const std::optional<std::uint64_t> elapsed_ms =
        number_after(out, "elapsed_ms=");
    if (!elapsed_ms.has_value() || *elapsed_ms == 0) {
        ADD_FAILURE() << "the simulator did not report its time: " << out;
        return;
    }
    const std::uint64_t per_second =
        seconds * REFERENCE_RATE * MILLISECONDS_PER_SECOND / *elapsed_ms;
    ::testing::Test::RecordProperty("updates_per_second",
                                    std::to_string(per_second));
    EXPECT_GE(per_second * PERCENT, REFERENCE_RATE * RATE_PERCENT);
}

// Releases `writer`, a simulator held with --hold, and waits up to `limit`
// for it to be done; then stops `readers` and waits for its exit.
void run_beside(ChildProcess& writer, ChildProcess& readers,
                std::chrono::seconds limit) {
    EXPECT_TRUE(writer.send_signal(SIGUSR1));
    EXPECT_TRUE(writer.wait_for_output(
        [](std::string_view out) {
            return out.find("done ") != std::string_view::npos;
        },
        limit))
        << writer.out() << writer.err();
    EXPECT_TRUE(readers.send_signal(SIGTERM));
    EXPECT_EQ(readers.wait_for_exit(WAIT_LIMIT), 0) << readers.err();
}

TEST_F(FalseTrips, Conformance26RealTimeReadersBesideTheOwnersTripNothing) {
    const std::uint64_t seconds = load_seconds();
    // In range: at most MAX_SECONDS.
    const std::chrono::seconds limit(static_cast<std::int64_t>(seconds * 4) +
                                     WAIT_LIMIT.count());
    const std::size_t cpus = scada::test::allowed_cpus().size();
    std::cout << "cpus=" << cpus << " seconds=" << seconds << "\n";
    RecordProperty("cpus", std::to_string(cpus));
    RecordProperty("seconds", std::to_string(seconds));

    write_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    std::optional<ChildProcess> writer = start_sim(
        load_layout(), {"--hold", "--rate", std::to_string(REFERENCE_RATE),
                        "--rounds", std::to_string(seconds)});
    if (!writer.has_value() || !prints(*writer, "ready\n")) {
        return;
    }
    std::optional<ChildProcess> readers = start_rt_readers(limit);
    if (!readers.has_value() || !prints(*readers, "running\n")) {
        return;
    }
    run_beside(*writer, *readers, limit);
    EXPECT_TRUE(stops_cleanly(*writer));
    stop(*tagd);

    const std::string counts = counts_line(readers->out());
    std::cout << counts;
    expect_reader_counts(counts);
    expect_busy_rate(counts);
    expect_no_trip_in(tagd->child.err());
    expect_reference_rate(writer->out(), seconds);
}

}  // namespace
