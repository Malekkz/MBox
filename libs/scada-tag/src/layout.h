#ifndef SCADA_TAG_LAYOUT_H
#define SCADA_TAG_LAYOUT_H

// The binary layout of the tag database: every structure of
// docs/contracts/tag-database.md, sections 4 to 10, layout version 1.
//
// This header is private to libscada-tag, because no other code may depend
// on the layout (contract, introduction). Each structure is followed by the
// checks that tie it to the contract: size, alignment, the offset of every
// field, and that it has no padding. A change here that does not match the
// contract does not compile.
//
// The structures hold plain integers only. A field marked `A` changes while
// other processes read it and is accessed only through `std::atomic_ref`
// (contract, section 1). Enumerations are stored as their underlying integer
// and converted with the `*_from_raw` functions after reading.

#include "scada/tag/types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace scada::tag {

// Contract, section 1: 64-bit little-endian targets only. The futex wait on
// the low 32 bits of a 64-bit counter (section 8.4) relies on it.
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(void*) == sizeof(std::uint64_t));

// True if a field of type `T` can be shared through `std::atomic_ref`: the
// access is lock-free, which a lock inside one process could never replace
// across processes, and the field's natural alignment is enough for it.
template <typename T>
inline constexpr bool IS_SHARED_ATOMIC =
    std::atomic_ref<T>::is_always_lock_free &&
    std::atomic_ref<T>::required_alignment <= alignof(T);

// The types of all fields marked `A`, and of the fields of a tag record and
// a string slot, which the sequence lock reads while their owner writes
// them (sections 6 and 7).
static_assert(IS_SHARED_ATOMIC<std::uint8_t>);
static_assert(IS_SHARED_ATOMIC<std::uint16_t>);
static_assert(IS_SHARED_ATOMIC<std::uint32_t>);
static_assert(IS_SHARED_ATOMIC<std::uint64_t>);
static_assert(IS_SHARED_ATOMIC<std::int64_t>);

// True if `T` may be placed in shared memory (coding standard, section 9):
// standard layout, trivially copyable, and every bit of it belongs to a
// field, so there is no padding, named or not, that the contract does not
// list.
template <typename T>
inline constexpr bool IS_SHARED_STRUCT =
    std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T> &&
    std::has_unique_object_representations_v<T>;

// One statement of the contract about a structure: its size, its alignment
// or the offset of a field, as the compiler has it and as the contract has
// it. The statements of a structure are collected in a constant table, which
// keeps each number next to the field it belongs to.
struct LayoutCheck {
    std::size_t actual = 0;
    std::size_t contract = 0;
};

// True if every statement in `checks` holds.
template <std::size_t Count>
[[nodiscard]] constexpr bool all_hold(
    const std::array<LayoutCheck, Count>& checks) noexcept {
    return std::ranges::all_of(checks, [](const LayoutCheck& check) {
        return check.actual == check.contract;
    });
}

// "SCADARGN" read as a little-endian integer.
inline constexpr std::uint64_t REGION_MAGIC = 0x4E47'5241'4441'4353U;
inline constexpr std::uint32_t LAYOUT_VERSION = 1;

// `RegionHeader.queue_id` of a region that is not a queue.
inline constexpr std::uint16_t NO_QUEUE_ID = 0xFFFFU;

// Alignment of a region and of the structures that are written while others
// read them: one cache line, so that two of them never share one.
inline constexpr std::size_t REGION_ALIGNMENT = 64;

// Sizes of the array fields.
inline constexpr std::size_t NAME_FIELD_SIZE = 32;
inline constexpr std::size_t CONFIG_HASH_SIZE = 32;
inline constexpr std::size_t STRING_SLOT_DATA_SIZE = 250;
inline constexpr std::size_t EVENT_PAYLOAD_SIZE = 232;

