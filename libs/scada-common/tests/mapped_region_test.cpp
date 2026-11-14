// Unit tests for the owning mapping of a file.

#include "scada/common/mapped_region.h"

#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using scada::Fd;
using scada::MapAccess;
using scada::MappedRegion;
using scada::Result;
using scada::StatusCode;

static_assert(!std::is_copy_constructible_v<MappedRegion>);
static_assert(!std::is_copy_assignable_v<MappedRegion>);
static_assert(std::is_nothrow_move_constructible_v<MappedRegion>);
static_assert(std::is_nothrow_move_assignable_v<MappedRegion>);

constexpr std::size_t FILE_SIZE = 8192;
// A byte that is not the zero the file is filled with.
constexpr std::byte MARK{0x5A};

// An unnamed file of FILE_SIZE zero bytes.
Fd make_file() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "scada-map-test-XXXXXX")
            .string();
    Fd file(::mkstemp(pattern.data()));
    if (file.valid()) {
        static_cast<void>(::unlink(pattern.c_str()));
        if (::ftruncate(file.get(), FILE_SIZE) != 0) {
            file.reset();
        }
    }
    return file;
}

TEST(MappedRegion, DefaultHoldsNothing) {
    const MappedRegion empty;
    EXPECT_FALSE(empty.valid());
    EXPECT_FALSE(empty.writable());
    EXPECT_TRUE(empty.bytes().empty());
}

TEST(MappedRegion, RejectsNoFileAndNoSize) {
    const Fd file = make_file();
    ASSERT_TRUE(file.valid());
    EXPECT_EQ(
        MappedRegion::map(-1, FILE_SIZE, MapAccess::READ_ONLY).status().code(),
        StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(
        MappedRegion::map(file.get(), 0, MapAccess::READ_ONLY).status().code(),
        StatusCode::INVALID_ARGUMENT);
}

TEST(MappedRegion, ReportsARefusedMapping) {
    // A descriptor opened for reading only cannot be mapped shared and
    // writable.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open has no other form
    const Fd file(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    ASSERT_TRUE(file.valid());
    EXPECT_EQ(MappedRegion::map(file.get(), FILE_SIZE, MapAccess::READ_WRITE)
                  .status()
                  .code(),
              StatusCode::IO_ERROR);
}

TEST(MappedRegion, TwoMappingsOfOneFileShareTheBytes) {
    const Fd file = make_file();
    ASSERT_TRUE(file.valid());
    Result<MappedRegion> writer =
        MappedRegion::map(file.get(), FILE_SIZE, MapAccess::READ_WRITE);
    Result<MappedRegion> reader =
        MappedRegion::map(file.get(), FILE_SIZE, MapAccess::READ_ONLY);
    ASSERT_TRUE(writer.ok());
    ASSERT_TRUE(reader.ok());
    EXPECT_TRUE(writer.value().writable());
    EXPECT_FALSE(reader.value().writable());
    ASSERT_EQ(writer.value().bytes().size(), FILE_SIZE);
    ASSERT_EQ(reader.value().bytes().size(), FILE_SIZE);

    EXPECT_EQ(reader.value().bytes().back(), std::byte{0});
    writer.value().bytes().back() = MARK;
    EXPECT_EQ(reader.value().bytes().back(), MARK);
}

TEST(MappedRegion, MappingOutlivesTheDescriptor) {
    Fd file = make_file();
    ASSERT_TRUE(file.valid());
    Result<MappedRegion> region =
        MappedRegion::map(file.get(), FILE_SIZE, MapAccess::READ_WRITE);
    ASSERT_TRUE(region.ok());
    file.reset();
    region.value().bytes().front() = std::byte{1};
    EXPECT_EQ(region.value().bytes().front(), std::byte{1});
}

TEST(MappedRegion, MoveTransfersTheMappingAndResetRemovesIt) {
    const Fd file = make_file();
    ASSERT_TRUE(file.valid());
    Result<MappedRegion> region =
        MappedRegion::map(file.get(), FILE_SIZE, MapAccess::READ_WRITE);
    ASSERT_TRUE(region.ok());
    const std::byte* const address = region.value().bytes().data();

    MappedRegion moved(std::move(region).value());
    EXPECT_TRUE(moved.valid());
    EXPECT_TRUE(moved.writable());
    EXPECT_EQ(moved.bytes().data(), address);

    MappedRegion assigned;
    assigned = std::move(moved);
    EXPECT_EQ(assigned.bytes().data(), address);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): the moved-from state is what is tested
    EXPECT_FALSE(moved.valid());

    assigned.reset();
    EXPECT_FALSE(assigned.valid());
    EXPECT_TRUE(assigned.bytes().empty());
    assigned.reset();  // a second reset does nothing
}

}  // namespace
