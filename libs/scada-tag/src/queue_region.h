#ifndef SCADA_TAG_QUEUE_REGION_H
#define SCADA_TAG_QUEUE_REGION_H

// The memory of a reliable queue (docs/contracts/tag-database.md, section
// 9): the queue header, the records and the read cursors in the consumer's
// `proc` region, and the checks of the records of section 9.4. Every access
// to memory another process may write goes through `std::atomic_ref`; a
// record is copied as 64-bit words, relaxed, between the loads and stores of
// `write_seq` and the cursor that order it. Private to libscada-tag.

#include "layout.h"

#include "scada/common/realtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::tag {

// The largest record of layout version 1 (EventRecord), in bytes and in
// 64-bit words.
inline constexpr std::size_t MAX_RECORD_SIZE = sizeof(EventRecord);
inline constexpr std::size_t MAX_RECORD_WORDS =
    MAX_RECORD_SIZE / sizeof(std::uint64_t);
static_assert(sizeof(WriteRequest) % sizeof(std::uint64_t) == 0 &&
              sizeof(WriteResult) % sizeof(std::uint64_t) == 0 &&
              sizeof(EventRecord) % sizeof(std::uint64_t) == 0);

// The bits a CMD request id keeps of `write_seq`: request_id is
// (queue_id << 48) | write_seq (section 9.4).
inline constexpr unsigned REQUEST_ID_SEQ_BITS = 48;
inline constexpr std::uint64_t MAX_REQUEST_SEQ =
    (std::uint64_t{1} << REQUEST_ID_SEQ_BITS) - 1;

// The cursor a consumer stores when its queue died (decision D-40): a
// reserved value that no `write_seq` reaches (a post that would make
// `write_seq` equal to it is refused), so every reader of a cursor, by the
// range rule of section 9.1, finds it out of range: the producer refuses
// posts with SCADA_E_PEER_DOWN, the consumer and the drain at attach
// consume nothing.
inline constexpr std::uint64_t DEAD_CURSOR = ~std::uint64_t{0};

// The longest lifetime of a write request, expiry_mono_ns - posted_mono_ns,
// in ns (section 14: 60 seconds).
inline constexpr std::int64_t MAX_REQUEST_LIFETIME_NS = 60'000'000'000;

// Values of EventRecord.kind (section 9.4 names them; the library numbers
// them in the order given, from 1).
// NOLINTNEXTLINE(performance-enum-size): the type of the field in the contract
enum class EventKind : std::uint16_t {
    ALARM = 1,
    COMMAND = 2,
    AUDIT = 3,
    SYSTEM = 4,
};

// One record in private memory, as 64-bit words. Only the first
// `record_size / 8` words are used.
struct RecordWords {
    std::array<std::uint64_t, MAX_RECORD_WORDS> words{};
};

// The request id of the record at `position` of the CMD queue `queue_id`.
SCADA_RT [[nodiscard]] constexpr std::uint64_t request_id_of(
    std::size_t queue_id, std::uint64_t position) noexcept {
    return (std::uint64_t{queue_id} << REQUEST_ID_SEQ_BITS) | position;
}

// True if a queue of `capacity` records is broken by the range rule of
// section 9.2, judged by a reader that is neither of its two sides while
// both may run (scada-tagd, issue #53): `written_before` is `write_seq`
// loaded before the cursor `read`, and `written_after` loaded after it.
// The consumer never moves the cursor past `write_seq` and the producer
// never moves `write_seq` more than `capacity` past the cursor, which only
// grows; so a pair in range at every moment is never judged broken, and a
// pair that does not move is judged exactly by the range rule.
SCADA_RT [[nodiscard]] constexpr bool queue_pair_broken(
    std::uint64_t written_before, std::uint64_t read,
    std::uint64_t written_after, std::uint32_t capacity) noexcept {
    return read > written_after ||
           (written_before > read && written_before - read > capacity);
}

// The queue header of the mapped queue region `region`.
SCADA_RT [[nodiscard]] QueueHeader& queue_header(
    std::span<std::byte> region) noexcept;

// `write_seq` of the queue in `region`, loaded with acquire.
SCADA_RT [[nodiscard]] std::uint64_t queue_write_seq(
    std::span<std::byte> region) noexcept;

// The cursor of queue `queue_id` in the mapped `proc` region `region`, or
// null if the region is too small to hold it (an empty span too). The
// pointer is valid as long as the mapping is; the field is accessed with
// `load_shared` and `store_shared` only.
SCADA_RT [[nodiscard]] std::uint64_t* cursor_field(
    std::span<std::byte> region, std::size_t queue_id) noexcept;

// Copies the record at `position` of the queue in `region` (`capacity`
// records, a power of two, of `record_size` bytes) into `out`, word by
// word, relaxed. The region was opened with this capacity and record size,
// so the record lies inside it.
SCADA_RT void load_record(std::span<std::byte> region, std::uint32_t capacity,
                          std::uint32_t record_size, std::uint64_t position,
                          RecordWords& out) noexcept;

// Stores `record` at `position` of the queue in `region`, mapped
// read-write, as `load_record` reads it.
SCADA_RT void store_record(std::span<std::byte> region, std::uint32_t capacity,
                           std::uint32_t record_size, std::uint64_t position,
                           const RecordWords& record) noexcept;

// The record `words` as the structure of its kind, and back.
SCADA_RT [[nodiscard]] EventRecord event_of(const RecordWords& words) noexcept;
SCADA_RT [[nodiscard]] WriteRequest request_of(
    const RecordWords& words) noexcept;
SCADA_RT [[nodiscard]] WriteResult result_of(const RecordWords& words) noexcept;
SCADA_RT [[nodiscard]] RecordWords words_of(const EventRecord& record) noexcept;
SCADA_RT [[nodiscard]] RecordWords words_of(
    const WriteRequest& record) noexcept;
SCADA_RT [[nodiscard]] RecordWords words_of(const WriteResult& record) noexcept;

// The checks of a record of section 9.4, apart from its id, which the
// library sets at post and the consumer compares with the position. Every
// reserved field is 0.
//
// An event: a kind of EventKind and a payload length of at most 232.
SCADA_RT [[nodiscard]] bool event_valid(const EventRecord& record) noexcept;

// A write request: an operation and a source of their enumerations; a
// value type of BOOL, INT64, UINT64 or FLOAT64 (STRING cannot be commanded),
// or 0 for CANCEL and READ_NOW, which carry no value, and then value 0; a
// BOOL value of 0 or 1; a user id not 0 for OPERATOR and 0 for AUTO and
// REMOTE; select_request_id not 0 for OPERATE and CANCEL and 0 otherwise.
// The times are not checked here.
SCADA_RT [[nodiscard]] bool request_valid(const WriteRequest& record) noexcept;

// A write result: a status of WriteStatus.
SCADA_RT [[nodiscard]] bool result_valid(const WriteResult& record) noexcept;

// True if the request `record` must not be executed at `now_mono_ns`
// (section 9.2; the conditions of decision D-40): its expiry is not after
// now; it was posted after now; it expires before it was posted; its
// lifetime (expiry - posted) is longer than MAX_REQUEST_LIFETIME_NS; or its
// expiry lies further than that ahead of now. The fields are input: no
// value of them overflows.
SCADA_RT [[nodiscard]] bool request_expired(const WriteRequest& record,
                                            std::int64_t now_mono_ns) noexcept;

}  // namespace scada::tag

#endif  // SCADA_TAG_QUEUE_REGION_H