// `RegionHeader.kind` (section 4).
// NOLINTNEXTLINE(performance-enum-size): the type of the field in the contract
enum class RegionKind : std::uint32_t {
    HEADER = 1,
    PROC = 2,
    TAGS = 3,
    STRINGS = 4,
    CHANGES = 5,
    EVENTS = 6,
    CMD = 7,
    RESULT = 8,
};

// `GlobalHeader.state` (section 5).
// NOLINTNEXTLINE(performance-enum-size): the type of the field in the contract
enum class GenerationState : std::uint32_t {
    BUILDING = 1,
    RUNNING = 2,
    RETIRING = 3,
};

// `ProcHeader.state` (section 10).
// NOLINTNEXTLINE(performance-enum-size): the type of the field in the contract
enum class ProcessState : std::uint32_t {
    STARTING = 1,
    RUNNING = 2,
    STOPPING = 3,
};

// `QueueEntry.kind` (section 5). The values are those of the queue's
// RegionKind.
// NOLINTNEXTLINE(performance-enum-size): the type of the field in the contract
enum class QueueKind : std::uint16_t {
    EVENTS = 6,
    CMD = 7,
    RESULT = 8,
};

// Bits of `ProcessEntry.flags` (section 5).
// NOLINTNEXTLINE(performance-enum-size): the type of the field in the contract
enum class ProcessFlag : std::uint32_t {
    REALTIME = 1U << 0U,
};
inline constexpr std::uint32_t PROCESS_FLAGS_MASK = 0x1U;

// Bits of `DirectoryEntry.flags` (section 5).
enum class TagFlag : std::uint8_t {
    RETAIN = 1U << 0U,
    WRITABLE_BY_COMMAND = 1U << 1U,
    RETAIN_NOW = 1U << 2U,  // section 12
};
inline constexpr std::uint8_t TAG_FLAGS_MASK = 0x7U;

// Each returns the enumerator with the value `raw`, or nothing if there is
// none.
SCADA_RT [[nodiscard]] constexpr std::optional<RegionKind> region_kind_from_raw(
    std::uint32_t raw) noexcept {
    return enum_from_raw(raw, RegionKind::HEADER, RegionKind::RESULT);
}

SCADA_RT [[nodiscard]] constexpr std::optional<GenerationState>
generation_state_from_raw(std::uint32_t raw) noexcept {
    return enum_from_raw(raw, GenerationState::BUILDING,
                         GenerationState::RETIRING);
}

SCADA_RT [[nodiscard]] constexpr std::optional<ProcessState>
process_state_from_raw(std::uint32_t raw) noexcept {
    return enum_from_raw(raw, ProcessState::STARTING, ProcessState::STOPPING);
}

SCADA_RT [[nodiscard]] constexpr std::optional<QueueKind> queue_kind_from_raw(
    std::uint16_t raw) noexcept {
    return enum_from_raw(raw, QueueKind::EVENTS, QueueKind::RESULT);
}

// Start of every region (section 4).
struct alignas(REGION_ALIGNMENT) RegionHeader {
    std::uint64_t magic = 0;           // REGION_MAGIC
    std::uint32_t layout_version = 0;  // LAYOUT_VERSION
    std::uint32_t kind = 0;            // RegionKind
    std::uint64_t generation = 0;
    std::uint64_t size_bytes = 0;   // total file size
    std::uint16_t owner_index = 0;  // index into the owner or process table
    std::uint16_t queue_id = 0;     // for queue regions, else NO_QUEUE_ID
    std::uint32_t ready = 0;  // A; 0 while being initialised, 1 when usable
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): size of a reserved field, fixed by the contract
    std::array<std::uint8_t, 24> reserved{};
};
inline constexpr std::array REGION_HEADER_CHECKS{
    LayoutCheck{sizeof(RegionHeader), 64},
    LayoutCheck{alignof(RegionHeader), 64},
    LayoutCheck{offsetof(RegionHeader, magic), 0},
    LayoutCheck{offsetof(RegionHeader, layout_version), 8},
    LayoutCheck{offsetof(RegionHeader, kind), 12},
    LayoutCheck{offsetof(RegionHeader, generation), 16},
    LayoutCheck{offsetof(RegionHeader, size_bytes), 24},
    LayoutCheck{offsetof(RegionHeader, owner_index), 32},
    LayoutCheck{offsetof(RegionHeader, queue_id), 34},
    LayoutCheck{offsetof(RegionHeader, ready), 36},
    LayoutCheck{offsetof(RegionHeader, reserved), 40},
};
static_assert(IS_SHARED_STRUCT<RegionHeader>);
static_assert(all_hold(REGION_HEADER_CHECKS));

