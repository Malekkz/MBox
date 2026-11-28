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
      limit_(DEVICE_EVENTS, channel.devices.size(), period_ns) {}

void EventLog::add(std::size_t device, DeviceEvent kind,
                   const EventRecord& events, std::int64_t now_ns,
                   std::int64_t wall_ns) noexcept {
    if (events.count == 0) {
        return;
    }
    if (limit_.admit(static_cast<std::size_t>(kind), device, now_ns,
                     events.count)) {
        push(device, kind, events, events.has_details, wall_ns);
    }
}

void EventLog::due(std::int64_t now_ns, std::int64_t wall_ns) noexcept {
    LogSummary summary;
    while (limit_.next_due(now_ns, summary)) {
        push(summary.key, static_cast<DeviceEvent>(summary.kind),
             EventRecord{.count = summary.count}, false, wall_ns);
    }
}

void EventLog::flush(std::int64_t wall_ns) noexcept {
    LogSummary summary;
    while (limit_.next_held(summary)) {
        push(summary.key, static_cast<DeviceEvent>(summary.kind),
             EventRecord{.count = summary.count}, false, wall_ns);
    }
}

// A summary carries the count of the events it stands for and no details:
// they were in the first record of the period, or in no record at all
// (D-148).
void EventLog::push(std::size_t device, DeviceEvent kind,
                    const EventRecord& events, bool details,
                    std::int64_t wall_ns) noexcept {
    if (!queue_ || device >= channel_->devices.size()) {
        return;
    }
    const std::string_view channel = channel_->name;
    const std::string_view name = channel_->devices[device].name;
    const Number count(events.count);
    const std::string_view text(events.text.data(),
                                std::min(events.length, events.text.size()));
    switch (kind) {
        case DeviceEvent::MISMATCH: {
            if (!details) {
                (void)queue_->push(LogLevel::WARNING, wall_ns,
                                   "identity mismatch",
                                   {{"MESSAGE_ID", "driver-identity-mismatch"},
                                    {"CHANNEL", channel},
                                    {"DEVICE", name},
                                    {"COUNT", count.view()}});
                return;
            }
            const Number check(events.check);
            (void)queue_->push(LogLevel::WARNING, wall_ns, "identity mismatch",
                               {{"MESSAGE_ID", "driver-identity-mismatch"},
                                {"CHANNEL", channel},
                                {"DEVICE", name},
                                {"REASON", reason_text(events.reason)},
                                {"CHECK", check.view()},
                                {"VALUE", text},
                                {"COUNT", count.view()}});
            return;
        }
        case DeviceEvent::MATCH:
            if (!details) {
                (void)queue_->push(LogLevel::NOTICE, wall_ns,
                                   "identity matched",
                                   {{"MESSAGE_ID", "driver-identity-matched"},
                                    {"CHANNEL", channel},
                                    {"DEVICE", name},
                                    {"COUNT", count.view()}});
                return;
            }
            (void)queue_->push(LogLevel::NOTICE, wall_ns, "identity matched",
                               {{"MESSAGE_ID", "driver-identity-matched"},
                                {"CHANNEL", channel},
                                {"DEVICE", name},
                                {"IDENTITY", text},
                                {"COUNT", count.view()}});
            return;
        case DeviceEvent::RESTART:
            (void)queue_->push(LogLevel::WARNING, wall_ns, "device restart",
                               {{"MESSAGE_ID", "driver-device-restart"},
                                {"CHANNEL", channel},
                                {"DEVICE", name},
                                {"COUNT", count.view()}});
            return;
    }
}

}  // namespace scada::driver::detail
