// Integration test for conformance test 22 of the tag database contract:
// the functions marked real-time safe make no system call and no
// allocation and take no page fault, the first read of a tag after its
// resolve included.
//
// The helper scada-tag-rt-probe calls those functions between two markers:
// scada_queue_post of an event, a request and a result among them, and
// scada_queue_next without waiting, which answers an expired request.
// This test runs it under the tracer of scada/test/syscall_trace.h, which
// records every system call between the markers; the helper counts its own
// allocations there. The test is not registered in the sanitizer builds,
// whose runtime makes system calls of its own, nor in a cross build that
// runs its programs through an emulator.

#include "tag_processes_fixture.h"
#include "tagd_fixture.h"

#include "scada/tag/scada_tag.h"
#include "scada/tag/tag_set.h"
#include "scada/test/child_process.h"
#include "scada/test/syscall_trace.h"

#include <gtest/gtest.h>

#include <sys/syscall.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace scada::test::tagd;
using scada::test::ChildProcess;
using scada::test::TraceResult;

constexpr std::chrono::seconds PROBE_LIMIT{60};
// The real-time safe calls the probe makes between its markers.
constexpr std::string_view ALL_CALLS_PASSED = "calls=13 failed=0 ";
// Its look at the request queue answered the expired request, and the
// answer was posted.
constexpr std::string_view EXPIRED_ANSWERED = " expired=1 dropped=0\n";

using Clock = std::chrono::steady_clock;

// The heartbeat timeout of `probe` in the small tag set: the default.
constexpr std::chrono::milliseconds PROBE_TIMEOUT{
    scada::tag::DEFAULT_HEARTBEAT_TIMEOUT_MS};
// How much longer than PROBE_TIMEOUT a late feeder is held after the
// probe has attached.
constexpr std::chrono::seconds LATE_MARGIN{1};

// The heartbeat count of process `index` as `ctx` sees it; 0 if it cannot
// be read (the process has not attached).
std::uint64_t heartbeat_of(scada_ctx* ctx, std::uint16_t index) {
    scada_proc_status_t status{};
    if (scada_proc_status(ctx, index, &status) != SCADA_OK) {
        return 0;
    }
    return status.heartbeat;
}

// Holds the stopped `feeder` until the probe has attached and then waited
// for longer than its heartbeat timeout, and lets it go on. Fails if the
// probe wrote no heartbeat meanwhile: the feeder would then see it down
// and never post (issue #73). Runs on its own thread while the probe is
// traced; the feeder goes on in every case, so that it can stop.
void release_late(const std::string& base_dir, ChildProcess& feeder) {
    {
        const Attached reader(base_dir, "reader");
        std::uint16_t index = 0;
        if (scada_proc_resolve(reader.ctx(), "probe", &index) != SCADA_OK) {
            ADD_FAILURE() << "probe not resolved";
        } else {
            const Clock::time_point limit = Clock::now() + WAIT_LIMIT;
            std::uint64_t first = 0;
            while (first == 0 && Clock::now() < limit) {
                // The region of the probe appeared after the reader asked
                // for it: notify looks again, at most once a second.
                static_cast<void>(scada_tag_notify(reader.ctx()));
                first = heartbeat_of(reader.ctx(), index);
                std::this_thread::yield();
            }
            // The clock is the condition: the property is a time.
            const Clock::time_point late =
                Clock::now() + PROBE_TIMEOUT + LATE_MARGIN;
            while (Clock::now() < late) {
                std::this_thread::yield();
            }
            EXPECT_NE(first, 0U) << "the probe did not attach";
            EXPECT_GT(heartbeat_of(reader.ctx(), index), first)
                << "the probe wrote no heartbeat while it waited";
        }
    }
    EXPECT_TRUE(feeder.send_signal(SIGCONT));
}

