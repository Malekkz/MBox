#ifndef SCADA_TAG_SUBSCRIPTION_H
#define SCADA_TAG_SUBSCRIPTION_H

// A subscription to the change ring of one owner
// (docs/contracts/tag-database.md, sections 8.2 to 8.4): the cursor in
// private memory, gap detection and the wait. The C interface `scada_sub_*`
// is a thin layer over this class. Private to libscada-tag.

#include "attachment.h"
#include "futex.h"

#include "scada/tag/scada_tag.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace scada::tag {

class WaitSet;

// The longest single wait of the library, in ns (section 8.4: 100 ms).
inline constexpr std::int64_t WAIT_SLICE_NS =
    std::int64_t{SCADA_WAIT_SLICE_MS} * 1'000'000;

// The end of a wait of `timeout_ms` from `now_mono_ns`, which a negative
// or huge timeout cannot overflow.
[[nodiscard]] std::int64_t deadline_after(std::int64_t now_mono_ns,
                                          int timeout_ms) noexcept;

// Sleeps until `deadline_mono_ns` (CLOCK_MONOTONIC). Used while nothing can
// be waited on because the regions are missing. Not real-time.
void sleep_until(std::int64_t deadline_mono_ns) noexcept;

// One thread uses a subscription (section 13.2). The attachment must
// outlive it.
class Subscription {
public:
    // Subscribes to the ring of the owner `owner` of `attachment`: its
    // regions are asked for, and the cursor starts at the end of the ring
    // if it is there. A return code of scada/tag/scada_tag.h, as
    // scada_sub_open states.
    [[nodiscard]] static int open(Attachment& attachment,
                                  std::string_view owner,
                                  std::unique_ptr<Subscription>& out);

    Subscription(Attachment& attachment, std::size_t owner_index) noexcept;
    ~Subscription();
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&&) = delete;
    Subscription& operator=(Subscription&&) = delete;

    // As scada_sub_next.
    [[nodiscard]] int next(scada_change& out, int timeout_ms);

    // As scada_sub_stats.
    [[nodiscard]] scada_sub_stats_t stats() const noexcept;

    // True if `next` would return at once with something other than
    // SCADA_E_TIMEOUT, except for a ring whose capacity is wrong, which
    // `next` has reported already. Changes nothing.
    [[nodiscard]] bool ready() const noexcept;

    // The word to wait on for the next entry, if the ring is mapped and is
    // the one the cursor belongs to.
    [[nodiscard]] std::optional<FutexWaiter> waiter() const noexcept;

    [[nodiscard]] Attachment& attachment() const noexcept {
        return attachment_;
    }
    [[nodiscard]] std::size_t owner_index() const noexcept {
        return owner_index_;
    }

    // The wait set the subscription is in, or null. Set by WaitSet only.
    [[nodiscard]] WaitSet* wait_set() const noexcept { return wait_set_; }
    void set_wait_set(WaitSet* wait_set) noexcept { wait_set_ = wait_set; }

private:
    // One look at the ring without waiting: SCADA_OK with `out`,
    // SCADA_E_GAP, SCADA_E_LAYOUT, or SCADA_E_TIMEOUT for nothing new.
    [[nodiscard]] int poll(scada_change& out) noexcept;

    // Takes the ring in `bytes`, mapped at a new place or there for the
    // first time, as the ring to follow from its end. False if its header
    // gives another capacity than the owner table.
    [[nodiscard]] bool follow(std::span<std::byte> bytes) noexcept;

    Attachment& attachment_;
    std::size_t owner_index_ = 0;
    std::uint16_t process_index_ = 0;
    std::uint32_t capacity_ = 0;  // from the owner table (section 8)

    // The start of the mapping the cursor belongs to; null while the ring
    // is missing. Another start means the ring appeared, disappeared or was
    // replaced: a gap.
    const std::byte* mapping_ = nullptr;
    // The ring header of `mapping_` gives another capacity.
    bool wrong_capacity_ = false;
    std::uint64_t cursor_ = 0;  // the next position wanted
    std::uint32_t epoch_ = 0;   // of the ring, as last seen

    std::uint64_t received_ = 0;
    std::uint64_t gaps_ = 0;
    std::uint64_t lost_ = 0;
    std::uint64_t invalid_ = 0;

    WaitSet* wait_set_ = nullptr;
};

}  // namespace scada::tag

#endif  // SCADA_TAG_SUBSCRIPTION_H