// Follows RegionHeader in the `header` region (section 5).
struct GlobalHeader {
    std::uint32_t state = 0;  // A; GenerationState
    std::uint32_t tag_count = 0;
    std::uint16_t owner_count = 0;
    std::uint16_t process_count = 0;
    std::uint16_t queue_count = 0;
    std::uint16_t reserved0 = 0;
    std::uint32_t heartbeat_check_ms = 0;  // how often tagd checks heartbeats
    std::uint32_t reserved1 = 0;
    std::uint64_t owner_table_offset = 0;
    std::uint64_t process_table_offset = 0;
    std::uint64_t queue_table_offset = 0;
    std::uint64_t directory_offset = 0;
    std::uint64_t name_pool_offset = 0;
    std::uint64_t name_pool_size = 0;
    std::int64_t created_time_ns = 0;
    // SHA-256 of the tag-set definition.
    std::array<std::uint8_t, CONFIG_HASH_SIZE> config_hash{};
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): size of a reserved field, fixed by the contract
    std::array<std::uint8_t, 16> reserved{};
};
inline constexpr std::array GLOBAL_HEADER_CHECKS{
    LayoutCheck{sizeof(GlobalHeader), 128},
    LayoutCheck{alignof(GlobalHeader), 8},
    LayoutCheck{offsetof(GlobalHeader, state), 0},
    LayoutCheck{offsetof(GlobalHeader, tag_count), 4},
    LayoutCheck{offsetof(GlobalHeader, owner_count), 8},
    LayoutCheck{offsetof(GlobalHeader, process_count), 10},
    LayoutCheck{offsetof(GlobalHeader, queue_count), 12},
    LayoutCheck{offsetof(GlobalHeader, reserved0), 14},
    LayoutCheck{offsetof(GlobalHeader, heartbeat_check_ms), 16},
    LayoutCheck{offsetof(GlobalHeader, reserved1), 20},
    LayoutCheck{offsetof(GlobalHeader, owner_table_offset), 24},
    LayoutCheck{offsetof(GlobalHeader, process_table_offset), 32},
    LayoutCheck{offsetof(GlobalHeader, queue_table_offset), 40},
    LayoutCheck{offsetof(GlobalHeader, directory_offset), 48},
    LayoutCheck{offsetof(GlobalHeader, name_pool_offset), 56},
    LayoutCheck{offsetof(GlobalHeader, name_pool_size), 64},
    LayoutCheck{offsetof(GlobalHeader, created_time_ns), 72},
    LayoutCheck{offsetof(GlobalHeader, config_hash), 80},
    LayoutCheck{offsetof(GlobalHeader, reserved), 112},
};
static_assert(IS_SHARED_STRUCT<GlobalHeader>);
static_assert(all_hold(GLOBAL_HEADER_CHECKS));

