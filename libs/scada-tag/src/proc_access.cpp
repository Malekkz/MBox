// The owner and process functions of Attachment
// (docs/contracts/tag-database.md, sections 5, 10 and 13): owner info,
// process resolve and process status.

#include "attachment.h"
#include "header_view.h"
#include "proc_region.h"

#include "scada/common/clock.h"
#include "scada/tag/liveness.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/tag_set.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace scada::tag {

namespace {

// Copies the validated name `text` into the C field `field`, NUL-padded.
// A name of the header is at most MAX_SHORT_NAME_LENGTH bytes, so it fits
// with its terminator.
SCADA_RT void copy_name(std::span<char, SCADA_OWNER_NAME_SIZE> field,
                        std::string_view text) noexcept {
    std::ranges::fill(field, '\0');
    const std::size_t length = std::min(text.size(), field.size() - 1);
    std::ranges::copy(text.substr(0, length), field.begin());
}

// The time from `then_mono_ns` to `now_mono_ns`, never negative, computed
// without signed overflow: `then_mono_ns` is input.
SCADA_RT std::int64_t elapsed_ns(std::int64_t now_mono_ns,
                                 std::int64_t then_mono_ns) noexcept {
    if (then_mono_ns >= now_mono_ns) {
        return 0;
    }
    const std::uint64_t elapsed = static_cast<std::uint64_t>(now_mono_ns) -
                                  static_cast<std::uint64_t>(then_mono_ns);
    constexpr auto LIMIT =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    // In range after the comparison.
    return static_cast<std::int64_t>(std::min(elapsed, LIMIT));
}

}  // namespace

int Attachment::owner_info(std::uint16_t owner_index,
                           scada_owner_info_t& out) const {
    const HeaderContents& contents = header_.contents();
    if (owner_index >= contents.owners.size()) {
        return SCADA_E_INVALID;
    }
    const OwnerInfo& owner = contents.owners[owner_index];
    scada_owner_info_t info{};
    copy_name(info.name, owner.name);
    info.tag_count = owner.tag_count;
    info.change_ring_capacity = owner.change_ring_capacity;
    info.process_index = owner.process_index;
    info.owned = access_.at(owner_index).own ? 1 : 0;
    out = info;
    return SCADA_OK;
}

int Attachment::proc_resolve(std::string_view name, std::uint16_t& index) {
    if (!is_valid_name(name, MAX_SHORT_NAME_LENGTH)) {
        return SCADA_E_INVALID;
    }
    const std::optional<std::uint16_t> found = header_.find_process(name);
    if (!found.has_value()) {
        return SCADA_E_NOT_FOUND;
    }
    // Mapped and brought into memory here, never in proc_status
    // (section 11.2).
    want_proc(*found);
    index = *found;
    return SCADA_OK;
}

int Attachment::proc_status(std::uint16_t index,
                            scada_proc_status_t& out) const noexcept {
    const HeaderContents& contents = header_.contents();
    if (index >= contents.processes.size()) {
        return SCADA_E_INVALID;
    }
    const ProcessInfo& process = contents.processes[index];
    const std::int64_t now = monotonic_ns();
    const std::span<std::byte> region = proc_of(index);

    scada_proc_status_t status{};
    copy_name(status.name, process.name);
    status.heartbeat_timeout_ms = process.heartbeat_timeout_ms;
    status.uid = process.uid;
    status.process_index = index;
    status.realtime = process.realtime ? 1 : 0;
    status.marked_down = header_.marked_down(index) ? 1 : 0;
    const DownReason reason =
        proc_liveness(region, process.heartbeat_timeout_ms, now);
    status.down = reason == DownReason::NONE ? 0 : 1;
    status.down_reason = static_cast<std::uint8_t>(reason);
    if (const std::optional<ProcFields> fields = read_proc_fields(region);
        fields.has_value()) {
        status.heartbeat = fields->heartbeat;
        status.heartbeat_mono_ns = fields->heartbeat_mono_ns;
        status.heartbeat_age_ns = elapsed_ns(now, fields->heartbeat_mono_ns);
        status.state = fields->state;
        status.cycle_seq = fields->cycle_seq;
        status.pid = fields->pid;
    }
    out = status;
    return SCADA_OK;
}

// The numbers of the C interface are those of DownReason.
static_assert(static_cast<int>(DownReason::NONE) == SCADA_DOWN_NONE);
static_assert(static_cast<int>(DownReason::REGION_MISSING) ==
              SCADA_DOWN_REGION_MISSING);
static_assert(static_cast<int>(DownReason::NOT_RUNNING) ==
              SCADA_DOWN_NOT_RUNNING);
static_assert(static_cast<int>(DownReason::HEARTBEAT_STALE) ==
              SCADA_DOWN_HEARTBEAT_STALE);

}  // namespace scada::tag
