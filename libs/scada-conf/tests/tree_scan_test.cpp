#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/tree.h"
#include "scada/test/temp_directory.h"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace scada::conf {
namespace {

namespace fs = std::filesystem;

using Lines = std::vector<std::string>;

// Sizes of the small limits some tests set.
constexpr std::size_t SMALL_TREE_BYTES = 100;
constexpr std::size_t SYSTEM_BYTES = 60;
constexpr std::size_t FIRST_TAGS_BYTES = 40;
constexpr std::size_t SMALL_FILE_LIMIT = 10;
constexpr int HUGE_DIRECTORY = 100;

class TreeScan : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(temp_);
        if (temp_) {
            base_ = temp_->path();
        }
        root_ = base_ / "tree";
        fs::create_directory(root_);
    }

    // The temporary directory that holds the tree, for things outside it.
    [[nodiscard]] const fs::path& base() const { return base_; }
    [[nodiscard]] const fs::path& root() const { return root_; }
    void set_root(fs::path root) { root_ = std::move(root); }
    [[nodiscard]] const Tree& tree() const { return tree_; }
    [[nodiscard]] std::size_t error_count() const { return error_count_; }

    void write(const std::string& path, const std::string& content) {
        const fs::path full = root_ / path;
        fs::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::binary);
        out << content;
        ASSERT_TRUE(out.good());
    }

    void write_size(const std::string& path, std::size_t size) {
        write(path, std::string(size, '#'));
    }

    // Scans the tree and returns the formatted errors, in report order.
    Lines scan(const ScanLimits& limits = {}) {
        Diagnostics diagnostics;
        Result<Tree> scanned = scan_tree(root_, diagnostics, limits);
        EXPECT_TRUE(scanned.ok());
        if (scanned.ok()) {
            tree_ = std::move(scanned).value();
        }
        Report report;
        diagnostics.move_into(report);
        error_count_ = report.error_count;
        Lines lines;
        lines.reserve(report.errors.size());
        for (const Diagnostic& diagnostic : report.errors) {
            lines.push_back(format(diagnostic));
        }
        return lines;
    }

    [[nodiscard]] Lines paths() const {
        Lines out;
        out.reserve(tree_.files.size());
        for (const TreeFile& file : tree_.files) {
            out.push_back(file.path);
        }
        return out;
    }

private:
    std::optional<test::TempDirectory> temp_ =
        test::TempDirectory::create("scada-conf-scan-");
    fs::path base_;
    fs::path root_;
    Tree tree_;
    std::size_t error_count_ = 0;
};

// Creates an empty file.
void touch(const fs::path& path) {
    const std::ofstream out(path);
    ASSERT_TRUE(out.good());
}

TEST_F(TreeScan, ReadsEveryFileOfTheTreeInPathOrder) {
    write("system.yaml", "a: 1\n");
    write("tags/b.yaml", "b");
    write("tags/a.yaml", "a");
    write("drivers/modbus/line1.yaml", "c");
    write("control/ps1_seq.st", "d");
    EXPECT_EQ(scan(), Lines{});
    EXPECT_EQ(paths(), (Lines{"control/ps1_seq.st", "drivers/modbus/line1.yaml",
                              "system.yaml", "tags/a.yaml", "tags/b.yaml"}));
    ASSERT_EQ(tree().files.size(), 5U);
    EXPECT_EQ(tree().files.at(2).content, "a: 1\n");
    EXPECT_EQ(tree().files.at(1).path_class.protocol, "modbus");
}

TEST_F(TreeScan, AnEmptyFileIsRead) {
    write("system.yaml", "");
    EXPECT_EQ(scan(), Lines{});
    EXPECT_EQ(paths(), Lines{"system.yaml"});
}

TEST_F(TreeScan, AMissingSystemFileIsCF103) {
    write("tags/a.yaml", "a");
    EXPECT_EQ(scan(), Lines{"system.yaml:1:1: error CF-103: system.yaml is "
                            "missing"});
}

TEST_F(TreeScan, AnUnknownFileIsCF101) {
    write("system.yaml", "");
    write("notes.txt", "");
    write("tags/.a.yaml.swp", "");
    EXPECT_EQ(scan(), (Lines{"notes.txt:1:1: error CF-101: notes.txt is not a "
                             "file of the configuration tree",
                             "tags/.a.yaml.swp:1:1: error CF-101: "
                             "tags/.a.yaml.swp is not a file of the "
                             "configuration tree"}));
    EXPECT_EQ(paths(), Lines{"system.yaml"});
}

