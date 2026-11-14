#include "change_ring.h"

#include "layout.h"
#include "shared_memory.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::tag {

namespace {

// Relaxed is enough for the fields of an entry: a consumer uses what it
// copied only after it has seen, after the copy, that `write_seq` did not
// pass the entry (section 8.2), and the fences keep the copies on the
// right side of the loads of `write_seq`.
constexpr std::memory_order RELAXED = std::memory_order_relaxed;

SCADA_RT std::size_t entry_offset(std::uint64_t position,
                                  std::uint32_t capacity) noexcept {
    // In range: below `capacity`, which the region holds.
    const auto index =
        static_cast<std::size_t>(position & (std::uint64_t{capacity} - 1));
    return change_ring_entries_offset() + (index * sizeof(ChangeEntry));
}

SCADA_RT ChangeEntry& entry_at(std::span<std::byte> region,
                               std::uint64_t position,
                               std::uint32_t capacity) noexcept {
    return shared_at<ChangeEntry>(region, entry_offset(position, capacity));
}

SCADA_RT void store_entry(ChangeEntry& target,
                          const ChangeEntry& entry) noexcept {
    store_shared(target.update_mono_ns, entry.update_mono_ns, RELAXED);
    store_shared(target.handle, entry.handle, RELAXED);
    store_shared(target.type, entry.type, RELAXED);
    store_shared(target.quality, entry.quality, RELAXED);
    store_shared(target.quality_flags, entry.quality_flags, RELAXED);
    store_shared(target.value, entry.value, RELAXED);
    store_shared(target.source_time_ns, entry.source_time_ns, RELAXED);
}

SCADA_RT ChangeEntry load_entry(ChangeEntry& source) noexcept {
    ChangeEntry copy;
    copy.update_mono_ns = load_shared(source.update_mono_ns, RELAXED);
    copy.handle = load_shared(source.handle, RELAXED);
    copy.type = load_shared(source.type, RELAXED);
    copy.quality = load_shared(source.quality, RELAXED);
    copy.quality_flags = load_shared(source.quality_flags, RELAXED);
    copy.value = load_shared(source.value, RELAXED);
    copy.source_time_ns = load_shared(source.source_time_ns, RELAXED);
    return copy;
}

// True if the entry at `cursor` may have been overwritten once `write_seq`
// reached `written`. Unsigned: a `write_seq` below the cursor, which only a
// damaged region holds, gives a large difference and counts as overrun.
SCADA_RT bool overrun(std::uint64_t written, std::uint64_t cursor,
                      std::uint32_t capacity) noexcept {
    return written - cursor >= capacity;
}

}  // namespace

ChangeRingHeader& ring_header(std::span<std::byte> region) noexcept {
    return shared_at<ChangeRingHeader>(region, sizeof(RegionHeader));
}

void append_change(std::span<std::byte> region, std::uint32_t capacity,
                   const ChangeEntry& entry) noexcept {
    ChangeRingHeader& header = ring_header(region);
    // Relaxed: the owner thread is the only writer of `write_seq`.
    const std::uint64_t written = load_shared(header.write_seq, RELAXED);
    // The entry stores must not pass the store of the previous `write_seq`
    // (section 8.1): a consumer that saw it may be copying this slot's
    // previous entry, and checks `write_seq` after the copy.
    fence_release();
    store_entry(entry_at(region, written, capacity), entry);
    // Release: publishes the entry.
    store_shared(header.write_seq, written + 1, std::memory_order_release);
}

RingRead read_change(std::span<std::byte> region, std::uint32_t capacity,
                     std::uint64_t& cursor, ChangeEntry& out,
                     std::uint64_t& write_seq) noexcept {
    ChangeRingHeader& header = ring_header(region);
    // Acquire: pairs with the release store of `append_change`.
    const std::uint64_t first =
        load_shared(header.write_seq, std::memory_order_acquire);
    write_seq = first;
    if (first == cursor) {
        return RingRead::NOTHING;
    }
    if (overrun(first, cursor, capacity)) {
        return RingRead::GAP;
    }
    const ChangeEntry copy = load_entry(entry_at(region, cursor, capacity));
    // The copy must complete before the second load.
    fence_acquire();
    const std::uint64_t second = load_shared(header.write_seq, RELAXED);
    if (overrun(second, cursor, capacity)) {
        write_seq = second;
        return RingRead::GAP;  // the slot may have been written meanwhile
    }
    out = copy;
    ++cursor;
    return RingRead::ENTRY;
}

std::uint64_t ring_write_seq(std::span<std::byte> region) noexcept {
    return load_shared(ring_header(region).write_seq,
                       std::memory_order_acquire);
}

std::uint32_t ring_epoch(std::span<std::byte> region) noexcept {
    return load_shared(ring_header(region).epoch, std::memory_order_acquire);
}

}  // namespace scada::tag
