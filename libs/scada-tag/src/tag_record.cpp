#include "tag_record.h"

#include "layout.h"
#include "shared_memory.h"

#include "scada/common/cpu_relax.h"
#include "scada/common/panic.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::tag {

namespace {

// Every order below is the one the contract writes (sections 6.1 and 6.2).
// Relaxed is enough for the fields other than `seq`: a reader uses what it
// copied only after two equal loads of `seq` around the copy, and the
// fences keep the copies between them.
constexpr std::memory_order RELAXED = std::memory_order_relaxed;

// The fields of `record` other than `seq`, each loaded once.
SCADA_RT RecordCopy copy_fields(TagRecord& record) noexcept {
    RecordCopy copy;
    copy.type = load_shared(record.type, RELAXED);
    copy.quality = load_shared(record.quality, RELAXED);
    copy.quality_flags = load_shared(record.quality_flags, RELAXED);
    copy.value = load_shared(record.value, RELAXED);
    copy.source_time_ns = load_shared(record.source_time_ns, RELAXED);
    copy.update_time_ns = load_shared(record.update_time_ns, RELAXED);
    copy.update_mono_ns = load_shared(record.update_mono_ns, RELAXED);
    copy.change_count = load_shared(record.change_count, RELAXED);
    return copy;
}

// True if `update` changes value, quality or quality flags of `record`.
// Called by the owner thread, the only writer.
SCADA_RT bool changes_record(TagRecord& record,
                             const RecordUpdate& update) noexcept {
    return load_shared(record.value, RELAXED) != update.value ||
           load_shared(record.quality, RELAXED) != update.quality ||
           load_shared(record.quality_flags, RELAXED) != update.quality_flags;
}

// The stores of section 6.1 between the two stores of `seq`.
SCADA_RT void store_fields(TagRecord& record, const RecordUpdate& update,
                           bool changed) noexcept {
    store_shared(record.quality, update.quality, RELAXED);
    store_shared(record.quality_flags, update.quality_flags, RELAXED);
    store_shared(record.value, update.value, RELAXED);
    store_shared(record.source_time_ns, update.source_time_ns, RELAXED);
    store_shared(record.update_time_ns, update.update_time_ns, RELAXED);
    store_shared(record.update_mono_ns, update.update_mono_ns, RELAXED);
    if (changed) {
        // Wraps modulo 2^32; a reader only compares two counts.
        store_shared(record.change_count,
                     load_shared(record.change_count, RELAXED) + 1, RELAXED);
    }
}

// Makes `seq` odd and returns the value it had (section 6.1).
SCADA_RT std::uint32_t begin_write(TagRecord& record) noexcept {
    const std::uint32_t seq = load_shared(record.seq, RELAXED);
    // Wraps modulo 2^32; readers only test the low bit and equality.
    store_shared(record.seq, seq + 1, RELAXED);
    // On ARM64 a store does not order the stores that follow it.
    fence_release();
    return seq;
}

// Makes `seq` even again. Release: publishes the stores since
// `begin_write`.
SCADA_RT void end_write(TagRecord& record, std::uint32_t seq) noexcept {
    store_shared(record.seq, seq + 2, std::memory_order_release);
}

// One attempt of section 6.2 after `first_seq` was loaded and found even:
// `copy` runs between the two loads of the sequence number. True if both
// loads gave the same number.
template <typename Copy>
SCADA_RT bool unchanged_during(TagRecord& record, std::uint32_t first_seq,
                               const Copy& copy) noexcept {
    copy();
    // The copies must complete before the second load.
    fence_acquire();
    return load_shared(record.seq, RELAXED) == first_seq;
}

}  // namespace

bool read_record(TagRecord& record, RecordCopy& out) noexcept {
    for (int attempt = 0; attempt < READ_ATTEMPTS; ++attempt) {
        // Acquire: pairs with the release store that ends a write.
        const std::uint32_t seq =
            load_shared(record.seq, std::memory_order_acquire);
        if (seq % 2 == 1) {
            cpu_relax();
            continue;
        }
        RecordCopy copy;
        if (unchanged_during(record, seq,
                             [&]() noexcept { copy = copy_fields(record); })) {
            out = copy;
            return true;
        }
    }
    return false;
}

bool read_string_record(TagRecord& record, StringSlot& slot, RecordCopy& out,
                        StringCopy& text) noexcept {
    for (int attempt = 0; attempt < READ_ATTEMPTS; ++attempt) {
        // Acquire: pairs with the release store that ends a write.
        const std::uint32_t seq =
            load_shared(record.seq, std::memory_order_acquire);
        if (seq % 2 == 1) {
            cpu_relax();
            continue;
        }
        RecordCopy copy;
        StringCopy string;
        const auto copy_both = [&]() noexcept {
            copy = copy_fields(record);
            string.len = load_shared(slot.len, RELAXED);
            // All of the data, whatever `len` says: `len` is checked only
            // once the copy is known to be whole.
            std::ranges::transform(slot.data, string.data.begin(),
                                   [](std::uint8_t& byte) noexcept {
                                       return load_shared(byte, RELAXED);
                                   });
        };
        if (unchanged_during(record, seq, copy_both)) {
            out = copy;
            text = string;
            return true;
        }
    }
    return false;
}

bool write_record(TagRecord& record, const RecordUpdate& update) noexcept {
    const bool changed = changes_record(record, update);
    const std::uint32_t seq = begin_write(record);
    store_fields(record, update, changed);
    end_write(record, seq);
    return changed;
}

bool write_string_record(TagRecord& record, StringSlot& slot,
                         std::span<const std::uint8_t> text,
                         const RecordUpdate& update) noexcept {
    if (text.size() > slot.data.size()) {
        panic("tag record: string longer than a string slot");
    }
    // The part of the slot the text takes.
    const std::span<std::uint8_t> stored =
        std::span<std::uint8_t>(slot.data).first(text.size());
    // The owner thread is the only writer of its slot.
    bool changed = changes_record(record, update) ||
                   load_shared(slot.len, RELAXED) != text.size();
    for (std::size_t i = 0; !changed && i < text.size(); ++i) {
        changed = load_shared(stored[i], RELAXED) != text[i];
    }

    const std::uint32_t seq = begin_write(record);
    // In range: at most STRING_SLOT_DATA_SIZE, checked above.
    store_shared(slot.len, static_cast<std::uint16_t>(text.size()), RELAXED);
    for (std::size_t i = 0; i < text.size(); ++i) {
        store_shared(stored[i], text[i], RELAXED);
    }
    store_fields(record, update, changed);
    end_write(record, seq);
    return changed;
}

}  // namespace scada::tag
