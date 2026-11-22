// Unit tests for attach (contract, sections 3, 6.3 and 11.2): which process
// may attach, what happens to its own regions, the repair after a restart,
// and the regions of other processes mapped read-only. The side of
// scada-tagd is a GenerationHost in the test process.

#include "attachment.h"

#include "attach_fixture.h"
#include "header_view.h"
#include "layout.h"
#include "own_region.h"
#include "peer_regions.h"
#include "region_file.h"
#include "shared_memory.h"
#include "temp_database.h"

#include "scada/common/clock.h"
#include "scada/common/fd.h"
#include "scada/common/mapped_region.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/generation_host.h"
#include "scada/tag/liveness.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace scada::tag;
using scada::MappedRegion;
using scada::Result;
using scada::StatusCode;
using scada::tag::testing::inode_of;
using scada::tag::testing::IO_TAG_COUNT;
using scada::tag::testing::mode_of;
using scada::tag::testing::names_in;
using scada::tag::testing::OWNER_IO;
using scada::tag::testing::PROCESS_IO;
using scada::tag::testing::QUEUE_CMD;
using scada::tag::testing::QUEUE_EVENTS;
using scada::tag::testing::SLOT_LABEL;
using scada::tag::testing::SLOT_LEVEL;
using scada::tag::testing::SLOT_NOTE;
using scada::tag::testing::SLOT_RUNNING;
using scada::tag::testing::STRING_SLOT_LABEL;
using scada::tag::testing::STRING_SLOT_NOTE;
using scada::tag::testing::UMASK_077;

constexpr mode_t REGION_MODE = 0640;
constexpr mode_t GENERATION_MODE = 03770;
// A layout version this library does not have.
constexpr std::uint32_t OTHER_LAYOUT_VERSION = 2;
// What a test leaves in a record or a string slot to see whether it stays.
constexpr std::uint64_t KEPT_VALUE = 0x4045'0000'0000'0000U;
constexpr std::uint32_t EVEN_SEQ = 6;
constexpr std::uint32_t ODD_SEQ = 7;
constexpr mode_t NOT_STICKY_MODE = 0770;
constexpr std::uint16_t KEPT_LENGTH = 5;
// A process id no process of the test has.
constexpr pid_t OTHER_PID = 1;

using Attached = std::unique_ptr<Attachment>;

class Attach : public scada::tag::testing::AttachFixture {
protected:
    [[nodiscard]] AttachOptions options() const {
        return {
            .base_dir = base().string(), .retain_dir = {}, .tagd_uid = uid()};
    }

    [[nodiscard]] Result<Attached> attach(std::string_view process) {
        detail_.clear();
        return Attachment::attach(process, options(), detail_);
    }

    // Attaches and expects success; null otherwise.
    [[nodiscard]] Attached attached(std::string_view process) {
        Result<Attached> result = attach(process);
        EXPECT_TRUE(result.ok())
            << result.status().message() << ": " << detail_;
        return result.ok() ? std::move(result).value() : nullptr;
    }

    [[nodiscard]] const std::string& detail() const { return detail_; }

    [[nodiscard]] static TagRecord& record(std::span<std::byte> tags,
                                           std::size_t slot) {
        return shared_at<TagRecord>(
            tags, sizeof(RegionHeader) + (slot * sizeof(TagRecord)));
    }

    [[nodiscard]] static StringSlot& string_slot(std::span<std::byte> strings,
                                                 std::size_t slot) {
        return shared_at<StringSlot>(
            strings, sizeof(RegionHeader) + (slot * sizeof(StringSlot)));
    }

    [[nodiscard]] static std::uint32_t epoch_of(std::span<std::byte> changes) {
        return load_shared(
            shared_at<ChangeRingHeader>(changes, sizeof(RegionHeader)).epoch,
            std::memory_order_acquire);
    }

private:
    std::string detail_;
};

