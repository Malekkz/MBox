#include "subscription.h"

#include "attachment.h"
#include "change_ring.h"
#include "futex.h"
#include "layout.h"
#include "shared_memory.h"
#include "wait_set.h"

#include "scada/common/clock.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace scada::tag {

namespace {

constexpr std::int64_t NANOSECONDS_PER_MILLISECOND = 1'000'000;

scada_change change_of(const ChangeEntry& entry) noexcept {
    scada_change change{};
    change.handle = entry.handle;
    change.type = entry.type;
    change.quality = entry.quality;
    change.quality_flags = entry.quality_flags;
    change.value = entry.value;
    change.source_time_ns = entry.source_time_ns;
    change.update_mono_ns = entry.update_mono_ns;
    return change;
}

}  // namespace

std::int64_t deadline_after(std::int64_t now_mono_ns, int timeout_ms) noexcept {
    const std::int64_t timeout =
        timeout_ms < 0 ? 0
                       : std::int64_t{timeout_ms} * NANOSECONDS_PER_MILLISECOND;
    // A monotonic time is far below the limit; the check keeps a nonsense
    // one from overflowing.
    if (now_mono_ns > std::numeric_limits<std::int64_t>::max() - timeout) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return now_mono_ns + timeout;
}

void sleep_until(std::int64_t deadline_mono_ns) noexcept {
    const timespec deadline{
        .tv_sec = deadline_mono_ns / NANOSECONDS_PER_SECOND,
        .tv_nsec = deadline_mono_ns % NANOSECONDS_PER_SECOND};
    // An interruption or an error only ends the sleep early; every caller
    // looks at the time and its sources again.
    static_cast<void>(
        ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr));
}

int Subscription::open(Attachment& attachment, std::string_view owner,
                       std::unique_ptr<Subscription>& out) {
    out.reset();
    if (!is_valid_name(owner, MAX_SHORT_NAME_LENGTH)) {
        return SCADA_E_INVALID;
    }
    const std::optional<std::size_t> index = attachment.find_owner(owner);
    if (!index.has_value()) {
        return SCADA_E_NOT_FOUND;
    }
    // Mapped and brought into memory here (section 11.2).
    attachment.want_changes(*index);
    auto subscription = std::make_unique<Subscription>(attachment, *index);
    // The state at open is the starting point, not a change.
    subscription->owner_down_ = attachment.owner_process_down(*index);
    const std::span<std::byte> bytes = attachment.changes_of(*index);
    if (!bytes.empty() && !subscription->follow(bytes)) {
        return SCADA_E_LAYOUT;
    }
    out = std::move(subscription);
    return SCADA_OK;
}

Subscription::Subscription(Attachment& attachment,
                           std::size_t owner_index) noexcept
    : attachment_(attachment),
      owner_index_(owner_index),
      process_index_(
          attachment.header().contents().owners.at(owner_index).process_index),
      capacity_(attachment.ring_capacity(owner_index)) {}

Subscription::~Subscription() {
    if (wait_set_ != nullptr) {
        wait_set_->remove(*this);
    }
}

bool Subscription::follow(std::span<std::byte> bytes) noexcept {
    mapping_ = bytes.data();
    // Relaxed would do for a field nobody changes after `ready`; acquire
    // is what every other load of a shared field here uses.
    wrong_capacity_ = load_shared(ring_header(bytes).capacity,
                                  std::memory_order_acquire) != capacity_;
    if (wrong_capacity_) {
        return false;
    }
    // The epoch first: an owner that attaches again between the two loads
    // then gives one gap too many, never one too few.
    epoch_ = ring_epoch(bytes);
    cursor_ = ring_write_seq(bytes);
    return true;
}

void Subscription::skip_to_end(std::span<std::byte> bytes) noexcept {
    if (!bytes.empty() && !wrong_capacity_) {
        // The epoch first, as in `follow`.
        epoch_ = ring_epoch(bytes);
        cursor_ = ring_write_seq(bytes);
    }
}

int Subscription::gap(std::uint8_t reason) noexcept {
    ++gaps_;
    last_gap_ = reason;
    return SCADA_E_GAP;
}

