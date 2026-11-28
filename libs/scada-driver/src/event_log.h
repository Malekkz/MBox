#ifndef SCADA_DRIVER_SRC_EVENT_LOG_H
#define SCADA_DRIVER_SRC_EVENT_LOG_H

// A worker's log of its devices' identity and restart events (driver
// interface contract 11.3; decisions D-147, D-148): the records go into the
// worker's own LogQueue, rate-limited per kind and device by LogLimit. It
// replaces 5b-2b's event slot and the main thread's throttle.
//
// Worker thread only. Nothing after the constructor allocates, locks or
// makes a system call. A record the full queue refuses gives its count back
// to the limit, so that the next summary carries it (PR 182 M2); the queue
// counts the refused record itself in its drops.

#include "log_limit.h"
#include "scada/common/log.h"
#include "scada/common/realtime.h"
#include "scada/driver/channel_engine.h"
#include "scada/driver/compiled.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace scada::driver::detail {

// The REASON of an identity mismatch's record (D-147, ruling item 3).
[[nodiscard]] std::string_view reason_text(std::uint8_t reason) noexcept;

// The REASON of a connection's lost or closed record (D-149).
[[nodiscard]] std::string_view close_reason_text(CloseReason reason) noexcept;

class EventLog {
public:
    // The log of `channel`'s devices into `queue`; a null queue logs
    // nothing. `channel` must outlive it. May throw std::bad_alloc.
    EventLog(const CompiledChannel& channel, std::shared_ptr<LogQueue> queue,
             std::int64_t period_ns = LOG_LIMIT_PERIOD_NS);

    // The events of `kind` of device `device` that the engine noted since
    // the last take: logged at once when the limit lets them, else held
    // and counted. `now_ns` is the monotonic clock, `wall_ns` the record's
    // time.
    SCADA_RT void add(std::size_t device, DeviceEvent kind,
                      const EventRecord& events, std::int64_t now_ns,
                      std::int64_t wall_ns) noexcept;

    // The same for the channel's connection events of `kind` (D-149),
    // limited per kind for the channel.
    SCADA_RT void add_channel(ChannelEvent kind, const EventRecord& events,
                              std::int64_t now_ns,
                              std::int64_t wall_ns) noexcept;

    // Logs the summaries whose period ended by `now_ns`, as many as the
    // queue has room for; the rest stay due for the next call.
    SCADA_RT void due(std::int64_t now_ns, std::int64_t wall_ns) noexcept;

    // At the stop: logs held summaries while the queue has room, and true
    // when none is left. False when the queue is full with summaries still
    // held: the caller waits for room and calls it again. With `last`, logs
    // every one, room or not: a full queue drops them, and counts them in
    // its drops. Takes no time of its own: the wait is the caller's, so
    // this too is SCADA_RT.
    SCADA_RT bool flush(std::int64_t wall_ns, bool last) noexcept;

private:
    // The record of limit kind `kind` (a DeviceEvent, or DEVICE_EVENTS
    // plus a ChannelEvent) and key `key` (a device, or the channel's slot).
    // False if the queue refused the record.
    SCADA_RT bool push(std::size_t kind, std::size_t key,
                       const EventRecord& events, bool details,
                       std::int64_t wall_ns) noexcept;
    SCADA_RT bool push_device(std::size_t device, DeviceEvent kind,
                              const EventRecord& events, bool details,
                              std::int64_t wall_ns) noexcept;
    SCADA_RT bool push_channel(ChannelEvent kind, const EventRecord& events,
                               bool details, std::int64_t wall_ns) noexcept;

    const CompiledChannel* channel_;
    std::shared_ptr<LogQueue> queue_;
    LogLimit limit_;
};

// A worker's stop (PR 182 M2): flushes `log` while its queue has room,
// calling `wait` between tries while it is full, at most `waits` times; then
// a last flush, which drops and counts what is still held, so that every
// held count reaches `log_dropped` as a drop if not as a record. `wall_ns`
// is the records' time. Not real-time: `wait` waits.
void flush_at_stop(EventLog& log, std::int64_t wall_ns, int waits,
                   const std::function<void()>& wait) noexcept;

}  // namespace scada::driver::detail

#endif  // SCADA_DRIVER_SRC_EVENT_LOG_H
