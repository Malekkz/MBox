// The tag functions of Attachment (docs/contracts/tag-database.md, sections
// 5.1, 6, 7 and 13): resolve, info, read and write. The sequence lock
// itself is in tag_record.cpp.

#include "attachment.h"
#include "clock_check.h"
#include "header_view.h"
#include "layout.h"
#include "peer_regions.h"
#include "proc_region.h"
#include "shared_memory.h"
#include "tag_record.h"
#include "utf8.h"

#include "scada/common/clock.h"
#include "scada/common/panic.h"
#include "scada/tag/handle.h"
#include "scada/tag/liveness.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace scada::tag {

namespace {

SCADA_RT constexpr std::size_t record_offset(std::uint32_t slot) noexcept {
    return sizeof(RegionHeader) + (std::size_t{slot} * sizeof(TagRecord));
}

SCADA_RT constexpr std::size_t string_slot_offset(
    std::uint64_t string_slot) noexcept {
    // In range: the caller has compared the number with the slot count.
    return sizeof(RegionHeader) +
           (static_cast<std::size_t>(string_slot) * sizeof(StringSlot));
}

// The time from `update_mono_ns` to `now_mono_ns`, never negative: a
// record written after the clock was read has the age 0. The stored time
// is input, so the difference is taken without signed overflow.
SCADA_RT std::int64_t age_ns(std::int64_t now_mono_ns,
                             std::int64_t update_mono_ns) noexcept {
    if (update_mono_ns >= now_mono_ns) {
        return 0;
    }
    const std::uint64_t age = static_cast<std::uint64_t>(now_mono_ns) -
                              static_cast<std::uint64_t>(update_mono_ns);
    constexpr auto LIMIT =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    // In range after the comparison.
    return static_cast<std::int64_t>(std::min(age, LIMIT));
}

// What a read gives for a tag whose record cannot be read and whose owner
// is down: no value, quality BAD with OWNER_DOWN.
SCADA_RT scada_value absent_value(TagType type,
                                  std::int64_t now_mono_ns) noexcept {
    scada_value value{};
    value.type = to_raw(type);
    value.quality = to_raw(Quality::BAD);
    value.quality_flags = to_raw(QualityFlag::OWNER_DOWN);
    value.age_ns = age_ns(now_mono_ns, 0);
    return value;
}

// True if `copy` holds what a record of a tag of type `type` may hold. A
// STRING record holds the number of its string slot, `string_slot`.
SCADA_RT bool record_valid(const RecordCopy& copy, TagType type,
                           std::uint64_t string_slot) noexcept {
    return copy.type == to_raw(type) &&
           quality_from_raw(copy.quality).has_value() &&
           quality_flags_valid(copy.quality_flags) &&
           (type != TagType::BOOL || copy.value <= 1) &&
           (type != TagType::STRING || copy.value == string_slot);
}

// The value a read returns for the validated `copy`, with the owner state
// laid over it (section 6.2).
SCADA_RT scada_value value_of(const RecordCopy& copy, bool owner_is_down,
                              std::int64_t now_mono_ns) noexcept {
    scada_value value{};
    value.type = copy.type;
    value.quality = copy.quality;
    value.quality_flags = copy.quality_flags;
    value.change_count = copy.change_count;
    value.value = copy.value;
    value.source_time_ns = copy.source_time_ns;
    value.update_time_ns = copy.update_time_ns;
    value.update_mono_ns = copy.update_mono_ns;
    value.age_ns = age_ns(now_mono_ns, copy.update_mono_ns);
    if (owner_is_down) {
        value.quality = to_raw(Quality::BAD);
        value.quality_flags |= to_raw(QualityFlag::OWNER_DOWN);
    }
    return value;
}

// What a write stores for quality, flags and source time `source_time_ns`
// at the times `time_ns` and `mono_ns` (section 6: without a device time
// the source time is the update time, and LOCAL_TIME is set).
SCADA_RT RecordUpdate update_of(std::uint8_t quality, std::uint16_t flags,
                                std::uint64_t value,
                                std::int64_t source_time_ns,
                                std::int64_t time_ns,
                                std::int64_t mono_ns) noexcept {
    RecordUpdate update;
    update.quality = quality;
    update.quality_flags = flags;
    update.value = value;
    update.source_time_ns = source_time_ns;
    update.update_time_ns = time_ns;
    update.update_mono_ns = mono_ns;
    if (source_time_ns == 0) {
        update.source_time_ns = time_ns;
        update.quality_flags |= to_raw(QualityFlag::LOCAL_TIME);
    }
    return update;
}

// Copies `text` into the NUL-padded `field`. `text` is a validated name,
// shorter than the field.
template <std::size_t Size>
void fill_name(std::array<char, Size>& field, std::string_view text) noexcept {
    field.fill('\0');
    if (text.size() >= field.size()) {
        panic("tag info: name longer than its field");
    }
    std::ranges::copy(text, field.begin());
}

}  // namespace

