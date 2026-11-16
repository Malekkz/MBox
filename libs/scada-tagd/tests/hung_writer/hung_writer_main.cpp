// scada-tagd-test-hung-writer: scada-tagd whose retain writer hangs in its
// first fsync of a retain file, as a write to a stalled storage device does
// (decision D-59, point 5). Everything else is the real service: the same
// arguments, the same log and the same stop. Test code; not installed.
//
// The fsync of a retain file waits for a condition nobody signals, at most
// HANG_LIMIT, then fails with EIO. A stop that reaches the 5-s bound of the
// writer before that must end the process at once with EXIT_UNSAVED, which
// `main` does with std::_Exit as the real `main` does.

#include "retain_file.h"

#include "scada/tagd/tagd.h"

#include <sys/types.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scada::tag::RetainFileOps;
using scada::tag::RetainWriteFile;

// Far beyond the 5-s bound of the writer and the 30 s a test waits for the
// exit; only a broken stop ever reaches it.
constexpr std::chrono::seconds HANG_LIMIT{120};

// A retain file whose fsync hangs.
class HangingFile final : public RetainWriteFile {
public:
    explicit HangingFile(std::unique_ptr<RetainWriteFile> file) noexcept
        : file_(std::move(file)) {}

    int set_mode(mode_t mode) noexcept override {
        return file_->set_mode(mode);
    }

    int write(std::span<const std::byte> bytes,
              std::size_t& written) noexcept override {
        return file_->write(bytes, written);
    }

    int sync() noexcept override {
        std::unique_lock lock(mutex_);
        // Nothing signals it: the predicate never holds.
        static_cast<void>(
            never_.wait_for(lock, HANG_LIMIT, [] { return false; }));
        return EIO;
    }

private:
    std::unique_ptr<RetainWriteFile> file_;
    std::mutex mutex_;
    std::condition_variable never_;
};

// The system's calls, with a HangingFile around every file created.
class HangingOps final : public RetainFileOps {
public:
    int create(int dir_fd, const std::string& name,
               std::unique_ptr<RetainWriteFile>& out) override {
        std::unique_ptr<RetainWriteFile> file;
        const int error = system().create(dir_fd, name, file);
        if (error == 0) {
            out = std::make_unique<HangingFile>(std::move(file));
        }
        return error;
    }

    int rename(int dir_fd, const std::string& from,
               const std::string& target) noexcept override {
        return system().rename(dir_fd, from, target);
    }

    int sync_directory(int dir_fd) noexcept override {
        return system().sync_directory(dir_fd);
    }

    int unlink(int dir_fd, const std::string& name) noexcept override {
        return system().unlink(dir_fd, name);
    }

private:
    static RetainFileOps& system() noexcept {
        return scada::tag::system_retain_file_ops();
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    const std::span<char*> raw_args(
        argv, argc > 0 ? static_cast<std::size_t>(argc) : 0);
    std::vector<std::string_view> args;
    if (raw_args.size() > 1) {
        const std::span<char*> options = raw_args.subspan(1);
        args.assign(options.begin(), options.end());
    }
    HangingOps ops;
    scada::tagd::RunHooks hooks;
    hooks.retain_file_ops = &ops;
    // As in the real main (coding standard, section 7).
    hooks.exit_now = [](int status) { std::_Exit(status); };
    return scada::tagd::run(args, std::cout, std::cerr, hooks);
}
