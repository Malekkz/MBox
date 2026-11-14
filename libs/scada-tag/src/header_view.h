#ifndef SCADA_TAG_HEADER_VIEW_H
#define SCADA_TAG_HEADER_VIEW_H

// The `header` region as a process that attaches reads it
// (docs/contracts/tag-database.md, sections 5 and 11.2). The region is
// memory written by another process: everything taken from it is checked
// against its size and the limits of the contract before use (coding
// standard, section 9). Private to libscada-tag.

#include "layout.h"

#include "scada/common/mapped_region.h"
#include "scada/common/realtime.h"
#include "scada/common/result.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scada::tag {

// The largest `header` file this library maps. A header of the limits of
// section 14 (1,000,000 tags with names of 127 bytes, 1,023 owners, 1,024
// processes, 4,096 queues) takes about 156 MiB.
inline constexpr std::uint64_t MAX_HEADER_BYTES = 256ULL * 1024 * 1024;

// One entry of the process table. The position in the table is the process
// index.
struct ProcessInfo {
    std::string name;
    std::uint32_t uid = 0;
    std::uint32_t heartbeat_timeout_ms = 0;
    bool realtime = false;
    std::size_t entry_offset = 0;  // of its ProcessEntry in the region
};

// One entry of the owner table. The position is the owner index.
struct OwnerInfo {
    std::string name;
    std::uint32_t uid = 0;
    std::uint32_t tag_count = 0;
    std::uint32_t string_slots = 0;
    std::uint32_t change_ring_capacity = 0;
    std::uint16_t process_index = 0;    // below the number of processes
    std::size_t slot_table_offset = 0;  // `tag_count` entries fit there
};

// One entry of the queue table. The position is the queue id.
struct QueueInfo {
    std::string name;
    QueueKind kind = QueueKind::EVENTS;
    std::uint16_t producer_process = 0;  // below the number of processes
    std::uint16_t consumer_process = 0;  // below the number of processes
    std::uint32_t capacity = 0;
    std::uint32_t record_size = 0;
};

// The parts of a header that do not change while the generation lives,
// copied out after validation.
struct HeaderContents {
    std::uint64_t generation = 0;
    std::uint32_t tag_count = 0;
    std::uint32_t heartbeat_check_ms = 0;
    ConfigHash config_hash{};
    std::size_t directory_offset = 0;  // `tag_count` entries fit there
    std::size_t name_pool_offset = 0;  // `name_pool_size` bytes fit there
    std::size_t name_pool_size = 0;
    std::vector<ProcessInfo> processes;
    std::vector<OwnerInfo> owners;
    std::vector<QueueInfo> queues;
};

// Validates `bytes`, the whole `header` region of the generation
// `generation`, and returns its contents.
//
// Checked: magic and layout version; `ready`; kind, generation and size of
// the RegionHeader; every count against the limits of section 14; that
// every table lies inside `bytes` at an offset that is a multiple of 8 (an
// empty table may start at the end); and every entry of the process, owner
// and queue tables: name grammar, indexes, capacities, record sizes, and
// that the tag counts of the owners add up. The tag directory and the slot
// tables are checked entry by entry when they are used (`slot_type`).
//
// `bytes` starts at an address aligned for a region (a mapping does).
//
// UNSUPPORTED if magic or layout version are not those of this library;
// UNAVAILABLE if the region is not ready; DATA_CORRUPT otherwise. On an
// error `detail` says what was found.
[[nodiscard]] Result<HeaderContents> read_header(std::span<std::byte> bytes,
                                                 std::uint64_t generation,
                                                 std::string& detail);

// The type of the tag in slot `slot` of owner `owner_index`, through the
// owner's slot table and the tag directory of the header `bytes` that
// `contents` was read from. Nothing if the owner or the slot does not
// exist, or the slot table or directory entry is not valid: an index
// outside the directory, a handle that is not this owner and slot, or a
// type that is none.
[[nodiscard]] std::optional<TagType> slot_type(std::span<std::byte> bytes,
                                               const HeaderContents& contents,
                                               std::size_t owner_index,
                                               std::uint32_t slot) noexcept;

