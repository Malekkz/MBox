#include "scada/test/child_process.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/poll.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::test {

namespace {

constexpr std::size_t READ_CHUNK_SIZE = 4096;

struct Pipe {
    UniqueFd read_end;
    UniqueFd write_end;
};

// Both ends are close-on-exec; the child gets its copy through dup2.
std::optional<Pipe> make_pipe() {
    std::array<int, 2> fds{-1, -1};
    if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
        return std::nullopt;
    }
    return Pipe{UniqueFd(fds[0]), UniqueFd(fds[1])};
}

// Only the read end: the flag is shared with every copy of a descriptor, and
// the child must keep blocking writes on its end.
bool set_non_blocking(const UniqueFd& descriptor) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl has no other form
    const int flags = ::fcntl(descriptor.get(), F_GETFL);
    return flags >= 0 &&
           // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl has no other form
           ::fcntl(descriptor.get(), F_SETFL, flags | O_NONBLOCK) == 0;
}

// Destroys the spawn file actions on every path out of start().
class FileActions {
public:
    FileActions() : ok_(::posix_spawn_file_actions_init(&actions_) == 0) {}
    ~FileActions() {
        if (ok_) {
            ::posix_spawn_file_actions_destroy(&actions_);
        }
    }
    FileActions(const FileActions&) = delete;
    FileActions& operator=(const FileActions&) = delete;
    FileActions(FileActions&&) = delete;
    FileActions& operator=(FileActions&&) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] posix_spawn_file_actions_t* get() noexcept {
        return &actions_;
    }

private:
    posix_spawn_file_actions_t actions_{};
    bool ok_;
};

// Moves everything that can be read without blocking from `fd` to `sink`,
// and closes `fd` at end of file.
void drain(UniqueFd& descriptor, std::string& sink) {
    std::array<char, READ_CHUNK_SIZE> buffer{};
    while (descriptor.valid()) {
        const ssize_t count =
            ::read(descriptor.get(), buffer.data(), buffer.size());
        if (count > 0) {
            sink.append(buffer.data(), static_cast<std::size_t>(count));
        } else if (count == 0) {
            descriptor.reset();
        } else if (errno != EINTR) {
            if (errno != EAGAIN) {
                descriptor.reset();
            }
            return;
        }
    }
}

}  // namespace

UniqueFd::UniqueFd(UniqueFd&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
        reset();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

void UniqueFd::reset() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::optional<ChildProcess> ChildProcess::start(
    std::span<const std::string> command) {
    if (command.empty()) {
        return std::nullopt;
    }

    std::optional<Pipe> out_pipe = make_pipe();
    std::optional<Pipe> err_pipe = make_pipe();
    if (!out_pipe || !err_pipe || !set_non_blocking(out_pipe->read_end) ||
        !set_non_blocking(err_pipe->read_end)) {
        return std::nullopt;
    }

    FileActions actions;
    if (!actions.ok() ||
        ::posix_spawn_file_actions_addopen(actions.get(), STDIN_FILENO,
                                           "/dev/null", O_RDONLY, 0) != 0 ||
        ::posix_spawn_file_actions_adddup2(
            actions.get(), out_pipe->write_end.get(), STDOUT_FILENO) != 0 ||
        ::posix_spawn_file_actions_adddup2(
            actions.get(), err_pipe->write_end.get(), STDERR_FILENO) != 0) {
        return std::nullopt;
    }

    // posix_spawn wants writable, null-terminated strings.
    std::vector<std::string> arguments(command.begin(), command.end());
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    // NOLINTNEXTLINE(misc-include-cleaner): from <sys/types.h>, which the check does not map
    pid_t pid = -1;
    if (::posix_spawnp(&pid, argv.front(), actions.get(), nullptr, argv.data(),
                       environ) != 0) {
        return std::nullopt;
    }

    ChildProcess child;
    child.pid_ = pid;
    child.out_fd_ = std::move(out_pipe->read_end);
    child.err_fd_ = std::move(err_pipe->read_end);
    // Called through syscall() because the C library wrapper is newer than
    // some of the toolchains this builds with. On failure `child` is
    // destroyed here, which kills and reaps the process.
    const auto pidfd =
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): syscall has no other form
        ::syscall(SYS_pidfd_open, pid, 0U);
    child.pidfd_ = UniqueFd(static_cast<int>(pidfd));
    if (!child.pidfd_.valid()) {
        return std::nullopt;
    }
    // The write ends close as the pipes go out of scope; only the child's
    // copies stay open, so its exit shows as end of file.
    return child;
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : pid_(std::exchange(other.pid_, -1)),
      pidfd_(std::move(other.pidfd_)),
      out_fd_(std::move(other.out_fd_)),
      err_fd_(std::move(other.err_fd_)),
      out_(std::move(other.out_)),
      err_(std::move(other.err_)),
      wait_status_(other.wait_status_) {}

ChildProcess::~ChildProcess() {
    if (pid_ > 0 && !wait_status_) {
        // NOLINTNEXTLINE(misc-include-cleaner): from <csignal>, which the check does not map
        ::kill(pid_, SIGKILL);
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
    }
}

void ChildProcess::check_exit() noexcept {
    if (wait_status_) {
        return;
    }
    int status = 0;
    // NOLINTNEXTLINE(misc-include-cleaner): from <sys/wait.h>, which the check does not map
    if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        wait_status_ = status;
    }
}

