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

#include "scada/test/child_process.h"
#include "scada/test/syscall_trace.h"

#include <gtest/gtest.h>

#include <sys/syscall.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scada::test::tagd;
using scada::test::ChildProcess;
using scada::test::TraceResult;

constexpr std::chrono::seconds PROBE_LIMIT{60};
// The real-time safe calls the probe makes between its markers.
constexpr std::string_view ALL_CALLS_PASSED = "calls=11 failed=0 ";
// Its look at the request queue answered the expired request, and the
// answer was posted.
constexpr std::string_view EXPIRED_ANSWERED = " expired=1 dropped=0\n";

class RealTimeSafety : public TagProcesses {
protected:
    // Runs the probe as the process `probe` against a running scada-tagd,
    // a running owner `io` and `feeder`, which posts one request to the
    // probe that expires 1 ms later, with `fault` as its last argument
    // unless empty.
    [[nodiscard]] TraceResult traced_probe(const std::string& fault) {
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
        const TraceResult result = scada::test::trace_marked_syscalls(
            command_from("SCADA_TAG_RT_PROBE_COMMAND", arguments), PROBE_LIMIT);
        EXPECT_TRUE(result.traced) << "the probe could not be traced";
        EXPECT_EQ(result.exit_code, 0) << result.out;
        EXPECT_EQ(result.marked_parts, 1);
        EXPECT_TRUE(result.out.starts_with(ALL_CALLS_PASSED)) << result.out;
        EXPECT_NE(result.out.find(EXPIRED_ANSWERED), std::string::npos)
            << result.out;
        EXPECT_TRUE(stops_cleanly(*feeder));
        EXPECT_TRUE(stops_cleanly(*owner));
        stop(*tagd);
        return result;
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
