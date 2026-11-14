// Integration test for conformance test 22 of the tag database contract:
// the functions marked real-time safe make no system call and no
// allocation, the first read of a tag after its resolve included.
//
// The helper scada-tag-rt-probe calls those functions between two markers.
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
constexpr std::string_view ALL_CALLS_PASSED = "calls=7 failed=0 ";

class RealTimeSafety : public TagProcesses {
protected:
    // Runs the probe as the process `probe` against a running scada-tagd
    // and a running owner `io`, with `fault` as its last argument unless
    // empty.
    [[nodiscard]] TraceResult traced_probe(const std::string& fault) {
        write_small_tag_set();
        std::optional<RunningTagd> tagd = ready_tagd();
        std::optional<ChildProcess> owner = attached_io();
        if (!tagd.has_value() || !owner.has_value()) {
            return {};
        }
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
        EXPECT_TRUE(stops_cleanly(*owner));
        stop(*tagd);
        return result;
    }

    // The system calls of `result` that the contract does not excuse: on a
    // machine whose clocks cannot be read without a system call, which the
    // library reports, reading the clock is one (section 13.1).
    [[nodiscard]] static std::vector<long> unexcused(
        const TraceResult& result) {
        std::vector<long> calls = result.syscalls;
        if (result.out.find("clock_syscall=1") != std::string::npos) {
            std::erase(calls, long{SYS_clock_gettime});
        }
        return calls;
    }
};

TEST_F(RealTimeSafety, Conformance22NoSystemCallAndNoAllocation) {
    const TraceResult result = traced_probe("");
    EXPECT_EQ(unexcused(result), std::vector<long>{})
        << "system calls inside real-time safe functions";
    EXPECT_NE(result.out.find(" allocations=0 "), std::string::npos)
        << result.out;
    // Reported, not judged: a machine without a clock in the vDSO breaks
    // the real-time rule, and the library says so.
    RecordProperty("probe", result.out);
}

// The instrumentation itself: a system call between the markers is found.
TEST_F(RealTimeSafety, ASystemCallBetweenTheMarkersIsFound) {
    const TraceResult result = traced_probe("syscall");
    EXPECT_EQ(unexcused(result), std::vector<long>{SYS_getppid});
    EXPECT_NE(result.out.find(" allocations=0 "), std::string::npos)
        << result.out;
}

// The instrumentation itself: an allocation between the markers is found.
TEST_F(RealTimeSafety, AnAllocationBetweenTheMarkersIsFound) {
    const TraceResult result = traced_probe("allocation");
    EXPECT_EQ(unexcused(result), std::vector<long>{});
    EXPECT_NE(result.out.find(" allocations=1 "), std::string::npos)
        << result.out;
}

}  // namespace
