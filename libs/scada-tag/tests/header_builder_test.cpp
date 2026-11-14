// Unit tests for the header builder: the image it produces is read back
// through the layout structures and compared with the tag set, part by part
// (contract, section 5).

#include "scada/tag/header_builder.h"

#include "layout.h"
#include "name_hash.h"
#include "tag_set_samples.h"

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/handle.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace scada::tag;
using scada::Result;
using scada::StatusCode;

// Every offset in a region is a multiple of this (contract, section 1).
constexpr std::size_t OFFSET_UNIT = 8;

constexpr HeaderParameters PARAMETERS{.generation = 7,
                                      .created_time_ns = 1'794'000'000'000};

HeaderImage build(const TagSet& tag_set) {
    std::string detail;
    Result<HeaderImage> image = build_header(tag_set, PARAMETERS, detail);
    EXPECT_TRUE(image.ok()) << detail;
    return image.ok() ? std::move(image).value() : HeaderImage{};
}

// Reads a `Value` at `offset` of `image`, or a zero `Value` after a failed
// expectation if it does not fit.
template <typename Value>
Value get(std::span<const std::byte> image, std::size_t offset) {
    std::array<std::byte, sizeof(Value)> raw{};
    if (offset > image.size() || sizeof(Value) > image.size() - offset) {
        ADD_FAILURE() << "read of " << sizeof(Value) << " bytes at " << offset
                      << " is outside the image of " << image.size();
        return std::bit_cast<Value>(raw);
    }
    std::ranges::copy(image.subspan(offset, sizeof(Value)), raw.begin());
    return std::bit_cast<Value>(raw);
}

// The text of `length` bytes at `offset`.
std::string get_text(std::span<const std::byte> image, std::size_t offset,
                     std::size_t length) {
    std::string text;
    if (offset > image.size() || length > image.size() - offset) {
        ADD_FAILURE() << "text outside the image";
        return text;
    }
    for (const std::byte byte : image.subspan(offset, length)) {
        text.push_back(static_cast<char>(byte));
    }
    return text;
}

// The content of a NUL-padded name field. Every byte after the name must be
// NUL.
std::string name_of(const std::array<char, NAME_FIELD_SIZE>& field) {
    const std::string_view all(field.data(), field.size());
    const std::size_t length = all.find('\0');
    EXPECT_NE(length, std::string_view::npos) << "name field not terminated";
    const std::string_view name = all.substr(0, length);
    EXPECT_EQ(all.substr(name.size()),
              std::string(all.size() - name.size(), '\0'));
    return std::string(name);
}

bool all_zero(std::span<const std::uint8_t> bytes) {
    return std::ranges::all_of(bytes,
                               [](std::uint8_t byte) { return byte == 0; });
}

// Everything in the image, decoded.
struct Decoded {
    RegionHeader region;
    GlobalHeader global;
    std::vector<OwnerEntry> owners;
    std::vector<ProcessEntry> processes;
    std::vector<QueueEntry> queues;
    std::vector<DirectoryEntry> directory;
    std::vector<std::string> names;  // of the directory entries
    std::vector<std::vector<std::uint32_t>> slot_tables;  // for each owner
};