// One entry of the owner table (section 5).
struct OwnerEntry {
    std::array<char, NAME_FIELD_SIZE> name{};  // NUL-padded
    std::uint32_t uid = 0;
    std::uint32_t tag_count = 0;     // slots in tags.<owner>
    std::uint32_t string_slots = 0;  // slots in strings.<owner>, may be 0
    std::uint32_t change_ring_capacity = 0;  // entries, power of two
    std::uint16_t process_index = 0;  // the process this owner is a thread of
    std::uint16_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    std::uint64_t slot_table_offset =
        0;  // uint32_t directory index for each slot
};
inline constexpr std::array OWNER_ENTRY_CHECKS{
    LayoutCheck{sizeof(OwnerEntry), 64},
    LayoutCheck{alignof(OwnerEntry), 8},
    LayoutCheck{offsetof(OwnerEntry, name), 0},
    LayoutCheck{offsetof(OwnerEntry, uid), 32},
    LayoutCheck{offsetof(OwnerEntry, tag_count), 36},
    LayoutCheck{offsetof(OwnerEntry, string_slots), 40},
    LayoutCheck{offsetof(OwnerEntry, change_ring_capacity), 44},
    LayoutCheck{offsetof(OwnerEntry, process_index), 48},
    LayoutCheck{offsetof(OwnerEntry, reserved0), 50},
    LayoutCheck{offsetof(OwnerEntry, reserved1), 52},
    LayoutCheck{offsetof(OwnerEntry, slot_table_offset), 56},
};
static_assert(IS_SHARED_STRUCT<OwnerEntry>);
static_assert(all_hold(OWNER_ENTRY_CHECKS));

// One entry of the process table (section 5).
struct ProcessEntry {
    std::array<char, NAME_FIELD_SIZE> name{};  // NUL-padded
    std::uint32_t uid = 0;
    std::uint32_t heartbeat_timeout_ms = 0;
    std::uint32_t flags = 0;  // ProcessFlag bits
    std::uint32_t down = 0;   // A; written by scada-tagd only (section 10.1)
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): size of a reserved field, fixed by the contract
    std::array<std::uint8_t, 16> reserved{};
};
inline constexpr std::array PROCESS_ENTRY_CHECKS{
    LayoutCheck{sizeof(ProcessEntry), 64},
    LayoutCheck{alignof(ProcessEntry), 4},
    LayoutCheck{offsetof(ProcessEntry, name), 0},
    LayoutCheck{offsetof(ProcessEntry, uid), 32},
    LayoutCheck{offsetof(ProcessEntry, heartbeat_timeout_ms), 36},
    LayoutCheck{offsetof(ProcessEntry, flags), 40},
    LayoutCheck{offsetof(ProcessEntry, down), 44},
    LayoutCheck{offsetof(ProcessEntry, reserved), 48},
};
static_assert(IS_SHARED_STRUCT<ProcessEntry>);
static_assert(all_hold(PROCESS_ENTRY_CHECKS));

// One entry of the queue table (section 5).
struct QueueEntry {
    // NUL-padded, e.g. "events.alarm".
    std::array<char, NAME_FIELD_SIZE> name{};
    std::uint16_t queue_id = 0;
    std::uint16_t kind = 0;  // QueueKind
    std::uint16_t producer_process = 0;
    std::uint16_t consumer_process = 0;
    std::uint32_t capacity = 0;     // records, power of two
    std::uint32_t record_size = 0;  // bytes
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): size of a reserved field, fixed by the contract
    std::array<std::uint8_t, 16> reserved{};
};
inline constexpr std::array QUEUE_ENTRY_CHECKS{
    LayoutCheck{sizeof(QueueEntry), 64},
    LayoutCheck{alignof(QueueEntry), 4},
    LayoutCheck{offsetof(QueueEntry, name), 0},
    LayoutCheck{offsetof(QueueEntry, queue_id), 32},
    LayoutCheck{offsetof(QueueEntry, kind), 34},
    LayoutCheck{offsetof(QueueEntry, producer_process), 36},
    LayoutCheck{offsetof(QueueEntry, consumer_process), 38},
    LayoutCheck{offsetof(QueueEntry, capacity), 40},
    LayoutCheck{offsetof(QueueEntry, record_size), 44},
    LayoutCheck{offsetof(QueueEntry, reserved), 48},
};
static_assert(IS_SHARED_STRUCT<QueueEntry>);
static_assert(all_hold(QUEUE_ENTRY_CHECKS));

