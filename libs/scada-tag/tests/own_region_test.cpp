// Unit tests for the regions a process writes (contract, sections 3.2 and
// 11.2, step 3): found and reused, or created. The `proc` region, which is
// also locked, has its tests in proc_region_test.cpp.

#include "own_region.h"

#include "layout.h"
#include "region_file.h"
#include "shared_memory.h"
#include "temp_database.h"

#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace {

using namespace scada::tag;
using scada::Fd;
using scada::Result;
using scada::StatusCode;
using scada::tag::testing::mode_of;
using scada::tag::testing::open_directory;
using scada::tag::testing::touch;

constexpr std::uint64_t GENERATION = 3;
constexpr std::uint32_t CAPACITY = 1024;
constexpr std::uint32_t OTHER_CAPACITY = 2048;
// What a test leaves in a region to see whether it stays.
constexpr std::uint64_t KEPT_WRITE_SEQ = 9;

class OwnRegions : public scada::tag::testing::TempDatabase {
protected:
    void SetUp() override {
        TempDatabase::SetUp();
        directory_ = open_directory(base());
        ASSERT_TRUE(directory_.valid());
    }

    // A change ring of `capacity` entries for owner 2. `initialised_`
    // counts how often a new region was filled in.
    [[nodiscard]] OwnRegionSpec ring(std::uint32_t capacity = CAPACITY) {
        OwnRegionSpec spec;
        spec.name = "changes.io";
        spec.identity.kind = RegionKind::CHANGES;
        spec.identity.generation = GENERATION;
        spec.identity.size_bytes = changes_region_size(CAPACITY);
        spec.identity.owner_index = 2;
        spec.identity.uid = uid();
        spec.initialise = [this, capacity](std::span<std::byte> bytes) {
            ++initialised_;
            shared_at<ChangeRingHeader>(bytes, sizeof(RegionHeader)).capacity =
                capacity;
        };
        spec.usable = [capacity](std::span<std::byte> bytes) {
            return shared_at<ChangeRingHeader>(bytes, sizeof(RegionHeader))
                       .capacity == capacity;
        };
        return spec;
    }

    [[nodiscard]] Result<OwnRegion> attach(const OwnRegionSpec& spec) {
        detail_.clear();
        return attach_own_region(directory_.get(), spec, detail_);
    }

    [[nodiscard]] int initialised() const { return initialised_; }
    [[nodiscard]] const std::string& detail() const { return detail_; }

private:
    Fd directory_;
    std::string detail_;
    int initialised_ = 0;
};

TEST_F(OwnRegions, AnAbsentRegionIsCreatedInitialisedAndReady) {
    const Result<OwnRegion> region = attach(ring());
    ASSERT_TRUE(region.ok()) << detail();
    EXPECT_FALSE(region.value().reused);
    EXPECT_TRUE(region.value().memory.writable());
    EXPECT_EQ(initialised(), 1);
    EXPECT_EQ(mode_of(base() / "changes.io"), REGION_FILE_MODE);
    const std::span<std::byte> bytes = region.value().memory.bytes();
    ASSERT_EQ(bytes.size(), changes_region_size(CAPACITY));
    const auto& header = shared_at<RegionHeader>(bytes, 0);
    EXPECT_EQ(header.ready, 1U);
    EXPECT_EQ(header.kind, to_raw(RegionKind::CHANGES));
    EXPECT_EQ(header.generation, GENERATION);
    EXPECT_EQ(header.owner_index, 2);
    EXPECT_EQ(shared_at<ChangeRingHeader>(bytes, sizeof(RegionHeader)).capacity,
              CAPACITY);
}

TEST_F(OwnRegions, AUsableRegionIsReusedAsItIs) {
    {
        const Result<OwnRegion> first = attach(ring());
        ASSERT_TRUE(first.ok()) << detail();
        shared_at<ChangeRingHeader>(first.value().memory.bytes(),
                                    sizeof(RegionHeader))
            .write_seq = KEPT_WRITE_SEQ;
    }
    const Result<OwnRegion> second = attach(ring());
    ASSERT_TRUE(second.ok()) << detail();
    EXPECT_TRUE(second.value().reused);
    EXPECT_EQ(initialised(), 1);
    EXPECT_EQ(shared_at<ChangeRingHeader>(second.value().memory.bytes(),
                                          sizeof(RegionHeader))
                  .write_seq,
              KEPT_WRITE_SEQ);
}

TEST_F(OwnRegions, WithoutALockARegionCanBeAttachedTwice) {
    // The lock of the process is on its `proc` region alone.
    const Result<OwnRegion> first = attach(ring());
    const Result<OwnRegion> second = attach(ring());
    EXPECT_TRUE(first.ok());
    EXPECT_TRUE(second.ok());
    EXPECT_TRUE(second.value().reused);
}

TEST_F(OwnRegions, ARegionThatIsNotUsableIsCreatedAgain) {
    ASSERT_TRUE(attach(ring(OTHER_CAPACITY)).ok()) << detail();
    const Result<OwnRegion> region = attach(ring());
    ASSERT_TRUE(region.ok()) << detail();
    EXPECT_FALSE(region.value().reused);
    EXPECT_EQ(initialised(), 2);
    EXPECT_EQ(shared_at<ChangeRingHeader>(region.value().memory.bytes(),
                                          sizeof(RegionHeader))
                  .capacity,
              CAPACITY);
}

TEST_F(OwnRegions, ARegionWithoutChecksOrContentIsAccepted) {
    OwnRegionSpec spec = ring();
    spec.initialise = nullptr;
    spec.usable = nullptr;
    ASSERT_TRUE(attach(spec).ok()) << detail();
    const Result<OwnRegion> again = attach(spec);
    ASSERT_TRUE(again.ok()) << detail();
    EXPECT_TRUE(again.value().reused);
}

TEST_F(OwnRegions, AnEntryOfAnotherUserIsLeftAndTheAttachFails) {
    // The file belongs to the user of the test; the region is to belong to
    // another one.
    touch(base() / "changes.io");
    OwnRegionSpec spec = ring();
    spec.identity.uid = other_uid();
    const Result<OwnRegion> region = attach(spec);
    EXPECT_EQ(region.status().code(), StatusCode::PERMISSION_DENIED);
    EXPECT_NE(detail().find("owned by user"), std::string::npos) << detail();
    EXPECT_TRUE(std::filesystem::exists(base() / "changes.io"));
    EXPECT_EQ(initialised(), 0);
}

TEST_F(OwnRegions, AnEmptyFileOfTheCallerIsReplaced) {
    touch(base() / "changes.io");
    const Result<OwnRegion> region = attach(ring());
    ASSERT_TRUE(region.ok()) << detail();
    EXPECT_FALSE(region.value().reused);
    EXPECT_EQ(std::filesystem::file_size(base() / "changes.io"),
              changes_region_size(CAPACITY));
}

}  // namespace