Decoded decode(std::span<const std::byte> image) {
    Decoded decoded;
    decoded.region = get<RegionHeader>(image, 0);
    decoded.global = get<GlobalHeader>(image, sizeof(RegionHeader));
    const GlobalHeader& global = decoded.global;
    for (std::size_t i = 0; i < global.owner_count; ++i) {
        decoded.owners.push_back(get<OwnerEntry>(
            image, global.owner_table_offset + (i * sizeof(OwnerEntry))));
    }
    for (std::size_t i = 0; i < global.process_count; ++i) {
        decoded.processes.push_back(get<ProcessEntry>(
            image, global.process_table_offset + (i * sizeof(ProcessEntry))));
    }
    for (std::size_t i = 0; i < global.queue_count; ++i) {
        decoded.queues.push_back(get<QueueEntry>(
            image, global.queue_table_offset + (i * sizeof(QueueEntry))));
    }
    for (std::size_t i = 0; i < global.tag_count; ++i) {
        const auto entry = get<DirectoryEntry>(
            image, global.directory_offset + (i * sizeof(DirectoryEntry)));
        decoded.directory.push_back(entry);
        EXPECT_LE(std::uint64_t{entry.name_offset} + entry.name_len,
                  global.name_pool_size);
        decoded.names.push_back(
            get_text(image, global.name_pool_offset + entry.name_offset,
                     entry.name_len));
    }
    for (const OwnerEntry& owner : decoded.owners) {
        std::vector<std::uint32_t> table;
        for (std::size_t slot = 0; slot < owner.tag_count; ++slot) {
            table.push_back(get<std::uint32_t>(
                image,
                owner.slot_table_offset + (slot * sizeof(std::uint32_t))));
        }
        decoded.slot_tables.push_back(std::move(table));
    }
    return decoded;
}

TEST(HeaderBuilder, WritesTheRegionHeader) {
    const HeaderImage image = build(samples::sample());
    const RegionHeader region = decode(image.bytes).region;
    EXPECT_EQ(region.magic, REGION_MAGIC);
    EXPECT_EQ(region.layout_version, LAYOUT_VERSION);
    EXPECT_EQ(region.kind, to_raw(RegionKind::HEADER));
    EXPECT_EQ(region.generation, PARAMETERS.generation);
    EXPECT_EQ(region.size_bytes, image.bytes.size());
    EXPECT_EQ(region.owner_index, 0);
    EXPECT_EQ(region.queue_id, NO_QUEUE_ID);
    // Not usable until scada-tagd says so (contract, section 11.1).
    EXPECT_EQ(region.ready, 0U);
    EXPECT_TRUE(all_zero(region.reserved));
}

TEST(HeaderBuilder, WritesTheGlobalHeader) {
    const TagSet tag_set = samples::sample();
    const HeaderImage image = build(tag_set);
    const GlobalHeader global = decode(image.bytes).global;
    EXPECT_EQ(global.state, to_raw(GenerationState::BUILDING));
    EXPECT_EQ(global.tag_count, tag_set.tags.size());
    EXPECT_EQ(global.owner_count, tag_set.owners.size());
    EXPECT_EQ(global.process_count, tag_set.processes.size());
    EXPECT_EQ(global.queue_count, tag_set.queues.size());
    EXPECT_EQ(global.heartbeat_check_ms, DEFAULT_HEARTBEAT_CHECK_MS);
    EXPECT_EQ(global.created_time_ns, PARAMETERS.created_time_ns);
    EXPECT_EQ(global.reserved0, 0);
    EXPECT_EQ(global.reserved1, 0U);
    EXPECT_TRUE(all_zero(global.reserved));
}

TEST(HeaderBuilder, StoresTheConfigHash) {
    const TagSet tag_set = samples::sample();
    const HeaderImage image = build(tag_set);
    std::string detail;
    const Result<ConfigHash> hash = config_hash(tag_set, detail);
    ASSERT_TRUE(hash.ok()) << detail;
    EXPECT_EQ(image.config_hash, hash.value());
    EXPECT_EQ(decode(image.bytes).global.config_hash, hash.value());
}

