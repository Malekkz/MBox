// Unit tests for the regions of other processes (contract, sections 3.1
// and 11.2): mapped on demand, missing or unusable ones tolerated and
// looked for again, at most once a second each.

#include "peer_regions.h"

#include "attach_fixture.h"
#include "attachment.h"
#include "header_view.h"
#include "layout.h"
#include "proc_region.h"
#include "region_file.h"
#include "shared_memory.h"
#include "temp_database.h"

#include "scada/common/clock.h"
#include "scada/common/fd.h"
#include "scada/common/mapped_region.h"
#include "scada/common/result.h"
#include "scada/tag/liveness.h"
#include "scada/tag/tag_set.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace {

using namespace scada::tag;
using scada::Fd;
using scada::MappedRegion;
using scada::Result;
using scada::tag::testing::IO_TAG_COUNT;
using scada::tag::testing::open_directory;
using scada::tag::testing::OWNER_IO;
using scada::tag::testing::PROCESS_GUEST;
using scada::tag::testing::PROCESS_IO;
using scada::tag::testing::QUEUE_EVENTS;

// Any time on the monotonic clock.
constexpr std::int64_t START_NS = 5'000'000'000'000;
constexpr std::int64_t SECOND_NS = PEER_RETRY_INTERVAL_NS;
// A ring capacity that is not the one of the owner `io`.
constexpr std::uint32_t OTHER_CAPACITY = 2048;

class Peers : public scada::tag::testing::AttachFixture {
protected:
    void SetUp() override {
        AttachFixture::SetUp();
        start_host();
        directory_ = open_directory(generation_dir());
        std::string why;
        Result<HeaderView> header =
            HeaderView::open(directory_.get(), 1, uid(), why);
        ASSERT_TRUE(header.ok()) << why;
        peers_ = std::make_unique<PeerRegions>(header.value().contents());
    }

    [[nodiscard]] std::unique_ptr<Attachment> attached_io() {
        std::string why;
        Result<std::unique_ptr<Attachment>> feeder = Attachment::attach(
            "io",
            {.base_dir = base().string(), .retain_dir = {}, .tagd_uid = uid()},
            why);
        EXPECT_TRUE(feeder.ok()) << why;
        return feeder.ok() ? std::move(feeder).value() : nullptr;
    }

    [[nodiscard]] PeerRegions& peers() { return *peers_; }
    [[nodiscard]] int directory() const { return directory_.get(); }

private:
    Fd directory_;
    std::unique_ptr<PeerRegions> peers_;
};

TEST_F(Peers, ARegionNobodyAskedForIsNotMapped) {
    const auto feeder = attached_io();
    EXPECT_TRUE(peers().bytes(peers().tags_slot(OWNER_IO)).empty());
    EXPECT_EQ(peers().refresh(directory(), START_NS), 0U);
    EXPECT_TRUE(peers().bytes(peers().tags_slot(OWNER_IO)).empty());
    // No such slot.
    EXPECT_TRUE(peers().bytes(~std::size_t{0}).empty());
}

TEST_F(Peers, ARegionThatExistsIsMappedWhenAskedFor) {
    const auto feeder = attached_io();
    const std::size_t tags = peers().tags_slot(OWNER_IO);
    peers().want(directory(), tags, START_NS);
    EXPECT_EQ(peers().bytes(tags).size(), tags_region_size(IO_TAG_COUNT));
    peers().want(directory(), peers().proc_slot(PROCESS_IO), START_NS);
    peers().want(directory(), peers().strings_slot(OWNER_IO), START_NS);
    peers().want(directory(), peers().changes_slot(OWNER_IO), START_NS);
    peers().want(directory(), peers().queue_slot(QUEUE_EVENTS), START_NS);
    EXPECT_EQ(peers().bytes(peers().proc_slot(PROCESS_IO)).size(),
              proc_region_size(3));
    EXPECT_EQ(peers().bytes(peers().strings_slot(OWNER_IO)).size(),
              strings_region_size(2));
    EXPECT_EQ(peers().bytes(peers().changes_slot(OWNER_IO)).size(),
              changes_region_size(1024));
    EXPECT_EQ(peers().bytes(peers().queue_slot(QUEUE_EVENTS)).size(),
              queue_region_size(1024, sizeof(EventRecord)));
    // The mapped `proc` region gives the liveness of the process.
    EXPECT_EQ(
        proc_liveness(peers().bytes(peers().proc_slot(PROCESS_IO)),
                      DEFAULT_HEARTBEAT_TIMEOUT_MS, scada::monotonic_ns()),
        DownReason::NONE);
}

