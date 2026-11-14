// Unit tests for the enumerations of the tag database: their values are part
// of the contract (sections 4 to 10), and an integer read from shared memory
// becomes an enumerator only if it is one.

#include "scada/tag/types.h"

#include "layout.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

namespace {

using namespace scada::tag;

// Usable in constant expressions.
static_assert(to_raw(TagType::BOOL) == 1);
static_assert(tag_type_from_raw(1) == TagType::BOOL);

// The values are fixed by the contract; the masks hold exactly the defined
// bits; a queue's kind in the queue table equals the kind of its region.
TEST(Types, ValuesMatchTheContract) {
    EXPECT_EQ(to_raw(TagType::BOOL), 1);
    EXPECT_EQ(to_raw(TagType::INT64), 2);
    EXPECT_EQ(to_raw(TagType::UINT64), 3);
    EXPECT_EQ(to_raw(TagType::FLOAT64), 4);
    EXPECT_EQ(to_raw(TagType::STRING), 5);
    EXPECT_EQ(to_raw(Quality::GOOD), 0);
    EXPECT_EQ(to_raw(Quality::UNCERTAIN), 1);
    EXPECT_EQ(to_raw(Quality::BAD), 2);
    EXPECT_EQ(to_raw(QualityFlag::STALE), 0x001);
    EXPECT_EQ(to_raw(QualityFlag::COMM_FAIL), 0x002);
    EXPECT_EQ(to_raw(QualityFlag::OUT_OF_RANGE), 0x004);
    EXPECT_EQ(to_raw(QualityFlag::SUBSTITUTED), 0x008);
    EXPECT_EQ(to_raw(QualityFlag::NOT_INIT), 0x010);
    EXPECT_EQ(to_raw(QualityFlag::OWNER_DOWN), 0x020);
    EXPECT_EQ(to_raw(QualityFlag::TIME_UNCERTAIN), 0x040);
    EXPECT_EQ(to_raw(QualityFlag::BLOCKED), 0x080);
    EXPECT_EQ(to_raw(QualityFlag::TEST), 0x100);
    EXPECT_EQ(to_raw(QualityFlag::OVERFLOW), 0x200);
    EXPECT_EQ(to_raw(QualityFlag::RESTORED), 0x400);
    EXPECT_EQ(to_raw(QualityFlag::LOCAL_TIME), 0x800);
    EXPECT_EQ(to_raw(WriteOperation::DIRECT), 1);
    EXPECT_EQ(to_raw(WriteOperation::SELECT), 2);
    EXPECT_EQ(to_raw(WriteOperation::OPERATE), 3);
    EXPECT_EQ(to_raw(WriteOperation::CANCEL), 4);
    EXPECT_EQ(to_raw(WriteOperation::READ_NOW), 5);
    EXPECT_EQ(to_raw(WriteSource::AUTO), 1);
    EXPECT_EQ(to_raw(WriteSource::OPERATOR), 2);
    EXPECT_EQ(to_raw(WriteSource::REMOTE), 3);
    EXPECT_EQ(to_raw(WriteStatus::OK), 1);
    EXPECT_EQ(to_raw(WriteStatus::REJECTED), 2);
    EXPECT_EQ(to_raw(WriteStatus::TIMEOUT), 3);
    EXPECT_EQ(to_raw(WriteStatus::DEVICE_ERROR), 4);
    EXPECT_EQ(to_raw(WriteStatus::NOT_AUTHORISED), 5);
    EXPECT_EQ(to_raw(WriteStatus::EXPIRED), 6);
    EXPECT_EQ(to_raw(WriteStatus::ABORTED), 7);
    EXPECT_EQ(to_raw(RegionKind::HEADER), 1);
    EXPECT_EQ(to_raw(RegionKind::PROC), 2);
    EXPECT_EQ(to_raw(RegionKind::TAGS), 3);
    EXPECT_EQ(to_raw(RegionKind::STRINGS), 4);
    EXPECT_EQ(to_raw(RegionKind::CHANGES), 5);
    EXPECT_EQ(to_raw(RegionKind::EVENTS), 6);
    EXPECT_EQ(to_raw(RegionKind::CMD), 7);
    EXPECT_EQ(to_raw(RegionKind::RESULT), 8);
    EXPECT_EQ(to_raw(GenerationState::BUILDING), 1);
    EXPECT_EQ(to_raw(GenerationState::RUNNING), 2);
    EXPECT_EQ(to_raw(GenerationState::RETIRING), 3);
    EXPECT_EQ(to_raw(ProcessState::STARTING), 1);
    EXPECT_EQ(to_raw(ProcessState::RUNNING), 2);
    EXPECT_EQ(to_raw(ProcessState::STOPPING), 3);
    EXPECT_EQ(to_raw(QueueKind::EVENTS), 6);
    EXPECT_EQ(to_raw(QueueKind::CMD), 7);
    EXPECT_EQ(to_raw(QueueKind::RESULT), 8);
    EXPECT_EQ(to_raw(ProcessFlag::REALTIME), 0x1);
    EXPECT_EQ(to_raw(TagFlag::RETAIN), 0x1);
    EXPECT_EQ(to_raw(TagFlag::WRITABLE_BY_COMMAND), 0x2);
    EXPECT_EQ(to_raw(TagFlag::RETAIN_NOW), 0x4);
    EXPECT_EQ(to_raw(QueueKind::EVENTS), to_raw(RegionKind::EVENTS));
    EXPECT_EQ(to_raw(QueueKind::CMD), to_raw(RegionKind::CMD));
    EXPECT_EQ(to_raw(QueueKind::RESULT), to_raw(RegionKind::RESULT));
    EXPECT_EQ(QUALITY_FLAGS_MASK, 0x0FFF);
    EXPECT_EQ(PROCESS_FLAGS_MASK, to_raw(ProcessFlag::REALTIME));
    EXPECT_EQ(TAG_FLAGS_MASK,
              (to_raw(TagFlag::RETAIN) | to_raw(TagFlag::WRITABLE_BY_COMMAND) |
               to_raw(TagFlag::RETAIN_NOW)));
}

TEST(Types, TagTypeFromRawAcceptsOnlyEnumerators) {
    EXPECT_EQ(tag_type_from_raw(0), std::nullopt);
    EXPECT_EQ(tag_type_from_raw(1), TagType::BOOL);
    EXPECT_EQ(tag_type_from_raw(5), TagType::STRING);
    EXPECT_EQ(tag_type_from_raw(6), std::nullopt);
    EXPECT_EQ(tag_type_from_raw(255), std::nullopt);
}

TEST(Types, QualityFromRawAcceptsOnlyEnumerators) {
    EXPECT_EQ(quality_from_raw(0), Quality::GOOD);
    EXPECT_EQ(quality_from_raw(2), Quality::BAD);
    EXPECT_EQ(quality_from_raw(3), std::nullopt);
    EXPECT_EQ(quality_from_raw(255), std::nullopt);
}

TEST(Types, WriteEnumerationsFromRawAcceptOnlyEnumerators) {
    EXPECT_EQ(write_operation_from_raw(0), std::nullopt);
    EXPECT_EQ(write_operation_from_raw(1), WriteOperation::DIRECT);
    EXPECT_EQ(write_operation_from_raw(5), WriteOperation::READ_NOW);
    EXPECT_EQ(write_operation_from_raw(6), std::nullopt);

    EXPECT_EQ(write_source_from_raw(0), std::nullopt);
    EXPECT_EQ(write_source_from_raw(1), WriteSource::AUTO);
    EXPECT_EQ(write_source_from_raw(3), WriteSource::REMOTE);
    EXPECT_EQ(write_source_from_raw(4), std::nullopt);

    EXPECT_EQ(write_status_from_raw(0), std::nullopt);
    EXPECT_EQ(write_status_from_raw(1), WriteStatus::OK);
    EXPECT_EQ(write_status_from_raw(7), WriteStatus::ABORTED);
    EXPECT_EQ(write_status_from_raw(8), std::nullopt);
}

TEST(Types, LayoutEnumerationsFromRawAcceptOnlyEnumerators) {
    EXPECT_EQ(region_kind_from_raw(0), std::nullopt);
    EXPECT_EQ(region_kind_from_raw(1), RegionKind::HEADER);
    EXPECT_EQ(region_kind_from_raw(8), RegionKind::RESULT);
    EXPECT_EQ(region_kind_from_raw(9), std::nullopt);
    EXPECT_EQ(region_kind_from_raw(0xFFFF'FFFFU), std::nullopt);

    EXPECT_EQ(generation_state_from_raw(0), std::nullopt);
    EXPECT_EQ(generation_state_from_raw(2), GenerationState::RUNNING);
    EXPECT_EQ(generation_state_from_raw(4), std::nullopt);

    EXPECT_EQ(process_state_from_raw(0), std::nullopt);
    EXPECT_EQ(process_state_from_raw(3), ProcessState::STOPPING);
    EXPECT_EQ(process_state_from_raw(4), std::nullopt);

    EXPECT_EQ(queue_kind_from_raw(5), std::nullopt);
    EXPECT_EQ(queue_kind_from_raw(6), QueueKind::EVENTS);
    EXPECT_EQ(queue_kind_from_raw(8), QueueKind::RESULT);
    EXPECT_EQ(queue_kind_from_raw(9), std::nullopt);
}

TEST(Types, QualityFlagsValidRejectsUndefinedBits) {
    EXPECT_TRUE(quality_flags_valid(0));
    EXPECT_TRUE(quality_flags_valid(0x0FFF));
    EXPECT_FALSE(quality_flags_valid(0x1000));
    EXPECT_FALSE(quality_flags_valid(0x8001));
}

TEST(Types, HasFlagTestsOneBit) {
    // The operands are promoted to int; both are 16 bits wide.
    const auto flags = static_cast<std::uint16_t>(
        to_raw(QualityFlag::NOT_INIT) | to_raw(QualityFlag::OWNER_DOWN));
    EXPECT_TRUE(has_flag(flags, QualityFlag::NOT_INIT));
    EXPECT_TRUE(has_flag(flags, QualityFlag::OWNER_DOWN));
    EXPECT_FALSE(has_flag(flags, QualityFlag::STALE));
    EXPECT_FALSE(has_flag(0, QualityFlag::LOCAL_TIME));
}

}  // namespace
