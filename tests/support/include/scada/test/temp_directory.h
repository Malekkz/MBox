#ifndef SCADA_TEST_TEMP_DIRECTORY_H
#define SCADA_TEST_TEMP_DIRECTORY_H

// Test support: a temporary directory that is removed even when the test
// process is killed. Linux only.

#include "scada/test/child_process.h"

#include <filesystem>
#include <optional>
#include <string_view>

namespace scada::test {

// A new directory in the temporary directory of the system, removed with
// everything in it when the object is destroyed. A small cleaner process
// (`scada-test-cleaner`) waits beside the test: if the test process ends
// without destroying the object (SIGKILL, a CTest timeout, a crash), the
// cleaner removes the directory (decision D-48). The cleaner has left its
// parent's process tree and session, so a CTest timeout or a signal to the
// process group does not reach it.
class TempDirectory {
public:
    // Creates the directory `<prefix>XXXXXX`, X replaced by mkdtemp(), and
    // starts its cleaner, waiting until it runs. Nothing if either fails;
    // nothing is left behind then.
    [[nodiscard]] static std::optional<TempDirectory> create(
        std::string_view prefix);

    // Removes the directory, then lets the cleaner go and waits until it
    // has ended.
    ~TempDirectory();
    TempDirectory(TempDirectory&& other) noexcept;
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory& operator=(TempDirectory&&) = delete;

    // The absolute path of the directory.
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    // The process id of the cleaner, for the test of the cleaner itself.
    [[nodiscard]] int cleaner_pid() const noexcept { return cleaner_pid_; }

private:
    TempDirectory() = default;

    std::filesystem::path path_;
    int cleaner_pid_ = -1;
    // Our end of the cleaner's standard input, a socket. A byte in it lets the
    // cleaner go; end of file without a byte means the test process is
    // gone.
    UniqueFd release_;
    // The read end of the cleaner's standard output: one byte once it runs,
    // end of file once it has ended.
    UniqueFd cleaner_out_;
};

}  // namespace scada::test

#endif  // SCADA_TEST_TEMP_DIRECTORY_H
