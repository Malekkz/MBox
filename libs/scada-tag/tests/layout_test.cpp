// Conformance test 1 of the tag database contract (section 15): structure
// sizes, alignments and offsets match the document, and no structure has
// padding. Also checks that every shared atomic field is lock-free.
//
// layout.h proves the same at compile time from offsets. This test starts
// from the other side: for each structure it lists the fields in the order
// and with the sizes written in the contract, and requires that they follow
// one another without a gap and fill the structure. A mistake made in both
// places would have to be the same mistake twice.

#include "layout.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace {

using namespace scada::tag;

// One field as the compiler laid it out, and its size in the contract.
struct Field {
    std::string_view name;
    std::size_t offset = 0;
    std::size_t size = 0;
    std::size_t documented_size = 0;
};

// Size and alignment of a structure in the contract, in bytes.
struct Documented {
    std::size_t size = 0;
    std::size_t alignment = 0;
};

// Describes the member `member` of `object`. The offset is the distance
// between the two addresses.
template <typename Struct, typename Member>
Field field(const Struct& object, Member Struct::*member, std::string_view name,
            std::size_t documented_size) {
    const auto base = std::bit_cast<std::uintptr_t>(&object);
    const auto address = std::bit_cast<std::uintptr_t>(&(object.*member));
    return {name, address - base, sizeof(Member), documented_size};
}

// Expects that `fields`, in order, each start where the previous one ends,
// have their documented size, and together are exactly `documented_size`
// bytes, which is also the size of `Struct`.
template <typename Struct>
void expect_layout(std::span<const Field> fields,
                   const Documented& documented) {
    const std::size_t documented_size = documented.size;
    EXPECT_EQ(sizeof(Struct), documented.size);
    EXPECT_EQ(alignof(Struct), documented.alignment);
    std::size_t expected_offset = 0;
    for (const Field& item : fields) {
        EXPECT_EQ(item.offset, expected_offset) << item.name;
        EXPECT_EQ(item.size, item.documented_size) << item.name;
        expected_offset += item.documented_size;
    }
    EXPECT_EQ(expected_offset, documented_size) << "padding or a missing field";
}

// True if the member `member` of `object` is lock-free when shared through
// std::atomic_ref: its type is always lock-free on this target, and the
// field sits where the atomic access needs it, at a multiple of the required
// alignment inside a structure that is aligned at least as much. The
// run-time query `is_lock_free()` is not used: with some compilers it is a
// call into the separate atomic support library, which a lock-free target
// must not need.
template <typename Struct, typename Member>
bool lock_free(const Struct& object, Member Struct::*member) {
    const std::size_t alignment = std::atomic_ref<Member>::required_alignment;
    return std::atomic_ref<Member>::is_always_lock_free &&
           alignof(Struct) % alignment == 0 &&
           field(object, member, "", 0).offset % alignment == 0;
}

TEST(Conformance01Layout, RegionHeader) {
    const RegionHeader object;
    const std::array fields{
        field(object, &RegionHeader::magic, "magic", 8),
        field(object, &RegionHeader::layout_version, "layout_version", 4),
        field(object, &RegionHeader::kind, "kind", 4),
        field(object, &RegionHeader::generation, "generation", 8),
        field(object, &RegionHeader::size_bytes, "size_bytes", 8),
        field(object, &RegionHeader::owner_index, "owner_index", 2),
        field(object, &RegionHeader::queue_id, "queue_id", 2),
        field(object, &RegionHeader::ready, "ready", 4),
        field(object, &RegionHeader::reserved, "reserved", 24),
    };
    const Documented documented{.size = 64, .alignment = 64};
    expect_layout<RegionHeader>(fields, documented);
}

