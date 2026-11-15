#include "writer.h"

#include "scada/sim/pattern.h"
#include "scada/sim/sim.h"
#include "scada/tag/scada_tag.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace scada::sim::detail {

namespace {

using std::chrono::nanoseconds;

// A paced owner writes what is due, then waits for the next tick.
constexpr std::chrono::milliseconds TICK{1};
constexpr double NANOSECONDS_PER_SECOND = 1e9;

// How the writing of an owner ended.
enum class Outcome : std::uint8_t { FINISHED, STOPPED, FAILED };

std::int64_t since(Clock::time_point start) {
    return std::chrono::duration_cast<nanoseconds>(Clock::now() - start)
        .count();
}

// The owner's state between updates.
class Owner {
public:
    // `plan` and `progress` must outlive the Owner.
    Owner(scada_ctx* ctx, const OwnerPlan& plan, OwnerProgress& progress)
        : ctx_(ctx),
          plan_(&plan),
          progress_(&progress),
          previous_(plan.handles.size(), 0.0) {}

    [[nodiscard]] std::uint64_t written() const noexcept { return written_; }

    // True if every update of a run bounded by a count is written.
    [[nodiscard]] bool complete() const noexcept {
        return plan_->updates.has_value() && written_ >= *plan_->updates;
    }

    // Writes up to `count` updates, stopping early if the run is complete.
    // False if a write or notify failed.
    bool write(std::uint64_t count) {
        for (std::uint64_t i = 0; i < count && !complete(); ++i) {
            if (!write_next()) {
                return false;
            }
            if (plan_->notify == Notify::EACH &&
                scada_tag_notify(ctx_) != SCADA_OK) {
                return false;
            }
        }
        // Relaxed: a counter for reports; no other data depends on it.
        progress_->written.store(written_, std::memory_order_relaxed);
        return plan_->notify == Notify::EACH ||
               scada_tag_notify(ctx_) == SCADA_OK;
    }

private:
    bool write_next() {
        const std::size_t tags = plan_->handles.size();
        // In range: below `tags`.
        const auto tag = static_cast<std::size_t>(written_ % tags);
        const std::uint64_t count = (written_ / tags) + 1;
        const Pattern pattern = plan_->patterns[tag];
        scada_value value{};
        if (pattern == Pattern::CHECK) {
            if (count > MAX_CHECK_COUNT) {
                return false;
            }
            value = check_value(count);
        } else {
            const double wave = wave_value(pattern, plan_->waveform, tag, count,
                                           previous_[tag]);
            previous_[tag] = wave;
            value.type = SCADA_TYPE_FLOAT64;
            value.quality = SCADA_QUALITY_GOOD;
            value.value = std::bit_cast<std::uint64_t>(wave);
        }
        if (scada_tag_write(ctx_, plan_->handles[tag], &value) != SCADA_OK) {
            return false;
        }
        ++written_;
        return true;
    }

    scada_ctx* ctx_;
    const OwnerPlan* plan_;         // borrowed, not null
    OwnerProgress* progress_;       // borrowed, not null
    std::vector<double> previous_;  // last value of each tag, for WALK
    std::uint64_t written_ = 0;
};

// As fast as it can, NOTIFY_BATCH updates at a time.
Outcome write_unpaced(Owner& owner, const OwnerPlan& plan,
                      Clock::time_point start, const std::atomic<bool>& stop) {
    // Relaxed: a stop request; the thread is joined before anything else
    // is read.
    while (!stop.load(std::memory_order_relaxed)) {
        if (owner.complete() || (plan.run_for.has_value() &&
                                 Clock::now() - start >= *plan.run_for)) {
            return Outcome::FINISHED;
        }
        if (!owner.write(NOTIFY_BATCH)) {
            return Outcome::FAILED;
        }
    }
    return Outcome::STOPPED;
}

// At `plan.per_second`: at each tick, what is due by then.
Outcome write_paced(Owner& owner, const OwnerPlan& plan,
                    Clock::time_point start, const std::atomic<bool>& stop,
                    OwnerProgress& progress) {
    const double ns_per_update = NANOSECONDS_PER_SECOND / plan.per_second;
    std::int64_t behind_max = 0;
    // Relaxed: as in write_unpaced.
    while (!stop.load(std::memory_order_relaxed)) {
        const std::int64_t elapsed = since(start);
        if (owner.complete() ||
            (plan.run_for.has_value() &&
             elapsed >= nanoseconds(*plan.run_for).count())) {
            return Outcome::FINISHED;
        }
        // In range: elapsed time of a run of at most MAX_SECONDS, at most
        // MAX_RATE updates per second.
        const auto due = static_cast<std::uint64_t>(
            static_cast<double>(elapsed) / ns_per_update);
        if (due > owner.written()) {
            // When the oldest update not yet written was due.
            const auto due_at = static_cast<std::int64_t>(
                static_cast<double>(owner.written() + 1) * ns_per_update);
            behind_max = std::max(behind_max, elapsed - due_at);
            // Relaxed: a figure for reports.
            progress.behind_max_ns.store(behind_max, std::memory_order_relaxed);
            if (!owner.write(due - owner.written())) {
                return Outcome::FAILED;
            }
        }
        const std::int64_t ticks = (elapsed / nanoseconds(TICK).count()) + 1;
        std::this_thread::sleep_until(start + (TICK * ticks));
    }
    return Outcome::STOPPED;
}

}  // namespace

void write_as_owner(scada_ctx* ctx, const OwnerPlan& plan,
                    Clock::time_point start, const std::atomic<bool>& stop,
                    OwnerProgress& progress) {
    Owner owner(ctx, plan, progress);
    const Outcome outcome =
        plan.per_second > 0.0 ? write_paced(owner, plan, start, stop, progress)
                              : write_unpaced(owner, plan, start, stop);
    // Relaxed: a figure for reports.
    progress.written.store(owner.written(), std::memory_order_relaxed);
    if (outcome == Outcome::FAILED) {
        // Release: pairs with the acquire load of the main thread.
        progress.failed.store(true, std::memory_order_release);
    } else if (outcome == Outcome::FINISHED) {
        // Release: the main thread reads `written` after it sees this.
        progress.finished_ns.store(since(start), std::memory_order_release);
    }
}

}  // namespace scada::sim::detail