class RealTimeSafety : public TagProcesses {
protected:
    // Runs the probe as the process `probe` against a running scada-tagd,
    // a running owner `io` and `feeder`, which posts one request to the
    // probe that expires 1 ms later, with `fault` as its last argument
    // unless empty. With `late_feeder` the feeder is stopped until the
    // probe has waited for longer than its heartbeat timeout.
    [[nodiscard]] TraceResult traced_probe(const std::string& fault,
                                           bool late_feeder = false) {
        write_small_tag_set();
        std::optional<RunningTagd> tagd = ready_tagd();
        std::optional<ChildProcess> owner = attached_io();
        std::optional<ChildProcess> feeder =
            start_helper("SCADA_TAG_QUEUE_COMMAND",
                         {base(), "feeder", "request", "cmd.probe.feeder"});
        if (!tagd.has_value() || !owner.has_value() || !feeder.has_value()) {
            return {};
        }
        // The probe's wait set finds the request, which the feeder posts
        // once the probe is up; the feeder's output is not waited for.
        std::vector<std::string> arguments{base(), "probe", "probe.out",
                                           "plant.level"};
        if (!fault.empty()) {
            arguments.push_back(fault);
        }
        std::optional<std::jthread> releaser;
        if (late_feeder) {
            // Attached, it handles SIGTERM; it cannot have posted, since
            // the probe has not attached yet.
            EXPECT_TRUE(feeder->wait_for_output(
                [](std::string_view out) {
                    return out.find("attached") != std::string_view::npos;
                },
                WAIT_LIMIT));
            EXPECT_TRUE(feeder->send_signal(SIGSTOP));
            releaser.emplace(release_late, base(), std::ref(*feeder));
        }
        const TraceResult result = scada::test::trace_marked_syscalls(
            command_from("SCADA_TAG_RT_PROBE_COMMAND", arguments), PROBE_LIMIT);
        releaser.reset();  // joins
        const ::testing::AssertionResult ran = ran_every_call(result);
        EXPECT_TRUE(stops_cleanly(*feeder));
        EXPECT_TRUE(ran) << "feeder: " << feeder->out() << feeder->err();
        EXPECT_TRUE(stops_cleanly(*owner));
        stop(*tagd);
        return result;
    }

    // The probe was traced, ended well, had one marked part in which every
    // call returned what it should, and answered the expired request.
    static ::testing::AssertionResult ran_every_call(
        const TraceResult& result) {
        if (!result.traced || result.exit_code != 0 ||
            result.marked_parts != 1) {
            return ::testing::AssertionFailure()
                   << "the probe was not traced, or failed: " << result.out;
        }
        if (!result.out.starts_with(ALL_CALLS_PASSED) ||
            result.out.find(EXPIRED_ANSWERED) == std::string::npos) {
            return ::testing::AssertionFailure() << result.out;
        }
        return ::testing::AssertionSuccess();
    }

    // The probe took no page fault and made no allocation between its
    // markers, apart from `allocations` on purpose.
    static void expect_quiet(const TraceResult& result, int allocations) {
        EXPECT_NE(result.out.find(
                      " allocations=" + std::to_string(allocations) + " "),
                  std::string::npos)
            << result.out;
        EXPECT_NE(result.out.find(" page_faults=0 "), std::string::npos)
            << result.out;
    }
};

TEST_F(RealTimeSafety, Conformance22NoSystemCallAllocationOrPageFault) {
    const TraceResult result = traced_probe("");
    EXPECT_EQ(result.syscalls, std::vector<long>{})
        << "system calls inside real-time safe functions";
    expect_quiet(result, 0);
    // A machine whose clocks cannot be read without a system call breaks
    // the real-time rule (contract 13.1). The library reports it, and this
    // test does not pass on such a machine.
    EXPECT_NE(result.out.find("clock_syscall=0"), std::string::npos)
        << "the library reports that clock reads are system calls here: "
        << result.out;
    RecordProperty("probe", result.out);
}

// A feeder that posts only after the probe has waited for longer than the
// probe's heartbeat timeout still reaches it: the probe writes its
// heartbeat while it waits (issue #73).
TEST_F(RealTimeSafety, AProbeThatWaitsLongStaysLiveForTheFeeder) {
    const TraceResult result = traced_probe("", true);
    EXPECT_EQ(result.syscalls, std::vector<long>{});
}

// The instrumentation itself: a system call between the markers is found.
TEST_F(RealTimeSafety, ASystemCallBetweenTheMarkersIsFound) {
    const TraceResult result = traced_probe("syscall");
    EXPECT_EQ(std::ranges::count(result.syscalls, long{SYS_getppid}), 1);
    expect_quiet(result, 0);
}

// The instrumentation itself: an allocation between the markers is found.
TEST_F(RealTimeSafety, AnAllocationBetweenTheMarkersIsFound) {
    const TraceResult result = traced_probe("allocation");
    EXPECT_EQ(std::ranges::count(result.syscalls, long{SYS_getppid}), 0);
    expect_quiet(result, 1);
}

}  // namespace
