// Unit tests for the files of the tag database: names, sizes, and the
// creation and checking rules of the contract, section 3.

#include "region_file.h"

#include "layout.h"
#include "shared_memory.h"
#include "temp_database.h"

#include "scada/common/fd.h"
#include "scada/common/mapped_region.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scada::tag;
using scada::Fd;
using scada::MapAccess;
using scada::Result;
using scada::StatusCode;
using scada::tag::testing::mode_of;
using scada::tag::testing::names_in;
using scada::tag::testing::open_directory;
using scada::tag::testing::touch;
using scada::tag::testing::UMASK_077;

constexpr std::uint64_t GENERATION = 3;
constexpr std::uint64_t REGION_SIZE = 4096;
// Modes a base directory must not have: without set-group-id, readable by
// others, writable by the group, closed to the group, and sticky.
constexpr std::array<mode_t, 5> WRONG_BASE_MODES{0750, 02755, 02770, 02700,
                                                 03750};

TEST(RegionNames, GenerationDirectoryNamesAreParsedStrictly) {
    EXPECT_EQ(generation_dir_name(1), "gen-1");
    EXPECT_EQ(parse_generation_dir_name("gen-1"), 1U);
    EXPECT_EQ(parse_generation_dir_name("gen-18446744073709551615"),
              18'446'744'073'709'551'615U);
    for (const std::string_view name :
         {"", "gen-", "gen-0", "gen-01", "gen--1", "gen-+1", "gen-1 ", "gen-1x",
          "gen-18446744073709551616", "Gen-1", "gen1", "current",
          ".tmp.current.1"}) {
        EXPECT_EQ(parse_generation_dir_name(name), std::nullopt) << name;
    }
}

TEST(RegionNames, TemporaryNamesMatchTheirRegionExactly) {
    EXPECT_EQ(temporary_name("proc.io", 123), ".tmp.proc.io.123");
    EXPECT_TRUE(is_temporary_name_of(".tmp.proc.io.123", "proc.io"));
    EXPECT_TRUE(is_temporary_name_of(".tmp.proc.io.5.123", "proc.io.5"));
    // The process id of another region's name, a prefix, and non-digits.
    EXPECT_FALSE(is_temporary_name_of(".tmp.proc.io.5.123", "proc.io"));
    EXPECT_FALSE(is_temporary_name_of(".tmp.proc.io2.123", "proc.io"));
    EXPECT_FALSE(is_temporary_name_of(".tmp.proc.io.", "proc.io"));
    EXPECT_FALSE(is_temporary_name_of(".tmp.proc.io", "proc.io"));
    EXPECT_FALSE(is_temporary_name_of(".tmp.proc.io.12a", "proc.io"));
    EXPECT_FALSE(is_temporary_name_of("proc.io.123", "proc.io"));
    EXPECT_FALSE(is_temporary_name_of("", "proc.io"));
}

TEST(RegionSizes, FollowTheLayoutOfTheContract) {
    EXPECT_EQ(proc_region_size(0), 128U);
    EXPECT_EQ(proc_region_size(3), 152U);
    EXPECT_EQ(tags_region_size(2), 64U + 128U);
    EXPECT_EQ(strings_region_size(2), 64U + 512U);
    EXPECT_EQ(changes_region_size(1024), 128U + (1024U * 32U));
    EXPECT_EQ(queue_region_size(1024, 256), 128U + (1024U * 256U));
}

class RegionFile : public scada::tag::testing::TempDatabase {
protected:
    void SetUp() override {
        TempDatabase::SetUp();
        directory_ = open_directory(base());
        ASSERT_TRUE(directory_.valid());
    }

    [[nodiscard]] int directory() const { return directory_.get(); }

    [[nodiscard]] static RegionIdentity identity() {
        RegionIdentity wanted;
        wanted.kind = RegionKind::PROC;
        wanted.generation = GENERATION;
        wanted.size_bytes = REGION_SIZE;
        wanted.owner_index = 2;
        wanted.uid = uid();
        return wanted;
    }

    // Creates and publishes the region `name` with `wanted`.
    void publish(std::string_view name, const RegionIdentity& wanted) {
        std::string detail;
        Result<RegionBuilder> builder =
            RegionBuilder::create(directory(), name, wanted, detail);
        ASSERT_TRUE(builder.ok()) << detail;
        ASSERT_TRUE(builder.value().publish(detail).ok()) << detail;
    }

    [[nodiscard]] StatusCode open_code(std::string_view name,
                                       const RegionIdentity& wanted) {
        std::string detail;
        const Result<OpenedRegion> region = open_region(
            directory(), name, wanted, MapAccess::READ_ONLY, detail);
        if (!region.ok()) {
            EXPECT_FALSE(detail.empty());
        }
        return region.status().code();
    }

    [[nodiscard]] std::vector<std::string> entries() const {
        return names_in(base());
    }

private:
    Fd directory_;
};

