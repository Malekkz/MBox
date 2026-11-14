#include "scada/tag/header_builder.h"

#include "layout.h"
#include "name_hash.h"
#include "resolved_tag_set.h"

#include "scada/common/panic.h"
#include "scada/common/result.h"
#include "scada/tag/handle.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace scada::tag {

namespace {

// Every offset in a region is a multiple of this (contract, section 1).
constexpr std::size_t OFFSET_ALIGNMENT = 8;

// The name pool is addressed with 32-bit offsets.
static_assert(MAX_TAGS * MAX_TAG_NAME_LENGTH <=
              std::numeric_limits<std::uint32_t>::max());
static_assert(MAX_TAG_NAME_LENGTH <= std::numeric_limits<std::uint16_t>::max());
static_assert(MAX_SHORT_NAME_LENGTH < NAME_FIELD_SIZE);
static_assert(CONFIG_HASH_BYTES == CONFIG_HASH_SIZE);
// The counts in GlobalHeader and the directory index in a slot table.
static_assert(MAX_OWNERS <= std::numeric_limits<std::uint16_t>::max());
static_assert(MAX_TAGS <= std::numeric_limits<std::uint32_t>::max());

// `value` rounded up to a multiple of OFFSET_ALIGNMENT.
constexpr std::size_t aligned(std::size_t value) noexcept {
    return (value + OFFSET_ALIGNMENT - 1) / OFFSET_ALIGNMENT * OFFSET_ALIGNMENT;
}

// Where each part of the region starts. All values are bytes from the start
// of the file and multiples of OFFSET_ALIGNMENT.
struct Offsets {
    std::size_t owner_table = 0;
    std::size_t process_table = 0;
    std::size_t queue_table = 0;
    std::size_t directory = 0;
    std::vector<std::size_t> slot_tables;  // one for each owner
    std::size_t name_pool = 0;
    std::size_t name_pool_size = 0;  // bytes of names, without padding
    std::size_t total = 0;
};

// The counts are bounded by the limits of the contract, so the sums stay far
// below the range of std::size_t.
Offsets place(const TagSet& tag_set, const ResolvedTagSet& resolved) {
    Offsets offsets;
    offsets.owner_table = sizeof(RegionHeader) + sizeof(GlobalHeader);
    offsets.process_table =
        offsets.owner_table + (resolved.owners.size() * sizeof(OwnerEntry));
    offsets.queue_table = offsets.process_table +
                          (resolved.processes.size() * sizeof(ProcessEntry));
    offsets.directory =
        offsets.queue_table + (resolved.queues.size() * sizeof(QueueEntry));
    std::size_t next =
        offsets.directory + (resolved.tags.size() * sizeof(DirectoryEntry));
    offsets.slot_tables.reserve(resolved.owners.size());
    for (const ResolvedOwner& owner : resolved.owners) {
        offsets.slot_tables.push_back(next);
        next += aligned(owner.tag_count * sizeof(std::uint32_t));
    }
    offsets.name_pool = next;
    for (const TagDefinition& tag : tag_set.tags) {
        offsets.name_pool_size += tag.name.size();
    }
    offsets.total = aligned(offsets.name_pool + offsets.name_pool_size);
    return offsets;
}

// Writes into the image. Every write is checked against the size of the
// image; a write outside it is a defect of the builder and ends the process.
class ImageWriter {
public:
    explicit ImageWriter(std::size_t size) : bytes_(size) {}

    // Stores the object representation of `value` at `offset`.
    template <typename Value>
        requires IS_SHARED_STRUCT<Value> || std::is_integral_v<Value>
    void put(std::size_t offset, const Value& value) {
        const auto representation =
            std::bit_cast<std::array<std::byte, sizeof(Value)>>(value);
        std::ranges::copy(representation, at(offset, sizeof(Value)).begin());
    }

    // Stores the characters of `text` at `offset`.
    void put_text(std::size_t offset, std::string_view text) {
        std::ranges::transform(
            text, at(offset, text.size()).begin(),
            [](char character) { return static_cast<std::byte>(character); });
    }