// One entry of the tag directory, validated.
struct TagEntry {
    std::uint32_t handle = 0;  // of an owner and a slot that exist
    std::uint16_t owner_index = 0;
    std::uint32_t slot = 0;
    TagType type = TagType::BOOL;
    std::uint8_t flags = 0;  // TagFlag bits, and no other
    std::string_view name;   // in the name pool of `bytes`
};

// The entry `directory_index` of the tag directory of the header `bytes`
// that `contents` was read from. Nothing if the index is outside the
// directory or the entry is not valid: a handle of no owner and slot of
// this generation, a type that is none, a flag that is none, or a name that
// lies outside the name pool or is no tag name. The name refers to `bytes`.
[[nodiscard]] std::optional<TagEntry> directory_entry(
    std::span<std::byte> bytes, const HeaderContents& contents,
    std::size_t directory_index) noexcept;

// The entry of the tag `name` (section 5: a binary search on the hash, then
// a comparison of the names). Nothing if there is no such tag or its entry
// is not valid.
[[nodiscard]] std::optional<TagEntry> find_tag(std::span<std::byte> bytes,
                                               const HeaderContents& contents,
                                               std::string_view name) noexcept;

// The entry of the tag in slot `slot` of owner `owner_index`, through the
// owner's slot table. Nothing if the owner or the slot does not exist, or
// the slot table or the entry is not valid; an entry whose handle is not
// this owner and slot is not valid.
[[nodiscard]] std::optional<TagEntry> slot_entry(std::span<std::byte> bytes,
                                                 const HeaderContents& contents,
                                                 std::size_t owner_index,
                                                 std::uint32_t slot) noexcept;

// The `header` region of one generation, mapped read-only. Move-only.
class HeaderView {
public:
    // Opens `header` in the generation directory open as `dir_fd` with the
    // checks of section 3.1 (not through a symbolic link, a regular file,
    // owned by `tagd_uid`, at most MAX_HEADER_BYTES) and validates it with
    // `read_header`. NOT_FOUND if there is no such file, PERMISSION_DENIED
    // if its owner is wrong, IO_ERROR for an error of the operating system,
    // otherwise the codes of `read_header`.
    [[nodiscard]] static Result<HeaderView> open(int dir_fd,
                                                 std::uint64_t generation,
                                                 std::uint32_t tagd_uid,
                                                 std::string& detail);

    [[nodiscard]] const HeaderContents& contents() const noexcept {
        return contents_;
    }

    // The index of the process `name`, or nothing.
    [[nodiscard]] std::optional<std::uint16_t> find_process(
        std::string_view name) const noexcept;

    // See the free function `slot_type`.
    [[nodiscard]] std::optional<TagType> slot_type(
        std::size_t owner_index, std::uint32_t slot) const noexcept;

    // See the free functions `find_tag` and `slot_entry`. The name of the
    // result refers to the mapping and is valid as long as this object.
    [[nodiscard]] std::optional<TagEntry> find_tag(
        std::string_view name) const noexcept;
    [[nodiscard]] std::optional<TagEntry> slot_entry(
        std::size_t owner_index, std::uint32_t slot) const noexcept;

    // `GlobalHeader.state` as it is now, or nothing for a value that is no
    // state.
    SCADA_RT [[nodiscard]] std::optional<GenerationState> state()
        const noexcept;

    // The `down` flag scada-tagd keeps for the process `process_index`
    // (section 10.1). True for an index that is no process.
    SCADA_RT [[nodiscard]] bool marked_down(
        std::size_t process_index) const noexcept;

private:
    HeaderView(MappedRegion memory, HeaderContents contents) noexcept;

    MappedRegion memory_;
    HeaderContents contents_;
};

}  // namespace scada::tag

#endif  // SCADA_TAG_HEADER_VIEW_H