// One entry of the tag directory (section 5).
struct DirectoryEntry {
    std::uint64_t name_hash = 0;  // fnv1a_64 of the tag name
    std::uint32_t handle = 0;
    std::uint32_t name_offset = 0;  // into the name pool
    std::uint16_t name_len = 0;
    std::uint8_t type = 0;   // TagType
    std::uint8_t flags = 0;  // TagFlag bits
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): size of a reserved field, fixed by the contract
    std::array<std::uint8_t, 4> reserved{};
};
inline constexpr std::array DIRECTORY_ENTRY_CHECKS{
    LayoutCheck{sizeof(DirectoryEntry), 24},
    LayoutCheck{alignof(DirectoryEntry), 8},
    LayoutCheck{offsetof(DirectoryEntry, name_hash), 0},
    LayoutCheck{offsetof(DirectoryEntry, handle), 8},
    LayoutCheck{offsetof(DirectoryEntry, name_offset), 12},
    LayoutCheck{offsetof(DirectoryEntry, name_len), 16},
    LayoutCheck{offsetof(DirectoryEntry, type), 18},
    LayoutCheck{offsetof(DirectoryEntry, flags), 19},
    LayoutCheck{offsetof(DirectoryEntry, reserved), 20},
};
static_assert(IS_SHARED_STRUCT<DirectoryEntry>);
static_assert(all_hold(DIRECTORY_ENTRY_CHECKS));

// One record of `tags.<owner>` (section 6).
struct alignas(REGION_ALIGNMENT) TagRecord {
    std::uint32_t seq = 0;            // A; odd while the owner is writing
    std::uint8_t type = 0;            // TagType
    std::uint8_t quality = 0;         // Quality
    std::uint16_t quality_flags = 0;  // QualityFlag bits
    std::uint64_t value = 0;          // bit pattern (section 1)
    std::int64_t source_time_ns = 0;
    std::int64_t update_time_ns = 0;
    std::int64_t update_mono_ns = 0;
    std::uint32_t change_count = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): size of a reserved field, fixed by the contract
    std::array<std::uint8_t, 20> reserved{};
};
inline constexpr std::array TAG_RECORD_CHECKS{
    LayoutCheck{sizeof(TagRecord), 64},
    LayoutCheck{alignof(TagRecord), 64},
    LayoutCheck{offsetof(TagRecord, seq), 0},
    LayoutCheck{offsetof(TagRecord, type), 4},
    LayoutCheck{offsetof(TagRecord, quality), 5},
    LayoutCheck{offsetof(TagRecord, quality_flags), 6},
    LayoutCheck{offsetof(TagRecord, value), 8},
    LayoutCheck{offsetof(TagRecord, source_time_ns), 16},
    LayoutCheck{offsetof(TagRecord, update_time_ns), 24},
    LayoutCheck{offsetof(TagRecord, update_mono_ns), 32},
    LayoutCheck{offsetof(TagRecord, change_count), 40},
    LayoutCheck{offsetof(TagRecord, reserved), 44},
};
static_assert(IS_SHARED_STRUCT<TagRecord>);
static_assert(all_hold(TAG_RECORD_CHECKS));

// One slot of `strings.<owner>` (section 7).
struct StringSlot {
    std::uint16_t len = 0;  // bytes used, at most STRING_SLOT_DATA_SIZE
    // UTF-8, not NUL-terminated.
    std::array<std::uint8_t, STRING_SLOT_DATA_SIZE> data{};
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): size of a reserved field, fixed by the contract
    std::array<std::uint8_t, 4> reserved{};
};
inline constexpr std::array STRING_SLOT_CHECKS{
    LayoutCheck{sizeof(StringSlot), 256},
    LayoutCheck{alignof(StringSlot), 2},
    LayoutCheck{offsetof(StringSlot, len), 0},
    LayoutCheck{offsetof(StringSlot, data), 2},
    LayoutCheck{offsetof(StringSlot, reserved), 252},
};
static_assert(IS_SHARED_STRUCT<StringSlot>);
static_assert(all_hold(STRING_SLOT_CHECKS));