TEST_F(Peers, AMissingRegionIsToleratedAndFoundLaterAtMostOnceASecond) {
    const std::size_t tags = peers().tags_slot(OWNER_IO);
    peers().want(directory(), tags, START_NS);
    EXPECT_TRUE(peers().bytes(tags).empty());
    EXPECT_EQ(proc_liveness(peers().bytes(tags), 1, START_NS),
              DownReason::REGION_MISSING);

    const auto feeder = attached_io();
    // Less than a second after the last look: not looked at.
    EXPECT_EQ(peers().refresh(directory(), START_NS + SECOND_NS - 1), 0U);
    EXPECT_TRUE(peers().bytes(tags).empty());
    EXPECT_EQ(peers().refresh(directory(), START_NS + SECOND_NS), 1U);
    EXPECT_EQ(peers().bytes(tags).size(), tags_region_size(IO_TAG_COUNT));
    // Nothing changes while the file stays.
    EXPECT_EQ(peers().refresh(directory(), START_NS + (2 * SECOND_NS)), 0U);
}

TEST_F(Peers, ARegionThatIsNotReadyIsMissingUntilItIs) {
    auto feeder = attached_io();
    feeder.reset();
    const std::size_t tags = peers().tags_slot(OWNER_IO);
    const MappedRegion file = map_file("tags.io");
    auto& header = shared_at<RegionHeader>(file.bytes(), 0);
    store_shared(header.ready, 0U, std::memory_order_release);

    peers().want(directory(), tags, START_NS);
    EXPECT_TRUE(peers().bytes(tags).empty());
    store_shared(header.ready, 1U, std::memory_order_release);
    EXPECT_EQ(peers().refresh(directory(), START_NS + SECOND_NS), 1U);
    EXPECT_FALSE(peers().bytes(tags).empty());
}

TEST_F(Peers, Conformance09ARegionCreatedByTheWrongUserIsRejected) {
    // The header gives `guest` to another user. A region under its name
    // that the user of the test created is not its region.
    ProcRegionParameters guest;
    guest.process = "guest";
    guest.process_index = PROCESS_GUEST;
    guest.generation = 1;
    guest.queue_count = 2;
    guest.uid = uid();
    std::string why;
    Result<OwnProcRegion> forged =
        OwnProcRegion::attach(directory(), guest, why);
    ASSERT_TRUE(forged.ok()) << why;
    forged.value().set_state(ProcessState::RUNNING);

    const std::size_t proc = peers().proc_slot(PROCESS_GUEST);
    peers().want(directory(), proc, START_NS);
    EXPECT_TRUE(peers().bytes(proc).empty());
    EXPECT_EQ(peers().refresh(directory(), START_NS + SECOND_NS), 0U);
    EXPECT_TRUE(peers().bytes(proc).empty());
}

TEST_F(Peers, Conformance09ASymbolicLinkUnderARegionNameIsNotFollowed) {
    auto feeder = attached_io();
    feeder.reset();
    // A real region of the right content, reached through a link.
    std::filesystem::rename(generation_dir() / "tags.io", root() / "real");
    std::filesystem::create_symlink(root() / "real",
                                    generation_dir() / "tags.io");
    const std::size_t tags = peers().tags_slot(OWNER_IO);
    peers().want(directory(), tags, START_NS);
    EXPECT_TRUE(peers().bytes(tags).empty());
}

