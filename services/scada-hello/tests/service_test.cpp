// Integration test for the scada-hello service. It starts the real executable
// with a NotifyListener standing in for systemd and checks the notification
// messages and the exit status.
//
// Usage: scada-hello-service-test <command> [<argument>...]
// where <command> runs scada-hello: the executable itself, or for a cross
// build an emulator, looked up on PATH, followed by the executable.
//
// It uses no test framework: the framework is chosen in issue P0-03.

#include "scada/hello/hello.h"
#include "scada/service/notify_listener.h"

#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal functions are declared here, not in <csignal>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using scada::service::NotifyListener;

// Watchdog interval given to the service. It feeds at half of it.
constexpr std::chrono::milliseconds WATCHDOG_INTERVAL{200};
// Upper limit for anything that is expected to happen.
constexpr std::chrono::milliseconds TIMEOUT{10000};
// How long the watchdog must stay silent to count as not fed: ten feed
// periods.
constexpr std::chrono::milliseconds SILENCE{1000};
// Exit code of a child that could not start the command, as a shell uses it.
constexpr int EXIT_NOT_STARTED = 127;

// Counts failed expectations and reports each one.
class Checker {
public:
    void expect(bool condition, std::string_view what) {
        if (!condition) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures_;
        }
    }

    [[nodiscard]] bool passed() const { return failures_ == 0; }

private:
    int failures_ = 0;
};

// How a child process ended.
struct Exit {
    bool signalled = false;
    int code = 0;  // Exit code, or the signal number if `signalled`.
};

// Owns one child process. Kills and reaps it on destruction if it still
// runs, so a failed test leaves nothing behind.
class Child {
public:
    // Starts `command` with NOTIFY_SOCKET set to `notify_socket`, the test's
    // watchdog interval, and no WATCHDOG_PID. The `env` utility sets the
    // variables, so this process's own environment is not changed.
    Child(std::span<char*> command, const std::string& notify_socket)
        : pid_(start(command, notify_socket)) {}

    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&&) = delete;
    Child& operator=(Child&&) = delete;

    ~Child() {
        if (started() && !reaped_) {
            static_cast<void>(::kill(pid_, SIGKILL));
            siginfo_t info{};
            static_cast<void>(::waitid(P_PID, child_id(), &info, WEXITED));
        }
    }

    [[nodiscard]] bool started() const { return pid_ > 0; }

    [[nodiscard]] bool signal(int signal_number) const {
        return started() && ::kill(pid_, signal_number) == 0;
    }

    // Waits up to `timeout` for the child to end. SIGCHLD must be blocked in
    // this thread; the wait is on that signal, not on a sleep.
    [[nodiscard]] std::optional<Exit> wait(std::chrono::milliseconds timeout) {
        if (!started() || reaped_) {
            return std::nullopt;
        }
        sigset_t child_signal{};
        if (::sigemptyset(&child_signal) != 0 ||
            ::sigaddset(&child_signal, SIGCHLD) != 0) {
            return std::nullopt;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            // si_pid stays zero if the child has not ended yet.
            siginfo_t info{};
            const int result =
                ::waitid(P_PID, child_id(), &info, WEXITED | WNOHANG);
            if (result == 0 && info.si_pid == pid_) {
                reaped_ = true;
                return Exit{info.si_code != CLD_EXITED, info.si_status};
            }
            const auto left = deadline - std::chrono::steady_clock::now();
            if (result != 0 || left <= std::chrono::nanoseconds::zero()) {
                return std::nullopt;
            }
            const auto seconds =
                std::chrono::duration_cast<std::chrono::seconds>(left);
            const std::chrono::nanoseconds rest = left - seconds;
            std::timespec limit{};
            limit.tv_sec = static_cast<std::time_t>(seconds.count());
            limit.tv_nsec = static_cast<long>(rest.count());
            static_cast<void>(::sigtimedwait(&child_signal, nullptr, &limit));
        }
    }

    [[nodiscard]] bool running() {
        return !wait(std::chrono::milliseconds(0)).has_value() && !reaped_;
    }

private:
    // Forks and runs the command in the child. Returns the child's process
    // id, or -1 if the fork failed.
    [[nodiscard]] static pid_t start(std::span<char*> command,
                                     const std::string& notify_socket) {
        const std::string usec = std::to_string(
            std::chrono::microseconds(WATCHDOG_INTERVAL).count());
        std::vector<std::string> words{"env", "-u", "WATCHDOG_PID",
                                       "NOTIFY_SOCKET=" + notify_socket,
                                       "WATCHDOG_USEC=" + usec};
        words.insert(words.end(), command.begin(), command.end());
        // execvp() wants writable strings and a null pointer at the end.
        std::vector<char*> arguments;
        arguments.reserve(words.size() + 1);
        for (std::string& word : words) {
            arguments.push_back(word.data());
        }
        arguments.push_back(nullptr);

        const pid_t pid = ::fork();
        if (pid != 0) {
            return pid;
        }
        // In the child. A watchdog abort must not write a core file.
        const rlimit no_core{0, 0};
        if (::setrlimit(RLIMIT_CORE, &no_core) == 0) {
            ::execvp(arguments.front(), arguments.data());
        }
        std::_Exit(EXIT_NOT_STARTED);
    }

    // The process id in the type waitid() takes; pid_ is positive here.
    [[nodiscard]] id_t child_id() const { return static_cast<id_t>(pid_); }

    pid_t pid_ = -1;
    bool reaped_ = false;
};