TEST_F(Attach, CreatesTheRegionsOfTheProcessAndNoOthers) {
    start_host();
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_EQ(names_in(generation_dir()),
              (std::vector<std::string>{"changes.io", "events.io", "header",
                                        "proc.io", "proc.tagd", "result.io.web",
                                        "strings.io", "tags.io"}));
    const AttachmentStatus status = feeder->status();
    EXPECT_EQ(status.generation, 1U);
    EXPECT_EQ(status.state, GenerationState::RUNNING);
    EXPECT_EQ(status.process_index, PROCESS_IO);
    EXPECT_EQ(status.regions_created, 6U);
    EXPECT_EQ(status.regions_reused, 0U);
    EXPECT_EQ(status.records_repaired, 0U);

    // A process without tags creates its `proc` region and the queue it
    // produces.
    const Attached web = attached("web");
    ASSERT_NE(web, nullptr);
    EXPECT_TRUE(web->owners().empty());
    ASSERT_EQ(web->queues().size(), 1U);
    EXPECT_EQ(web->queues()[0].queue_id, QUEUE_CMD);
    EXPECT_EQ(web->status().regions_created, 2U);
    EXPECT_TRUE(std::filesystem::exists(generation_dir() / "cmd.io.web"));
}

TEST_F(Attach, Conformance11RegionsGetMode0640UnderUmask077) {
    start_host();
    const mode_t before = ::umask(UMASK_077);
    const Attached feeder = attached("io");
    ::umask(before);
    ASSERT_NE(feeder, nullptr);
    for (const char* name :
         {"proc.io", "tags.io", "strings.io", "changes.io", "events.io"}) {
        EXPECT_EQ(mode_of(generation_dir() / name), REGION_MODE) << name;
    }
    EXPECT_EQ(mode_of(generation_dir()), GENERATION_MODE);
}

// Success if `found` is in the initial state of a record of type `type`
// (contract, section 6), apart from its value.
::testing::AssertionResult is_initial(const TagRecord& found, TagType type) {
    const bool initial = found.seq == 0 && found.type == to_raw(type) &&
                         found.quality == to_raw(Quality::BAD) &&
                         found.quality_flags == to_raw(QualityFlag::NOT_INIT) &&
                         found.source_time_ns == 0 &&
                         found.update_time_ns == 0 &&
                         found.update_mono_ns == 0 && found.change_count == 0;
    return initial ? ::testing::AssertionSuccess()
                   : ::testing::AssertionFailure()
                         << "seq " << found.seq << ", type " << int{found.type}
                         << ", quality " << int{found.quality} << ", flags "
                         << found.quality_flags;
}

TEST_F(Attach, ANewTagsRegionHoldsTheInitialStateOfEveryRecord) {
    start_host();
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    ASSERT_EQ(feeder->owners().size(), 1U);
    const OwnedOwner& owner = feeder->owners()[0];
    EXPECT_EQ(owner.owner_index, OWNER_IO);
    const std::span<std::byte> tags = owner.tags.memory.bytes();
    ASSERT_EQ(tags.size(), tags_region_size(IO_TAG_COUNT));

    EXPECT_TRUE(is_initial(record(tags, SLOT_LABEL), TagType::STRING));
    EXPECT_TRUE(is_initial(record(tags, SLOT_LEVEL), TagType::FLOAT64));
    EXPECT_TRUE(is_initial(record(tags, SLOT_NOTE), TagType::STRING));
    EXPECT_TRUE(is_initial(record(tags, SLOT_RUNNING), TagType::BOOL));
    // The value of a STRING record is its string slot.
    EXPECT_EQ(record(tags, SLOT_LABEL).value, STRING_SLOT_LABEL);
    EXPECT_EQ(record(tags, SLOT_NOTE).value, STRING_SLOT_NOTE);
    EXPECT_EQ(record(tags, SLOT_LEVEL).value, 0U);
    EXPECT_EQ(owner.strings.memory.bytes().size(), strings_region_size(2));
}