TEST(Conformance01Layout, GlobalHeader) {
    const GlobalHeader object;
    const std::array fields{
        field(object, &GlobalHeader::state, "state", 4),
        field(object, &GlobalHeader::tag_count, "tag_count", 4),
        field(object, &GlobalHeader::owner_count, "owner_count", 2),
        field(object, &GlobalHeader::process_count, "process_count", 2),
        field(object, &GlobalHeader::queue_count, "queue_count", 2),
        field(object, &GlobalHeader::reserved0, "reserved0", 2),
        field(object, &GlobalHeader::heartbeat_check_ms, "heartbeat_check_ms",
              4),
        field(object, &GlobalHeader::reserved1, "reserved1", 4),
        field(object, &GlobalHeader::owner_table_offset, "owner_table_offset",
              8),
        field(object, &GlobalHeader::process_table_offset,
              "process_table_offset", 8),
        field(object, &GlobalHeader::queue_table_offset, "queue_table_offset",
              8),
        field(object, &GlobalHeader::directory_offset, "directory_offset", 8),
        field(object, &GlobalHeader::name_pool_offset, "name_pool_offset", 8),
        field(object, &GlobalHeader::name_pool_size, "name_pool_size", 8),
        field(object, &GlobalHeader::created_time_ns, "created_time_ns", 8),
        field(object, &GlobalHeader::config_hash, "config_hash", 32),
        field(object, &GlobalHeader::reserved, "reserved", 16),
    };
    const Documented documented{.size = 128, .alignment = 8};
    expect_layout<GlobalHeader>(fields, documented);
}

TEST(Conformance01Layout, OwnerEntry) {
    const OwnerEntry object;
    const std::array fields{
        field(object, &OwnerEntry::name, "name", 32),
        field(object, &OwnerEntry::uid, "uid", 4),
        field(object, &OwnerEntry::tag_count, "tag_count", 4),
        field(object, &OwnerEntry::string_slots, "string_slots", 4),
        field(object, &OwnerEntry::change_ring_capacity, "change_ring_capacity",
              4),
        field(object, &OwnerEntry::process_index, "process_index", 2),
        field(object, &OwnerEntry::reserved0, "reserved0", 2),
        field(object, &OwnerEntry::reserved1, "reserved1", 4),
        field(object, &OwnerEntry::slot_table_offset, "slot_table_offset", 8),
    };
    const Documented documented{.size = 64, .alignment = 8};
    expect_layout<OwnerEntry>(fields, documented);
}

TEST(Conformance01Layout, ProcessEntry) {
    const ProcessEntry object;
    const std::array fields{
        field(object, &ProcessEntry::name, "name", 32),
        field(object, &ProcessEntry::uid, "uid", 4),
        field(object, &ProcessEntry::heartbeat_timeout_ms,
              "heartbeat_timeout_ms", 4),
        field(object, &ProcessEntry::flags, "flags", 4),
        field(object, &ProcessEntry::down, "down", 4),
        field(object, &ProcessEntry::reserved, "reserved", 16),
    };
    const Documented documented{.size = 64, .alignment = 4};
    expect_layout<ProcessEntry>(fields, documented);
}

TEST(Conformance01Layout, QueueEntry) {
    const QueueEntry object;
    const std::array fields{
        field(object, &QueueEntry::name, "name", 32),
        field(object, &QueueEntry::queue_id, "queue_id", 2),
        field(object, &QueueEntry::kind, "kind", 2),
        field(object, &QueueEntry::producer_process, "producer_process", 2),
        field(object, &QueueEntry::consumer_process, "consumer_process", 2),
        field(object, &QueueEntry::capacity, "capacity", 4),
        field(object, &QueueEntry::record_size, "record_size", 4),
        field(object, &QueueEntry::reserved, "reserved", 16),
    };
    const Documented documented{.size = 64, .alignment = 4};
    expect_layout<QueueEntry>(fields, documented);
}

TEST(Conformance01Layout, DirectoryEntry) {
    const DirectoryEntry object;
    const std::array fields{
        field(object, &DirectoryEntry::name_hash, "name_hash", 8),
        field(object, &DirectoryEntry::handle, "handle", 4),
        field(object, &DirectoryEntry::name_offset, "name_offset", 4),
        field(object, &DirectoryEntry::name_len, "name_len", 2),
        field(object, &DirectoryEntry::type, "type", 1),
        field(object, &DirectoryEntry::flags, "flags", 1),
        field(object, &DirectoryEntry::reserved, "reserved", 4),
    };
    const Documented documented{.size = 24, .alignment = 8};
    expect_layout<DirectoryEntry>(fields, documented);
}

TEST(Conformance01Layout, TagRecord) {
    const TagRecord object;
    const std::array fields{
        field(object, &TagRecord::seq, "seq", 4),
        field(object, &TagRecord::type, "type", 1),
        field(object, &TagRecord::quality, "quality", 1),
        field(object, &TagRecord::quality_flags, "quality_flags", 2),
        field(object, &TagRecord::value, "value", 8),
        field(object, &TagRecord::source_time_ns, "source_time_ns", 8),
        field(object, &TagRecord::update_time_ns, "update_time_ns", 8),
        field(object, &TagRecord::update_mono_ns, "update_mono_ns", 8),
        field(object, &TagRecord::change_count, "change_count", 4),
        field(object, &TagRecord::reserved, "reserved", 20),
    };
    const Documented documented{.size = 64, .alignment = 64};
    expect_layout<TagRecord>(fields, documented);
}

