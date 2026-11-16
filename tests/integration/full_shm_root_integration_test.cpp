// Conformance test 11, second part, on a file system that is really full
// (contract, sections 3.1 and 15): attach fails with an error, not a
// signal, when the file system of the tag database has no room left.
//
// The test needs to mount a file system, so it must run as root: the CTest
// label `root` keeps it out of the presets that run unprivileged, and the
// pipeline runs it with sudo (`ctest --preset root`). It never touches
// /dev/shm. The test process first moves into a mount namespace of its
// own whose mounts do not propagate back, and mounts a small tmpfs over
// its temporary directory there; scada-tagd and the helpers it starts
// inherit that namespace. TearDown unmounts it, and if the test binary
// dies instead, the namespace, and the mount with it, ends with the last
// of its processes, which die with the test binary (decision D-48).

#include "tag_processes_fixture.h"
#include "tagd_fixture.h"

#include "scada/tag/scada_tag.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace {

using namespace scada::test::tagd;
using scada::test::ChildEnding;
using scada::test::ChildProcess;

// Room for the header and the regions of scada-tagd of the small tag set,
// with some to spare.
constexpr const char* TMPFS_OPTIONS = "size=1m,mode=0700";
constexpr std::size_t FILL_CHUNK = 4096;
// The exit code of scada-tag-test-peer when attach fails with another code
// than SCADA_E_BUSY.
constexpr int PEER_EXIT_FAILED = 1;

// The text of the error number errno holds now.
std::string last_error() {
    return std::error_code(errno, std::generic_category()).message();
}

// The owner helper ended by itself with the error of attach, not by a
// signal.
::testing::AssertionResult failed_to_attach(ChildProcess& owner) {
    const std::optional<ChildEnding> ending = owner.wait_for_ending(WAIT_LIMIT);
    const std::string expected =
        "attach failed with code " + std::to_string(SCADA_E_IO);
    if (!ending.has_value()) {
        return ::testing::AssertionFailure() << "the owner did not end";
    }
    if (ending->by_signal || ending->number != PEER_EXIT_FAILED ||
        owner.err().find(expected) == std::string::npos) {
        return ::testing::AssertionFailure()
               << describe(*ending) << ": " << owner.err();
    }
    return ::testing::AssertionSuccess();
}

// An attach as `reader` to the database in `base_dir`, in this process,
// fails with SCADA_E_IO and errno ENOSPC.
::testing::AssertionResult attach_fails_for_lack_of_room(
    const std::string& base_dir) {
    const scada_attach_opts options{.base_dir = base_dir.c_str(),
                                    .retain_dir = nullptr,
                                    .tagd_uid = std::int64_t{::geteuid()}};
    scada_ctx* ctx = nullptr;
    errno = 0;
    const int code = scada_tag_attach_opts("reader", &options, &ctx);
    const int error = errno;
    if (code != SCADA_E_IO || error != ENOSPC || ctx != nullptr) {
        scada_tag_detach(ctx);
        return ::testing::AssertionFailure()
               << "code " << code << ", errno " << error;
    }
    return ::testing::AssertionSuccess();
}

class FullShm : public TagProcesses {
protected:
    void SetUp() override {
        ASSERT_EQ(::geteuid(), 0U)
            << "this test mounts a file system; run it as root (sudo ctest "
               "--preset root)";
        TagProcesses::SetUp();
        if (HasFatalFailure()) {
            return;
        }
        // A mount namespace for this thread, and the processes it starts,
        // whose mounts are not seen outside it.
        ASSERT_EQ(::unshare(CLONE_NEWNS), 0) << last_error();
        ASSERT_EQ(::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr),
                  0)
            << last_error();
        ASSERT_EQ(::mount("tmpfs", root().c_str(), "tmpfs",
                          MS_NOSUID | MS_NODEV | MS_NOEXEC, TMPFS_OPTIONS),
                  0)
            << last_error();
        mounted_ = true;
        // The tmpfs hides what the fixture made under root(): make the base
        // directory again on it.
        std::error_code error;
        std::filesystem::create_directory(base(), error);
        ASSERT_FALSE(error) << error.message();
        ASSERT_EQ(::chmod(base().c_str(), BASE_DIR_MODE), 0);
    }

    void TearDown() override {
        if (mounted_) {
            EXPECT_EQ(::umount2(root().c_str(), MNT_DETACH), 0) << last_error();
        }
        TagProcesses::TearDown();
    }

    // Fills the file system of the database with the file `filler` beside
    // the base directory, until a write finds no room. True if it did.
    [[nodiscard]] bool fill() const {
        const std::string path = filler().string();
        constexpr int FLAGS = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open has no other form
        const int file = ::open(path.c_str(), FLAGS, S_IRUSR | S_IWUSR);
        if (file < 0) {
            return false;
        }
        const std::array<char, FILL_CHUNK> chunk{};
        bool full = false;
        // Bounded: the file system is TMPFS_OPTIONS large.
        while (!full) {
            const ssize_t written = ::write(file, chunk.data(), chunk.size());
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written < 0) {
                full = errno == ENOSPC;
                break;
            }
        }
        ::close(file);
        return full;
    }

    [[nodiscard]] std::filesystem::path filler() const {
        return root() / "filler";
    }

private:
    bool mounted_ = false;
};

TEST_F(FullShm, Conformance11AttachToAFullFileSystemFailsWithAnError) {
    write_small_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    ASSERT_TRUE(fill());

    // A process with tags: its regions find no room. It exits with the
    // error, it is not killed by a signal.
    std::optional<ChildProcess> owner = start_helper(
        "SCADA_TAG_PEER_COMMAND", {base(), "io", std::to_string(IO_PERIOD_MS)});
    if (!owner.has_value()) {
        return;
    }
    EXPECT_TRUE(failed_to_attach(*owner));

    // A process without tags, in this process: its `proc` region finds no
    // room either.
    EXPECT_TRUE(attach_fails_for_lack_of_room(base()));

    // With room again, the same process attaches: nothing it left behind
    // is in the way.
    ASSERT_TRUE(std::filesystem::remove(filler()));
    std::optional<ChildProcess> again = attached_io();
    if (!again.has_value()) {
        return;
    }
    EXPECT_TRUE(stops_cleanly(*again));
    stop(*tagd);
}

}  // namespace
