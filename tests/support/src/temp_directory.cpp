#include "scada/test/temp_directory.h"

#include "exec.h"
#include "scada/test/child_process.h"

#include <fcntl.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace scada::test {

namespace {

// The command that starts the cleaner, elements separated by `|`: the
// emulator of a cross build, if any, then the program. Set by CMake.
constexpr std::string_view CLEANER_COMMAND = SCADA_TEST_CLEANER_COMMAND;

// How long the cleaner may take to start and to end. Only matters when
// something is broken.
constexpr std::chrono::milliseconds CLEANER_WAIT_LIMIT{30'000};

// The byte the cleaner writes once it runs, and the one that lets it go.
constexpr char CLEANER_RUNS = 'r';
constexpr char CLEANER_RELEASE = 'x';

constexpr int EXIT_NOT_STARTED = 127;

using PidBytes = std::array<char, sizeof(pid_t)>;

struct Pipe {
    UniqueFd read_end;
    UniqueFd write_end;
};

std::optional<Pipe> make_pipe() {
    std::array<int, 2> fds{-1, -1};
    if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
        return std::nullopt;
    }
    return Pipe{UniqueFd(fds[0]), UniqueFd(fds[1])};
}

// A connected pair of sockets in place of a pipe for the cleaner's
// standard input: send() with MSG_NOSIGNAL to a cleaner that is gone
// returns an error instead of raising SIGPIPE in the test process.
std::optional<Pipe> make_socket_pair() {
    std::array<int, 2> fds{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds.data()) != 0) {
        return std::nullopt;
    }
    return Pipe{UniqueFd(fds[0]), UniqueFd(fds[1])};
}

std::vector<std::string> cleaner_command(const std::filesystem::path& path) {
    std::vector<std::string> command;
    std::string_view rest = CLEANER_COMMAND;
    while (!rest.empty()) {
        const std::size_t end = rest.find('|');
        command.emplace_back(rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view{}
                                             : rest.substr(end + 1);
    }
    command.push_back(path.string());
    return command;
}

// In the grandchild, between fork and exec: only async-signal-safe calls.
// A session of its own takes it out of its parent's process group, so that
// a signal to that group does not reach it.
[[noreturn]] void become_cleaner(const char* program, char* const* argv,
                                 int input, int output) {
    if (::setsid() >= 0 && ::dup2(input, STDIN_FILENO) >= 0 &&
        ::dup2(output, STDOUT_FILENO) >= 0) {
        ::execve(program, argv, environ);
    }
    ::_exit(EXIT_NOT_STARTED);
}

// In the child, between fork and exec. Starts the cleaner as a grandchild
// and ends at once, so that the cleaner's parent becomes the reaper of the
// system: it is no longer a descendant of the test process, which CTest
// kills, descendants included, on a timeout. Reports the cleaner's id in
// `report`.
[[noreturn]] void start_cleaner(const char* program, char* const* argv,
                                int input, int output, int report) {
    // NOLINTNEXTLINE(misc-include-cleaner): from <sys/types.h>, which the check does not map
    const pid_t cleaner = ::fork();
    if (cleaner == 0) {
        become_cleaner(program, argv, input, output);
    }
    if (cleaner < 0) {
        ::_exit(1);
    }
    const auto bytes = std::bit_cast<PidBytes>(cleaner);
    const bool reported = ::write(report, bytes.data(), bytes.size()) ==
                          static_cast<ssize_t>(bytes.size());
    ::_exit(reported ? 0 : 1);
}

// Reads exactly `bytes.size()` bytes. False on end of file or an error.
bool read_exactly(const UniqueFd& descriptor, std::span<char> bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t count = ::read(
            descriptor.get(), bytes.subspan(done).data(), bytes.size() - done);
        if (count > 0) {
            done += static_cast<std::size_t>(count);
        } else if (count == 0 || errno != EINTR) {
            return false;
        }
    }
    return true;
}