TEST(Conformance01Layout, StringSlot) {
    const StringSlot object;
    const std::array fields{
        field(object, &StringSlot::len, "len", 2),
        field(object, &StringSlot::data, "data", 250),
        field(object, &StringSlot::reserved, "reserved", 4),
    };
    const Documented documented{.size = 256, .alignment = 2};
    expect_layout<StringSlot>(fields, documented);
}

TEST(Conformance01Layout, ChangeRingHeader) {
    const ChangeRingHeader object;
    const std::array fields{
        field(object, &ChangeRingHeader::capacity, "capacity", 4),
        field(object, &ChangeRingHeader::epoch, "epoch", 4),
        field(object, &ChangeRingHeader::write_seq, "write_seq", 8),
        field(object, &ChangeRingHeader::reserved, "reserved", 48),
    };
    const Documented documented{.size = 64, .alignment = 64};
    expect_layout<ChangeRingHeader>(fields, documented);
}

TEST(Conformance01Layout, ChangeEntry) {
    const ChangeEntry object;
    const std::array fields{
        field(object, &ChangeEntry::update_mono_ns, "update_mono_ns", 8),
        field(object, &ChangeEntry::handle, "handle", 4),
        field(object, &ChangeEntry::type, "type", 1),
        field(object, &ChangeEntry::quality, "quality", 1),
        field(object, &ChangeEntry::quality_flags, "quality_flags", 2),
        field(object, &ChangeEntry::value, "value", 8),
        field(object, &ChangeEntry::source_time_ns, "source_time_ns", 8),
    };
    const Documented documented{.size = 32, .alignment = 8};
    expect_layout<ChangeEntry>(fields, documented);
}

TEST(Conformance01Layout, QueueHeader) {
    const QueueHeader object;
    const std::array fields{
        field(object, &QueueHeader::capacity, "capacity", 4),
        field(object, &QueueHeader::record_size, "record_size", 4),
        field(object, &QueueHeader::write_seq, "write_seq", 8),
        field(object, &QueueHeader::reserved, "reserved", 48),
    };
    const Documented documented{.size = 64, .alignment = 64};
    expect_layout<QueueHeader>(fields, documented);
}

TEST(Conformance01Layout, EventRecord) {
    const EventRecord object;
    const std::array fields{
        field(object, &EventRecord::seq, "seq", 8),
        field(object, &EventRecord::time_ns, "time_ns", 8),
        field(object, &EventRecord::kind, "kind", 2),
        field(object, &EventRecord::payload_len, "payload_len", 2),
        field(object, &EventRecord::source_handle, "source_handle", 4),
        field(object, &EventRecord::payload, "payload", 232),
    };
    const Documented documented{.size = 256, .alignment = 8};
    expect_layout<EventRecord>(fields, documented);
}

TEST(Conformance01Layout, WriteRequest) {
    const WriteRequest object;
    const std::array fields{
        field(object, &WriteRequest::request_id, "request_id", 8),
        field(object, &WriteRequest::target_handle, "target_handle", 4),
        field(object, &WriteRequest::operation, "operation", 1),
        field(object, &WriteRequest::source, "source", 1),
        field(object, &WriteRequest::value_type, "value_type", 1),
        field(object, &WriteRequest::reserved0, "reserved0", 1),
        field(object, &WriteRequest::value, "value", 8),
        field(object, &WriteRequest::user_id, "user_id", 4),
        field(object, &WriteRequest::reserved1, "reserved1", 4),
        field(object, &WriteRequest::expiry_mono_ns, "expiry_mono_ns", 8),
        field(object, &WriteRequest::posted_mono_ns, "posted_mono_ns", 8),
        field(object, &WriteRequest::select_request_id, "select_request_id", 8),
        field(object, &WriteRequest::reserved, "reserved", 8),
    };
    const Documented documented{.size = 64, .alignment = 8};
    expect_layout<WriteRequest>(fields, documented);
}