int Subscription::poll(scada_change& out) noexcept {
    const std::span<std::byte> bytes = attachment_.changes_of(owner_index_);
    const bool owner_down = attachment_.owner_process_down(owner_index_);
    if (bytes.data() != mapping_) {
        // The ring appeared, disappeared or was replaced (section 8.3:
        // whatever the consumer knew may be lost; it resynchronises). A
        // change of the owner's state at the same look is the same gap.
        // The owner's reason is reported then: it is what an alarm system
        // acts on.
        std::uint8_t reason = SCADA_GAP_RING;
        if (owner_down != owner_down_) {
            owner_down_ = owner_down;
            ++owner_changes_;
            reason = owner_down ? SCADA_GAP_OWNER_DOWN : SCADA_GAP_OWNER_BACK;
        }
        if (bytes.empty()) {
            mapping_ = nullptr;
            wrong_capacity_ = false;
        } else if (!follow(bytes)) {
            return SCADA_E_LAYOUT;
        }
        return gap(reason);
    }
    if (owner_down != owner_down_) {
        // The owner's process went down or came back (section 10.1). A dead
        // owner writes no change, so this is how a consumer of the ring
        // learns it: it reads the tags, which give OWNER_DOWN, or values
        // again. Whatever the ring holds up to here, the epoch included, is
        // passed over: the reads after the gap supersede it.
        owner_down_ = owner_down;
        ++owner_changes_;
        skip_to_end(bytes);
        return gap(owner_down ? SCADA_GAP_OWNER_DOWN : SCADA_GAP_OWNER_BACK);
    }
    if (bytes.empty()) {
        return SCADA_E_TIMEOUT;
    }
    if (wrong_capacity_) {
        return SCADA_E_LAYOUT;
    }
    const std::uint32_t epoch = ring_epoch(bytes);
    if (epoch != epoch_) {
        // The owner attached again (section 6.3): the change entry of its
        // last write may be lost.
        epoch_ = epoch;
        cursor_ = ring_write_seq(bytes);
        return gap(SCADA_GAP_RESTART);
    }
    ChangeEntry entry;
    std::uint64_t written = 0;
    switch (read_change(bytes, capacity_, cursor_, entry, written)) {
        case RingRead::NOTHING:
            return SCADA_E_TIMEOUT;
        case RingRead::GAP: {
            // Section 8.3: the cursor is set first; the consumer reads the
            // tags afterwards.
            const std::uint64_t now_at = ring_write_seq(bytes);
            // Entries the cursor jumps over; none for a `write_seq` that
            // went backwards, which only a damaged region shows.
            lost_ += now_at > cursor_ ? now_at - cursor_ : 0;
            cursor_ = now_at;
            return gap(SCADA_GAP_OVERRUN);
        }
        case RingRead::ENTRY:
            break;
    }
    if (!attachment_.change_valid(owner_index_, entry)) {
        ++invalid_;
        return SCADA_E_LAYOUT;
    }
    out = change_of(entry);
    if (owner_down_) {
        // The overlay of a read (section 6.2): an owner whose process is
        // down, whose threads may still write, gives no good value.
        out.quality = to_raw(Quality::BAD);
        out.quality_flags |= to_raw(QualityFlag::OWNER_DOWN);
    }
    ++received_;
    return SCADA_OK;
}

int Subscription::next(scada_change& out, int timeout_ms) {
    if (timeout_ms < 0) {
        return SCADA_E_INVALID;
    }
    const std::int64_t deadline = deadline_after(monotonic_ns(), timeout_ms);
    for (;;) {
        if (const int code = poll(out); code != SCADA_E_TIMEOUT) {
            return code;
        }
        const std::int64_t now = monotonic_ns();
        if (now >= deadline) {
            return SCADA_E_TIMEOUT;
        }
        // Section 8.4: no wait is longer than the slice, so a missed wake
        // delays an entry by one slice at most.
        const std::int64_t slice_end =
            deadline - now < WAIT_SLICE_NS ? deadline : now + WAIT_SLICE_NS;
        const std::optional<FutexWaiter> word = waiter();
        if (!word.has_value()) {
            // The ring is missing: look for it again (at most once a
            // second), and meanwhile wait a slice.
            attachment_.refresh_peers();
            sleep_until(slice_end);
            continue;
        }
        if (futex_wait(word->word, word->expected, slice_end) ==
            FutexWait::FAILED) {
            return SCADA_E_IO;
        }
    }
}

bool Subscription::ready() const noexcept {
    const std::span<std::byte> bytes = attachment_.changes_of(owner_index_);
    if (bytes.data() != mapping_ ||
        attachment_.owner_process_down(owner_index_) != owner_down_) {
        return true;
    }
    if (bytes.empty() || wrong_capacity_) {
        return false;
    }
    return ring_epoch(bytes) != epoch_ || ring_write_seq(bytes) != cursor_;
}

std::optional<FutexWaiter> Subscription::waiter() const noexcept {
    const std::span<std::byte> bytes = attachment_.changes_of(owner_index_);
    if (bytes.empty() || bytes.data() != mapping_ || wrong_capacity_) {
        return std::nullopt;
    }
    return FutexWaiter{.word = &ring_header(bytes).write_seq,
                       .expected = futex_word_of(cursor_),
                       .process_private = false};
}

scada_sub_stats_t Subscription::stats() const noexcept {
    scada_sub_stats_t stats{};
    stats.received = received_;
    stats.gaps = gaps_;
    stats.lost = lost_;
    stats.invalid = invalid_;
    stats.epoch = epoch_;
    // Below MAX_OWNERS, which fits 16 bits.
    stats.owner_index = static_cast<std::uint16_t>(owner_index_);
    stats.process_index = process_index_;
    stats.owner_changes = owner_changes_;
    stats.owner_down = owner_down_ ? 1 : 0;
    stats.last_gap = last_gap_;
    return stats;
}

}  // namespace scada::tag
