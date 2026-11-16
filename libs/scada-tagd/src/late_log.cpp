#include "late_log.h"

#include "scada/tag/generation_host.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace scada::tagd {

namespace {

// `now + interval`, held at the largest time instead of overflowing.
std::int64_t later_by(std::int64_t now, std::int64_t interval) noexcept {
    // Both are at least 0 for CLOCK_MONOTONIC; a negative `now` from a
    // caller cannot overflow upwards either.
    constexpr std::int64_t MAX = std::numeric_limits<std::int64_t>::max();
    if (now > 0 && interval > MAX - now) {
        return MAX;
    }
    return now + interval;
}

LateRecord record_of(const scada::tag::LateHeartbeats& late) {
    return LateRecord{
        .process = late.process,
        .added = late.added,
        .total = late.total,
        .longest_gap_ns = late.longest_gap_ns,
        .last_late_mono_ns = late.last_late_mono_ns,
        .checks = 1,
        .baseline = late.baseline,
    };
}

}  // namespace

LateLog::LateLog(std::int64_t interval_ns) noexcept
    : interval_ns_(std::max(std::int64_t{0}, interval_ns)) {}

LateLog::Entry& LateLog::entry_of(const std::string& process) {
    const auto found = std::ranges::find_if(
        entries_,
        [&](const Entry& candidate) { return candidate.process == process; });
    if (found != entries_.end()) {
        return *found;
    }
    // The first growth of a process: it is due at once.
    Entry entry;
    entry.process = process;
    return entries_.emplace_back(std::move(entry));
}

std::vector<LateRecord> LateLog::admit(
    std::span<const scada::tag::LateHeartbeats> late,
    std::int64_t now_mono_ns) {
    std::vector<LateRecord> out;
    for (const scada::tag::LateHeartbeats& beats : late) {
        Entry& entry = entry_of(beats.process);
        if (beats.baseline) {
            // What was held belongs to the region looked at before. Logged
            // now, it starts a new interval like any other record.
            if (entry.held) {
                out.push_back(std::move(entry.pending));
                entry.held = false;
                entry.next_mono_ns = later_by(now_mono_ns, interval_ns_);
            }
            out.push_back(record_of(beats));
            continue;
        }
        if (entry.held) {
            // Summed in 64 bits: 2^32 per check cannot overflow it in any
            // number of checks a process lives.
            entry.pending.added += beats.added;
            entry.pending.total = beats.total;
            entry.pending.longest_gap_ns = beats.longest_gap_ns;
            entry.pending.last_late_mono_ns = beats.last_late_mono_ns;
            ++entry.pending.checks;
        } else {
            entry.pending = record_of(beats);
            entry.held = true;
        }
    }
    // Held growth whose interval has ended, including what this check added
    // to a process that had no record in the interval.
    for (Entry& entry : entries_) {
        if (entry.held && now_mono_ns >= entry.next_mono_ns) {
            out.push_back(std::move(entry.pending));
            entry.held = false;
            entry.next_mono_ns = later_by(now_mono_ns, interval_ns_);
        }
    }
    return out;
}

std::vector<LateRecord> LateLog::drain() {
    std::vector<LateRecord> out;
    for (Entry& entry : entries_) {
        if (entry.held) {
            out.push_back(std::move(entry.pending));
            entry.held = false;
        }
    }
    return out;
}

}  // namespace scada::tagd
