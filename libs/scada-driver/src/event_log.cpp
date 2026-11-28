#include "event_log.h"

#include "log_limit.h"
#include "scada/common/log.h"
#include "scada/driver/channel_engine.h"
#include "scada/driver/compiled.h"
#include "scada/driver/identity_run.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

namespace scada::driver::detail {

namespace {

// Decimal digits of the largest 64-bit unsigned value.
constexpr std::size_t DIGITS = 20;

// `value` in decimal in `buffer`, without allocating.
class Number {
public:
    explicit Number(std::uint64_t value) noexcept {
        const auto result = std::to_chars(
            buffer_.data(), buffer_.data() + buffer_.size(), value);
        length_ = static_cast<std::size_t>(result.ptr - buffer_.data());
    }
    [[nodiscard]] std::string_view view() const noexcept {
        return {buffer_.data(), length_};
    }

private:
    std::array<char, DIGITS> buffer_{};
    std::size_t length_ = 0;
};

}  // namespace

std::string_view close_reason_text(CloseReason reason) noexcept {
    switch (reason) {
        case CloseReason::PEER:
            return "peer";
        case CloseReason::TIMEOUTS:
            return "time-outs";
        case CloseReason::NOT_RESUMED:
            return "not resumed";
        case CloseReason::PARTIAL:
            return "partial frame";
        case CloseReason::BROKEN:
            return "broken answer";
        case CloseReason::SIDE_EFFECT:
            return "side-effect read";
        case CloseReason::REFUSED:
            return "refused";
        case CloseReason::TIMED_OUT:
            return "timed out";
        case CloseReason::UNREACHABLE:
            return "unreachable";
        case CloseReason::OPEN_FAILED:
            return "open failed";
    }
    return "unknown";
}

std::string_view reason_text(std::uint8_t reason) noexcept {
    switch (static_cast<MismatchReason>(reason)) {
        case MismatchReason::VALUE:
            return "value";
        case MismatchReason::ABSENT:
            return "absent";
        case MismatchReason::UNREADABLE:
            return "unreadable";
    }
    return "unknown";
}

EventLog::EventLog(const CompiledChannel& channel,
                   std::shared_ptr<LogQueue> queue, std::int64_t period_ns)
    : channel_(&channel),
      queue_(std::move(queue)),
      // The channel's kinds after the devices', keyed on the limit's slot
      // of no key.
      limit_(DEVICE_EVENTS + CHANNEL_EVENTS, channel.devices.size(),
             period_ns) {}

void EventLog::add(std::size_t device, DeviceEvent kind,
                   const EventRecord& events, std::int64_t now_ns,
                   std::int64_t wall_ns) noexcept {
    if (events.count == 0) {
        return;
    }
    const auto which = static_cast<std::size_t>(kind);
    if (limit_.admit(which, device, now_ns, events.count) &&
        !push(which, device, events, events.has_details, wall_ns)) {
        limit_.give_back(which, device, events.count);
    }
}

void EventLog::add_channel(ChannelEvent kind, const EventRecord& events,
                           std::int64_t now_ns, std::int64_t wall_ns) noexcept {
    if (events.count == 0) {
        return;
    }
    const std::size_t which = DEVICE_EVENTS + static_cast<std::size_t>(kind);
    const std::size_t key = channel_->devices.size();
    if (limit_.admit(which, key, now_ns, events.count) &&
        !push(which, key, events, events.has_details, wall_ns)) {
        limit_.give_back(which, key, events.count);
    }
}

void EventLog::due(std::int64_t now_ns, std::int64_t wall_ns) noexcept {
    LogSummary summary;
    // While the queue is full the summaries stay due, and go at a later
    // step once the Logger's thread made room, not a period later. One
    // refused all the same is given back, with a new period, so the loop
    // does not take it again.
    while ((!queue_ || !queue_->full()) && limit_.next_due(now_ns, summary)) {
        if (!push(summary.kind, summary.key,
                  EventRecord{.count = summary.count}, false, wall_ns)) {
            limit_.give_back(summary.kind, summary.key, summary.count);
        }
    }
}

bool EventLog::flush(std::int64_t wall_ns, bool last) noexcept {
    LogSummary summary;
    for (;;) {
        if (!last && queue_ && queue_->full()) {
            return !limit_.holds();
        }
        if (!limit_.next_held(summary)) {
            return true;
        }
        if (!push(summary.kind, summary.key,
                  EventRecord{.count = summary.count}, false, wall_ns) &&
            !last) {
            limit_.give_back(summary.kind, summary.key, summary.count);
            return false;
        }
    }
}

void flush_at_stop(EventLog& log, std::int64_t wall_ns, int waits,
                   const std::function<void()>& wait) noexcept {
    for (int step = 0; step < waits; ++step) {
        if (log.flush(wall_ns, false)) {
            return;
        }
        wait();
    }
    // Out of time: what is left is dropped, and counted (D-148).
    (void)log.flush(wall_ns, true);
}

// A summary carries the count of the events it stands for and no details:
// they were in the first record of the period, or in no record at all
// (D-148).
bool EventLog::push(std::size_t kind, std::size_t key,
                    const EventRecord& events, bool details,
                    std::int64_t wall_ns) noexcept {
    if (!queue_) {
        return true;  // nothing to log into: nothing to give back
    }
    if (kind >= DEVICE_EVENTS) {
        return push_channel(static_cast<ChannelEvent>(kind - DEVICE_EVENTS),
                            events, details, wall_ns);
    }
    if (key >= channel_->devices.size()) {
        return true;  // no device to name
    }
    return push_device(key, static_cast<DeviceEvent>(kind), events, details,
                       wall_ns);
}

bool EventLog::push_channel(ChannelEvent kind, const EventRecord& events,
                            bool details, std::int64_t wall_ns) noexcept {
    const std::string_view channel = channel_->name;
    const Number count(events.count);
    const std::string_view reason =
        close_reason_text(static_cast<CloseReason>(events.reason));
    switch (kind) {
        case ChannelEvent::LOST:
            if (!details) {
                return queue_->push(LogLevel::WARNING, wall_ns,
                                    "connection lost",
                                    {{"MESSAGE_ID", "driver-connection-lost"},
                                     {"CHANNEL", channel},
                                     {"COUNT", count.view()}});
            }
            return queue_->push(LogLevel::WARNING, wall_ns, "connection lost",
                                {{"MESSAGE_ID", "driver-connection-lost"},
                                 {"CHANNEL", channel},
                                 {"REASON", reason},
                                 {"COUNT", count.view()}});
        case ChannelEvent::REFUSED:
            if (!details) {
                return queue_->push(
                    LogLevel::WARNING, wall_ns, "connect failed",
                    {{"MESSAGE_ID", "driver-connection-refused"},
                     {"CHANNEL", channel},
                     {"COUNT", count.view()}});
            }
            return queue_->push(LogLevel::WARNING, wall_ns, "connect failed",
                                {{"MESSAGE_ID", "driver-connection-refused"},
                                 {"CHANNEL", channel},
                                 {"REASON", reason},
                                 {"COUNT", count.view()}});
        case ChannelEvent::CLOSED:
            if (!details) {
                return queue_->push(LogLevel::WARNING, wall_ns,
                                    "connection closed by the driver",
                                    {{"MESSAGE_ID", "driver-connection-closed"},
                                     {"CHANNEL", channel},
                                     {"COUNT", count.view()}});
            }
            return queue_->push(LogLevel::WARNING, wall_ns,
                                "connection closed by the driver",
                                {{"MESSAGE_ID", "driver-connection-closed"},
                                 {"CHANNEL", channel},
                                 {"REASON", reason},
                                 {"COUNT", count.view()}});
    }
    return true;  // no such kind: nothing to log
}

bool EventLog::push_device(std::size_t device, DeviceEvent kind,
                           const EventRecord& events, bool details,
                           std::int64_t wall_ns) noexcept {
    const std::string_view channel = channel_->name;
    const std::string_view name = channel_->devices[device].name;
    const Number count(events.count);
    const std::string_view text(events.text.data(),
                                std::min(events.length, events.text.size()));
    switch (kind) {
        case DeviceEvent::MISMATCH: {
            if (!details) {
                return queue_->push(LogLevel::WARNING, wall_ns,
                                    "identity mismatch",
                                    {{"MESSAGE_ID", "driver-identity-mismatch"},
                                     {"CHANNEL", channel},
                                     {"DEVICE", name},
                                     {"COUNT", count.view()}});
            }
            const Number check(events.check);
            return queue_->push(LogLevel::WARNING, wall_ns, "identity mismatch",
                                {{"MESSAGE_ID", "driver-identity-mismatch"},
                                 {"CHANNEL", channel},
                                 {"DEVICE", name},
                                 {"REASON", reason_text(events.reason)},
                                 {"CHECK", check.view()},
                                 {"VALUE", text},
                                 {"COUNT", count.view()}});
        }
        case DeviceEvent::MATCH:
            if (!details) {
                return queue_->push(LogLevel::NOTICE, wall_ns,
                                    "identity matched",
                                    {{"MESSAGE_ID", "driver-identity-matched"},
                                     {"CHANNEL", channel},
                                     {"DEVICE", name},
                                     {"COUNT", count.view()}});
            }
            return queue_->push(LogLevel::NOTICE, wall_ns, "identity matched",
                                {{"MESSAGE_ID", "driver-identity-matched"},
                                 {"CHANNEL", channel},
                                 {"DEVICE", name},
                                 {"IDENTITY", text},
                                 {"COUNT", count.view()}});
        case DeviceEvent::DEMOTED:
            return queue_->push(LogLevel::WARNING, wall_ns, "device demoted",
                                {{"MESSAGE_ID", "driver-device-demoted"},
                                 {"CHANNEL", channel},
                                 {"DEVICE", name},
                                 {"COUNT", count.view()}});
        case DeviceEvent::RECOVERED:
            return queue_->push(LogLevel::NOTICE, wall_ns, "device recovered",
                                {{"MESSAGE_ID", "driver-device-recovered"},
                                 {"CHANNEL", channel},
                                 {"DEVICE", name},
                                 {"COUNT", count.view()}});
        case DeviceEvent::POINT_ABSENT: {
            if (!details) {
                return queue_->push(LogLevel::WARNING, wall_ns, "points absent",
                                    {{"MESSAGE_ID", "driver-point-absent"},
                                     {"CHANNEL", channel},
                                     {"DEVICE", name},
                                     {"COUNT", count.view()}});
            }
            const Number points(events.check);
            return queue_->push(LogLevel::WARNING, wall_ns, "points absent",
                                {{"MESSAGE_ID", "driver-point-absent"},
                                 {"CHANNEL", channel},
                                 {"DEVICE", name},
                                 {"POINTS", points.view()},
                                 {"COUNT", count.view()}});
        }
        case DeviceEvent::RESTART:
            return queue_->push(LogLevel::WARNING, wall_ns, "device restart",
                                {{"MESSAGE_ID", "driver-device-restart"},
                                 {"CHANNEL", channel},
                                 {"DEVICE", name},
                                 {"COUNT", count.view()}});
    }
    return true;  // no such kind: nothing to log
}

}  // namespace scada::driver::detail
