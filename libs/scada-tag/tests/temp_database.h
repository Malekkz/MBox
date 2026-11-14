#ifndef SCADA_TAG_TESTS_TEMP_DATABASE_H
#define SCADA_TAG_TESTS_TEMP_DATABASE_H

// A base directory of a tag database for one test, in the temporary
// directory of the machine, removed afterwards. It has the owner and mode
// the contract requires (section 3.1): the user the test runs as, mode 2750.

#include <gtest/gtest.h>

#include "scada/common/fd.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace scada::tag::testing {

using scada::Fd;

// The permission bits, the set-id bits and the sticky bit of a mode.
inline constexpr mode_t MODE_BITS = 07777;

// A umask that takes every permission from the group and from others.
inline constexpr mode_t UMASK_077 = 077;

// Mode 2750 with write permission for the group added.
inline constexpr mode_t GROUP_WRITABLE_BASE_MODE = 02770;

// The mode a test gives its base directory before it sets the real one.
inline constexpr mode_t PRIVATE_DIR_MODE = 0700;

// Mode of `path` itself (a symbolic link is not followed), or 0 with a
// failed expectation.
inline mode_t mode_of(const std::filesystem::path& path) {
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) {
        ADD_FAILURE() << "cannot examine " << path;
        return 0;
    }
    return info.st_mode & MODE_BITS;
}

// Inode number of `path` itself, or 0 with a failed expectation.
inline ino_t inode_of(const std::filesystem::path& path) {
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) {
        ADD_FAILURE() << "cannot examine " << path;
        return 0;
    }
    return info.st_ino;
}

// Opens the directory `path`, to be used with the *at functions. Invalid,
// with a failed expectation, if that fails.
inline Fd open_directory(const std::filesystem::path& path) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open has no other form
    Fd directory(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    EXPECT_TRUE(directory.valid()) << path;
    return directory;
}

// Opens the existing file `path` for reading and writing. Invalid, with a
// failed expectation, if that fails.
inline Fd open_file(const std::filesystem::path& path) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open has no other form
    Fd file(::open(path.c_str(), O_RDWR | O_CLOEXEC));
    EXPECT_TRUE(file.valid()) << path;
    return file;
}

// Creates the empty file `path`, owned by the user of the test.
inline void touch(const std::filesystem::path& path) {
    const std::ofstream file(path);
    EXPECT_TRUE(file.is_open()) << path;
}

// The names in the directory `path`, sorted.
inline std::vector<std::string> names_in(const std::filesystem::path& path) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

class TempDatabase : public ::testing::Test {
protected:
    // Mode of the base directory (contract, section 3.1).
    static constexpr mode_t BASE_MODE = 02750;

    void SetUp() override {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "scada-tag-test-XXXXXX")
                .string();
        ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
        root_ = pattern;
        base_ = root_ / "base";
        ASSERT_EQ(::mkdir(base_.c_str(), PRIVATE_DIR_MODE), 0);
        // Owner and mode as the contract requires (section 3.1).
        ASSERT_EQ(::chmod(base_.c_str(), BASE_MODE), 0);
    }

    void TearDown() override {
        std::error_code ignored;
        // A test may have taken the permissions away.
        std::filesystem::permissions(base_, std::filesystem::perms::owner_all,
                                     ignored);
        std::filesystem::remove_all(root_, ignored);
    }

    // The base directory.
    [[nodiscard]] const std::filesystem::path& base() const { return base_; }

    // The directory that holds the base directory; free for other files.
    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

    // The user the test runs as.
    [[nodiscard]] static std::uint32_t uid() { return ::geteuid(); }

    // Another user. Nothing is done as that user; it only is not `uid()`.
    [[nodiscard]] static std::uint32_t other_uid() { return ::geteuid() + 1; }

private:
    std::filesystem::path root_;
    std::filesystem::path base_;
};

}  // namespace scada::tag::testing

#endif  // SCADA_TAG_TESTS_TEMP_DATABASE_H