TEST_F(Attach, ANewRingAndANewQueueAreEmpty) {
    start_host();
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    ASSERT_EQ(feeder->owners().size(), 1U);
    const auto& ring = shared_at<ChangeRingHeader>(
        feeder->owners()[0].changes.memory.bytes(), sizeof(RegionHeader));
    EXPECT_EQ(ring.capacity, MIN_CHANGE_RING_CAPACITY);
    EXPECT_EQ(ring.epoch, 0U);
    EXPECT_EQ(ring.write_seq, 0U);

    ASSERT_EQ(feeder->queues().size(), 2U);
    EXPECT_EQ(feeder->queues()[0].queue_id, QUEUE_EVENTS);
    const std::span<std::byte> bytes =
        feeder->queues()[0].region.memory.bytes();
    const auto& queue = shared_at<QueueHeader>(bytes, sizeof(RegionHeader));
    EXPECT_EQ(queue.capacity, DEFAULT_QUEUE_CAPACITY);
    EXPECT_EQ(queue.record_size, sizeof(EventRecord));
    EXPECT_EQ(queue.write_seq, 0U);
    // A queue region names the process that produces it.
    EXPECT_EQ(shared_at<RegionHeader>(bytes, 0).owner_index, PROCESS_IO);
}

TEST_F(Attach, TheProcessIsRunningWithAFreshHeartbeatAndDownAfterDetach) {
    start_host();
    Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    static_cast<void>(host().check(scada::monotonic_ns()));
    EXPECT_EQ(host().is_down("io"), false);

    EXPECT_EQ(feeder->heartbeat(), SCADA_OK);
    // Conformance test 6: a process that stops cleanly reads as down at
    // once.
    feeder.reset();
    static_cast<void>(host().check(scada::monotonic_ns()));
    EXPECT_EQ(host().is_down("io"), true);
}

TEST_F(Attach, AProcessThatIsNotInTheProcessTableCannotAttach) {
    start_host();
    const Result<Attached> result = attach("stranger");
    EXPECT_EQ(result.status().code(), StatusCode::NOT_FOUND);
    EXPECT_NE(detail().find("stranger"), std::string::npos) << detail();
    EXPECT_FALSE(std::filesystem::exists(generation_dir() / "proc.stranger"));
}

TEST_F(Attach, AProcessRunningAsTheWrongUserCannotAttach) {
    start_host();
    // The header gives `guest` to another user than the one of the test.
    const Result<Attached> result = attach("guest");
    EXPECT_EQ(result.status().code(), StatusCode::PERMISSION_DENIED);
    EXPECT_NE(detail().find("not as user"), std::string::npos) << detail();
    EXPECT_FALSE(std::filesystem::exists(generation_dir() / "proc.guest"));
}

TEST_F(Attach, ANameThatIsNoProcessNameIsRefused) {
    start_host();
    for (const char* name :
         {"", "IO", "a/b", "../io", "0123456789012345678901234567890x"}) {
        EXPECT_EQ(attach(name).status().code(), StatusCode::INVALID_ARGUMENT)
            << name;
    }
}

TEST_F(Attach, Conformance10ASecondProcessOfTheSameNameIsRefused) {
    start_host();
    const Attached first = attached("io");
    ASSERT_NE(first, nullptr);
    const ino_t tags = inode_of(generation_dir() / "tags.io");
    const Result<Attached> second = attach("io");
    EXPECT_EQ(second.status().code(), StatusCode::ALREADY_EXISTS);
    // The refused one changed nothing: the first is still running on its
    // regions.
    EXPECT_EQ(inode_of(generation_dir() / "tags.io"), tags);
    static_cast<void>(host().check(scada::monotonic_ns()));
    EXPECT_EQ(host().is_down("io"), false);
}