// Follows RegionHeader in `changes.<owner>` (section 8).
struct alignas(REGION_ALIGNMENT) ChangeRingHeader {
    std::uint32_t capacity = 0;   // power of two
    std::uint32_t epoch = 0;      // A; +1 each time the owner reattaches
    std::uint64_t write_seq = 0;  // A; number of entries ever written
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): size of a reserved field, fixed by the contract
    std::array<std::uint8_t, 48> reserved{};
};
inline constexpr std::array CHANGE_RING_HEADER_CHECKS{
    LayoutCheck{sizeof(ChangeRingHeader), 64},
    LayoutCheck{alignof(ChangeRingHeader), 64},
    LayoutCheck{offsetof(ChangeRingHeader, capacity), 0},
    LayoutCheck{offsetof(ChangeRingHeader, epoch), 4},
    LayoutCheck{offsetof(ChangeRingHeader, write_seq), 8},
    LayoutCheck{offsetof(ChangeRingHeader, reserved), 16},
};
static_assert(IS_SHARED_STRUCT<ChangeRingHeader>);
static_assert(all_hold(CHANGE_RING_HEADER_CHECKS));

// One entry of a change ring (section 8).
struct ChangeEntry {
    std::int64_t update_mono_ns = 0;  // same instant as in the tag record
    std::uint32_t handle = 0;
    std::uint8_t type = 0;            // TagType
    std::uint8_t quality = 0;         // Quality
    std::uint16_t quality_flags = 0;  // QualityFlag bits
    std::uint64_t value = 0;
    std::int64_t source_time_ns = 0;
};
inline constexpr std::array CHANGE_ENTRY_CHECKS{
    LayoutCheck{sizeof(ChangeEntry), 32},
    LayoutCheck{alignof(ChangeEntry), 8},
    LayoutCheck{offsetof(ChangeEntry, update_mono_ns), 0},
    LayoutCheck{offsetof(ChangeEntry, handle), 8},
    LayoutCheck{offsetof(ChangeEntry, type), 12},
    LayoutCheck{offsetof(ChangeEntry, quality), 13},
    LayoutCheck{offsetof(ChangeEntry, quality_flags), 14},
    LayoutCheck{offsetof(ChangeEntry, value), 16},
    LayoutCheck{offsetof(ChangeEntry, source_time_ns), 24},
};
static_assert(IS_SHARED_STRUCT<ChangeEntry>);
static_assert(all_hold(CHANGE_ENTRY_CHECKS));

// Follows RegionHeader in a reliable queue region (section 9).
struct alignas(REGION_ALIGNMENT) QueueHeader {
    std::uint32_t capacity = 0;  // power of two
    std::uint32_t record_size = 0;
    std::uint64_t write_seq = 0;  // A
    // Position of the first record of this file, set by the producer that
    // created it; never changes (issue #53). Meaningful only with
    // QUEUE_BASE_SET in `flags`; a file made by an older build holds 0.
    std::uint64_t base_seq = 0;
    std::uint32_t flags = 0;  // QUEUE_BASE_SET
    std::uint32_t reserved0 = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): size of a reserved field, fixed by the contract
    std::array<std::uint8_t, 32> reserved{};
};