    // Hands over the finished image. The writer is empty afterwards.
    [[nodiscard]] std::vector<std::byte> take() { return std::move(bytes_); }

private:
    [[nodiscard]] std::span<std::byte> at(std::size_t offset,
                                          std::size_t size) {
        if (offset > bytes_.size() || size > bytes_.size() - offset) {
            panic("header builder: write outside the image");
        }
        return std::span(bytes_).subspan(offset, size);
    }

    std::vector<std::byte> bytes_;
};

// Returns `name` as a NUL-padded name field. A name that does not leave room
// for a terminating NUL was let through validation by a defect.
std::array<char, NAME_FIELD_SIZE> name_field(std::string_view name) {
    if (name.size() >= NAME_FIELD_SIZE) {
        panic("header builder: name too long for a name field");
    }
    std::array<char, NAME_FIELD_SIZE> field{};
    std::ranges::copy(name, field.begin());
    return field;
}

void write_headers(ImageWriter& image, const TagSet& tag_set,
                   const ResolvedTagSet& resolved, const Offsets& offsets,
                   const HeaderParameters& parameters, const ConfigHash& hash) {
    RegionHeader region;
    region.magic = REGION_MAGIC;
    region.layout_version = LAYOUT_VERSION;
    region.kind = to_raw(RegionKind::HEADER);
    region.generation = parameters.generation;
    region.size_bytes = offsets.total;
    region.owner_index = 0;
    region.queue_id = NO_QUEUE_ID;
    region.ready = 0;  // set by scada-tagd once the file is complete
    image.put(0, region);

    GlobalHeader global;
    global.state = to_raw(GenerationState::BUILDING);
    // The counts are within the limits of the contract, which fit the fields
    // (static_asserts above and in tag_set.cpp).
    global.tag_count = static_cast<std::uint32_t>(resolved.tags.size());
    global.owner_count = static_cast<std::uint16_t>(resolved.owners.size());
    global.process_count =
        static_cast<std::uint16_t>(resolved.processes.size());
    global.queue_count = static_cast<std::uint16_t>(resolved.queues.size());
    global.heartbeat_check_ms = tag_set.heartbeat_check_ms;
    global.owner_table_offset = offsets.owner_table;
    global.process_table_offset = offsets.process_table;
    global.queue_table_offset = offsets.queue_table;
    global.directory_offset = offsets.directory;
    global.name_pool_offset = offsets.name_pool;
    global.name_pool_size = offsets.name_pool_size;
    global.created_time_ns = parameters.created_time_ns;
    global.config_hash = hash;
    image.put(sizeof(RegionHeader), global);
}

void write_tables(ImageWriter& image, const TagSet& tag_set,
                  const ResolvedTagSet& resolved, const Offsets& offsets) {
    for (std::size_t i = 0; i < resolved.owners.size(); ++i) {
        const ResolvedOwner& owner = resolved.owners[i];
        const OwnerDefinition& definition = tag_set.owners.at(owner.definition);
        OwnerEntry entry;
        entry.name = name_field(definition.name);
        entry.uid =
            tag_set.processes.at(resolved.processes.at(owner.process_index))
                .uid;
        entry.tag_count = owner.tag_count;
        entry.string_slots = owner.string_slots;
        entry.change_ring_capacity = definition.change_ring_capacity;
        entry.process_index = owner.process_index;
        entry.slot_table_offset = offsets.slot_tables.at(i);
        image.put(offsets.owner_table + (i * sizeof(OwnerEntry)), entry);
    }

    for (std::size_t i = 0; i < resolved.processes.size(); ++i) {
        const ProcessDefinition& definition =
            tag_set.processes.at(resolved.processes[i]);
        ProcessEntry entry;
        entry.name = name_field(definition.name);
        entry.uid = definition.uid;
        entry.heartbeat_timeout_ms = definition.heartbeat_timeout_ms;
        entry.flags = definition.realtime ? to_raw(ProcessFlag::REALTIME) : 0;
        image.put(offsets.process_table + (i * sizeof(ProcessEntry)), entry);
    }

    for (std::size_t i = 0; i < resolved.queues.size(); ++i) {
        const ResolvedQueue& queue = resolved.queues[i];
        const QueueDefinition& definition = tag_set.queues.at(queue.definition);
        QueueEntry entry;
        entry.name = name_field(definition.name);
        // Below MAX_QUEUES, which fits 16 bits.
        entry.queue_id = static_cast<std::uint16_t>(i);
        entry.kind = to_raw(queue.kind);
        entry.producer_process = queue.producer_process;
        entry.consumer_process = queue.consumer_process;
        entry.capacity = definition.capacity;
        entry.record_size = queue_record_size(queue.kind);
        image.put(offsets.queue_table + (i * sizeof(QueueEntry)), entry);
    }
}

// One tag with what the directory is sorted by.
struct DirectoryItem {
    std::uint64_t name_hash = 0;
    std::string_view name;             // refers to the TagSet being built from
    const ResolvedTag* tag = nullptr;  // refers to the ResolvedTagSet
};

// Writes the directory, sorted by (name_hash, name), with the slot tables
// and the name pool, whose names are in directory order.
void write_directory(ImageWriter& image, const TagSet& tag_set,
                     const ResolvedTagSet& resolved, const Offsets& offsets) {
    std::vector<DirectoryItem> items;
    items.reserve(resolved.tags.size());
    for (const ResolvedTag& tag : resolved.tags) {
        const std::string_view name = tag_set.tags.at(tag.definition).name;
        items.push_back(DirectoryItem{fnv1a_64(name), name, &tag});
    }
    std::ranges::sort(
        items, [](const DirectoryItem& left, const DirectoryItem& right) {
            return std::tie(left.name_hash, left.name) <
                   std::tie(right.name_hash, right.name);
        });

    std::size_t name_offset = 0;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const DirectoryItem& item = items[i];
        const ResolvedTag& tag = *item.tag;
        const std::optional<std::uint32_t> handle =
            pack_handle(tag.owner_index, tag.slot);
        if (!handle.has_value()) {
            panic("header builder: tag beyond the handle limits");
        }
        DirectoryEntry entry;
        entry.name_hash = item.name_hash;
        entry.handle = *handle;
        // The pool is below 2^32 bytes and a name below 2^16
        // (static_asserts above).
        entry.name_offset = static_cast<std::uint32_t>(name_offset);
        entry.name_len = static_cast<std::uint16_t>(item.name.size());
        entry.type = to_raw(tag_set.tags.at(tag.definition).type);
        entry.flags = tag.flags;
        image.put(offsets.directory + (i * sizeof(DirectoryEntry)), entry);

        // At most MAX_TAGS entries, so the index fits 32 bits.
        image.put(offsets.slot_tables.at(tag.owner_index) +
                      (tag.slot * sizeof(std::uint32_t)),
                  static_cast<std::uint32_t>(i));

        image.put_text(offsets.name_pool + name_offset, item.name);
        name_offset += item.name.size();
    }
}

}  // namespace

Result<HeaderImage> build_header(const TagSet& tag_set,
                                 const HeaderParameters& parameters,
                                 std::string& detail) {
    const Result<ResolvedTagSet> resolved = resolve(tag_set, detail);
    if (!resolved.ok()) {
        return resolved.status();
    }
    const ResolvedTagSet& tags = resolved.value();
    const Offsets offsets = place(tag_set, tags);

    HeaderImage image;
    image.config_hash = config_hash_of(tag_set, tags);
    ImageWriter writer(offsets.total);
    write_headers(writer, tag_set, tags, offsets, parameters,
                  image.config_hash);
    write_tables(writer, tag_set, tags, offsets);
    write_directory(writer, tag_set, tags, offsets);
    image.bytes = writer.take();
    return image;
}

}  // namespace scada::tag
