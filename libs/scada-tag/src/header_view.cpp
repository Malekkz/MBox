#include "header_view.h"

#include "layout.h"
#include "name_hash.h"
#include "region_file.h"
#include "resolved_tag_set.h"
#include "shared_memory.h"

#include "scada/common/fd.h"
#include "scada/common/mapped_region.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/handle.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace scada::tag {

namespace {

constexpr std::size_t GLOBAL_HEADER_OFFSET = sizeof(RegionHeader);
constexpr std::size_t OFFSET_ALIGNMENT = 8;
constexpr std::size_t SLOT_TABLE_ENTRY_SIZE = sizeof(std::uint32_t);

Status corrupt(std::string& detail, std::string what) {
    detail = std::move(what);
    return {StatusCode::DATA_CORRUPT, "the header region is not valid"};
}

// The name in a NUL-padded name field: 1 to 31 bytes of the name grammar,
// followed by NUL bytes only. Nothing otherwise.
std::optional<std::string> name_of(
    const std::array<char, NAME_FIELD_SIZE>& field) {
    const std::string_view text(field.data(), field.size());
    const std::size_t end = text.find('\0');
    if (end == std::string_view::npos ||
        text.find_first_not_of('\0', end) != std::string_view::npos) {
        return std::nullopt;
    }
    std::string name(text.substr(0, end));
    if (!is_valid_name(name, MAX_SHORT_NAME_LENGTH)) {
        return std::nullopt;
    }
    return name;
}

// True if a table of `count` entries of `entry_size` bytes lies inside a
// region of `region_size` bytes at `offset`, which is a multiple of 8. The
// offset of an empty table may be the end of the region. `count` and
// `entry_size` are small enough that the product does not overflow.
bool table_fits(std::size_t region_size, std::uint64_t offset,
                std::uint64_t count, std::uint64_t entry_size) noexcept {
    return offset % OFFSET_ALIGNMENT == 0 && offset <= region_size &&
           count * entry_size <= region_size - offset;
}

Status read_processes(std::span<std::byte> bytes, const GlobalHeader& global,
                      HeaderContents& contents, std::string& detail) {
    contents.processes.reserve(global.process_count);
    for (std::size_t i = 0; i < global.process_count; ++i) {
        ProcessInfo info;
        // In range: the table was checked to lie inside the region.
        info.entry_offset = static_cast<std::size_t>(
            global.process_table_offset + (i * sizeof(ProcessEntry)));
        // Field by field: `down` changes while it is read.
        const auto& entry = shared_at<ProcessEntry>(bytes, info.entry_offset);
        std::optional<std::string> name = name_of(entry.name);
        if (!name.has_value() || entry.heartbeat_timeout_ms == 0 ||
            entry.uid > MAX_UID || (entry.flags & ~PROCESS_FLAGS_MASK) != 0) {
            return corrupt(detail, "process " + std::to_string(i) +
                                       " of the header is not valid");
        }
        info.name = std::move(*name);
        info.uid = entry.uid;
        info.heartbeat_timeout_ms = entry.heartbeat_timeout_ms;
        info.realtime = (entry.flags & to_raw(ProcessFlag::REALTIME)) != 0;
        contents.processes.push_back(std::move(info));
    }
    return {};
}

Status read_owners(std::span<std::byte> bytes, const GlobalHeader& global,
                   HeaderContents& contents, std::string& detail) {
    contents.owners.reserve(global.owner_count);
    std::uint64_t tags = 0;
    for (std::size_t i = 0; i < global.owner_count; ++i) {
        const auto entry = shared_at<OwnerEntry>(
            bytes, static_cast<std::size_t>(global.owner_table_offset +
                                            (i * sizeof(OwnerEntry))));
        std::optional<std::string> name = name_of(entry.name);
        const bool valid =
            name.has_value() && entry.process_index < global.process_count &&
            entry.tag_count <= MAX_TAGS_PER_OWNER &&
            entry.string_slots <= entry.tag_count &&
            std::has_single_bit(entry.change_ring_capacity) &&
            entry.change_ring_capacity >= MIN_CHANGE_RING_CAPACITY &&
            entry.change_ring_capacity <= MAX_CHANGE_RING_CAPACITY &&
            table_fits(bytes.size(), entry.slot_table_offset, entry.tag_count,
                       SLOT_TABLE_ENTRY_SIZE);
        if (!valid ||
            entry.uid != contents.processes.at(entry.process_index).uid) {
            return corrupt(detail, "owner " + std::to_string(i) +
                                       " of the header is not valid");
        }
        OwnerInfo info;
        info.name = std::move(*name);
        info.uid = entry.uid;
        info.tag_count = entry.tag_count;
        info.string_slots = entry.string_slots;
        info.change_ring_capacity = entry.change_ring_capacity;
        info.process_index = entry.process_index;
        // In range: checked by table_fits.
        info.slot_table_offset =
            static_cast<std::size_t>(entry.slot_table_offset);
        tags += entry.tag_count;
        contents.owners.push_back(std::move(info));
    }
    if (tags != global.tag_count) {
        return corrupt(detail,
                       "the tag counts of the owners do not add up to the "
                       "tag count of the header");
    }
    return {};
}

Status read_queues(std::span<std::byte> bytes, const GlobalHeader& global,
                   HeaderContents& contents, std::string& detail) {
    contents.queues.reserve(global.queue_count);
    for (std::size_t i = 0; i < global.queue_count; ++i) {
        const auto entry = shared_at<QueueEntry>(
            bytes, static_cast<std::size_t>(global.queue_table_offset +
                                            (i * sizeof(QueueEntry))));
        std::optional<std::string> name = name_of(entry.name);
        const std::optional<QueueKind> kind = queue_kind_from_raw(entry.kind);
        const bool valid = name.has_value() && kind.has_value() &&
                           entry.queue_id == i &&
                           entry.producer_process < global.process_count &&
                           entry.consumer_process < global.process_count &&
                           std::has_single_bit(entry.capacity) &&
                           entry.capacity <= MAX_QUEUE_CAPACITY;
        if (!valid || entry.record_size != queue_record_size(*kind)) {
            return corrupt(detail, "queue " + std::to_string(i) +
                                       " of the header is not valid");
        }
        QueueInfo info;
        info.name = std::move(*name);
        info.kind = *kind;
        info.producer_process = entry.producer_process;
        info.consumer_process = entry.consumer_process;
        info.capacity = entry.capacity;
        info.record_size = entry.record_size;
        contents.queues.push_back(std::move(info));
    }
    return {};
}

}  // namespace