std::optional<Attachment::TagRef> Attachment::tag_of(
    std::uint32_t handle) const noexcept {
    const std::size_t owner_index = handle >> HANDLE_SLOT_BITS;
    const std::uint32_t slot = handle & MAX_TAGS_PER_OWNER;
    // Two comparisons (section 13.1). INVALID_HANDLE fails the first: there
    // are at most MAX_OWNERS owners.
    if (owner_index >= access_.size() ||
        slot >= access_[owner_index].tag_count) {
        return std::nullopt;
    }
    const OwnerAccess& owner = access_[owner_index];
    return TagRef{&owner, slot, types_[owner.first_type + slot]};
}

std::uint64_t Attachment::string_slot_of(const TagRef& tag) const noexcept {
    const std::span<const std::uint32_t> strings =
        std::span<const std::uint32_t>(string_tags_)
            .subspan(tag.owner->first_string, tag.owner->string_slots);
    // In range: the difference of two iterators of one span is not
    // negative.
    return static_cast<std::uint64_t>(
        std::ranges::lower_bound(strings, tag.slot) - strings.begin());
}

bool Attachment::owner_down(const OwnerAccess& owner,
                            std::int64_t now_mono_ns) const noexcept {
    // The first three rules of section 10.1, which the library evaluates
    // itself. The `down` flag of scada-tagd is the same judgement, made up
    // to one check interval earlier, and is not looked at: a flag left by a
    // scada-tagd that has stopped would keep a running owner down.
    const std::span<std::byte> proc = owner.process_index == process_index_
                                          ? proc_.bytes()
                                          : peers_.bytes(owner.proc_slot);
    return proc_liveness(proc, owner.heartbeat_timeout_ms, now_mono_ns) !=
           DownReason::NONE;
}

std::span<std::byte> Attachment::tags_of(
    const OwnerAccess& owner) const noexcept {
    return owner.own ? owner.own_tags : peers_.bytes(owner.tags_slot);
}

std::span<std::byte> Attachment::strings_of(
    const OwnerAccess& owner) const noexcept {
    if (owner.string_slots == 0) {
        return {};
    }
    return owner.own ? owner.own_strings : peers_.bytes(owner.strings_slot);
}

void Attachment::check_clock(std::int64_t now_mono_ns) noexcept {
    // Relaxed: see the declaration.
    next_clock_check_mono_ns_.store(now_mono_ns + CLOCK_CHECK_INTERVAL_NS,
                                    std::memory_order_relaxed);
    clock_syscall_.store(clock_reads_enter_kernel_now(),
                         std::memory_order_relaxed);
}

int Attachment::resolve(std::string_view name, std::uint32_t& handle) {
    handle = INVALID_HANDLE;
    if (!is_valid_name(name, MAX_TAG_NAME_LENGTH)) {
        return SCADA_E_INVALID;
    }
    const std::optional<TagEntry> entry = header_.find_tag(name);
    if (!entry.has_value()) {
        return SCADA_E_NOT_FOUND;
    }
    const OwnerAccess& owner = access_.at(entry->owner_index);
    if (!owner.own) {
        // Mapped and brought into memory here, never in a read (section
        // 11.2).
        const std::int64_t now = monotonic_ns();
        peers_.want(directory_.get(), owner.tags_slot, now);
        if (owner.string_slots != 0) {
            peers_.want(directory_.get(), owner.strings_slot, now);
        }
        if (owner.process_index != process_index_) {
            peers_.want(directory_.get(), owner.proc_slot, now);
        }
    }
    handle = entry->handle;
    return SCADA_OK;
}

