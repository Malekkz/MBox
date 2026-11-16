#include "queue_region.h"

#include "layout.h"
#include "shared_memory.h"

#include "scada/tag/types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace scada::tag {

namespace {

// Relaxed is enough for the words of a record: the producer stores them
// before its release store of `write_seq`, and the consumer loads them
// after its acquire load of it; the consumer's release store of its cursor
// and the producer's acquire load of it order the reuse of a slot.
constexpr std::memory_order RELAXED = std::memory_order_relaxed;

constexpr std::size_t WORD = sizeof(std::uint64_t);

SCADA_RT std::size_t record_offset(std::uint64_t position,
                                   std::uint32_t capacity,
                                   std::uint32_t record_size) noexcept {
    // In range: below `capacity`, which the region holds.
    const auto index =
        static_cast<std::size_t>(position & (std::uint64_t{capacity} - 1));
    return sizeof(RegionHeader) + sizeof(QueueHeader) + (index * record_size);
}

using RecordBytes = std::array<std::byte, MAX_RECORD_SIZE>;

template <typename Record>
SCADA_RT Record record_from(const RecordWords& words) noexcept {
    const auto bytes = std::bit_cast<RecordBytes>(words.words);
    return copy_from<Record>(bytes, 0);
}

template <typename Record>
SCADA_RT RecordWords words_from(const Record& record) noexcept {
    RecordBytes bytes{};
    copy_to(bytes, 0, record);
    return RecordWords{
        std::bit_cast<std::array<std::uint64_t, MAX_RECORD_WORDS>>(bytes)};
}

template <typename Array>
SCADA_RT bool all_zero(const Array& field) noexcept {
    return std::ranges::all_of(field, [](auto byte) { return byte == 0; });
}

}  // namespace

QueueHeader& queue_header(std::span<std::byte> region) noexcept {
    return shared_at<QueueHeader>(region, sizeof(RegionHeader));
}

std::uint64_t queue_write_seq(std::span<std::byte> region) noexcept {
    return load_shared(queue_header(region).write_seq,
                       std::memory_order_acquire);
}

std::uint64_t* cursor_field(std::span<std::byte> region,
                            std::size_t queue_id) noexcept {
    const std::size_t first = sizeof(RegionHeader) + sizeof(ProcHeader);
    if (queue_id > (region.size() / WORD) ||
        !fits(region.size(), first + (queue_id * WORD), WORD)) {
        return nullptr;
    }
    return &shared_at<std::uint64_t>(region, first + (queue_id * WORD));
}

void load_record(std::span<std::byte> region, std::uint32_t capacity,
                 std::uint32_t record_size, std::uint64_t position,
                 RecordWords& out) noexcept {
    std::size_t offset = record_offset(position, capacity, record_size);
    for (std::uint64_t& word : std::span(out.words).first(
             std::min<std::size_t>(record_size / WORD, MAX_RECORD_WORDS))) {
        word = load_shared(shared_at<std::uint64_t>(region, offset), RELAXED);
        offset += WORD;
    }
}

void store_record(std::span<std::byte> region, std::uint32_t capacity,
                  std::uint32_t record_size, std::uint64_t position,
                  const RecordWords& record) noexcept {
    std::size_t offset = record_offset(position, capacity, record_size);
    for (const std::uint64_t word :
         std::span(record.words)
             .first(
                 std::min<std::size_t>(record_size / WORD, MAX_RECORD_WORDS))) {
        store_shared(shared_at<std::uint64_t>(region, offset), word, RELAXED);
        offset += WORD;
    }
}

EventRecord event_of(const RecordWords& words) noexcept {
    return record_from<EventRecord>(words);
}
WriteRequest request_of(const RecordWords& words) noexcept {
    return record_from<WriteRequest>(words);
}
WriteResult result_of(const RecordWords& words) noexcept {
    return record_from<WriteResult>(words);
}
RecordWords words_of(const EventRecord& record) noexcept {
    return words_from(record);
}
RecordWords words_of(const WriteRequest& record) noexcept {
    return words_from(record);
}
RecordWords words_of(const WriteResult& record) noexcept {
    return words_from(record);
}

bool event_valid(const EventRecord& record) noexcept {
    return enum_from_raw(record.kind, EventKind::ALARM, EventKind::SYSTEM)
               .has_value() &&
           record.payload_len <= EVENT_PAYLOAD_SIZE;
}

bool request_valid(const WriteRequest& record) noexcept {
    const std::optional<WriteOperation> operation =
        write_operation_from_raw(record.operation);
    const std::optional<WriteSource> source =
        write_source_from_raw(record.source);
    if (!operation.has_value() || !source.has_value() ||
        record.reserved0 != 0 || record.reserved1 != 0 ||
        !all_zero(record.reserved)) {
        return false;
    }
    const bool carries_value = *operation != WriteOperation::CANCEL &&
                               *operation != WriteOperation::READ_NOW;
    if (record.value_type == 0) {
        if (carries_value || record.value != 0) {
            return false;
        }
    } else {
        const std::optional<TagType> type =
            tag_type_from_raw(record.value_type);
        if (!type.has_value() || *type == TagType::STRING ||
            (*type == TagType::BOOL && record.value > 1)) {
            return false;
        }
    }
    // An operator command names its user (reference 8.2, step 1, and 13.4);
    // automatic and remote ones have none. 0 means no user (decision D-44).
    if ((*source == WriteSource::OPERATOR) != (record.user_id != 0)) {
        return false;
    }
    const bool needs_select = *operation == WriteOperation::OPERATE ||
                              *operation == WriteOperation::CANCEL;
    return needs_select == (record.select_request_id != 0);
}

bool result_valid(const WriteResult& record) noexcept {
    return write_status_from_raw(record.status).has_value() &&
           all_zero(record.reserved0) && all_zero(record.reserved);
}

bool request_expired(const WriteRequest& record,
                     std::int64_t now_mono_ns) noexcept {
    const std::int64_t expiry = record.expiry_mono_ns;
    const std::int64_t posted = record.posted_mono_ns;
    // After these, posted <= now < expiry, so both differences below are
    // positive and fit 64 bits unsigned, whatever the fields hold.
    if (expiry <= now_mono_ns || posted > now_mono_ns || expiry < posted) {
        return true;
    }
    constexpr auto LIMIT = static_cast<std::uint64_t>(MAX_REQUEST_LIFETIME_NS);
    const std::uint64_t lifetime =
        static_cast<std::uint64_t>(expiry) - static_cast<std::uint64_t>(posted);
    const std::uint64_t ahead = static_cast<std::uint64_t>(expiry) -
                                static_cast<std::uint64_t>(now_mono_ns);
    // The second bounds the lifetime on the consumer's own clock, whatever
    // posted_mono_ns says.
    return lifetime > LIMIT || ahead > LIMIT;
}

}  // namespace scada::tag