TEST_F(Attach, Conformance09ABaseDirectoryWithTheWrongOwnerOrModeIsRefused) {
    start_host();
    AttachOptions wrong_owner = options();
    wrong_owner.tagd_uid = other_uid();
    std::string why;
    EXPECT_EQ(Attachment::attach("io", wrong_owner, why).status().code(),
              StatusCode::PERMISSION_DENIED);

    ASSERT_EQ(
        ::chmod(base().c_str(), scada::tag::testing::GROUP_WRITABLE_BASE_MODE),
        0);
    EXPECT_EQ(attach("io").status().code(), StatusCode::PERMISSION_DENIED);
    EXPECT_FALSE(std::filesystem::exists(generation_dir() / "proc.io"));
}

TEST_F(Attach, AGenerationDirectoryWithTheWrongModeIsRefused) {
    start_host();
    ASSERT_EQ(::chmod(generation_dir().c_str(), NOT_STICKY_MODE), 0);
    EXPECT_EQ(attach("io").status().code(), StatusCode::PERMISSION_DENIED);
}

TEST_F(Attach, WithoutABaseDirectoryOrAGenerationNothingIsAttached) {
    // No generation yet.
    EXPECT_EQ(attach("io").status().code(), StatusCode::UNAVAILABLE);
    AttachOptions nowhere = options();
    nowhere.base_dir = (root() / "nowhere").string();
    std::string why;
    EXPECT_EQ(Attachment::attach("io", nowhere, why).status().code(),
              StatusCode::UNAVAILABLE);
}

TEST_F(Attach, ACurrentThatIsNoLinkToAGenerationIsRefused) {
    start_host();
    drop_host();
    std::filesystem::remove(base() / "current");
    std::filesystem::create_symlink("elsewhere", base() / "current");
    EXPECT_EQ(attach("io").status().code(), StatusCode::DATA_CORRUPT);
    std::filesystem::remove(base() / "current");
    std::filesystem::create_directory(base() / "current");
    EXPECT_EQ(attach("io").status().code(), StatusCode::DATA_CORRUPT);
}

TEST_F(Attach, Conformance12AHeaderOfAnotherLayoutVersionIsRefused) {
    start_host();
    {
        const MappedRegion header = map_file("header");
        shared_at<RegionHeader>(header.bytes(), 0).layout_version =
            OTHER_LAYOUT_VERSION;
    }
    EXPECT_EQ(attach("io").status().code(), StatusCode::UNSUPPORTED);
    EXPECT_NE(detail().find("layout version 2"), std::string::npos) << detail();
    EXPECT_FALSE(std::filesystem::exists(generation_dir() / "proc.io"));
}

TEST_F(Attach, ARetiringGenerationIsNotAttachedTo) {
    start_host();
    set_generation_state(GenerationState::RETIRING);
    EXPECT_EQ(attach("io").status().code(), StatusCode::FAILED_PRECONDITION);
    set_generation_state(GenerationState::BUILDING);
    EXPECT_EQ(attach("io").status().code(), StatusCode::UNAVAILABLE);
}

TEST_F(Attach, TheHeartbeatAndTheStatusReportARetiringGeneration) {
    start_host();
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_EQ(feeder->heartbeat(), SCADA_OK);
    set_generation_state(GenerationState::RETIRING);
    EXPECT_EQ(feeder->heartbeat(), SCADA_E_RETIRING);
    EXPECT_EQ(feeder->status().state, GenerationState::RETIRING);
}

TEST_F(Attach, TheStatusFollowsTheLivenessOfTagd) {
    start_host();
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    host().heartbeat(scada::monotonic_ns());
    EXPECT_EQ(feeder->status().tagd, DownReason::NONE);
    // A clean stop of scada-tagd is seen at once; the attachment keeps
    // working.
    host().stop();
    EXPECT_EQ(feeder->status().tagd, DownReason::NOT_RUNNING);
    EXPECT_EQ(feeder->heartbeat(), SCADA_OK);
}