int Attachment::info(std::uint32_t handle, scada_tag_info_t& out) const {
    const std::optional<HandleParts> parts = unpack_handle(handle);
    if (!parts.has_value() || !tag_of(handle).has_value()) {
        return SCADA_E_INVALID;
    }
    // Checked when the directory was read at attach; checked again because
    // the name is taken from the region now.
    const std::optional<TagEntry> entry =
        header_.slot_entry(parts->owner_index, parts->slot);
    if (!entry.has_value()) {
        return SCADA_E_LAYOUT;
    }
    const OwnerInfo& owner = header_.contents().owners.at(parts->owner_index);
    scada_tag_info_t info{};
    info.type = to_raw(entry->type);
    info.flags = entry->flags;
    info.owned = access_.at(parts->owner_index).own ? 1 : 0;
    info.owner_index = parts->owner_index;
    info.process_index = owner.process_index;
    std::array<char, sizeof(info.name)> name{};
    std::array<char, sizeof(info.owner)> owner_name{};
    fill_name(name, entry->name);
    fill_name(owner_name, owner.name);
    std::ranges::copy(name, std::begin(info.name));
    std::ranges::copy(owner_name, std::begin(info.owner));
    out = info;
    return SCADA_OK;
}

int Attachment::read_at(std::uint32_t handle, scada_value& out,
                        std::int64_t now_mono_ns) noexcept {
    const std::optional<TagRef> tag = tag_of(handle);
    if (!tag.has_value()) {
        return SCADA_E_INVALID;
    }
    const bool down = owner_down(*tag->owner, now_mono_ns);
    const std::span<std::byte> tags = tags_of(*tag->owner);
    if (tags.empty()) {
        // Section 11.2: a region that is missing is not an error.
        out = absent_value(tag->type, now_mono_ns);
        return SCADA_OK;
    }
    RecordCopy copy;
    if (!read_record(shared_at<TagRecord>(tags, record_offset(tag->slot)),
                     copy)) {
        if (!down) {
            return SCADA_E_BUSY;
        }
        // The owner died in the middle of this write: what it left is not
        // a value.
        out = absent_value(tag->type, now_mono_ns);
        return SCADA_OK;
    }
    const std::uint64_t string_slot =
        tag->type == TagType::STRING ? string_slot_of(*tag) : 0;
    if (!record_valid(copy, tag->type, string_slot)) {
        return SCADA_E_LAYOUT;
    }
    out = value_of(copy, down, now_mono_ns);
    return SCADA_OK;
}

int Attachment::read(std::uint32_t handle, scada_value& out) noexcept {
    return read_at(handle, out, monotonic_ns());
}

int Attachment::read_many(std::span<const std::uint32_t> handles,
                          std::span<scada_value> out,
                          std::span<int> status) noexcept {
    if (out.size() != handles.size() ||
        (!status.empty() && status.size() != handles.size())) {
        panic("read_many: spans of different sizes");
    }
    // One reading of the clock for the whole call (section 13).
    const std::int64_t now = monotonic_ns();
    int first_error = SCADA_OK;
    for (std::size_t i = 0; i < handles.size(); ++i) {
        const int code = read_at(handles[i], out[i], now);
        if (!status.empty()) {
            status[i] = code;
        }
        if (code != SCADA_OK && first_error == SCADA_OK) {
            first_error = code;
        }
    }
    return first_error;
}

int Attachment::check_write(std::uint32_t handle, const scada_value& value,
                            TagRef& tag) const noexcept {
    const std::optional<TagRef> found = tag_of(handle);
    if (!found.has_value()) {
        return SCADA_E_INVALID;
    }
    if (!found->owner->own) {
        return SCADA_E_NOT_OWNER;
    }
    if (found->type == TagType::STRING || value.type != to_raw(found->type)) {
        return SCADA_E_TYPE;
    }
    if (!quality_from_raw(value.quality).has_value() ||
        !quality_flags_valid(value.quality_flags) ||
        (found->type == TagType::BOOL && value.value > 1)) {
        return SCADA_E_INVALID;
    }
    tag = *found;
    return SCADA_OK;
}

int Attachment::write(std::uint32_t handle, const scada_value& value) noexcept {
    TagRef tag;
    if (const int code = check_write(handle, value, tag); code != SCADA_OK) {
        return code;
    }
    // Two clock reads for one write (section 16).
    const RecordUpdate update =
        update_of(value.quality, value.quality_flags, value.value,
                  value.source_time_ns, realtime_ns(), monotonic_ns());
    // The change entry of section 8 is appended here from issue P1-07 on.
    static_cast<void>(write_record(
        shared_at<TagRecord>(tag.owner->own_tags, record_offset(tag.slot)),
        update));
    return SCADA_OK;
}