// Section 5: RegionHeader, GlobalHeader, owner table, process table, queue
// table, tag directory, slot tables, name pool, in that order and without
// gaps other than padding to a multiple of 8.
TEST(HeaderBuilder, PlacesThePartsInTheOrderOfTheContract) {
    const TagSet tag_set = samples::sample();
    const HeaderImage image = build(tag_set);
    const Decoded decoded = decode(image.bytes);
    const GlobalHeader& global = decoded.global;

    // Where each part starts and where the file ends, as written...
    std::vector<std::uint64_t> actual{
        global.owner_table_offset, global.process_table_offset,
        global.queue_table_offset, global.directory_offset};
    for (const OwnerEntry& owner : decoded.owners) {
        actual.push_back(owner.slot_table_offset);
    }
    actual.push_back(global.name_pool_offset);
    actual.push_back(image.bytes.size());

    // ...and as the sizes of the parts before it give.
    std::vector<std::uint64_t> expected;
    std::uint64_t next = sizeof(RegionHeader) + sizeof(GlobalHeader);
    for (const std::size_t part_size :
         {tag_set.owners.size() * sizeof(OwnerEntry),
          tag_set.processes.size() * sizeof(ProcessEntry),
          tag_set.queues.size() * sizeof(QueueEntry),
          tag_set.tags.size() * sizeof(DirectoryEntry)}) {
        expected.push_back(next);
        next += part_size;
    }
    // No owner of the sample has more than two tags, so each slot table is
    // one 8-byte unit.
    for (std::size_t i = 0; i < tag_set.owners.size(); ++i) {
        expected.push_back(next);
        next += OFFSET_UNIT;
    }
    expected.push_back(next);
    std::size_t name_bytes = 0;
    for (const TagDefinition& tag : tag_set.tags) {
        name_bytes += tag.name.size();
    }
    next += name_bytes;
    expected.push_back((next + OFFSET_UNIT - 1) / OFFSET_UNIT * OFFSET_UNIT);

    EXPECT_EQ(actual, expected);
    EXPECT_EQ(global.name_pool_size, name_bytes);
}

// Contract, section 1: every offset in a region is a multiple of 8.
TEST(HeaderBuilder, AlignsEveryOffset) {
    TagSet tag_set = samples::sample();
    constexpr std::size_t EXTRA_TAGS = 3;  // makes a slot table of odd length
    samples::add_tags(tag_set, "io", "extra", EXTRA_TAGS);
    const HeaderImage image = build(tag_set);
    const Decoded decoded = decode(image.bytes);
    const GlobalHeader& global = decoded.global;
    std::vector<std::uint64_t> offsets{
        global.owner_table_offset, global.process_table_offset,
        global.queue_table_offset, global.directory_offset,
        global.name_pool_offset,   image.bytes.size()};
    for (const OwnerEntry& owner : decoded.owners) {
        offsets.push_back(owner.slot_table_offset);
    }
    for (const std::uint64_t offset : offsets) {
        EXPECT_EQ(offset % OFFSET_UNIT, 0U);
        EXPECT_LE(offset, image.bytes.size());
    }
}

// One line of text for each entry of a table, so that a whole table is
// compared, and printed on failure, at once.
std::vector<std::string> describe_processes(const Decoded& decoded) {
    std::vector<std::string> lines;
    lines.reserve(decoded.processes.size());
    for (const ProcessEntry& process : decoded.processes) {
        lines.push_back(
            name_of(process.name) + " uid=" + std::to_string(process.uid) +
            " timeout=" + std::to_string(process.heartbeat_timeout_ms) +
            " flags=" + std::to_string(process.flags) +
            " down=" + std::to_string(process.down));
    }
    return lines;
}

std::vector<std::string> describe_owners(const Decoded& decoded) {
    std::vector<std::string> lines;
    lines.reserve(decoded.owners.size());
    for (const OwnerEntry& owner : decoded.owners) {
        lines.push_back(name_of(owner.name) +
                        " uid=" + std::to_string(owner.uid) +
                        " process=" + std::to_string(owner.process_index) +
                        " tags=" + std::to_string(owner.tag_count) +
                        " strings=" + std::to_string(owner.string_slots) +
                        " ring=" + std::to_string(owner.change_ring_capacity));
    }
    return lines;
}