TEST(Conformance01Layout, WriteResult) {
    const WriteResult object;
    const std::array fields{
        field(object, &WriteResult::request_id, "request_id", 8),
        field(object, &WriteResult::status, "status", 1),
        field(object, &WriteResult::reserved0, "reserved0", 3),
        field(object, &WriteResult::protocol_error, "protocol_error", 4),
        field(object, &WriteResult::completed_time_ns, "completed_time_ns", 8),
        field(object, &WriteResult::reserved, "reserved", 8),
    };
    const Documented documented{.size = 32, .alignment = 8};
    expect_layout<WriteResult>(fields, documented);
}

TEST(Conformance01Layout, ProcHeader) {
    const ProcHeader object;
    const std::array fields{
        field(object, &ProcHeader::heartbeat, "heartbeat", 8),
        field(object, &ProcHeader::heartbeat_mono_ns, "heartbeat_mono_ns", 8),
        field(object, &ProcHeader::pid, "pid", 4),
        field(object, &ProcHeader::state, "state", 4),
        field(object, &ProcHeader::cycle_seq, "cycle_seq", 4),
        field(object, &ProcHeader::reserved, "reserved", 36),
    };
    const Documented documented{.size = 64, .alignment = 64};
    expect_layout<ProcHeader>(fields, documented);
}

// The cursors that follow ProcHeader are 64-bit and start on a multiple of 8.
TEST(Conformance01Layout, CursorArrayFollowsProcHeader) {
    EXPECT_EQ(
        (sizeof(RegionHeader) + sizeof(ProcHeader)) % sizeof(std::uint64_t),
        0U);
}

// "SCADARGN", so that a dump of a region starts with readable text: the
// first character is the lowest byte.
TEST(Conformance01Layout, RegionMagicSpellsItsName) {
    const std::string_view text = "SCADARGN";
    std::uint64_t magic = 0;
    std::uint64_t shift = 0;
    for (const char character : text) {
        magic |= std::uint64_t{static_cast<std::uint8_t>(character)} << shift;
        shift += std::numeric_limits<std::uint8_t>::digits;
    }
    EXPECT_EQ(magic, REGION_MAGIC);
}

// A structure created by the library starts as all zero bytes, so reserved
// fields are written as zero without naming them.
TEST(Conformance01Layout, DefaultValueIsAllZero) {
    EXPECT_EQ((std::bit_cast<std::array<std::uint8_t, sizeof(TagRecord)>>(
                  TagRecord{})),
              (std::array<std::uint8_t, sizeof(TagRecord)>{}));
    EXPECT_EQ((std::bit_cast<std::array<std::uint8_t, sizeof(WriteRequest)>>(
                  WriteRequest{})),
              (std::array<std::uint8_t, sizeof(WriteRequest)>{}));
    EXPECT_EQ((std::bit_cast<std::array<std::uint8_t, sizeof(StringSlot)>>(
                  StringSlot{})),
              (std::array<std::uint8_t, sizeof(StringSlot)>{}));
}

// Every field the contract marks `A`. The compile-time check in layout.h
// covers the types; this covers the fields at their real offsets.
TEST(SharedAtomics, EveryAtomicFieldIsLockFree) {
    const RegionHeader region_header;
    EXPECT_TRUE(lock_free(region_header, &RegionHeader::ready));
    const GlobalHeader global_header;
    EXPECT_TRUE(lock_free(global_header, &GlobalHeader::state));
    const ProcessEntry process_entry;
    EXPECT_TRUE(lock_free(process_entry, &ProcessEntry::down));
    const TagRecord tag_record;
    EXPECT_TRUE(lock_free(tag_record, &TagRecord::seq));
    const ChangeRingHeader change_ring_header;
    EXPECT_TRUE(lock_free(change_ring_header, &ChangeRingHeader::epoch));
    EXPECT_TRUE(lock_free(change_ring_header, &ChangeRingHeader::write_seq));
    const QueueHeader queue_header;
    EXPECT_TRUE(lock_free(queue_header, &QueueHeader::write_seq));
    const ProcHeader proc_header;
    EXPECT_TRUE(lock_free(proc_header, &ProcHeader::heartbeat));
    EXPECT_TRUE(lock_free(proc_header, &ProcHeader::heartbeat_mono_ns));
    EXPECT_TRUE(lock_free(proc_header, &ProcHeader::state));
    EXPECT_TRUE(lock_free(proc_header, &ProcHeader::cycle_seq));
    // The cursors after ProcHeader are bare 64-bit integers at multiples of
    // 8 (CursorArrayFollowsProcHeader).
    EXPECT_TRUE(IS_SHARED_ATOMIC<std::uint64_t>);
}

}  // namespace