Result<HeaderContents> read_header(std::span<std::byte> bytes,
                                   std::uint64_t generation,
                                   std::string& detail) {
    if (!fits(bytes.size(), GLOBAL_HEADER_OFFSET, sizeof(GlobalHeader))) {
        return corrupt(detail, "the header region is too small");
    }
    auto& region = shared_at<RegionHeader>(bytes, 0);
    // First, and before `ready`: these two fields have the same place in
    // every layout version (section 11.2, step 1).
    if (region.magic != REGION_MAGIC ||
        region.layout_version != LAYOUT_VERSION) {
        detail = "the header region has magic or layout version " +
                 std::to_string(region.layout_version) +
                 " that this library (layout version " +
                 std::to_string(LAYOUT_VERSION) + ") does not read";
        return Status(StatusCode::UNSUPPORTED,
                      "magic or layout version do not match");
    }
    // The other fields are complete only once `ready` is set (section 4).
    if (load_shared(region.ready, std::memory_order_acquire) != 1U) {
        detail = "the header region is not ready";
        return Status(StatusCode::UNAVAILABLE, "the header is not ready");
    }
    if (region.kind != to_raw(RegionKind::HEADER) ||
        region.generation != generation || region.size_bytes != bytes.size() ||
        region.owner_index != 0 || region.queue_id != NO_QUEUE_ID) {
        return corrupt(detail,
                       "the region header of the header region does "
                       "not match its generation and size");
    }

    // Field by field: `state` changes while it is read.
    const auto& global = shared_at<GlobalHeader>(bytes, GLOBAL_HEADER_OFFSET);
    if (global.process_count == 0 || global.process_count > MAX_PROCESSES ||
        global.owner_count > MAX_OWNERS || global.queue_count > MAX_QUEUES ||
        global.tag_count > MAX_TAGS || global.heartbeat_check_ms == 0) {
        return corrupt(detail, "a count of the header is outside the limits");
    }
    const std::size_t size = bytes.size();
    if (!table_fits(size, global.process_table_offset, global.process_count,
                    sizeof(ProcessEntry)) ||
        !table_fits(size, global.owner_table_offset, global.owner_count,
                    sizeof(OwnerEntry)) ||
        !table_fits(size, global.queue_table_offset, global.queue_count,
                    sizeof(QueueEntry)) ||
        !table_fits(size, global.directory_offset, global.tag_count,
                    sizeof(DirectoryEntry)) ||
        !table_fits(size, global.name_pool_offset, global.name_pool_size, 1)) {
        return corrupt(detail, "a table of the header lies outside the region");
    }

    HeaderContents contents;
    contents.generation = generation;
    contents.tag_count = global.tag_count;
    contents.heartbeat_check_ms = global.heartbeat_check_ms;
    contents.config_hash = global.config_hash;
    contents.created_time_ns = global.created_time_ns;
    // In range: checked by table_fits.
    contents.directory_offset =
        static_cast<std::size_t>(global.directory_offset);
    contents.name_pool_offset =
        static_cast<std::size_t>(global.name_pool_offset);
    contents.name_pool_size = static_cast<std::size_t>(global.name_pool_size);
    if (Status read = read_processes(bytes, global, contents, detail);
        !read.ok()) {
        return read;
    }
    if (Status read = read_owners(bytes, global, contents, detail);
        !read.ok()) {
        return read;
    }
    if (Status read = read_queues(bytes, global, contents, detail);
        !read.ok()) {
        return read;
    }
    return contents;
}