TEST_F(RegionFile, BaseDirectoryMustBeOwnedByTheUserOfTagd) {
    std::string detail;
    EXPECT_TRUE(open_base_directory(base().string(), uid(), detail).ok())
        << detail;
    EXPECT_EQ(open_base_directory(base().string(), other_uid(), detail)
                  .status()
                  .code(),
              StatusCode::PERMISSION_DENIED);
    EXPECT_NE(detail.find("owned by user"), std::string::npos) << detail;
}

TEST_F(RegionFile, BaseDirectoryMustHaveMode2750Exactly) {
    std::string detail;
    for (const mode_t mode : WRONG_BASE_MODES) {
        ASSERT_EQ(::chmod(base().c_str(), mode), 0);
        EXPECT_EQ(
            open_base_directory(base().string(), uid(), detail).status().code(),
            StatusCode::PERMISSION_DENIED)
            << mode;
    }
}

TEST_F(RegionFile, BaseDirectoryMustBeADirectoryThatExists) {
    std::string detail;
    EXPECT_EQ(open_base_directory((root() / "none").string(), uid(), detail)
                  .status()
                  .code(),
              StatusCode::NOT_FOUND);

    // A symbolic link to a good directory, and a file, are not directories.
    const std::filesystem::path link = root() / "link";
    std::filesystem::create_directory_symlink(base(), link);
    EXPECT_EQ(open_base_directory(link.string(), uid(), detail).status().code(),
              StatusCode::PERMISSION_DENIED);
    const std::filesystem::path file = root() / "file";
    touch(file);
    EXPECT_EQ(open_base_directory(file.string(), uid(), detail).status().code(),
              StatusCode::PERMISSION_DENIED);
}

// Conformance test 11, first part, for one region: the mode of section 3.1
// under umask 077.
TEST_F(RegionFile, Conformance11RegionHasMode0640UnderUmask077) {
    const mode_t previous = ::umask(UMASK_077);
    publish("proc.io", identity());
    static_cast<void>(::umask(previous));

    EXPECT_EQ(mode_of(base() / "proc.io"), REGION_FILE_MODE);
    EXPECT_EQ(std::filesystem::file_size(base() / "proc.io"), REGION_SIZE);
    EXPECT_EQ(entries(), std::vector<std::string>{"proc.io"});
    EXPECT_EQ(open_code("proc.io", identity()), StatusCode::OK);
}

TEST_F(RegionFile, BuilderWritesTheRegionHeaderAndZeroes) {
    std::string detail;
    Result<RegionBuilder> builder =
        RegionBuilder::create(directory(), "proc.io", identity(), detail);
    ASSERT_TRUE(builder.ok()) << detail;
    ASSERT_EQ(builder.value().bytes().size(), REGION_SIZE);

    const RegionHeader& header =
        shared_at<RegionHeader>(builder.value().bytes(), 0);
    EXPECT_EQ(header.magic, REGION_MAGIC);
    EXPECT_EQ(header.layout_version, LAYOUT_VERSION);
    EXPECT_EQ(header.kind, to_raw(RegionKind::PROC));
    EXPECT_EQ(header.generation, GENERATION);
    EXPECT_EQ(header.size_bytes, REGION_SIZE);
    EXPECT_EQ(header.owner_index, 2U);
    EXPECT_EQ(header.queue_id, NO_QUEUE_ID);
    EXPECT_EQ(header.ready, 0U);
    EXPECT_TRUE(std::ranges::all_of(
        builder.value().bytes().subspan(sizeof(RegionHeader)),
        [](std::byte value) { return value == std::byte{0}; }));

    // Until it is published the region has only its temporary name.
    EXPECT_EQ(entries(),
              std::vector<std::string>{temporary_name("proc.io", ::getpid())});
    EXPECT_EQ(open_code("proc.io", identity()), StatusCode::NOT_FOUND);
}

TEST_F(RegionFile, BuilderThatIsNotPublishedLeavesNothing) {
    {
        std::string detail;
        const Result<RegionBuilder> builder =
            RegionBuilder::create(directory(), "proc.io", identity(), detail);
        ASSERT_TRUE(builder.ok()) << detail;
    }
    EXPECT_TRUE(entries().empty());
}

TEST_F(RegionFile, PublishNeverReplacesAnExistingRegion) {
    publish("proc.io", identity());
    const auto first = scada::tag::testing::inode_of(base() / "proc.io");

    std::string detail;
    Result<RegionBuilder> second =
        RegionBuilder::create(directory(), "proc.io", identity(), detail);
    ASSERT_TRUE(second.ok()) << detail;
    EXPECT_EQ(second.value().publish(detail).status().code(),
              StatusCode::ALREADY_EXISTS);
    EXPECT_EQ(scada::tag::testing::inode_of(base() / "proc.io"), first);
    EXPECT_EQ(entries(), std::vector<std::string>{"proc.io"});
}