bool ChildProcess::wait_until(const std::function<bool()>& done,
                              std::chrono::milliseconds timeout) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point deadline = Clock::now() + timeout;

    while (true) {
        // Exit first, output second: whatever the process wrote before it
        // exited is then already in the pipes.
        check_exit();
        drain(out_fd_, out_);
        drain(err_fd_, err_);
        if (done()) {
            return true;
        }
        if (wait_status_ && !out_fd_.valid() && !err_fd_.valid()) {
            return false;
        }

        const Clock::time_point now = Clock::now();
        if (now >= deadline) {
            return false;
        }
        // Rounded up, so that the last wait does not end before the deadline.
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                  now) +
            std::chrono::milliseconds(1);

        // poll() ignores entries with a negative descriptor, so closed pipes
        // and an already reaped process need no special case.
        std::array<pollfd, 3> fds{{
            {out_fd_.get(), POLLIN, 0},
            {err_fd_.get(), POLLIN, 0},
            {wait_status_ ? -1 : pidfd_.get(), POLLIN, 0},
        }};
        const auto wait_ms =
            static_cast<int>(std::min<std::chrono::milliseconds::rep>(
                remaining.count(), 60'000));
        // An error other than an interruption would repeat at once; stop
        // instead of spinning.
        if (::poll(fds.data(), fds.size(), wait_ms) < 0 && errno != EINTR) {
            return false;
        }
    }
}

bool ChildProcess::wait_for_output(
    const std::function<bool(std::string_view)>& condition,
    std::chrono::milliseconds timeout) {
    return wait_until([&] { return condition(out_); }, timeout);
}

bool ChildProcess::wait_for_error(
    const std::function<bool(std::string_view)>& condition,
    std::chrono::milliseconds timeout) {
    return wait_until([&] { return condition(err_); }, timeout);
}

std::optional<int> ChildProcess::wait_for_end(
    std::chrono::milliseconds timeout) {
    const bool ended = wait_until(
        [this] { return wait_status_ && !out_fd_.valid() && !err_fd_.valid(); },
        timeout);
    if (!ended) {
        return std::nullopt;
    }
    return wait_status_;
}

std::optional<int> ChildProcess::wait_for_exit(
    std::chrono::milliseconds timeout) {
    const std::optional<int> ended = wait_for_end(timeout);
    if (!ended.has_value()) {
        return std::nullopt;
    }
    const int status = *ended;
    // NOLINTNEXTLINE(misc-include-cleaner): from <sys/wait.h>, which the check does not map
    if (!WIFEXITED(status)) {
        return std::nullopt;
    }
    // NOLINTNEXTLINE(misc-include-cleaner): from <sys/wait.h>, which the check does not map
    return WEXITSTATUS(status);
}

std::optional<int> ChildProcess::wait_for_signal_exit(
    std::chrono::milliseconds timeout) {
    const std::optional<int> ended = wait_for_end(timeout);
    if (!ended.has_value()) {
        return std::nullopt;
    }
    const int status = *ended;
    // The process is never traced or stopped here, so a status that is not
    // a normal exit is the end by a signal.
    // NOLINTNEXTLINE(misc-include-cleaner): from <sys/wait.h>, which the check does not map
    if (WIFEXITED(status)) {
        return std::nullopt;
    }
    // NOLINTNEXTLINE(misc-include-cleaner): from <sys/wait.h>, which the check does not map
    return WTERMSIG(status);
}

bool ChildProcess::send_signal(int signal_number) noexcept {
    check_exit();
    // Not after the process is reaped: its id may belong to another by then.
    // NOLINTNEXTLINE(misc-include-cleaner): from <csignal>, which the check does not map
    return pid_ > 0 && !wait_status_ && ::kill(pid_, signal_number) == 0;
}

}  // namespace scada::test