TEST_F(TreeScan, AnUnknownDirectoryIsRefusedAndNotRead) {
    write("system.yaml", "");
    write("drivers/foo/a.yaml", "");
    write("drivers/foo/b/c.yaml", "");
    write("templates/sub/a.yaml", "");
    EXPECT_EQ(scan(), (Lines{"drivers/foo:1:1: error CF-101: drivers/foo is "
                             "not a file of the configuration tree",
                             "templates/sub:1:1: error CF-101: templates/sub "
                             "is not a file of the configuration tree"}));
}

TEST_F(TreeScan, ASymbolicLinkIsRefusedAndNotFollowed) {
    write("system.yaml", "");
    const fs::path outside = base() / "outside.yaml";
    std::ofstream(outside) << "secret";
    fs::create_symlink(outside, root() / "areas.yaml");
    fs::create_directory(base() / "elsewhere");
    std::ofstream(base() / "elsewhere" / "a.yaml") << "x";
    fs::create_directory_symlink(base() / "elsewhere", root() / "tags");
    EXPECT_EQ(scan(), (Lines{"areas.yaml:1:1: error CF-101: areas.yaml is not "
                             "a file of the configuration tree",
                             "tags:1:1: error CF-101: tags is not a file of "
                             "the configuration tree"}));
    EXPECT_EQ(paths(), Lines{"system.yaml"});
}

TEST_F(TreeScan, ALinkThatPointsIntoTheTreeIsRefusedToo) {
    write("system.yaml", "");
    fs::create_symlink("system.yaml", root() / "areas.yaml");
    EXPECT_EQ(scan(), Lines{"areas.yaml:1:1: error CF-101: areas.yaml is not "
                            "a file of the configuration tree"});
}

TEST_F(TreeScan, AFifoIsRefusedWithoutBlocking) {
    write("system.yaml", "");
    constexpr mode_t FIFO_MODE = 0600;
    ASSERT_EQ(mkfifo((root() / "areas.yaml").c_str(), FIFO_MODE), 0);
    EXPECT_EQ(scan(), Lines{"areas.yaml:1:1: error CF-101: areas.yaml is not "
                            "a file of the configuration tree"});
}

TEST_F(TreeScan, ASocketIsRefused) {
    write("system.yaml", "");
    const std::string path = (root() / "enums.yaml").string();
    const int socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(socket_fd, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    ASSERT_LT(path.size(), sizeof(address.sun_path));
    path.copy(static_cast<char*>(address.sun_path), path.size());
    const int bound =
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,scada-banned): the socket interface takes every address type through a sockaddr pointer
        bind(socket_fd, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address));
    close(socket_fd);
    ASSERT_EQ(bound, 0);
    EXPECT_EQ(scan(), Lines{"enums.yaml:1:1: error CF-101: enums.yaml is not "
                            "a file of the configuration tree"});
}

// /etc/scada is a link to the active tree (contract 13.2): the root itself
// may be one.
TEST_F(TreeScan, TheRootMayBeALink) {
    write("system.yaml", "x");
    const fs::path link = base() / "active";
    fs::create_directory_symlink(root(), link);
    set_root(link);
    EXPECT_EQ(scan(), Lines{});
    EXPECT_EQ(paths(), Lines{"system.yaml"});
}

TEST_F(TreeScan, AMissingRootFails) {
    Diagnostics diagnostics;
    const Result<Tree> scanned = scan_tree(base() / "absent", diagnostics);
    ASSERT_FALSE(scanned.ok());
    EXPECT_EQ(scanned.status().code(), StatusCode::NOT_FOUND);
}

TEST_F(TreeScan, ARootThatIsAFileFails) {
    write("system.yaml", "");
    Diagnostics diagnostics;
    const Result<Tree> scanned = scan_tree(root() / "system.yaml", diagnostics);
    ASSERT_FALSE(scanned.ok());
    EXPECT_EQ(scanned.status().code(), StatusCode::NOT_FOUND);
}

// Section 1.3: 8 MiB a file.
TEST_F(TreeScan, AFileOfExactlyEightMebibytesIsRead) {
    write("system.yaml", "");
    write_size("tags/a.yaml", MAX_FILE_BYTES);
    EXPECT_EQ(scan(), Lines{});
    ASSERT_EQ(tree().files.size(), 2U);
    EXPECT_EQ(tree().files.at(1).content.size(), MAX_FILE_BYTES);
}

TEST_F(TreeScan, AFileOneByteOverEightMebibytesIsCF113) {
    write("system.yaml", "");
    write_size("tags/a.yaml", MAX_FILE_BYTES + 1);
    EXPECT_EQ(scan(), Lines{"tags/a.yaml:1:1: error CF-113: the file exceeds "
                            "its limit of 8 MiB"});
    EXPECT_EQ(paths(), Lines{"system.yaml"});
}

