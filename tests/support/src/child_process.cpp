#include "scada/test/child_process.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <utility>
#include <vector>

namespace scada::test {

namespace {

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

bool set_non_blocking(const UniqueFd& fd) {
    const int flags = ::fcntl(fd.get(), F_GETFL);
    return flags >= 0 && ::fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK) == 0;
}

// Destroys the spawn file actions on every path out of start().
class FileActions {
public:
    FileActions() { ok_ = ::posix_spawn_file_actions_init(&actions_) == 0; }
    ~FileActions() {
        if (ok_) {
            ::posix_spawn_file_actions_destroy(&actions_);
        }
    }
    FileActions(const FileActions&) = delete;
    FileActions& operator=(const FileActions&) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] posix_spawn_file_actions_t* get() noexcept {
        return &actions_;
    }

private:
    posix_spawn_file_actions_t actions_{};
    bool ok_ = false;
};

// Moves everything that can be read without blocking from `fd` to `sink`,
// and closes `fd` at end of file.
void drain(UniqueFd& fd, std::string& sink) {
    std::array<char, 4096> buffer{};
    while (fd.valid()) {
        const ssize_t count = ::read(fd.get(), buffer.data(), buffer.size());
        if (count > 0) {
            sink.append(buffer.data(), static_cast<std::size_t>(count));
        } else if (count == 0) {
            fd.reset();
        } else if (errno != EINTR) {
            if (errno != EAGAIN) {
                fd.reset();
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
    child.pidfd_ = UniqueFd(
        static_cast<int>(::syscall(SYS_pidfd_open, pid, 0U)));
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
        const auto remaining =
            std::chrono::ceil<std::chrono::milliseconds>(deadline - now);

        // poll() ignores entries with a negative descriptor, so closed pipes
        // and an already reaped process need no special case.
        std::array<pollfd, 3> fds{{
            {out_fd_.get(), POLLIN, 0},
            {err_fd_.get(), POLLIN, 0},
            {wait_status_ ? -1 : pidfd_.get(), POLLIN, 0},
        }};
        const auto wait_ms = static_cast<int>(
            std::min<std::chrono::milliseconds::rep>(remaining.count(), 60'000));
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

std::optional<int> ChildProcess::wait_for_exit(
    std::chrono::milliseconds timeout) {
    const bool exited = wait_until(
        [this] {
            return wait_status_ && !out_fd_.valid() && !err_fd_.valid();
        },
        timeout);
    if (!exited || !WIFEXITED(*wait_status_)) {
        return std::nullopt;
    }
    return WEXITSTATUS(*wait_status_);
}

}  // namespace scada::test