TEST_F(Peers, ARegionOfAnotherGenerationOrSizeIsRejected) {
    auto feeder = attached_io();
    feeder.reset();
    {
        const MappedRegion file = map_file("tags.io");
        shared_at<RegionHeader>(file.bytes(), 0).generation = 2;
    }
    std::filesystem::resize_file(generation_dir() / "changes.io",
                                 changes_region_size(OTHER_CAPACITY));
    peers().want(directory(), peers().tags_slot(OWNER_IO), START_NS);
    peers().want(directory(), peers().changes_slot(OWNER_IO), START_NS);
    EXPECT_TRUE(peers().bytes(peers().tags_slot(OWNER_IO)).empty());
    EXPECT_TRUE(peers().bytes(peers().changes_slot(OWNER_IO)).empty());
}

TEST_F(Peers, ARemovedRegionReadsAsMissingAndTheOldSpanStaysValid) {
    auto feeder = attached_io();
    const std::size_t tags = peers().tags_slot(OWNER_IO);
    peers().want(directory(), tags, START_NS);
    const std::span<std::byte> before = peers().bytes(tags);
    ASSERT_FALSE(before.empty());

    feeder.reset();
    std::filesystem::remove(generation_dir() / "tags.io");
    EXPECT_EQ(peers().refresh(directory(), START_NS + SECOND_NS), 1U);
    EXPECT_TRUE(peers().bytes(tags).empty());
    // A reader that got the span before can still read through it.
    EXPECT_EQ(shared_at<RegionHeader>(before, 0).magic, REGION_MAGIC);

    // The owner comes back and creates the region again: mapped anew.
    feeder = attached_io();
    EXPECT_EQ(peers().refresh(directory(), START_NS + (2 * SECOND_NS)), 1U);
    const std::span<std::byte> after = peers().bytes(tags);
    ASSERT_FALSE(after.empty());
    EXPECT_NE(after.data(), before.data());
}

TEST_F(Peers, AShrunkRegionIsDropped) {
    auto feeder = attached_io();
    const std::size_t ring = peers().changes_slot(OWNER_IO);
    peers().want(directory(), ring, START_NS);
    ASSERT_FALSE(peers().bytes(ring).empty());
    feeder.reset();
    // Contract 3.3: the owner of a region can shrink its file.
    std::filesystem::resize_file(generation_dir() / "changes.io",
                                 sizeof(RegionHeader));
    EXPECT_EQ(peers().refresh(directory(), START_NS + SECOND_NS), 1U);
    EXPECT_TRUE(peers().bytes(ring).empty());
}

TEST_F(Peers, ARegionReplacedTooOftenStaysMissing) {
    const std::size_t tags = peers().tags_slot(OWNER_IO);
    peers().want(directory(), tags, START_NS);
    std::int64_t now = START_NS;
    // Each round the region appears, is mapped, and is removed again.
    std::uint32_t mapped = 0;
    std::uint32_t dropped = 0;
    for (std::uint32_t i = 0; i < MAX_MAPPINGS_PER_REGION; ++i) {
        auto feeder = attached_io();
        now += SECOND_NS;
        mapped += static_cast<std::uint32_t>(peers().refresh(directory(), now));
        feeder.reset();
        std::filesystem::remove(generation_dir() / "tags.io");
        now += SECOND_NS;
        dropped +=
            static_cast<std::uint32_t>(peers().refresh(directory(), now));
    }
    EXPECT_EQ(mapped, MAX_MAPPINGS_PER_REGION);
    EXPECT_EQ(dropped, MAX_MAPPINGS_PER_REGION);
    const auto feeder = attached_io();
    now += SECOND_NS;
    EXPECT_EQ(peers().refresh(directory(), now), 0U);
    EXPECT_TRUE(peers().bytes(tags).empty());
}

}  // namespace
