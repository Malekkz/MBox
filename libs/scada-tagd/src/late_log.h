#ifndef SCADA_TAGD_LATE_LOG_H
#define SCADA_TAGD_LATE_LOG_H

// The rate limit of the `heartbeat late` log record (issue #61): at most one
// record per process in LATE_LOG_INTERVAL_NS, whatever the check interval.
// Private to libscada-tagd.

#include "scada/tag/generation_host.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace scada::tagd {

// The shortest time between two `heartbeat late` records of one process:
// 10 s. With the default check interval of 250 ms that is one record in 40
// checks. It allows at most 4 records of one process in any 30 s, so even a
// full process table (1,024, contract section 14) late all the time stays
// below journald's default rate limit of 10,000 records in 30 s per service.
inline constexpr std::int64_t LATE_LOG_INTERVAL_NS = 10'000'000'000;

// One record to log.
struct LateRecord {
    std::string process;
    // Late heartbeats since the previous record of this process, or, with
    // `baseline`, the whole count found at the first look at a region.
    // 64 bits: the growth of several checks is summed.
    std::uint64_t added = 0;
    std::uint32_t total = 0;             // since the region was created; wraps
    std::int64_t longest_gap_ns = 0;     // between any two heartbeats
    std::int64_t last_late_mono_ns = 0;  // CLOCK_MONOTONIC; end of the gap
    std::uint32_t checks = 0;            // checks the record sums, at least 1
    bool baseline = false;
};

// Merges the late heartbeats of successive liveness checks into at most one
// record per process and interval. Not thread-safe; used by the main loop of
// scada-tagd only.
class LateLog {
public:
    // `interval_ns` is the shortest time between two records of one
    // process; it must be at least 0.
    explicit LateLog(std::int64_t interval_ns = LATE_LOG_INTERVAL_NS) noexcept;

    // Takes the late heartbeats of one check made at `now_mono_ns` and
    // returns the records to log now. Call it at every check, also
    // when `late` is empty, so that held growth is logged when its interval
    // ends.
    // - The first growth of a process is returned at once; later growth
    //   within the interval after a record is held and summed, and is
    //   returned at the first check after the interval.
    // - A baseline is returned at once and does not start an interval: a
    //   region is looked at for the first time at most once per life of the
    //   process. Growth held before it, which belongs to the previous
    //   region, is returned first and starts a new interval.
    [[nodiscard]] std::vector<LateRecord> admit(
        std::span<const scada::tag::LateHeartbeats> late,
        std::int64_t now_mono_ns);

    // Returns every held record and forgets them; for a stop, so that no
    // count is lost.
    [[nodiscard]] std::vector<LateRecord> drain();

private:
    struct Entry {
        std::string process;
        std::int64_t next_mono_ns = 0;  // the earliest time of the next record
        bool held = false;              // `pending` holds growth to log
        LateRecord pending;
    };

    [[nodiscard]] Entry& entry_of(const std::string& process);

    std::int64_t interval_ns_ = 0;
    // One entry per process that has had late heartbeats; at most the size
    // of the process table.
    std::vector<Entry> entries_;
};

}  // namespace scada::tagd

#endif  // SCADA_TAGD_LATE_LOG_H