std::optional<TagEntry> directory_entry(std::span<std::byte> bytes,
                                        const HeaderContents& contents,
                                        std::size_t directory_index) noexcept {
    if (directory_index >= contents.tag_count) {
        return std::nullopt;
    }
    const auto entry = copy_from<DirectoryEntry>(
        bytes,
        contents.directory_offset + (directory_index * sizeof(DirectoryEntry)));
    const std::optional<HandleParts> parts = unpack_handle(entry.handle);
    const std::optional<TagType> type = tag_type_from_raw(entry.type);
    if (!parts.has_value() || !type.has_value() ||
        parts->owner_index >= contents.owners.size() ||
        parts->slot >= contents.owners[parts->owner_index].tag_count ||
        (entry.flags & ~TAG_FLAGS_MASK) != 0 ||
        !fits(contents.name_pool_size, entry.name_offset, entry.name_len)) {
        return std::nullopt;
    }
    TagEntry tag;
    tag.handle = entry.handle;
    tag.owner_index = parts->owner_index;
    tag.slot = parts->slot;
    tag.type = *type;
    tag.flags = entry.flags;
    // Inside the region: the name pool was checked to lie inside it, and
    // the name to lie inside the name pool.
    tag.name = text_at(bytes, contents.name_pool_offset + entry.name_offset,
                       entry.name_len);
    if (!is_valid_name(tag.name, MAX_TAG_NAME_LENGTH)) {
        return std::nullopt;
    }
    return tag;
}

std::optional<TagEntry> find_tag(std::span<std::byte> bytes,
                                 const HeaderContents& contents,
                                 std::string_view name) noexcept {
    const std::uint64_t hash = fnv1a_64(name);
    const auto hash_at = [&](std::size_t index) {
        return copy_from<DirectoryEntry>(
                   bytes,
                   contents.directory_offset + (index * sizeof(DirectoryEntry)))
            .name_hash;
    };
    // The first entry whose hash is not below `hash`. A directory that is
    // not sorted makes the search miss; it cannot make it leave the table.
    std::size_t first = 0;
    std::size_t count = contents.tag_count;
    while (count > 0) {
        const std::size_t half = count / 2;
        if (hash_at(first + half) < hash) {
            first += half + 1;
            count -= half + 1;
        } else {
            count = half;
        }
    }
    for (std::size_t index = first;
         index < contents.tag_count && hash_at(index) == hash; ++index) {
        const std::optional<TagEntry> entry =
            directory_entry(bytes, contents, index);
        if (entry.has_value() && entry->name == name) {
            return entry;
        }
    }
    return std::nullopt;
}

std::optional<TagEntry> slot_entry(std::span<std::byte> bytes,
                                   const HeaderContents& contents,
                                   std::size_t owner_index,
                                   std::uint32_t slot) noexcept {
    if (owner_index >= contents.owners.size()) {
        return std::nullopt;
    }
    const OwnerInfo& owner = contents.owners[owner_index];
    if (slot >= owner.tag_count) {
        return std::nullopt;
    }
    const std::size_t table_entry =
        owner.slot_table_offset + (std::size_t{slot} * SLOT_TABLE_ENTRY_SIZE);
    const std::optional<TagEntry> entry = directory_entry(
        bytes, contents, copy_from<std::uint32_t>(bytes, table_entry));
    if (!entry.has_value() || entry->owner_index != owner_index ||
        entry->slot != slot) {
        return std::nullopt;
    }
    return entry;
}

std::optional<TagType> slot_type(std::span<std::byte> bytes,
                                 const HeaderContents& contents,
                                 std::size_t owner_index,
                                 std::uint32_t slot) noexcept {
    const std::optional<TagEntry> entry =
        slot_entry(bytes, contents, owner_index, slot);
    if (!entry.has_value()) {
        return std::nullopt;
    }
    return entry->type;
}