// QueueHeader.flags: the creator set `base_seq`.
inline constexpr std::uint32_t QUEUE_BASE_SET = 1U << 0U;
inline constexpr std::array QUEUE_HEADER_CHECKS{
    LayoutCheck{sizeof(QueueHeader), 64},
    LayoutCheck{alignof(QueueHeader), 64},
    LayoutCheck{offsetof(QueueHeader, capacity), 0},
    LayoutCheck{offsetof(QueueHeader, record_size), 4},
    LayoutCheck{offsetof(QueueHeader, write_seq), 8},
    LayoutCheck{offsetof(QueueHeader, base_seq), 16},
    LayoutCheck{offsetof(QueueHeader, flags), 24},
    LayoutCheck{offsetof(QueueHeader, reserved0), 28},
    LayoutCheck{offsetof(QueueHeader, reserved), 32},
};
static_assert(IS_SHARED_STRUCT<QueueHeader>);
static_assert(all_hold(QUEUE_HEADER_CHECKS));

// Record of a queue of kind EVENTS (section 9.4).
struct EventRecord {
    std::uint64_t seq = 0;  // the queue's write_seq for this record
    std::int64_t time_ns = 0;
    std::uint16_t kind = 0;         // event kind; values come with the journal
    std::uint16_t payload_len = 0;  // at most EVENT_PAYLOAD_SIZE
    std::uint32_t source_handle =
        0;  // tag the event concerns, or INVALID_HANDLE
    // Serialised message, defined with the journal.
    std::array<std::uint8_t, EVENT_PAYLOAD_SIZE> payload{};
};
inline constexpr std::array EVENT_RECORD_CHECKS{
    LayoutCheck{sizeof(EventRecord), 256},
    LayoutCheck{alignof(EventRecord), 8},
    LayoutCheck{offsetof(EventRecord, seq), 0},
    LayoutCheck{offsetof(EventRecord, time_ns), 8},
    LayoutCheck{offsetof(EventRecord, kind), 16},
    LayoutCheck{offsetof(EventRecord, payload_len), 18},
    LayoutCheck{offsetof(EventRecord, source_handle), 20},
    LayoutCheck{offsetof(EventRecord, payload), 24},
};
static_assert(IS_SHARED_STRUCT<EventRecord>);
static_assert(all_hold(EVENT_RECORD_CHECKS));

// Record of a queue of kind CMD (section 9.4).
struct WriteRequest {
    std::uint64_t request_id = 0;  // (queue_id << 48) | write_seq
    std::uint32_t target_handle = 0;
    std::uint8_t operation = 0;   // WriteOperation
    std::uint8_t source = 0;      // WriteSource
    std::uint8_t value_type = 0;  // TagType
    std::uint8_t reserved0 = 0;
    std::uint64_t value = 0;
    std::uint32_t user_id = 0;  // the user for OPERATOR, 0 for AUTO and REMOTE
    std::uint32_t reserved1 = 0;
    std::int64_t expiry_mono_ns = 0;  // the driver must not execute after this
    std::int64_t posted_mono_ns = 0;
    std::uint64_t select_request_id =
        0;  // for OPERATE and CANCEL: the SELECT, else 0
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): size of a reserved field, fixed by the contract
    std::array<std::uint8_t, 8> reserved{};
};
inline constexpr std::array WRITE_REQUEST_CHECKS{
    LayoutCheck{sizeof(WriteRequest), 64},
    LayoutCheck{alignof(WriteRequest), 8},
    LayoutCheck{offsetof(WriteRequest, request_id), 0},
    LayoutCheck{offsetof(WriteRequest, target_handle), 8},
    LayoutCheck{offsetof(WriteRequest, operation), 12},
    LayoutCheck{offsetof(WriteRequest, source), 13},
    LayoutCheck{offsetof(WriteRequest, value_type), 14},
    LayoutCheck{offsetof(WriteRequest, reserved0), 15},
    LayoutCheck{offsetof(WriteRequest, value), 16},
    LayoutCheck{offsetof(WriteRequest, user_id), 24},
    LayoutCheck{offsetof(WriteRequest, reserved1), 28},
    LayoutCheck{offsetof(WriteRequest, expiry_mono_ns), 32},
    LayoutCheck{offsetof(WriteRequest, posted_mono_ns), 40},
    LayoutCheck{offsetof(WriteRequest, select_request_id), 48},
    LayoutCheck{offsetof(WriteRequest, reserved), 56},
};
static_assert(IS_SHARED_STRUCT<WriteRequest>);
static_assert(all_hold(WRITE_REQUEST_CHECKS));

