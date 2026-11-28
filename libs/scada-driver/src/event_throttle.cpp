#include "event_throttle.h"

#include "scada/driver/channel_engine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace scada::driver::detail {

namespace {

constexpr std::int64_t NEVER = std::numeric_limits<std::int64_t>::max();

// `left + right`, held at the largest value instead of overflowing.
std::int64_t add_ns(std::int64_t left, std::int64_t right) noexcept {
    return left > NEVER - right ? NEVER : left + right;
}

}  // namespace

EventThrottle::EventThrottle(std::size_t slots, std::int64_t period_ns)
    : pending_(slots), period_ns_(std::max<std::int64_t>(period_ns, 0)) {}

void EventThrottle::add(std::size_t slot, const EventRecord& record) noexcept {
    if (slot >= pending_.size() || record.count == 0) {
        return;
    }
    EventRecord& held = pending_[slot].record;
    const std::uint64_t count = held.count + record.count;
    if (record.has_details) {
        held = record;
    }
    held.count = count;
}

bool EventThrottle::take(std::size_t slot, std::int64_t now_ns, bool all,
                         EventRecord& out) noexcept {
    if (slot >= pending_.size()) {
        return false;
    }
    Pending& pending = pending_[slot];
    if (pending.record.count == 0 || (!all && now_ns < pending.next_ns)) {
        return false;
    }
    out = pending.record;
    // The details stay for a later record whose events bring none.
    pending.record.count = 0;
    pending.next_ns = add_ns(now_ns, period_ns_);
    return true;
}

}  // namespace scada::driver::detail
