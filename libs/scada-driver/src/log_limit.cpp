#include "log_limit.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace scada::driver::detail {

namespace {

constexpr std::int64_t NEVER = std::numeric_limits<std::int64_t>::max();

// `left + right`, held at the largest value instead of overflowing.
std::int64_t add(std::int64_t left, std::int64_t right) noexcept {
    return left > NEVER - right ? NEVER : left + right;
}

}  // namespace

LogLimit::LogLimit(std::size_t kinds, std::size_t devices,
                   std::int64_t period_ns)
    : kinds_(kinds),
      devices_(devices),
      period_ns_(std::max<std::int64_t>(period_ns, 1)),
      entries_(kinds * (devices + 1)) {}

bool LogLimit::admit(std::size_t kind, std::size_t device,
                     std::int64_t now_ns) noexcept {
    if (kind >= kinds_ || device > devices_) {
        return true;
    }
    Entry& entry = entries_[index(kind, device)];
    if (entry.held == 0 && now_ns >= entry.until_ns) {
        entry.until_ns = add(now_ns, period_ns_);
        return true;
    }
    // Within the period, or after it with a summary not yet taken: the
    // summary goes first and counts this one.
    if (entry.held == 0) {
        next_due_ns_ = std::min(next_due_ns_, entry.until_ns);
    }
    ++entry.held;
    return false;
}

void LogLimit::take(std::size_t slot, LogSummary& out) noexcept {
    Entry& entry = entries_[slot];
    out = {.kind = slot / (devices_ + 1),
           .device = slot % (devices_ + 1),
           .count = entry.held};
    entry.held = 0;
}

bool LogLimit::next_due(std::int64_t now_ns, LogSummary& out) noexcept {
    if (now_ns < next_due_ns_) {
        return false;  // the constant-time answer of most steps
    }
    for (std::size_t slot = 0; slot < entries_.size(); ++slot) {
        Entry& entry = entries_[slot];
        if (entry.held != 0 && entry.until_ns <= now_ns) {
            take(slot, out);
            // The summary starts the next period.
            entry.until_ns = add(now_ns, period_ns_);
            return true;
        }
    }
    find_next_due();
    return false;
}

bool LogLimit::next_held(LogSummary& out) noexcept {
    for (std::size_t slot = 0; slot < entries_.size(); ++slot) {
        if (entries_[slot].held != 0) {
            take(slot, out);
            return true;
        }
    }
    return false;
}

void LogLimit::find_next_due() noexcept {
    next_due_ns_ = NEVER;
    for (const Entry& entry : entries_) {
        if (entry.held != 0) {
            next_due_ns_ = std::min(next_due_ns_, entry.until_ns);
        }
    }
}

}  // namespace scada::driver::detail