// Record of a queue of kind RESULT (section 9.4).
struct WriteResult {
    std::uint64_t request_id = 0;
    std::uint8_t status = 0;  // WriteStatus
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): size of a reserved field, fixed by the contract
    std::array<std::uint8_t, 3> reserved0{};
    std::int32_t protocol_error = 0;  // driver-specific, 0 if none
    std::int64_t completed_time_ns = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): size of a reserved field, fixed by the contract
    std::array<std::uint8_t, 8> reserved{};
};
inline constexpr std::array WRITE_RESULT_CHECKS{
    LayoutCheck{sizeof(WriteResult), 32},
    LayoutCheck{alignof(WriteResult), 8},
    LayoutCheck{offsetof(WriteResult, request_id), 0},
    LayoutCheck{offsetof(WriteResult, status), 8},
    LayoutCheck{offsetof(WriteResult, reserved0), 9},
    LayoutCheck{offsetof(WriteResult, protocol_error), 12},
    LayoutCheck{offsetof(WriteResult, completed_time_ns), 16},
    LayoutCheck{offsetof(WriteResult, reserved), 24},
};
static_assert(IS_SHARED_STRUCT<WriteResult>);
static_assert(all_hold(WRITE_RESULT_CHECKS));

// Follows RegionHeader in `proc.<process>` (section 10). After it come
// `queue_count` cursors: std::uint64_t each, A, indexed by queue_id.
struct alignas(REGION_ALIGNMENT) ProcHeader {
    std::uint64_t heartbeat = 0;  // A; incremented by scada_tag_heartbeat
    std::int64_t heartbeat_mono_ns = 0;  // A
    std::uint32_t pid = 0;    // for diagnosis only; never used for a decision
    std::uint32_t state = 0;  // A; ProcessState
    std::uint32_t cycle_seq = 0;  // A; incremented by scada_tag_notify
    // For diagnosis only, never used to judge liveness (section 10.1):
    std::uint32_t late_beats = 0;        // A; heartbeats later than the timeout
    std::int64_t longest_gap_ns = 0;     // A; between two published times
    std::int64_t last_late_mono_ns = 0;  // A; end of the latest late gap
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): size of a reserved field, fixed by the contract
    std::array<std::uint8_t, 16> reserved{};
};
inline constexpr std::array PROC_HEADER_CHECKS{
    LayoutCheck{sizeof(ProcHeader), 64},
    LayoutCheck{alignof(ProcHeader), 64},
    LayoutCheck{offsetof(ProcHeader, heartbeat), 0},
    LayoutCheck{offsetof(ProcHeader, heartbeat_mono_ns), 8},
    LayoutCheck{offsetof(ProcHeader, pid), 16},
    LayoutCheck{offsetof(ProcHeader, state), 20},
    LayoutCheck{offsetof(ProcHeader, cycle_seq), 24},
    LayoutCheck{offsetof(ProcHeader, late_beats), 28},
    LayoutCheck{offsetof(ProcHeader, longest_gap_ns), 32},
    LayoutCheck{offsetof(ProcHeader, last_late_mono_ns), 40},
    LayoutCheck{offsetof(ProcHeader, reserved), 48},
};
static_assert(IS_SHARED_STRUCT<ProcHeader>);
static_assert(all_hold(PROC_HEADER_CHECKS));
}  // namespace scada::tag

#endif  // SCADA_TAG_LAYOUT_H
