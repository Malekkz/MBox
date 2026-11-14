#ifndef SCADA_TEST_SYSCALL_TRACE_H
#define SCADA_TEST_SYSCALL_TRACE_H

// Test support: run a program and record the system calls it makes inside
// the parts it marks. This is the instrumentation behind conformance test
// 22 of the tag database contract: functions marked real-time safe make no
// system call. Linux 5.3 or newer; needs permission to trace a child
// process, which a normal user has.
//
// The traced program calls `mark_trace_begin()` before and
// `mark_trace_end()` after the code under test. Each marker is itself a
// system call (a seek on a descriptor that cannot exist), which is how the
// tracer sees it.

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace scada::test {

// What a traced run did.
struct TraceResult {
    // False if the program could not be started or traced; nothing else
    // is filled then.
    bool traced = false;
    // The exit code, or nothing if a signal ended the program, which
    // includes the time limit.
    std::optional<int> exit_code;
    // Its standard output, at most 64 KiB.
    std::string out;
    // How many marked parts it ran through, begin to end.
    int marked_parts = 0;
    // The number of every system call entered inside a marked part, in
    // order. Numbers are those of the architecture (<sys/syscall.h>).
    std::vector<long> syscalls;
};

// Runs `command`, the program followed by its arguments, with its standard
// output captured, and traces the thread that starts it. The program is
// ended by a signal if it runs longer than `limit`. Threads the program
// starts are not traced. The program should print little: output beyond
// 64 KiB blocks it until the limit.
[[nodiscard]] TraceResult trace_marked_syscalls(
    std::span<const std::string> command, std::chrono::seconds limit);

// The markers, for the traced program. Each makes one system call and
// nothing else.
void mark_trace_begin() noexcept;
void mark_trace_end() noexcept;

}  // namespace scada::test

#endif  // SCADA_TEST_SYSCALL_TRACE_H
