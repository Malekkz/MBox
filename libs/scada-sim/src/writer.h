#ifndef SCADA_SIM_SRC_WRITER_H
#define SCADA_SIM_SRC_WRITER_H

// The owner threads of one simulator process. Private to libscada-sim.

#include "scada/sim/pattern.h"
#include "scada/sim/sim.h"
#include "scada/tag/scada_tag.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace scada::sim::detail {

using Clock = std::chrono::steady_clock;

// What one owner thread writes. Its tags are written in turn: update j,
// from 0, is write number first_counts[t] + j / tags + 1 of tag
// t = j % tags.
struct OwnerPlan {
    std::vector<std::uint32_t> handles;  // the owner's tags, in slot order
    std::vector<Pattern> patterns;       // of each tag
    // Of each tag: the write count it had before this run (CHECK only).
    std::vector<std::uint64_t> first_counts;
    Waveform waveform;
    // Updates per second of this owner; 0: as fast as it can.
    double per_second = 0.0;
    std::optional<std::uint64_t> updates;    // stop after this many
    std::optional<Clock::duration> run_for;  // stop after this long; only
                                             // when writing as fast as it can
    Notify notify = Notify::BATCH;
};

// What an owner thread reports to the main thread. Written by the owner
// thread only.
struct OwnerProgress {
    std::atomic<std::uint64_t> written{0};
    // How late the latest-written update was, at worst, in ns.
    std::atomic<std::int64_t> behind_max_ns{0};
    // When the thread had written everything, in ns since the start; -1
    // while it has not.
    std::atomic<std::int64_t> finished_ns{-1};
    std::atomic<bool> failed{false};
};

// The body of an owner thread: writes `plan` from `start` on, until it is
// complete or `stop` is set, and keeps `progress` current. A write that
// fails sets `progress.failed` and ends the thread. `ctx` and `stop` must
// outlive the call; `plan` must outlive the call as well.
void write_as_owner(scada_ctx* ctx, const OwnerPlan& plan,
                    Clock::time_point start, const std::atomic<bool>& stop,
                    OwnerProgress& progress);

}  // namespace scada::sim::detail

// Planted fault for #120: a function-like macro (not to be merged).
#define SCADA_SIM_PROBE_SQUARE(x) ((x) * (x))

#endif  // SCADA_SIM_SRC_WRITER_H
