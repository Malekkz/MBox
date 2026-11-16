// scada-test-cleaner <directory>: removes <directory> once the test process
// that started it has ended without removing it itself. Started by
// TempDirectory (scada/test/temp_directory.h); not meant to be run by hand.
//
// Standard input is a pipe from the test process. A byte in it means the
// test process has removed the directory: the cleaner ends. End of file
// without a byte means the test process is gone: the cleaner removes the
// directory. Writes one byte to standard output once it runs.

#include <poll.h>
#include <signal.h>  // NOLINT(modernize-deprecated-headers): sigset_t and pthread_sigmask are declared here, not in <csignal>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <span>
#include <system_error>

namespace {

constexpr int EXIT_OK = 0;
constexpr int EXIT_NOT_REMOVED = 1;
constexpr int EXIT_USAGE = 2;

// The children of the killed test process got SIGKILL as it ended, but a
// child that has not ended yet can still create files and make a removal
// fail. The removal is tried again until the directory is gone, at most
// this often and with this pause, which bounds the cleaner to about 5 s.
constexpr int REMOVE_ATTEMPTS = 500;
constexpr int REMOVE_PAUSE_MS = 10;

int clean(const std::filesystem::path& directory) {
    char byte = 0;
    ssize_t count = ::read(STDIN_FILENO, &byte, 1);
    while (count < 0 && errno == EINTR) {
        count = ::read(STDIN_FILENO, &byte, 1);
    }
    if (count == 1) {
        return EXIT_OK;
    }
    for (int attempt = 0; attempt < REMOVE_ATTEMPTS; ++attempt) {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        if (!std::filesystem::exists(directory, error) && !error) {
            return EXIT_OK;
        }
        // Not a wait for a condition: nothing announces that the last child
        // has ended.
        static_cast<void>(::poll(nullptr, 0, REMOVE_PAUSE_MS));
    }
    return EXIT_NOT_REMOVED;
}

}  // namespace

int main(int argc, char** argv) {
    // The test process may be gone before the first byte is written; that
    // must not end the cleaner. Blocked, SIGPIPE leaves the write failing
    // with EPIPE.
    sigset_t blocked{};
    if (::sigemptyset(&blocked) != 0 || ::sigaddset(&blocked, SIGPIPE) != 0 ||
        ::pthread_sigmask(SIG_BLOCK, &blocked, nullptr) != 0) {
        return EXIT_USAGE;
    }
    const std::span<char*> arguments(argv, static_cast<std::size_t>(argc));
    if (arguments.size() != 2) {
        return EXIT_USAGE;
    }
    const std::filesystem::path directory(arguments[1]);
    if (!directory.is_absolute() || directory == directory.root_path()) {
        return EXIT_USAGE;
    }
    const char runs = 'r';
    if (::write(STDOUT_FILENO, &runs, 1) != 1) {
        return EXIT_USAGE;
    }
    return clean(directory);
}