// Receives messages until `wanted` arrives. Watchdog messages before it are
// skipped; any other message, or none within TIMEOUT, is a failure.
bool expect_message(NotifyListener& listener, std::string_view wanted) {
    const auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const std::optional<std::string> message = listener.receive(left);
        if (!message) {
            std::cerr << "no '" << wanted << "' within the timeout\n";
            return false;
        }
        if (*message == wanted) {
            return true;
        }
        if (*message != "WATCHDOG=1") {
            std::cerr << "got '" << *message << "' while waiting for '"
                      << wanted << "'\n";
            return false;
        }
    }
}

void test_ready_feed_stop(Checker& checker, std::span<char*> command) {
    std::optional<NotifyListener> listener = NotifyListener::create();
    checker.expect(listener.has_value(), "stop: listener created");
    if (!listener) {
        return;
    }
    Child child(command, listener->socket_name());
    checker.expect(child.started(), "stop: service started");

    // READY=1 must be the first message.
    checker.expect(listener->receive(TIMEOUT) == "READY=1",
                   "stop: reports readiness first");
    checker.expect(listener->receive(TIMEOUT) == "WATCHDOG=1",
                   "stop: feeds the watchdog");
    checker.expect(listener->receive(TIMEOUT) == "WATCHDOG=1",
                   "stop: keeps feeding the watchdog");

    checker.expect(child.signal(SIGTERM), "stop: SIGTERM sent");
    checker.expect(expect_message(*listener, "STOPPING=1"),
                   "stop: reports that it is stopping");
    const std::optional<Exit> exit = child.wait(TIMEOUT);
    checker.expect(
        exit && !exit->signalled && exit->code == scada::hello::EXIT_OK,
        "stop: exits with code 0");
}

void test_hang(Checker& checker, std::span<char*> command) {
    std::optional<NotifyListener> listener = NotifyListener::create();
    checker.expect(listener.has_value(), "hang: listener created");
    if (!listener) {
        return;
    }
    Child child(command, listener->socket_name());
    checker.expect(child.started(), "hang: service started");
    checker.expect(listener->receive(TIMEOUT) == "READY=1",
                   "hang: reports readiness");
    checker.expect(listener->receive(TIMEOUT) == "WATCHDOG=1",
                   "hang: feeds the watchdog before the hang");

    checker.expect(child.signal(SIGUSR1), "hang: SIGUSR1 sent");
    const std::string hang_status =
        "STATUS=" + std::string(scada::hello::HANG_STATUS);
    checker.expect(expect_message(*listener, hang_status),
                   "hang: reports the hang");
    // The status is the last thing the service sends, so from here on any
    // message is a watchdog feed that must not happen.
    checker.expect(!listener->receive(SILENCE),
                   "hang: watchdog is no longer fed");
    checker.expect(child.running(), "hang: process is still there");

    // A hung process does not react to SIGTERM.
    checker.expect(child.signal(SIGTERM), "hang: SIGTERM sent");
    checker.expect(!child.wait(SILENCE), "hang: SIGTERM does not end it");

    // systemd ends a service that missed its watchdog with SIGABRT.
    checker.expect(child.signal(SIGABRT), "hang: SIGABRT sent");
    const std::optional<Exit> exit = child.wait(TIMEOUT);
    checker.expect(exit && exit->signalled && exit->code == SIGABRT,
                   "hang: the watchdog signal ends it");
}

void test_invalid_environment(Checker& checker, std::span<char*> command) {
    Child child(command, "not-a-socket-name");
    checker.expect(child.started(), "invalid: service started");
    const std::optional<Exit> exit = child.wait(TIMEOUT);
    checker.expect(
        exit && !exit->signalled && exit->code == scada::hello::EXIT_ERROR,
        "invalid: exits with code 1");
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::span<char*> raw_args(
        argv, argc > 0 ? static_cast<std::size_t>(argc) : 0);
    if (raw_args.size() < 2) {
        std::cerr
            << "Usage: scada-hello-service-test <command> [<argument>...]\n";
        return 2;
    }
    const std::span<char*> command = raw_args.subspan(1);

    // Child::wait() collects SIGCHLD synchronously.
    sigset_t child_signal{};
    if (::sigemptyset(&child_signal) != 0 ||
        ::sigaddset(&child_signal, SIGCHLD) != 0 ||
        ::pthread_sigmask(SIG_BLOCK, &child_signal, nullptr) != 0) {
        std::cerr << "cannot block SIGCHLD\n";
        return 1;
    }

    Checker checker;
    test_ready_feed_stop(checker, command);
    test_hang(checker, command);
    test_invalid_environment(checker, command);
    return checker.passed() ? 0 : 1;
}
