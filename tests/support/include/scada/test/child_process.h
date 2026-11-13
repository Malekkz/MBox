#ifndef SCADA_TEST_CHILD_PROCESS_H
#define SCADA_TEST_CHILD_PROCESS_H

// Test support: start a real process and wait for what it does. Linux only.
//
// Every wait is on a condition with a timeout. Nothing here sleeps: the waits
// block in poll() until the process writes output or exits.

#include <chrono>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace scada::test {

// Owns a file descriptor and closes it once.
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int descriptor) noexcept : fd_(descriptor) {}
    ~UniqueFd() { reset(); }

    UniqueFd(UniqueFd&& other) noexcept;
    UniqueFd& operator=(UniqueFd&& other) noexcept;
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    // Closes the descriptor, if any.
    void reset() noexcept;

private:
    int fd_ = -1;
};

// A child process whose standard output and standard error are captured.
// Its standard input is empty. If the process is still running when the
// object is destroyed, it is killed and reaped, so a failed test leaves
// nothing behind.
class ChildProcess {
public:
    // Starts `command`: the program followed by its arguments. A program
    // name without a slash is looked up on PATH. The child inherits the
    // environment. Returns nothing if `command` is empty or the process
    // could not be started. A program that cannot be executed is normally
    // reported that way too, but under a user-mode emulator it shows as a
    // process that exits with code 127.
    [[nodiscard]] static std::optional<ChildProcess> start(
        std::span<const std::string> command);

    ~ChildProcess();
    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess& operator=(ChildProcess&&) = delete;

    // Blocks until `condition`, called with all standard output captured so
    // far, returns true. Returns false if `timeout` passes first, or if the
    // process has exited and closed its output without meeting the condition.
    [[nodiscard]] bool wait_for_output(
        const std::function<bool(std::string_view)>& condition,
        std::chrono::milliseconds timeout);

    // Blocks until the process has exited and all its output is captured.
    // Returns its exit code, or nothing if `timeout` passes first or the
    // process was ended by a signal.
    [[nodiscard]] std::optional<int> wait_for_exit(
        std::chrono::milliseconds timeout);

    // Everything captured so far.
    [[nodiscard]] const std::string& out() const noexcept { return out_; }
    [[nodiscard]] const std::string& err() const noexcept { return err_; }

private:
    ChildProcess() = default;

    // Collects output and the exit status until `done` returns true. Returns
    // false on timeout, or when nothing further can happen.
    bool wait_until(const std::function<bool()>& done,
                    std::chrono::milliseconds timeout);
    void check_exit() noexcept;

    int pid_ = -1;
    // Becomes readable when the process exits, which lets poll() wait for
    // output and exit together.
    UniqueFd pidfd_;
    UniqueFd out_fd_;
    UniqueFd err_fd_;
    std::string out_;
    std::string err_;
    // The raw wait status, once the process has been reaped.
    std::optional<int> wait_status_;
};

}  // namespace scada::test

#endif  // SCADA_TEST_CHILD_PROCESS_H