// Waits until `descriptor` is readable, at most CLEANER_WAIT_LIMIT, and
// reads one byte. The byte, or nothing on end of file, an error or the
// limit.
std::optional<char> read_byte_within_limit(const UniqueFd& descriptor) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point deadline = Clock::now() + CLEANER_WAIT_LIMIT;
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now());
        if (left.count() < 0) {
            return std::nullopt;
        }
        pollfd wait{.fd = descriptor.get(), .events = POLLIN, .revents = 0};
        const int ready = ::poll(&wait, 1, static_cast<int>(left.count()) + 1);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            return std::nullopt;
        }
        char byte = 0;
        const ssize_t count = ::read(descriptor.get(), &byte, 1);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count != 1) {
            return std::nullopt;
        }
        return byte;
    }
}

}  // namespace

std::optional<TempDirectory> TempDirectory::create(std::string_view prefix) {
    std::error_code error;
    const std::filesystem::path temp =
        std::filesystem::temp_directory_path(error);
    if (error) {
        return std::nullopt;
    }
    std::string pattern = (temp / prefix).string() + "XXXXXX";
    if (::mkdtemp(pattern.data()) == nullptr) {
        return std::nullopt;
    }
    // From here on the object owns the directory, and destroying it on a
    // failure removes the directory.
    TempDirectory directory;
    directory.path_ = std::filesystem::absolute(pattern, error);
    if (error) {
        directory.path_ = pattern;
        return std::nullopt;
    }

    const std::vector<std::string> command = cleaner_command(directory.path_);
    const std::optional<std::string> program =
        detail::find_program(command.front());
    std::optional<Pipe> input = make_socket_pair();
    std::optional<Pipe> output = make_pipe();
    std::optional<Pipe> report = make_pipe();
    if (!program || !input || !output || !report) {
        return std::nullopt;
    }
    const detail::ArgumentVector arguments(command);

    // NOLINTNEXTLINE(misc-include-cleaner): from <sys/types.h>, which the check does not map
    const pid_t child = ::fork();
    if (child < 0) {
        return std::nullopt;
    }
    if (child == 0) {
        start_cleaner(program->c_str(), arguments.get(), input->read_end.get(),
                      output->write_end.get(), report->write_end.get());
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    // Only the cleaner holds these ends now, so that its end shows as end
    // of file.
    input->read_end.reset();
    output->write_end.reset();
    report->write_end.reset();
    directory.release_ = std::move(input->write_end);
    directory.cleaner_out_ = std::move(output->read_end);

    PidBytes bytes{};
    // NOLINTNEXTLINE(misc-include-cleaner): from <sys/wait.h>, which the check does not map
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        !read_exactly(report->read_end, bytes)) {
        return std::nullopt;
    }
    directory.cleaner_pid_ = std::bit_cast<pid_t>(bytes);
    if (read_byte_within_limit(directory.cleaner_out_) != CLEANER_RUNS) {
        return std::nullopt;
    }
    return directory;
}

TempDirectory::TempDirectory(TempDirectory&& other) noexcept
    : path_(std::exchange(other.path_, {})),
      cleaner_pid_(std::exchange(other.cleaner_pid_, -1)),
      release_(std::move(other.release_)),
      cleaner_out_(std::move(other.cleaner_out_)) {}

TempDirectory::~TempDirectory() {
    if (!path_.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    if (release_.valid()) {
        // Only after the removal: a test process killed in between still
        // has its directory removed by the cleaner.
        static_cast<void>(
            ::send(release_.get(), &CLEANER_RELEASE, 1, MSG_NOSIGNAL));
        release_.reset();
    }
    if (cleaner_out_.valid()) {
        // End of file: the cleaner has ended, so a test that ends after
        // this leaves no process behind.
        while (read_byte_within_limit(cleaner_out_).has_value()) {
        }
    }
}

}  // namespace scada::test