TEST_F(Attach, ARestartReusesTheRegionsAndKeepsTheirContent) {
    start_host();
    Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    const ino_t tags_file = inode_of(generation_dir() / "tags.io");
    const ino_t proc_file = inode_of(generation_dir() / "proc.io");
    {
        const std::span<std::byte> tags =
            feeder->owners()[0].tags.memory.bytes();
        record(tags, SLOT_LEVEL).value = KEPT_VALUE;
        record(tags, SLOT_LEVEL).quality = to_raw(Quality::GOOD);
        record(tags, SLOT_LEVEL).seq = EVEN_SEQ;
    }
    feeder.reset();

    feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_EQ(inode_of(generation_dir() / "tags.io"), tags_file);
    EXPECT_EQ(inode_of(generation_dir() / "proc.io"), proc_file);
    const AttachmentStatus status = feeder->status();
    EXPECT_EQ(status.regions_created, 0U);
    EXPECT_EQ(status.regions_reused, 6U);
    EXPECT_EQ(status.records_repaired, 0U);
    const TagRecord& level =
        record(feeder->owners()[0].tags.memory.bytes(), SLOT_LEVEL);
    EXPECT_EQ(level.value, KEPT_VALUE);
    EXPECT_EQ(level.quality, to_raw(Quality::GOOD));
    EXPECT_EQ(level.seq, EVEN_SEQ);
}

// Leaves the regions of the owner `io` as its process leaves them when it
// dies in the middle of two writes: the number `plant.level` and the string
// `plant.note` have an odd sequence number. `plant.running` and
// `plant.label` are complete, with a value, a length and quality GOOD.
void leave_mid_write(const OwnedOwner& owner) {
    const std::span<std::byte> tags = owner.tags.memory.bytes();
    const std::span<std::byte> strings = owner.strings.memory.bytes();
    const auto record_at = [&](std::size_t slot) -> TagRecord& {
        return shared_at<TagRecord>(
            tags, sizeof(RegionHeader) + (slot * sizeof(TagRecord)));
    };
    for (const std::uint32_t slot :
         {SLOT_LABEL, SLOT_LEVEL, SLOT_NOTE, SLOT_RUNNING}) {
        record_at(slot).quality = to_raw(Quality::GOOD);
        record_at(slot).quality_flags = 0;
        record_at(slot).seq = EVEN_SEQ;
    }
    record_at(SLOT_LEVEL).value = KEPT_VALUE;
    record_at(SLOT_RUNNING).value = KEPT_VALUE;
    record_at(SLOT_LEVEL).seq = ODD_SEQ;
    record_at(SLOT_NOTE).seq = ODD_SEQ;
    for (const std::size_t slot : {STRING_SLOT_LABEL, STRING_SLOT_NOTE}) {
        shared_at<StringSlot>(
            strings, sizeof(RegionHeader) + (slot * sizeof(StringSlot)))
            .len = KEPT_LENGTH;
    }
}

// Success if `found` was repaired after ODD_SEQ (contract, section 6.3).
::testing::AssertionResult is_repaired(const TagRecord& found) {
    const bool repaired = found.seq == ODD_SEQ + 1 &&
                          found.quality == to_raw(Quality::BAD) &&
                          found.quality_flags == to_raw(QualityFlag::NOT_INIT);
    return repaired ? ::testing::AssertionSuccess()
                    : ::testing::AssertionFailure()
                          << "seq " << found.seq << ", quality "
                          << int{found.quality} << ", flags "
                          << found.quality_flags;
}

// Success if `found` is as `leave_mid_write` left a complete record.
::testing::AssertionResult is_kept(const TagRecord& found) {
    const bool kept = found.seq == EVEN_SEQ &&
                      found.quality == to_raw(Quality::GOOD) &&
                      found.quality_flags == 0;
    return kept ? ::testing::AssertionSuccess()
                : ::testing::AssertionFailure()
                      << "seq " << found.seq << ", quality "
                      << int{found.quality} << ", flags "
                      << found.quality_flags;
}