std::vector<std::string> describe_queues(const Decoded& decoded) {
    std::vector<std::string> lines;
    lines.reserve(decoded.queues.size());
    for (const QueueEntry& queue : decoded.queues) {
        lines.push_back(name_of(queue.name) +
                        " id=" + std::to_string(queue.queue_id) +
                        " kind=" + std::to_string(queue.kind) +
                        " producer=" + std::to_string(queue.producer_process) +
                        " consumer=" + std::to_string(queue.consumer_process) +
                        " capacity=" + std::to_string(queue.capacity) +
                        " record=" + std::to_string(queue.record_size));
    }
    return lines;
}

// Processes are sorted by name: control is 0, io is 1, journal is 2. An
// owner has the uid of its process. A queue's id is its position, its kind
// comes from its name (6 EVENTS, 7 CMD, 8 RESULT) and its record size from
// its kind.
TEST(HeaderBuilder, WritesTheTablesInNameOrder) {
    const Decoded decoded = decode(build(samples::sample()).bytes);
    EXPECT_EQ(describe_processes(decoded),
              (std::vector<std::string>{
                  "control uid=992 timeout=500 flags=1 down=0",
                  "io uid=991 timeout=500 flags=1 down=0",
                  "journal uid=993 timeout=3000 flags=0 down=0",
              }));
    EXPECT_EQ(describe_owners(decoded),
              (std::vector<std::string>{
                  "control.fast uid=992 process=0 tags=1 strings=0 ring=65536",
                  "control.slow uid=992 process=0 tags=2 strings=1 ring=65536",
                  "io uid=991 process=1 tags=2 strings=0 ring=1024",
              }));
    EXPECT_EQ(describe_queues(decoded),
              (std::vector<std::string>{
                  "cmd.io.control.fast id=0 kind=7 producer=0 consumer=1 "
                  "capacity=1024 record=64",
                  "events.control.fast id=1 kind=6 producer=0 consumer=2 "
                  "capacity=4096 record=256",
                  "result.io.control.fast id=2 kind=8 producer=1 consumer=0 "
                  "capacity=1024 record=32",
              }));
}

// The TagFlag bits a tag definition stands for.
std::uint8_t flags_of(const TagDefinition& tag) {
    std::uint8_t flags = 0;
    if (tag.retain) {
        flags |= to_raw(TagFlag::RETAIN);
    }
    if (tag.writable_by_command) {
        flags |= to_raw(TagFlag::WRITABLE_BY_COMMAND);
    }
    if (tag.retain_now) {
        flags |= to_raw(TagFlag::RETAIN_NOW);
    }
    return flags;
}

// The handle of directory entry `index` names owner `owner` and one of its
// slots, and the slot table of that owner leads back to the entry.
void expect_handle_matches(const Decoded& decoded, std::size_t index,
                           std::string_view owner) {
    const std::optional<HandleParts> parts =
        unpack_handle(decoded.directory.at(index).handle);
    if (!parts.has_value() ||
        parts.value().owner_index >= decoded.owners.size()) {
        ADD_FAILURE() << "entry " << index << " has no valid handle";
        return;
    }
    const std::size_t owner_index = parts.value().owner_index;
    EXPECT_EQ(name_of(decoded.owners.at(owner_index).name), owner);
    const auto& table = decoded.slot_tables.at(owner_index);
    ASSERT_LT(parts.value().slot, table.size());
    EXPECT_EQ(table.at(parts.value().slot), index);
}

// Checks directory entry `index` against the tag it names.
void expect_entry_matches(const Decoded& decoded, std::size_t index,
                          const TagDefinition& tag) {
    const DirectoryEntry& entry = decoded.directory.at(index);
    EXPECT_EQ(entry.name_hash, fnv1a_64(tag.name));
    EXPECT_EQ(entry.type, to_raw(tag.type));
    EXPECT_EQ(entry.flags, flags_of(tag)) << tag.name;
    EXPECT_TRUE(all_zero(entry.reserved));
    expect_handle_matches(decoded, index, tag.owner);
}