int Attachment::write_many(std::span<const std::uint32_t> handles,
                           std::span<const scada_value> values) noexcept {
    if (values.size() != handles.size()) {
        panic("write_many: spans of different sizes");
    }
    // All or nothing: every pair is checked before the first is written.
    for (std::size_t i = 0; i < handles.size(); ++i) {
        TagRef tag;
        if (const int code = check_write(handles[i], values[i], tag);
            code != SCADA_OK) {
            return code;
        }
    }
    // One pair of clock reads for the whole call (section 13).
    const std::int64_t time_ns = realtime_ns();
    const std::int64_t mono_ns = monotonic_ns();
    for (std::size_t i = 0; i < handles.size(); ++i) {
        const std::optional<TagRef> tag = tag_of(handles[i]);
        if (!tag.has_value()) {
            continue;  // cannot be: checked above, and nothing changed
        }
        const RecordUpdate update = update_of(
            values[i].quality, values[i].quality_flags, values[i].value,
            values[i].source_time_ns, time_ns, mono_ns);
        static_cast<void>(
            write_record(shared_at<TagRecord>(tag->owner->own_tags,
                                              record_offset(tag->slot)),
                         update));
    }
    return SCADA_OK;
}

int Attachment::read_string(std::uint32_t handle, std::span<char> buffer,
                            std::size_t& length, scada_value* meta) noexcept {
    const std::optional<TagRef> tag = tag_of(handle);
    if (!tag.has_value()) {
        return SCADA_E_INVALID;
    }
    if (tag->type != TagType::STRING) {
        return SCADA_E_TYPE;
    }
    const std::int64_t now = monotonic_ns();
    const bool down = owner_down(*tag->owner, now);
    const std::span<std::byte> tags = tags_of(*tag->owner);
    const std::span<std::byte> strings = strings_of(*tag->owner);
    const std::uint64_t string_slot = string_slot_of(*tag);

    RecordCopy copy;
    StringCopy text;
    scada_value value = absent_value(TagType::STRING, now);
    // Both regions, or the tag reads as one whose region is missing.
    if (!tags.empty() && !strings.empty()) {
        if (read_string_record(
                shared_at<TagRecord>(tags, record_offset(tag->slot)),
                shared_at<StringSlot>(strings, string_slot_offset(string_slot)),
                copy, text)) {
            if (!record_valid(copy, TagType::STRING, string_slot) ||
                text.len > text.data.size()) {
                return SCADA_E_LAYOUT;
            }
            value = value_of(copy, down, now);
        } else if (!down) {
            return SCADA_E_BUSY;
        }
    }
    length = text.len;
    if (buffer.size() < text.len) {
        return SCADA_E_INVALID;
    }
    std::ranges::transform(
        std::span<const std::uint8_t>(text.data).first(text.len),
        buffer.begin(),
        [](std::uint8_t byte) { return static_cast<char>(byte); });
    if (meta != nullptr) {
        *meta = value;
    }
    return SCADA_OK;
}

int Attachment::write_string(std::uint32_t handle, std::string_view text,
                             std::uint8_t quality, std::uint16_t flags,
                             std::int64_t source_time_ns) noexcept {
    const std::optional<TagRef> tag = tag_of(handle);
    if (!tag.has_value()) {
        return SCADA_E_INVALID;
    }
    if (!tag->owner->own) {
        return SCADA_E_NOT_OWNER;
    }
    if (tag->type != TagType::STRING) {
        return SCADA_E_TYPE;
    }
    if (text.size() > STRING_SLOT_DATA_SIZE || !is_utf8(text) ||
        !quality_from_raw(quality).has_value() || !quality_flags_valid(flags)) {
        return SCADA_E_INVALID;
    }
    const std::uint64_t string_slot = string_slot_of(*tag);
    std::array<std::uint8_t, STRING_SLOT_DATA_SIZE> bytes{};
    std::ranges::transform(text, bytes.begin(), [](char character) {
        return static_cast<std::uint8_t>(character);
    });
    const RecordUpdate update =
        update_of(quality, flags, string_slot, source_time_ns, realtime_ns(),
                  monotonic_ns());
    // The change entry of section 8 is appended here from issue P1-07 on.
    static_cast<void>(write_string_record(
        shared_at<TagRecord>(tag->owner->own_tags, record_offset(tag->slot)),
        shared_at<StringSlot>(tag->owner->own_strings,
                              string_slot_offset(string_slot)),
        std::span<const std::uint8_t>(bytes).first(text.size()), update));
    return SCADA_OK;
}

}  // namespace scada::tag
