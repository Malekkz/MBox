#ifndef SCADA_TAG_CHANGE_RING_H
#define SCADA_TAG_CHANGE_RING_H

// The change ring of one owner (docs/contracts/tag-database.md, section 8):
// the write of the owner thread and the read of a consumer, with the orders
// and fences of the contract. Every access to a field of the ring header or
// of an entry goes through `std::atomic_ref`, field by field, as in the
// sequence lock (decision D-29). Private to libscada-tag.

#include "layout.h"

#include "scada/common/realtime.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::tag {

// Where the entries of a `changes` region start.
[[nodiscard]] constexpr std::size_t change_ring_entries_offset() noexcept {
    return sizeof(RegionHeader) + sizeof(ChangeRingHeader);
}

// The ring header of the mapped `changes` region `region`, which is at
// least as large as a region of the header's capacity.
SCADA_RT [[nodiscard]] ChangeRingHeader& ring_header(
    std::span<std::byte> region) noexcept;

// Appends `entry` to the ring in `region`, a `changes` region of `capacity`
// entries (a power of two) mapped read-write, with the protocol of section
// 8.1. Only the owner thread calls it.
SCADA_RT void append_change(std::span<std::byte> region, std::uint32_t capacity,
                            const ChangeEntry& entry) noexcept;

// What one read of a ring found.
enum class RingRead : std::uint8_t {
    NOTHING,  // the cursor is at `write_seq`
    ENTRY,    // an entry was copied and the cursor advanced by one
    GAP,      // the cursor was overrun (section 8.3)
};

// Reads the entry at `cursor` of the ring in `region`, a `changes` region
// of `capacity` entries that may be mapped read-only, with the protocol of
// section 8.2. On ENTRY `out` holds the copy, which the caller still
// validates, and `cursor` is one further. On GAP `cursor` and `out` are
// unchanged, and `write_seq` holds the position the ring had reached. On
// NOTHING `write_seq` holds the cursor.
SCADA_RT [[nodiscard]] RingRead read_change(std::span<std::byte> region,
                                            std::uint32_t capacity,
                                            std::uint64_t& cursor,
                                            ChangeEntry& out,
                                            std::uint64_t& write_seq) noexcept;

// `write_seq` and `epoch` of the ring in `region`, loaded with acquire.
SCADA_RT [[nodiscard]] std::uint64_t ring_write_seq(
    std::span<std::byte> region) noexcept;
SCADA_RT [[nodiscard]] std::uint32_t ring_epoch(
    std::span<std::byte> region) noexcept;

}  // namespace scada::tag

#endif  // SCADA_TAG_CHANGE_RING_H