TEST_F(Attach, Conformance07AnOwnerRestartRepairsItsRegion) {
    start_host();
    Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);

    // A reader that maps the regions of the owner before the restart and
    // keeps its mappings through it.
    const Attached web = attached("web");
    ASSERT_NE(web, nullptr);
    PeerRegions& peers = web->peers();
    const std::int64_t now = scada::monotonic_ns();
    peers.want(web->directory(), peers.tags_slot(OWNER_IO), now);
    peers.want(web->directory(), peers.strings_slot(OWNER_IO), now);
    const std::span<std::byte> seen_tags =
        peers.bytes(peers.tags_slot(OWNER_IO));
    const std::span<std::byte> seen_strings =
        peers.bytes(peers.strings_slot(OWNER_IO));
    ASSERT_FALSE(seen_tags.empty());
    ASSERT_FALSE(seen_strings.empty());

    leave_mid_write(feeder->owners()[0]);
    feeder.reset();
    feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_EQ(feeder->status().records_repaired, 2U);

    // The reader sees the repair through the mappings it had: it did not
    // attach again.
    EXPECT_EQ(peers.bytes(peers.tags_slot(OWNER_IO)).data(), seen_tags.data());
    EXPECT_TRUE(is_repaired(record(seen_tags, SLOT_LEVEL)));
    EXPECT_TRUE(is_repaired(record(seen_tags, SLOT_NOTE)));
    // The string slot of the repaired record is empty.
    EXPECT_EQ(string_slot(seen_strings, STRING_SLOT_NOTE).len, 0);

    // Complete records keep their last value.
    EXPECT_TRUE(is_kept(record(seen_tags, SLOT_RUNNING)));
    EXPECT_EQ(record(seen_tags, SLOT_RUNNING).value, KEPT_VALUE);
    EXPECT_TRUE(is_kept(record(seen_tags, SLOT_LABEL)));
    EXPECT_EQ(string_slot(seen_strings, STRING_SLOT_LABEL).len, KEPT_LENGTH);
}

TEST_F(Attach, Conformance07EveryRestartOfAnOwnerIncrementsTheEpochOfItsRing) {
    start_host();
    Attached feeder = attached("io");
    const Attached web = attached("web");
    ASSERT_NE(feeder, nullptr);
    ASSERT_NE(web, nullptr);
    PeerRegions& peers = web->peers();
    peers.want(web->directory(), peers.changes_slot(OWNER_IO),
               scada::monotonic_ns());
    const std::span<std::byte> seen = peers.bytes(peers.changes_slot(OWNER_IO));
    ASSERT_FALSE(seen.empty());
    EXPECT_EQ(epoch_of(seen), 0U);

    // Every consumer of the ring learns of a restart from its epoch,
    // whether a record had to be repaired or not.
    leave_mid_write(feeder->owners()[0]);
    feeder.reset();
    feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_EQ(epoch_of(seen), 1U);
    feeder.reset();
    feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_EQ(epoch_of(seen), 2U);
    EXPECT_EQ(feeder->status().records_repaired, 0U);
}

TEST_F(Attach, AnUnreadyOrWrongSizedOwnRegionIsRemovedAndCreatedAgain) {
    start_host();
    Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    feeder.reset();

    const ino_t queue_file = inode_of(generation_dir() / "events.io");
    {
        const MappedRegion tags = map_file("tags.io");
        record(tags.bytes(), SLOT_LEVEL).quality = to_raw(Quality::GOOD);
        store_shared(shared_at<RegionHeader>(tags.bytes(), 0).ready, 0U,
                     std::memory_order_release);
    }
    std::filesystem::resize_file(generation_dir() / "changes.io",
                                 sizeof(RegionHeader));

    feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    // A file that is removed gives its inode number back, so the number
    // shows which file stayed, not which one is new.
    EXPECT_EQ(inode_of(generation_dir() / "events.io"), queue_file);
    const AttachmentStatus status = feeder->status();
    EXPECT_EQ(status.regions_created, 2U);
    EXPECT_EQ(status.regions_reused, 4U);
    EXPECT_EQ(
        record(feeder->owners()[0].tags.memory.bytes(), SLOT_LEVEL).quality,
        to_raw(Quality::BAD));
    EXPECT_EQ(std::filesystem::file_size(generation_dir() / "changes.io"),
              changes_region_size(MIN_CHANGE_RING_CAPACITY));
    // A new ring starts at epoch 0: its consumers map it anew.
    EXPECT_EQ(epoch_of(feeder->owners()[0].changes.memory.bytes()), 0U);
}

