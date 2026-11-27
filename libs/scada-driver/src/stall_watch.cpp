#include "stall_watch.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace scada::driver::detail {
namespace {

constexpr std::int64_t MAX_NS = std::numeric_limits<std::int64_t>::max();

std::int64_t add(std::int64_t left, std::int64_t right) noexcept {
    return left > MAX_NS - right ? MAX_NS : left + right;
}

}  // namespace

void StallWatch::start(std::size_t workers, std::int64_t now_ns) {
    last_check_ns_ = now_ns;
    last_pause_ns_ = 0;
    seen_.assign(workers, 0);
    moved_ns_.assign(workers, now_ns);
}

std::optional<std::size_t> StallWatch::check(
    std::int64_t now_ns, std::span<const Watched> workers) noexcept {
    const std::int64_t gap =
        now_ns > last_check_ns_ ? now_ns - last_check_ns_ : 0;
    last_check_ns_ = now_ns;
    last_pause_ns_ = 0;
    if (gap > period_ns_ * PAUSE_PERIODS) {
        // The main thread itself did not run for that time (review M7,
        // delta 1): only what it lost is credited, the history kept, so
        // that repeated short pauses never hide a worker that is stuck.
        last_pause_ns_ = gap;
        const std::int64_t lost = gap - period_ns_;
        for (std::int64_t& moved : moved_ns_) {
            moved = add(moved, lost);
        }
    }
    for (std::size_t index = 0;
         index < workers.size() && index < moved_ns_.size(); ++index) {
        const Watched& worker = workers[index];
        if (worker.returned) {
            continue;
        }
        if (worker.progress != seen_[index]) {
            seen_[index] = worker.progress;
            moved_ns_[index] = now_ns;
        } else if (now_ns - moved_ns_[index] >= worker.limit_ns) {
            return index;
        }
    }
    return std::nullopt;
}

}  // namespace scada::driver::detail
