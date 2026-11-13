#ifndef SCADA_TAG_HANDLE_H
#define SCADA_TAG_HANDLE_H

// Tag handles (docs/contracts/tag-database.md, section 5.1): a 32-bit
// reference to a tag, 10 bits owner index and 22 bits slot, valid for one
// generation.

#include "scada/common/realtime.h"

#include <cstdint>
#include <limits>
#include <optional>

namespace scada::tag {

// The handle that refers to no tag.
inline constexpr std::uint32_t INVALID_HANDLE = 0xFFFF'FFFFU;

inline constexpr std::uint32_t HANDLE_OWNER_BITS = 10;
inline constexpr std::uint32_t HANDLE_SLOT_BITS = 22;

// Limits of layout version 1 (section 14). Each is one less than its bit
// field could count, so that no tag can have the bit pattern of
// INVALID_HANDLE.
inline constexpr std::uint32_t MAX_OWNERS = 1023;
inline constexpr std::uint32_t MAX_TAGS_PER_OWNER = 4'194'303;

static_assert(HANDLE_OWNER_BITS + HANDLE_SLOT_BITS ==
              std::numeric_limits<std::uint32_t>::digits);
static_assert(MAX_OWNERS == (1U << HANDLE_OWNER_BITS) - 1);
static_assert(MAX_TAGS_PER_OWNER == (1U << HANDLE_SLOT_BITS) - 1);
static_assert(((MAX_OWNERS << HANDLE_SLOT_BITS) | MAX_TAGS_PER_OWNER) ==
              INVALID_HANDLE);

// The two parts of a handle.
struct HandleParts {
    std::uint16_t owner_index = 0;  // index into the owner table
    std::uint32_t slot = 0;         // record number in tags.<owner>

    [[nodiscard]] friend constexpr bool operator==(
        const HandleParts&, const HandleParts&) noexcept = default;
};

// Returns the handle of slot `slot` of owner `owner_index`, or nothing if
// `owner_index` is not below MAX_OWNERS or `slot` is not below
// MAX_TAGS_PER_OWNER. It does not know the generation: whether that owner and
// slot exist is checked against the header region by the caller.
SCADA_RT [[nodiscard]] constexpr std::optional<std::uint32_t> pack_handle(
    std::uint32_t owner_index, std::uint32_t slot) noexcept {
    if (owner_index >= MAX_OWNERS || slot >= MAX_TAGS_PER_OWNER) {
        return std::nullopt;
    }
    return (owner_index << HANDLE_SLOT_BITS) | slot;
}

// Splits `handle`. Returns nothing for INVALID_HANDLE and for any other
// handle whose owner index or slot is beyond the limits above, so a result
// can always be packed again. The caller still checks both parts against the
// counts of the generation before use.
SCADA_RT [[nodiscard]] constexpr std::optional<HandleParts> unpack_handle(
    std::uint32_t handle) noexcept {
    const std::uint32_t owner_index = handle >> HANDLE_SLOT_BITS;
    const std::uint32_t slot = handle & MAX_TAGS_PER_OWNER;
    if (owner_index >= MAX_OWNERS || slot >= MAX_TAGS_PER_OWNER) {
        return std::nullopt;
    }
    // owner_index is below 1023, so it fits 16 bits.
    return HandleParts{static_cast<std::uint16_t>(owner_index), slot};
}

}  // namespace scada::tag

#endif  // SCADA_TAG_HANDLE_H