HeaderView::HeaderView(MappedRegion memory, HeaderContents contents) noexcept
    : memory_(std::move(memory)), contents_(std::move(contents)) {}

Result<HeaderView> HeaderView::open(int dir_fd, std::uint64_t generation,
                                    std::uint32_t tagd_uid,
                                    std::string& detail) {
    const std::string name(HEADER_REGION_NAME);
    // O_NONBLOCK: a FIFO under the name must not block the open.
    const Fd file =
        open_at(dir_fd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (!file.valid()) {
        const int error = errno;
        detail = "cannot open header: " + errno_text(error);
        if (error == ENOENT) {
            return Status(StatusCode::NOT_FOUND,
                          "the generation has no header");
        }
        return error == ELOOP
                   ? Status(StatusCode::DATA_CORRUPT,
                            "a symbolic link is in the place of the header")
                   : Status(StatusCode::IO_ERROR, "cannot open the header");
    }
    struct stat info {};
    if (::fstat(file.get(), &info) != 0) {
        detail = "cannot examine header: " + errno_text(errno);
        return Status(StatusCode::IO_ERROR, "cannot examine the header");
    }
    if (!S_ISREG(info.st_mode)) {
        return corrupt(detail, "header is not a regular file");
    }
    if (info.st_uid != tagd_uid) {
        detail = "header is owned by user " + std::to_string(info.st_uid) +
                 ", not by user " + std::to_string(tagd_uid);
        return Status(StatusCode::PERMISSION_DENIED,
                      "the header has the wrong owner");
    }
    if (info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) > MAX_HEADER_BYTES ||
        static_cast<std::uint64_t>(info.st_size) <
            GLOBAL_HEADER_OFFSET + sizeof(GlobalHeader)) {
        return corrupt(detail, "header has an impossible size");
    }
    // In range: checked above.
    Result<MappedRegion> memory =
        MappedRegion::map(file.get(), static_cast<std::size_t>(info.st_size),
                          MapAccess::READ_ONLY);
    if (!memory.ok()) {
        detail = "cannot map header: " + errno_text(errno);
        return memory.status();
    }
    Result<HeaderContents> contents =
        read_header(memory.value().bytes(), generation, detail);
    if (!contents.ok()) {
        return contents.status();
    }
    return HeaderView(std::move(memory).value(), std::move(contents).value());
}

std::optional<std::uint16_t> HeaderView::find_process(
    std::string_view name) const noexcept {
    const auto found =
        std::ranges::find(contents_.processes, name, &ProcessInfo::name);
    if (found == contents_.processes.end()) {
        return std::nullopt;
    }
    // Below MAX_PROCESSES, which fits 16 bits.
    return static_cast<std::uint16_t>(found - contents_.processes.begin());
}

std::optional<TagType> HeaderView::slot_type(
    std::size_t owner_index, std::uint32_t slot) const noexcept {
    return scada::tag::slot_type(memory_.bytes(), contents_, owner_index, slot);
}

std::optional<TagEntry> HeaderView::find_tag(
    std::string_view name) const noexcept {
    return scada::tag::find_tag(memory_.bytes(), contents_, name);
}

std::optional<TagEntry> HeaderView::slot_entry(
    std::size_t owner_index, std::uint32_t slot) const noexcept {
    return scada::tag::slot_entry(memory_.bytes(), contents_, owner_index,
                                  slot);
}

std::optional<GenerationState> HeaderView::state() const noexcept {
    auto& global =
        shared_at<GlobalHeader>(memory_.bytes(), GLOBAL_HEADER_OFFSET);
    // Acquire: pairs with the release store of scada-tagd.
    return generation_state_from_raw(
        load_shared(global.state, std::memory_order_acquire));
}

bool HeaderView::retiring() const noexcept {
    auto& global =
        shared_at<GlobalHeader>(memory_.bytes(), GLOBAL_HEADER_OFFSET);
    // seq_cst: with the seq_cst store of `write_seq` in a post and of the
    // state in scada-tagd, a post that sees RUNNING after it published is
    // seen by a drain that saw RETIRING first (decision D-54).
    return load_shared(global.state, std::memory_order_seq_cst) ==
           to_raw(GenerationState::RETIRING);
}

bool HeaderView::marked_down(std::size_t process_index) const noexcept {
    if (process_index >= contents_.processes.size()) {
        return true;
    }
    auto& entry = shared_at<ProcessEntry>(
        memory_.bytes(), contents_.processes[process_index].entry_offset);
    // Acquire: pairs with the release store of scada-tagd.
    return load_shared(entry.down, std::memory_order_acquire) != 0;
}

}  // namespace scada::tag
