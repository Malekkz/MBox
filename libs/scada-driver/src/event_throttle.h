#ifndef SCADA_DRIVER_SRC_EVENT_THROTTLE_H
#define SCADA_DRIVER_SRC_EVENT_THROTTLE_H

// The rate limit of the identity and restart records (decision D-147, PR
// 177 S1): per slot (a device and a kind of event), the first record goes at
// once, then at most one per period with the count of the events since the
// last. The main thread's, until 5b-3's log queues and rate limit replace
// it.

#include "scada/driver/channel_engine.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace scada::driver::detail {

// The period: 10 s (driver interface contract 11.3).
inline constexpr std::int64_t EVENT_LOG_PERIOD_NS = 10'000'000'000;

class EventThrottle {
public:
    // `slots` slots, every one free to log at once. `period_ns` is at least
    // 0. May throw std::bad_alloc.
    explicit EventThrottle(std::size_t slots,
                           std::int64_t period_ns = EVENT_LOG_PERIOD_NS);

    // Adds the events of `record` to `slot`: the counts add up, and the
    // details become the record's when it has any, else stay (S4). A slot
    // out of range is ignored.
    void add(std::size_t slot, const EventRecord& record) noexcept;

    // The events of `slot` to log at `now_ns` (monotonic) into `out`: when
    // it holds any and its period ended, or with `all` (the stop). The
    // period starts again at `now_ns`, and the slot is empty. False when
    // nothing is to be logged.
    bool take(std::size_t slot, std::int64_t now_ns, bool all,
              EventRecord& out) noexcept;

private:
    struct Pending {
        EventRecord record;
        std::int64_t next_ns = 0;  // the earliest time of the next record
    };

    std::vector<Pending> pending_;  // fixed size
    std::int64_t period_ns_;
};

}  // namespace scada::driver::detail

#endif  // SCADA_DRIVER_SRC_EVENT_THROTTLE_H
