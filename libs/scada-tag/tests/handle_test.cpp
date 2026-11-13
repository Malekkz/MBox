// Unit tests for tag handles (contract, section 5.1).

#include "scada/tag/handle.h"

#include <gtest/gtest.h>

#include <optional>

namespace {

using scada::tag::HandleParts;
using scada::tag::INVALID_HANDLE;
using scada::tag::MAX_OWNERS;
using scada::tag::MAX_TAGS_PER_OWNER;
using scada::tag::pack_handle;
using scada::tag::unpack_handle;

// Usable in constant expressions.
static_assert(pack_handle(1, 1).has_value());
static_assert(unpack_handle(1).has_value());

// The formula of the contract: handle = (owner_index << 22) | slot.
TEST(Handle, PacksOwnerInTheTopTenBitsAndSlotBelow) {
    EXPECT_EQ(pack_handle(0, 0), 0U);
    EXPECT_EQ(pack_handle(0, 1), 1U);
    EXPECT_EQ(pack_handle(1, 0), 0x0040'0000U);
    EXPECT_EQ(pack_handle(3, 5), 0x00C0'0005U);
    EXPECT_EQ(pack_handle(5, 1234), (5U << 22U) | 1234U);
}

TEST(Handle, UnpacksWhatWasPacked) {
    const auto handle = pack_handle(700, 3'000'000).value_or(INVALID_HANDLE);
    EXPECT_EQ(unpack_handle(handle), (HandleParts{700, 3'000'000}));
}

TEST(Handle, AcceptsTheLargestOwnerAndSlot) {
    const auto handle = pack_handle(MAX_OWNERS - 1, MAX_TAGS_PER_OWNER - 1);
    EXPECT_EQ(handle, 0xFFBF'FFFEU);
    EXPECT_EQ(unpack_handle(0xFFBF'FFFEU), (HandleParts{1022, 4'194'302}));
}

TEST(Handle, RejectsAnOwnerOrSlotBeyondTheLimits) {
    EXPECT_EQ(pack_handle(MAX_OWNERS, 0), std::nullopt);
    EXPECT_EQ(pack_handle(0, MAX_TAGS_PER_OWNER), std::nullopt);
    // Values that would spill into the other field if they were not checked.
    EXPECT_EQ(pack_handle(0, 1U << 22U), std::nullopt);
    EXPECT_EQ(pack_handle(1U << 10U, 0), std::nullopt);
    EXPECT_EQ(pack_handle(0xFFFF'FFFFU, 0xFFFF'FFFFU), std::nullopt);
}

TEST(Handle, NothingPacksToTheInvalidHandle) {
    EXPECT_EQ(pack_handle(MAX_OWNERS, MAX_TAGS_PER_OWNER), std::nullopt);
}

TEST(Handle, RejectsTheInvalidHandleAndItsNeighbours) {
    EXPECT_EQ(unpack_handle(INVALID_HANDLE), std::nullopt);
    // Owner index 1023 with a valid slot.
    EXPECT_EQ(unpack_handle(0xFFC0'0000U), std::nullopt);
    // Slot 4,194,303 with a valid owner.
    EXPECT_EQ(unpack_handle(0x003F'FFFFU), std::nullopt);
}

TEST(Handle, UnpacksZero) {
    EXPECT_EQ(unpack_handle(0), (HandleParts{0, 0}));
}

}  // namespace