TEST_F(Attach, AnOwnRegionWithOtherContentThanTheHeaderAsksForIsCreatedAgain) {
    start_host();
    Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    feeder.reset();
    {
        const MappedRegion tags = map_file("tags.io");
        record(tags.bytes(), SLOT_LEVEL).type = to_raw(TagType::BOOL);
        const MappedRegion ring = map_file("changes.io");
        shared_at<ChangeRingHeader>(ring.bytes(), sizeof(RegionHeader))
            .capacity = MIN_CHANGE_RING_CAPACITY * 2;
        const MappedRegion queue = map_file("events.io");
        shared_at<QueueHeader>(queue.bytes(), sizeof(RegionHeader))
            .record_size = sizeof(WriteResult);
    }
    feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    const AttachmentStatus status = feeder->status();
    EXPECT_EQ(status.regions_created, 3U);
    EXPECT_EQ(status.regions_reused, 3U);
    EXPECT_EQ(record(feeder->owners()[0].tags.memory.bytes(), SLOT_LEVEL).type,
              to_raw(TagType::FLOAT64));
    EXPECT_EQ(shared_at<QueueHeader>(feeder->queues()[0].region.memory.bytes(),
                                     sizeof(RegionHeader))
                  .record_size,
              sizeof(EventRecord));
}

TEST_F(Attach, Conformance09ASymbolicLinkUnderARegionNameIsNotFollowed) {
    start_host();
    const std::filesystem::path elsewhere = root() / "elsewhere";
    std::ofstream(elsewhere) << "untouched";
    std::filesystem::create_symlink(elsewhere, generation_dir() / "tags.io");

    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    // The link was the caller's own entry: removed, and the region created
    // in its place.
    EXPECT_TRUE(std::filesystem::is_regular_file(
        std::filesystem::symlink_status(generation_dir() / "tags.io")));
    EXPECT_EQ(std::filesystem::file_size(elsewhere), 9U);
}

TEST_F(Attach, Conformance10AProcessKilledWhileCreatingARegionAttachesAgain) {
    start_host();
    // What a process leaves that died between creating a region and
    // renaming it: the file under its temporary name, not ready.
    const std::string stale = temporary_name("tags.io", OTHER_PID);
    std::ofstream(generation_dir() / stale) << "half built";

    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_FALSE(std::filesystem::exists(generation_dir() / stale));
    EXPECT_EQ(feeder->status().regions_created, 6U);
}

TEST_F(Attach, ADirectoryUnderAnOwnRegionNameFailsTheAttach) {
    start_host();
    std::filesystem::create_directory(generation_dir() / "changes.io");
    EXPECT_EQ(attach("io").status().code(), StatusCode::DATA_CORRUPT);
    // The process did not come up half attached: it reads as down.
    static_cast<void>(host().check(scada::monotonic_ns()));
    EXPECT_EQ(host().is_down("io"), true);
}

TEST_F(Attach, AHeaderWithADamagedTagDirectoryFailsTheAttach) {
    start_host();
    Result<HeaderView> header = [&] {
        const scada::Fd directory =
            scada::tag::testing::open_directory(generation_dir());
        std::string why;
        return HeaderView::open(directory.get(), 1, uid(), why);
    }();
    ASSERT_TRUE(header.ok());
    {
        const MappedRegion bytes = map_file("header");
        copy_to(bytes.bytes(),
                header.value().contents().owners[OWNER_IO].slot_table_offset,
                header.value().contents().tag_count);
    }
    EXPECT_EQ(attach("io").status().code(), StatusCode::DATA_CORRUPT);
    EXPECT_FALSE(std::filesystem::exists(generation_dir() / "tags.io"));
}