// Section 1.3: 1 MiB a `.st` file.
TEST_F(TreeScan, AStructuredTextFileHasALimitOfOneMebibyte) {
    write("system.yaml", "");
    write_size("control/a.st", MAX_STRUCTURED_TEXT_BYTES);
    write_size("control/b.st", MAX_STRUCTURED_TEXT_BYTES + 1);
    EXPECT_EQ(scan(), Lines{"control/b.st:1:1: error CF-113: the file "
                            "exceeds its limit of 1 MiB"});
}

// Section 1.3: 20,000 files a tree. The directories of the tree do not
// count; every other entry does, refused ones too, and the scan stops at the
// limit, before the next entry is read (D-105).
TEST_F(TreeScan, TwentyThousandFilesAreReadAndOneMoreIsCF113) {
    write("system.yaml", "");
    fs::create_directory(root() / "tags");
    for (std::size_t i = 0; i + 1 < MAX_TREE_FILES; ++i) {
        touch(root() / "tags" / ("t" + std::to_string(i) + ".yaml"));
    }
    EXPECT_EQ(scan(), Lines{});
    EXPECT_EQ(tree().files.size(), MAX_TREE_FILES);
    touch(root() / "tags" / "one_more.yaml");
    // The 20,001st file in path order is the last name of tags/.
    EXPECT_EQ(scan(), Lines{"tags/t9999.yaml:1:1: error CF-113: the tree "
                            "exceeds its limit of 20000 files"});
    EXPECT_EQ(error_count(), 1U);
}

TEST_F(TreeScan, RefusedEntriesCountTowardTheLimit) {
    write("a.txt", "");
    write("b.txt", "");
    write("c.txt", "");
    write("system.yaml", "");
    ScanLimits limits;
    limits.max_entries = 2;
    // a.txt and b.txt are counted and refused; c.txt passes the limit and
    // stops the scan, so system.yaml is never looked at and gives no CF-103.
    EXPECT_EQ(scan(limits),
              (Lines{"a.txt:1:1: error CF-101: a.txt is not a file of the "
                     "configuration tree",
                     "b.txt:1:1: error CF-101: b.txt is not a file of the "
                     "configuration tree",
                     "c.txt:1:1: error CF-113: the tree exceeds its limit of "
                     "2 files"}));
}

// A directory with far more entries than the limit is not even listed to
// its end.
TEST_F(TreeScan, AHugeDirectoryIsNotListedToItsEnd) {
    fs::create_directory(root() / "tags");
    for (int i = 0; i < HUGE_DIRECTORY; ++i) {
        touch(root() / "tags" / ("x" + std::to_string(i) + ".txt"));
    }
    ScanLimits limits;
    limits.max_entries = SMALL_FILE_LIMIT;
    EXPECT_EQ(scan(limits), Lines{"tags:1:1: error CF-113: the tree exceeds "
                                  "its limit of 10 files"});
}

TEST_F(TreeScan, TheDirectoriesOfTheTreeDoNotCount) {
    write("system.yaml", "");
    write("tags/a.yaml", "");
    write("drivers/modbus/a.yaml", "");
    write("drivers/io/a.yaml", "");
    ScanLimits limits;
    limits.max_entries = 4;
    EXPECT_EQ(scan(limits), Lines{});
}

// Section 1.3: 256 MiB a tree, made smaller here.
TEST_F(TreeScan, TheTreeSizeLimitIsCheckedBeforeAFileIsKept) {
    write("system.yaml", std::string(SYSTEM_BYTES, 'a'));
    write("tags/a.yaml", std::string(FIRST_TAGS_BYTES, 'b'));
    write("tags/b.yaml", std::string(1, 'c'));
    write("tags/c.yaml", std::string(1, 'd'));
    ScanLimits limits;
    limits.max_tree_bytes = SMALL_TREE_BYTES;
    EXPECT_EQ(scan(limits), Lines{"tags/b.yaml:1:1: error CF-113: the tree "
                                  "exceeds its limit of 100 bytes"});
    EXPECT_EQ(paths(), (Lines{"system.yaml", "tags/a.yaml"}));
}

TEST_F(TreeScan, TheDefaultTreeLimitsAreThoseOfTheContract) {
    const ScanLimits limits;
    EXPECT_EQ(limits.max_entries, MAX_TREE_FILES);
    EXPECT_EQ(limits.max_tree_bytes, MAX_TREE_BYTES);
    EXPECT_FALSE(limits.max_file_bytes);
    EXPECT_EQ(MAX_TREE_FILES, 20000U);
    EXPECT_EQ(MAX_TREE_BYTES, 256U * MIB);
    EXPECT_EQ(MAX_FILE_BYTES, 8U * MIB);
    EXPECT_EQ(MAX_STRUCTURED_TEXT_BYTES, MIB);
}

}  // namespace
}  // namespace scada::conf