TEST_F(RegionFile, ImpossibleSizeIsRefused) {
    std::string detail;
    RegionIdentity wanted = identity();
    wanted.size_bytes = sizeof(RegionHeader) - 1;
    EXPECT_EQ(RegionBuilder::create(directory(), "proc.io", wanted, detail)
                  .status()
                  .code(),
              StatusCode::INVALID_ARGUMENT);
    EXPECT_TRUE(entries().empty());
}

TEST_F(RegionFile, OpenRejectsWrongOwnerSizeAndHeaderFields) {
    publish("proc.io", identity());

    RegionIdentity wanted = identity();
    wanted.uid = other_uid();
    EXPECT_EQ(open_code("proc.io", wanted), StatusCode::PERMISSION_DENIED);

    wanted = identity();
    wanted.size_bytes = REGION_SIZE * 2;
    EXPECT_EQ(open_code("proc.io", wanted), StatusCode::DATA_CORRUPT);

    wanted = identity();
    wanted.kind = RegionKind::TAGS;
    EXPECT_EQ(open_code("proc.io", wanted), StatusCode::DATA_CORRUPT);

    wanted = identity();
    wanted.generation = GENERATION + 1;
    EXPECT_EQ(open_code("proc.io", wanted), StatusCode::DATA_CORRUPT);

    wanted = identity();
    wanted.owner_index = 3;
    EXPECT_EQ(open_code("proc.io", wanted), StatusCode::DATA_CORRUPT);

    wanted = identity();
    wanted.queue_id = 0;
    EXPECT_EQ(open_code("proc.io", wanted), StatusCode::DATA_CORRUPT);
}

TEST_F(RegionFile, OpenRejectsWrongMagicLayoutVersionAndNotReady) {
    publish("proc.io", identity());
    std::string detail;
    Result<OpenedRegion> region = open_region(
        directory(), "proc.io", identity(), MapAccess::READ_WRITE, detail);
    ASSERT_TRUE(region.ok()) << detail;
    auto& header = shared_at<RegionHeader>(region.value().memory.bytes(), 0);

    header.magic += 1;
    EXPECT_EQ(open_code("proc.io", identity()), StatusCode::DATA_CORRUPT);
    header.magic = REGION_MAGIC;

    header.layout_version = LAYOUT_VERSION + 1;
    EXPECT_EQ(open_code("proc.io", identity()), StatusCode::DATA_CORRUPT);
    header.layout_version = LAYOUT_VERSION;

    store_shared(header.ready, 0U, std::memory_order_release);
    EXPECT_EQ(open_code("proc.io", identity()), StatusCode::UNAVAILABLE);
    store_shared(header.ready, 1U, std::memory_order_release);
    EXPECT_EQ(open_code("proc.io", identity()), StatusCode::OK);
}

// Conformance test 9, second part: a symbolic link under a region name is
// not followed, even to a region that would pass every check.
TEST_F(RegionFile, Conformance09SymlinkUnderARegionNameIsNotFollowed) {
    publish("proc.real", identity());
    std::filesystem::create_symlink("proc.real", base() / "proc.io");
    EXPECT_EQ(open_code("proc.real", identity()), StatusCode::OK);
    EXPECT_EQ(open_code("proc.io", identity()), StatusCode::DATA_CORRUPT);
}

TEST_F(RegionFile, OpenRejectsWhatIsNotARegularFileWithoutBlocking) {
    ASSERT_EQ(::mkfifo((base() / "proc.fifo").c_str(), REGION_FILE_MODE), 0);
    EXPECT_EQ(open_code("proc.fifo", identity()), StatusCode::DATA_CORRUPT);
    std::filesystem::create_directory(base() / "proc.dir");
    EXPECT_EQ(open_code("proc.dir", identity()), StatusCode::DATA_CORRUPT);
    EXPECT_EQ(open_code("proc.none", identity()), StatusCode::NOT_FOUND);
}

TEST_F(RegionFile, StaleTemporariesOfTheRegionAndUserAreRemoved) {
    touch(base() / ".tmp.proc.io.1");
    touch(base() / ".tmp.proc.io.4000000");
    touch(base() / ".tmp.proc.io.5.7");  // of the region proc.io.5
    touch(base() / ".tmp.proc.iox.1");
    touch(base() / ".tmp.tags.io.1");
    touch(base() / "proc.io");

    // Files of another user stay.
    remove_stale_temporaries(directory(), "proc.io", other_uid());
    EXPECT_EQ(entries().size(), 6U);

    remove_stale_temporaries(directory(), "proc.io", uid());
    EXPECT_EQ(entries(),
              (std::vector<std::string>{".tmp.proc.io.5.7", ".tmp.proc.iox.1",
                                        ".tmp.tags.io.1", "proc.io"}));
}

}  // namespace