// The directory is sorted by (name_hash, name), and the name pool holds the
// names in directory order without gaps.
void expect_directory_sorted(const Decoded& decoded) {
    std::size_t expected_name_offset = 0;
    for (std::size_t i = 0; i < decoded.directory.size(); ++i) {
        const DirectoryEntry& entry = decoded.directory.at(i);
        EXPECT_EQ(entry.name_offset, expected_name_offset);
        expected_name_offset += entry.name_len;
        if (i == 0) {
            continue;
        }
        const DirectoryEntry& previous = decoded.directory.at(i - 1);
        EXPECT_LT(std::tie(previous.name_hash, decoded.names.at(i - 1)),
                  std::tie(entry.name_hash, decoded.names.at(i)));
    }
    EXPECT_EQ(decoded.global.name_pool_size, expected_name_offset);
}

// The slots of each owner are its tags in name order: reading the names
// through the slot table gives the sorted list of the owner's tag names.
void expect_slots_in_name_order(const TagSet& tag_set, const Decoded& decoded) {
    for (std::size_t owner = 0; owner < decoded.owners.size(); ++owner) {
        const std::string owner_name = name_of(decoded.owners.at(owner).name);
        std::vector<std::string> expected;
        for (const TagDefinition& tag : tag_set.tags) {
            if (tag.owner == owner_name) {
                expected.push_back(tag.name);
            }
        }
        std::ranges::sort(expected);
        std::vector<std::string> by_slot;
        for (const std::uint32_t index : decoded.slot_tables.at(owner)) {
            ASSERT_LT(index, decoded.names.size());
            by_slot.push_back(decoded.names.at(index));
        }
        EXPECT_EQ(by_slot, expected) << owner_name;
    }
}

// Checks the directory, slot tables and name pool of `image` against
// `tag_set`.
void expect_directory_matches(const TagSet& tag_set,
                              std::span<const std::byte> image) {
    const Decoded decoded = decode(image);
    ASSERT_EQ(decoded.directory.size(), tag_set.tags.size());
    std::map<std::string, const TagDefinition*> by_name;
    for (const TagDefinition& tag : tag_set.tags) {
        by_name[tag.name] = &tag;
    }
    for (std::size_t i = 0; i < decoded.directory.size(); ++i) {
        const auto found = by_name.find(decoded.names.at(i));
        ASSERT_NE(found, by_name.end()) << decoded.names.at(i);
        expect_entry_matches(decoded, i, *found->second);
    }
    expect_directory_sorted(decoded);
    expect_slots_in_name_order(tag_set, decoded);
}

TEST(HeaderBuilder, WritesTheDirectorySlotTablesAndNamePool) {
    const TagSet tag_set = samples::sample();
    expect_directory_matches(tag_set, build(tag_set).bytes);
}

// The flags and types of the sample, spelled out as numbers once: RETAIN is
// 1, WRITABLE_BY_COMMAND 2, RETAIN_NOW 4.
TEST(HeaderBuilder, WritesTypeAndFlagsOfEachTag) {
    const Decoded decoded = decode(build(samples::sample()).bytes);
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < decoded.directory.size(); ++i) {
        lines.push_back(
            decoded.names.at(i) +
            " type=" + std::to_string(decoded.directory.at(i).type) +
            " flags=" + std::to_string(decoded.directory.at(i).flags));
    }
    std::ranges::sort(lines);
    EXPECT_EQ(lines, (std::vector<std::string>{
                         "plant.pump1.label type=5 flags=0",
                         "plant.pump1.mode type=2 flags=5",
                         "plant.pump1.running type=1 flags=0",
                         "plant.pump1.speed type=4 flags=2",
                         "plant.pump1.starts type=3 flags=1",
                     }));
}

