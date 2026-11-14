#ifndef SCADA_TAG_TAG_RECORD_H
#define SCADA_TAG_TAG_RECORD_H

// The sequence lock of a tag record (docs/contracts/tag-database.md,
// sections 6.1, 6.2 and 7): the one place where fields that are not atomic
// are read while another process may write them (coding standard, section
// 9). Every access to a field of a record or of a string slot goes through
// `std::atomic_ref`, field by field, with the orders and fences of the
// contract. Private to libscada-tag.

#include "layout.h"

#include "scada/common/realtime.h"

#include <array>
#include <cstdint>
#include <span>

namespace scada::tag {

// How often a read tries before it gives up (section 6.2).
inline constexpr int READ_ATTEMPTS = 64;

// The fields of a tag record other than `seq`, as one read found them
// together. What it holds is input: the caller validates it before use.
struct RecordCopy {
    std::uint8_t type = 0;
    std::uint8_t quality = 0;
    std::uint16_t quality_flags = 0;
    std::uint64_t value = 0;
    std::int64_t source_time_ns = 0;
    std::int64_t update_time_ns = 0;
    std::int64_t update_mono_ns = 0;
    std::uint32_t change_count = 0;
};

// A string slot as one read found it. `len` is input and may be larger than
// the data.
struct StringCopy {
    std::uint16_t len = 0;
    std::array<std::uint8_t, STRING_SLOT_DATA_SIZE> data{};
};

// What a write stores in a record. The type of a record never changes.
struct RecordUpdate {
    std::uint8_t quality = 0;
    std::uint16_t quality_flags = 0;
    std::uint64_t value = 0;
    std::int64_t source_time_ns = 0;
    std::int64_t update_time_ns = 0;
    std::int64_t update_mono_ns = 0;
};

// Reads `record` with the protocol of section 6.2. True and the fields in
// `out` if one of READ_ATTEMPTS attempts found the record between two
// writes; false, with `out` unchanged, if the owner was in the middle of a
// write at every attempt. `record` may lie in a read-only mapping.
SCADA_RT [[nodiscard]] bool read_record(TagRecord& record,
                                        RecordCopy& out) noexcept;

// As `read_record`, for a STRING tag whose string slot is `slot` (section
// 7): the record and the slot are copied between the same two loads of the
// sequence number, so that a string is never paired with the quality or
// times of another write. On false `out` and `text` are unchanged.
[[nodiscard]] bool read_string_record(TagRecord& record, StringSlot& slot,
                                      RecordCopy& out,
                                      StringCopy& text) noexcept;

// Writes `update` to `record` with the protocol of section 6.1 and returns
// whether value, quality or quality flags changed, in which case
// `change_count` was incremented. Only the owner thread of the record calls
// it; `record` lies in a mapping that is writable.
SCADA_RT bool write_record(TagRecord& record,
                           const RecordUpdate& update) noexcept;

// As `write_record`, for a STRING tag whose string slot is `slot` (section
// 7): the slot is written while the sequence number is odd. `update.value`
// is the number of the slot. The value counts as changed if the text
// differs. Precondition: `text` holds at most STRING_SLOT_DATA_SIZE bytes;
// a longer one ends the process.
bool write_string_record(TagRecord& record, StringSlot& slot,
                         std::span<const std::uint8_t> text,
                         const RecordUpdate& update) noexcept;

}  // namespace scada::tag

#endif  // SCADA_TAG_TAG_RECORD_H
