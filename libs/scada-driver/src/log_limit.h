#ifndef SCADA_DRIVER_SRC_LOG_LIMIT_H
#define SCADA_DRIVER_SRC_LOG_LIMIT_H

// The rate limit of a thread's log records (driver interface contract 11.3,
// the pattern of the tag database's `heartbeat late` record): per kind of
// record (its MESSAGE_ID) and key (a small integer: in the driver, the
// device), the first record goes at once and starts a period; records
// within the period are held and counted; at the first look after the
// period, one summary with the count goes and starts the next period. A
// device that flaps every period therefore writes two records per period at
// most. Nothing in it is particular to the driver, so that it can move to
// libscada-common when a second service needs it (decision D-148).
//
// One per thread, made at set-up; nothing after the constructor allocates.

#include "scada/common/realtime.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace scada::driver::detail {

// The period: 10 s (11.3).
inline constexpr std::int64_t LOG_LIMIT_PERIOD_NS = 10'000'000'000;

// Records held in one period of one kind and key.
struct LogSummary {
    std::size_t kind = 0;
    std::size_t key = 0;
    std::uint64_t count = 0;

    friend bool operator==(const LogSummary&, const LogSummary&) = default;
};

class LogLimit {
public:
    // Entries for `kinds` kinds of record and `keys` keys, plus one per
    // kind for records of no key (key `keys`).
    // `period_ns` is at least 1. May throw std::bad_alloc.
    LogLimit(std::size_t kinds, std::size_t keys,
             std::int64_t period_ns = LOG_LIMIT_PERIOD_NS);

    // True if the record of `kind` and `key` at `now_ns` (monotonic) is
    // to be logged now; otherwise its `count` events are held and added,
    // and a later `next_due` gives their summary. A kind or key out of
    // range is always logged: a wrong index must not lose records.
    SCADA_RT bool admit(std::size_t kind, std::size_t key, std::int64_t now_ns,
                        std::uint64_t count = 1) noexcept;

    // Takes one summary whose period ended by `now_ns` into `out` and starts
    // its next period; false when none is due. Call it until false at every
    // step of the thread, so that a held count is logged when its period
    // ends. Takes constant time when nothing is due.
    SCADA_RT bool next_due(std::int64_t now_ns, LogSummary& out) noexcept;

    // Takes one held summary, due or not, into `out`; false when none is
    // held. For the stop, so that no count is lost.
    SCADA_RT bool next_held(LogSummary& out) noexcept;

private:
    static constexpr std::int64_t IDLE =
        std::numeric_limits<std::int64_t>::min();

    struct Entry {
        std::int64_t until_ns = IDLE;  // end of the current period
        std::uint64_t held = 0;        // records held in it
    };

    [[nodiscard]] std::size_t index(std::size_t kind,
                                    std::size_t key) const noexcept {
        return kind * (keys_ + 1) + key;
    }
    void take(std::size_t slot, LogSummary& out) noexcept;
    // Sets `next_due_ns_` to the earliest end of a period with held
    // records, or never.
    void find_next_due() noexcept;

    std::size_t kinds_;
    std::size_t keys_;
    std::int64_t period_ns_;
    std::vector<Entry> entries_;  // fixed size
    // No summary is due before this time: the earliest end of a period
    // with held records, or earlier, never later.
    std::int64_t next_due_ns_ = std::numeric_limits<std::int64_t>::max();
};

}  // namespace scada::driver::detail

#endif  // SCADA_DRIVER_SRC_LOG_LIMIT_H