TEST(HeaderBuilder, HandlesManyTagsAcrossSeveralOwners) {
    constexpr std::size_t OWNERS = 7;
    constexpr std::size_t TAGS_PER_OWNER = 300;
    TagSet tag_set;
    tag_set.processes = {{.name = "p", .uid = 1}};
    for (std::size_t owner = 0; owner < OWNERS; ++owner) {
        const std::string name = "p.w" + std::to_string(owner);
        tag_set.owners.push_back({.name = name, .process = "p"});
        samples::add_tags(tag_set, name,
                          "area" + std::to_string(owner) + ".point",
                          TAGS_PER_OWNER + owner);
    }
    expect_directory_matches(tag_set, build(tag_set).bytes);
}

// Acceptance: the same tag set in a different order yields the same
// config_hash and the same directory. The whole image is the same.
TEST(HeaderBuilder, GivesTheSameImageForAnyOrderOfTheEntries) {
    constexpr std::size_t EXTRA_TAGS = 200;
    constexpr std::size_t ORDERS = 12;
    TagSet tag_set = samples::sample();
    samples::add_tags(tag_set, "io", "plant.extra", EXTRA_TAGS);
    const HeaderImage expected = build(tag_set);
    for (std::size_t step = 1; step <= ORDERS; ++step) {
        const HeaderImage image = build(samples::reordered(tag_set, step));
        EXPECT_EQ(image.config_hash, expected.config_hash) << "order " << step;
        EXPECT_TRUE(image.bytes == expected.bytes) << "order " << step;
    }
}

// Generation and creation time are in the header but not in the hash.
TEST(HeaderBuilder, KeepsTheParametersOutOfTheHash) {
    constexpr HeaderParameters OTHER{.generation = 8, .created_time_ns = 5};
    const TagSet tag_set = samples::sample();
    std::string detail;
    const Result<HeaderImage> other = build_header(tag_set, OTHER, detail);
    ASSERT_TRUE(other.ok()) << detail;
    const HeaderImage image = build(tag_set);
    EXPECT_EQ(other.value().config_hash, image.config_hash);
    EXPECT_FALSE(other.value().bytes == image.bytes);
    const Decoded decoded = decode(other.value().bytes);
    EXPECT_EQ(decoded.region.generation, OTHER.generation);
    EXPECT_EQ(decoded.global.created_time_ns, OTHER.created_time_ns);
}

TEST(HeaderBuilder, BuildsTheEmptyTagSet) {
    const HeaderImage image = build(TagSet{});
    EXPECT_EQ(image.bytes.size(), sizeof(RegionHeader) + sizeof(GlobalHeader));
    const Decoded decoded = decode(image.bytes);
    EXPECT_EQ(decoded.region.size_bytes, image.bytes.size());
    EXPECT_EQ(decoded.global.tag_count, 0U);
    EXPECT_EQ(decoded.global.name_pool_offset, image.bytes.size());
    EXPECT_EQ(decoded.global.name_pool_size, 0U);
}

// A process without tags appears only in the process table; an owner
// without tags has an empty slot table.
TEST(HeaderBuilder, AcceptsProcessesAndOwnersWithoutTags) {
    TagSet tag_set = samples::one_owner();
    tag_set.processes.push_back({.name = "api", .uid = samples::UID_OTHER});
    const Decoded decoded = decode(build(tag_set).bytes);
    ASSERT_EQ(decoded.owners.size(), 1U);
    EXPECT_EQ(decoded.processes.size(), 2U);
    EXPECT_EQ(decoded.owners[0].tag_count, 0U);
    EXPECT_EQ(decoded.owners[0].slot_table_offset,
              decoded.global.name_pool_offset);
}

TEST(HeaderBuilder, RefusesAnInvalidTagSetWithTheReason) {
    TagSet tag_set = samples::sample();
    tag_set.tags.push_back({.name = "orphan", .owner = "nobody"});
    std::string detail;
    const Result<HeaderImage> image = build_header(tag_set, PARAMETERS, detail);
    EXPECT_EQ(image.status().code(), StatusCode::NOT_FOUND);
    EXPECT_EQ(detail,
              "tag 'orphan': owner 'nobody' is not an owner of the tag set");
}

}  // namespace
