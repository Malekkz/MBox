#ifndef SCADA_DRIVER_SRC_STALL_WATCH_H
#define SCADA_DRIVER_SRC_STALL_WATCH_H

// The arithmetic of the channel watchdog (driver interface contract 2.6,
// decision D-142), apart from the threads and the clock so that a unit
// test pins it: when each worker last moved, and how much of a late check
// is credited to them.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace scada::driver::detail {

// What a check sees of one worker.
struct Watched {
    std::uint64_t progress = 0;  // its progress counter now
    std::int64_t limit_ns = 0;   // its stall limit
    bool returned = false;       // its thread has returned: not judged
};

class StallWatch {
public:
    // A check later than this many periods after the one before is a pause
    // of the main thread.
    static constexpr std::int64_t PAUSE_PERIODS = 2;

    // `period_ns`, above 0, is the time between two checks.
    explicit StallWatch(std::int64_t period_ns) noexcept
        : period_ns_(period_ns) {}

    // Starts the watch of `workers` workers at `now_ns`: each counts as
    // moved now, and so does the watch's own clock, so that a pause before
    // the first check is credited as any other. Allocates.
    void start(std::size_t workers, std::int64_t now_ns);

    // One check at `now_ns` (one Watched per worker, in the order of
    // `start`). A check more than PAUSE_PERIODS periods after the one
    // before credits every worker with the time the main thread lost, the
    // gap less one period, and keeps its history. Returns the index of a
    // worker whose counter has not moved for its stall limit, if any.
    [[nodiscard]] std::optional<std::size_t> check(
        std::int64_t now_ns, std::span<const Watched> workers) noexcept;

    // The gap of the last check if it was a pause, else 0.
    [[nodiscard]] std::int64_t last_pause_ns() const noexcept {
        return last_pause_ns_;
    }

private:
    std::int64_t period_ns_;
    std::int64_t last_check_ns_ = 0;
    std::int64_t last_pause_ns_ = 0;
    std::vector<std::uint64_t> seen_;
    std::vector<std::int64_t> moved_ns_;
};

}  // namespace scada::driver::detail

#endif  // SCADA_DRIVER_SRC_STALL_WATCH_H