// The permissions column (for example "r--s") of the line of
// /proc/self/maps whose range starts at `start`; empty if there is none.
std::string permissions_of_mapping_at(const void* start) {
    // /proc/self/maps writes the start of a range in lower-case hex.
    constexpr int HEX = 16;
    std::array<char, 2 * sizeof(std::uintptr_t)> digits{};
    const auto [end, error] =
        std::to_chars(digits.data(), digits.data() + digits.size(),
                      std::bit_cast<std::uintptr_t>(start), HEX);
    if (error != std::errc{}) {
        return {};
    }
    const std::string wanted = std::string(digits.data(), end) + "-";
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.starts_with(wanted)) {
            std::istringstream fields(line);
            std::string range;
            std::string permissions;
            fields >> range >> permissions;
            return permissions;
        }
    }
    return {};
}

TEST_F(Attach, RegionsOfOtherProcessesAreMappedReadOnly) {
    start_host();
    const Attached feeder = attached("io");
    const Attached web = attached("web");
    ASSERT_NE(feeder, nullptr);
    ASSERT_NE(web, nullptr);
    PeerRegions& peers = web->peers();
    peers.want(web->directory(), peers.tags_slot(OWNER_IO),
               scada::monotonic_ns());
    const std::span<std::byte> tags = peers.bytes(peers.tags_slot(OWNER_IO));
    ASSERT_EQ(tags.size(), tags_region_size(IO_TAG_COUNT));
    // The kernel holds the reader's mapping of the owner's region read-only
    // (reference 18.2: read-only mappings cannot write). The owner's own
    // mapping of the same file in this test process is read-write.
    EXPECT_EQ(permissions_of_mapping_at(tags.data()).substr(0, 2), "r-");
    EXPECT_EQ(permissions_of_mapping_at(
                  feeder->owners()[0].tags.memory.bytes().data())
                  .substr(0, 2),
              "rw");
    // What the owner writes, the reader sees.
    record(feeder->owners()[0].tags.memory.bytes(), SLOT_LEVEL).value =
        KEPT_VALUE;
    EXPECT_EQ(record(tags, SLOT_LEVEL).value, KEPT_VALUE);
}

TEST(TagRecords, RepairLeavesARegionWithoutOddRecordsAlone) {
    const std::array<TagType, 2> types{TagType::BOOL, TagType::STRING};
    const std::vector<std::byte> zero(
        static_cast<std::size_t>(tags_region_size(types.size())));
    scada::tag::testing::AlignedImage tags(zero);
    initialise_tag_records(tags.bytes(), types);
    const std::vector<std::byte> before(tags.bytes().begin(),
                                        tags.bytes().end());
    EXPECT_EQ(repair_tag_records(tags.bytes(), {}, types), 0U);
    EXPECT_TRUE(std::ranges::equal(tags.bytes(), before));
}

TEST(TagRecords,
     RepairOfAStringRecordWithoutAStringsRegionDoesNotReachOutside) {
    const std::array<TagType, 1> types{TagType::STRING};
    const std::vector<std::byte> zero(
        static_cast<std::size_t>(tags_region_size(types.size())));
    scada::tag::testing::AlignedImage tags(zero);
    initialise_tag_records(tags.bytes(), types);
    shared_at<TagRecord>(tags.bytes(), sizeof(RegionHeader)).seq = 1;
    EXPECT_EQ(repair_tag_records(tags.bytes(), {}, types), 1U);
    EXPECT_EQ(shared_at<TagRecord>(tags.bytes(), sizeof(RegionHeader)).seq, 2U);
}

}  // namespace
