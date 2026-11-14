#include "scada/test/syscall_trace.h"

#include "scada/test/child_process.h"

#include <fcntl.h>
#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal functions are declared here, not in <csignal>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace scada::test {

namespace {

// Descriptors that no process has: a seek on them fails and changes
// nothing.
constexpr int BEGIN_MARKER_FD = -4242;
constexpr int END_MARKER_FD = -4243;

constexpr std::size_t OUTPUT_LIMIT = std::size_t{64} * 1024;
constexpr std::size_t READ_CHUNK = 4096;
constexpr int EXIT_NOT_STARTED = 127;
// The stop a traced process makes at a system call, once
// PTRACE_O_TRACESYSGOOD is set.
constexpr int SYSCALL_STOP = SIGTRAP | 0x80;

// The first argument of a system call, a descriptor, as the `int` the
// program passed: the kernel reports the full register.
int descriptor_argument(std::uint64_t argument) noexcept {
    // Deliberate truncation to the low 32 bits, then the same bits as int.
    return static_cast<int>(static_cast<std::uint32_t>(argument));
}

// Everything `descriptor` holds until its other end is closed, up to
// OUTPUT_LIMIT bytes.
std::string read_all(int descriptor) {
    std::string text;
    std::array<char, READ_CHUNK> chunk{};
    while (text.size() < OUTPUT_LIMIT) {
        const ssize_t count = ::read(descriptor, chunk.data(), chunk.size());
        if (count <= 0) {
            break;
        }
        // In range: positive and at most the size of the chunk.
        text.append(chunk.data(), static_cast<std::size_t>(count));
    }
    return text;
}

// In the child, between fork and exec: only calls that are safe there.
// Never returns.
[[noreturn]] void become_traced_program(const std::vector<char*>& arguments,
                                        int output, unsigned limit_seconds) {
    // The limit survives exec: a program that hangs is ended by SIGALRM.
    ::alarm(limit_seconds);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): ptrace has no other form
    if (::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) == 0 &&
        ::dup2(output, STDOUT_FILENO) >= 0) {
        // Stops with SIGTRAP at the exec, where the tracer takes over.
        ::execvp(arguments.front(), arguments.data());
    }
    ::_exit(EXIT_NOT_STARTED);
}

// Looks at the system call stop the traced process `pid` is in. At the
// entry of a system call it records a marker or, inside a marked part
// (`inside`), the call itself.
void record_syscall_stop(pid_t pid, bool& inside, TraceResult& result) {
    __ptrace_syscall_info info{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): ptrace has no other form
    if (::ptrace(PTRACE_GET_SYSCALL_INFO, pid, sizeof(info), &info) <= 0 ||
        info.op != PTRACE_SYSCALL_INFO_ENTRY) {
        return;  // the stop at the end of a system call
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): the kernel reports a system call entry in this member
    const auto& entry = info.entry;
    const bool is_seek = entry.nr == SYS_lseek;
    const int descriptor = descriptor_argument(entry.args[0]);
    if (is_seek && descriptor == BEGIN_MARKER_FD) {
        inside = true;
    } else if (is_seek && descriptor == END_MARKER_FD) {
        result.marked_parts += inside ? 1 : 0;
        inside = false;
    } else if (inside) {
        // In range: a system call number is small.
        result.syscalls.push_back(static_cast<long>(entry.nr));
    }
}

// Takes over the traced process `pid`, which has stopped at its exec.
// False, with the process ended, if that fails.
bool begin_trace(pid_t pid) {
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status)) {
        return false;  // it ended before the exec
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): ptrace has no other form
    if (::ptrace(PTRACE_SETOPTIONS, pid, nullptr,
                 PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL) != 0) {
        static_cast<void>(::kill(pid, SIGKILL));
        static_cast<void>(::waitpid(pid, &status, 0));
        return false;
    }
    return true;
}

// Follows the traced process `pid` from its stop at exec to its end and
// fills `result`.
void follow(pid_t pid, TraceResult& result) {
    result.traced = begin_trace(pid);
    bool inside = false;
    int signal_to_deliver = 0;
    while (result.traced) {
        int status = 0;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): ptrace has no other form
        if (::ptrace(PTRACE_SYSCALL, pid, nullptr, signal_to_deliver) != 0 ||
            ::waitpid(pid, &status, 0) != pid) {
            result.traced = false;
            return;
        }
        signal_to_deliver = 0;
        if (WIFEXITED(status)) {
            result.exit_code = WEXITSTATUS(status);
            return;
        }
        if (!WIFSTOPPED(status)) {
            return;  // ended by a signal
        }
        if (WSTOPSIG(status) == SYSCALL_STOP) {
            record_syscall_stop(pid, inside, result);
        } else if (WSTOPSIG(status) != SIGTRAP) {
            // A signal for the program: it gets it when it continues. The
            // stop at exec is reported as SIGTRAP and is not passed on.
            signal_to_deliver = WSTOPSIG(status);
        }
    }
}

}  // namespace

TraceResult trace_marked_syscalls(std::span<const std::string> command,
                                  std::chrono::seconds limit) {
    TraceResult result;
    if (command.empty() || limit.count() <= 0) {
        return result;
    }
    // The argument vector is built before the fork: no allocation after it.
    std::vector<std::string> owned(command.begin(), command.end());
    std::vector<char*> arguments;
    arguments.reserve(owned.size() + 1);
    for (std::string& argument : owned) {
        arguments.push_back(argument.data());
    }
    arguments.push_back(nullptr);

    std::array<int, 2> ends{-1, -1};
    if (::pipe2(ends.data(), O_CLOEXEC) != 0) {
        return result;
    }
    const UniqueFd read_end(ends[0]);
    UniqueFd write_end(ends[1]);

    const pid_t pid = ::fork();
    if (pid < 0) {
        return result;
    }
    if (pid == 0) {
        // In range: checked to be positive; seconds of a test limit.
        become_traced_program(arguments, write_end.get(),
                              static_cast<unsigned>(limit.count()));
    }
    write_end.reset();
    follow(pid, result);
    result.out = read_all(read_end.get());
    return result;
}

void mark_trace_begin() noexcept {
    static_cast<void>(::lseek(BEGIN_MARKER_FD, 0, SEEK_CUR));
}

void mark_trace_end() noexcept {
    static_cast<void>(::lseek(END_MARKER_FD, 0, SEEK_CUR));
}

}  // namespace scada::test
