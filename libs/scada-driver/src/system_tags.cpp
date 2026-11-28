#include "system_tags.h"

#include "scada/driver/channel_engine.h"
#include "scada/driver/compiled.h"
#include "scada/driver/diagnostics.h"
#include "scada/driver/failure.h"
#include "scada/driver/plan.h"
#include "scada/driver/scheduler.h"
#include "scada/tag/scada_tag.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace scada::driver::detail {
namespace {

constexpr double NS_PER_MS = 1'000'000.0;
constexpr std::int64_t MAX_NS = std::numeric_limits<std::int64_t>::max();

constexpr std::array<SignalSpec, CHANNEL_SIGNALS> CHANNEL_SPECS{{
    {.name = "state", .type = SCADA_TYPE_INT64},
    {.name = "connects", .type = SCADA_TYPE_UINT64},
    {.name = "refused_connects", .type = SCADA_TYPE_UINT64},
    {.name = "secured", .type = SCADA_TYPE_BOOL},
    {.name = "requests_per_s", .type = SCADA_TYPE_FLOAT64},
}};

constexpr std::array<SignalSpec, DEVICE_SIGNALS> DEVICE_SPECS{{
    {.name = "state", .type = SCADA_TYPE_INT64},
    {.name = "last_ok_time", .type = SCADA_TYPE_INT64},
    {.name = "requests", .type = SCADA_TYPE_UINT64},
    {.name = "errors", .type = SCADA_TYPE_UINT64},
    {.name = "timeouts", .type = SCADA_TYPE_UINT64},
    {.name = "response_ms", .type = SCADA_TYPE_FLOAT64},
    {.name = "requests_per_scan", .type = SCADA_TYPE_UINT64},
    {.name = "pacing_limited", .type = SCADA_TYPE_BOOL},
    {.name = "last_status", .type = SCADA_TYPE_INT64},
    {.name = "exceptions", .type = SCADA_TYPE_UINT64},
    {.name = "refusals", .type = SCADA_TYPE_UINT64},
    {.name = "sentinels", .type = SCADA_TYPE_UINT64},
    {.name = "invalid", .type = SCADA_TYPE_UINT64},
    {.name = "points_absent", .type = SCADA_TYPE_UINT64},
    {.name = "restarts", .type = SCADA_TYPE_UINT64},
    {.name = "identity_ok", .type = SCADA_TYPE_BOOL},
    {.name = "identity", .type = SCADA_TYPE_STRING},
}};

constexpr std::array<SignalSpec, PROCESS_SIGNALS> PROCESS_SPECS{{
    {.name = "realtime", .type = SCADA_TYPE_BOOL},
    {.name = "config_id", .type = SCADA_TYPE_UINT64},
    {.name = "config_state", .type = SCADA_TYPE_INT64},
    {.name = "stale_outcomes", .type = SCADA_TYPE_UINT64},
    {.name = "answers_dropped", .type = SCADA_TYPE_UINT64},
    {.name = "safe_unjournalled", .type = SCADA_TYPE_UINT64},
    {.name = "log_dropped", .type = SCADA_TYPE_UINT64},
}};

std::uint64_t bits_of(std::int64_t value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}
std::uint64_t bits_of(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}
std::uint64_t bits_of(bool value) noexcept {
    return value ? 1U : 0U;
}

// FNV-1a of `text`: a change of the identity text is seen without keeping
// a copy of it in the slot.
std::uint64_t hash_of(std::string_view text) noexcept {
    constexpr std::uint64_t OFFSET = 14695981039346656037ULL;
    constexpr std::uint64_t PRIME = 1099511628211ULL;
    std::uint64_t hash = OFFSET;
    for (const char byte : text) {
        hash = (hash ^ static_cast<unsigned char>(byte)) * PRIME;
    }
    return hash;
}

std::size_t index_of(ChannelSignal signal) noexcept {
    return static_cast<std::size_t>(signal);
}

}  // namespace

const SignalSpec& channel_signal(ChannelSignal signal) noexcept {
    return CHANNEL_SPECS.at(index_of(signal));
}

const SignalSpec& device_signal(DeviceSignal signal) noexcept {
    return DEVICE_SPECS.at(static_cast<std::size_t>(signal));
}

const SignalSpec& process_signal(ProcessSignal signal) noexcept {
    return PROCESS_SPECS.at(static_cast<std::size_t>(signal));
}

std::string channel_tag_name(std::string_view channel,
                             std::string_view signal) {
    std::string name = "sys.drv.";
    name.append(channel).append(".").append(signal);
    return name;
}

std::string device_tag_name(std::string_view device, std::string_view signal) {
    std::string name = "sys.dev.";
    name.append(device).append(".").append(signal);
    return name;
}

std::string process_tag_name(std::string_view process,
                             std::string_view signal) {
    std::string name = "sys.proc.";
    name.append(process).append(".").append(signal);
    return name;
}

double rounded_ms(double average_ms) noexcept {
    // Averages are finite and far below 2^53 tenths of a ms. A division by
    // 10 rounds once, to the double nearest the tenth; a product with 0.1
    // would round twice (review, the delta check).
    return std::round(average_ms * TENTHS_PER_MS) / TENTHS_PER_MS;
}

bool write_slot(TagWriteFn write, scada_ctx* ctx, SystemSlot& slot,
                std::uint64_t bits, bool all, std::uint64_t& refused) noexcept {
    if (slot.handle == SCADA_INVALID_HANDLE ||
        (!all && slot.written && slot.last == bits)) {
        return false;
    }
    // GOOD, no flags, source time 0: the library stores its own update
    // time with LOCAL_TIME (TD 13), the time the value was taken.
    scada_value value{};
    value.type = slot.type;
    value.value = bits;
    value.quality = SCADA_QUALITY_GOOD;
    value.quality_flags = 0;
    value.source_time_ns = 0;
    if (write(ctx, slot.handle, &value) != SCADA_OK) {
        ++refused;
        return false;
    }
    slot.written = true;
    slot.last = bits;
    return true;
}

ProcessTags::ProcessTags(scada_ctx* ctx, const Slots& slots,
                         TagWriteFn write) noexcept
    : ctx_(ctx), write_(write), slots_(slots) {}

bool ProcessTags::publish_start() noexcept {
    static_assert(static_cast<int>(ConfigState::LOADING) == 0);
    // Every signal 0 at the start: false, no `config_id`, LOADING, no count.
    bool wrote = false;
    for (SystemSlot& slot : slots_) {
        wrote = write_slot(write_, ctx_, slot, 0, true, refused_) || wrote;
    }
    return wrote;
}

bool ProcessTags::applied() noexcept {
    SystemSlot& state =
        slots_.at(static_cast<std::size_t>(ProcessSignal::CONFIG_STATE));
    return write_slot(write_, ctx_, state,
                      bits_of(static_cast<std::int64_t>(ConfigState::APPLIED)),
                      false, refused_);
}

bool ProcessTags::stopped() noexcept {
    SystemSlot& state =
        slots_.at(static_cast<std::size_t>(ProcessSignal::CONFIG_STATE));
    return write_slot(write_, ctx_, state,
                      bits_of(static_cast<std::int64_t>(ConfigState::LOADING)),
                      false, refused_);
}

SystemTags::SystemTags(scada_ctx* ctx, SystemTagHandles handles) noexcept
    : ctx_(ctx),
      write_(handles.write),
      write_text_(handles.write_text),
      handles_(std::move(handles)) {}

std::size_t SystemTags::device_slot(std::size_t device,
                                    DeviceSignal signal) const noexcept {
    return CHANNEL_SIGNALS + handles_.groups + (device * DEVICE_SIGNALS) +
           static_cast<std::size_t>(signal);
}

bool SystemTags::put(std::size_t slot, std::uint64_t bits, bool all) noexcept {
    if (slot >= handles_.slots.size()) {
        return false;
    }
    return write_slot(write_, ctx_, handles_.slots[slot], bits, all, refused_);
}

bool SystemTags::put_text(std::size_t slot, std::string_view text,
                          bool all) noexcept {
    if (slot >= handles_.slots.size()) {
        return false;
    }
    SystemSlot& target = handles_.slots[slot];
    const std::uint64_t hash = hash_of(text);
    if (target.handle == SCADA_INVALID_HANDLE ||
        (!all && target.written && target.last == hash)) {
        return false;
    }
    // GOOD, no flags, source time 0, as every system tag.
    if (write_text_(ctx_, target.handle, text.data(), text.size(),
                    SCADA_QUALITY_GOOD, 0, 0) != SCADA_OK) {
        ++refused_;
        return false;
    }
    target.written = true;
    target.last = hash;
    return true;
}

bool SystemTags::put_identity(ChannelEngine& engine,
                              const CompiledChannel& channel,
                              bool all) noexcept {
    bool wrote = false;
    for (std::size_t device = 0;
         device < handles_.devices && device < channel.devices.size();
         ++device) {
        const auto index = static_cast<std::uint32_t>(device);
        const std::optional<bool> matched = engine.identity_ok(index);
        if (!matched.has_value()) {
            continue;  // no verdict yet: NOT_INIT from the walk of 9.1
        }
        wrote = put(device_slot(device, DeviceSignal::IDENTITY_OK),
                    bits_of(*matched), all) ||
                wrote;
        wrote = put_text(device_slot(device, DeviceSignal::IDENTITY),
                         engine.identity_text(index), all) ||
                wrote;
    }
    return wrote;
}

bool SystemTags::link(ChannelLink link) noexcept {
    return put(index_of(ChannelSignal::STATE),
               bits_of(static_cast<std::int64_t>(link)), false);
}

bool SystemTags::flush(ChannelEngine& engine, const CompiledChannel& channel,
                       std::int64_t now_ns) noexcept {
    return put_counters(engine, channel, now_ns, false);
}

bool SystemTags::stopped(const CompiledChannel& channel) noexcept {
    bool wrote = put(
        index_of(ChannelSignal::STATE),
        bits_of(static_cast<std::int64_t>(ChannelLink::DISCONNECTED)), false);
    for (std::size_t device = 0;
         device < handles_.devices && device < channel.devices.size();
         ++device) {
        const DeviceStatus state = channel.devices[device].enabled
                                       ? DeviceStatus::CONNECTING
                                       : DeviceStatus::DISABLED;
        wrote = put(device_slot(device, DeviceSignal::STATE),
                    bits_of(static_cast<std::int64_t>(state)), false) ||
                wrote;
        wrote = put(device_slot(device, DeviceSignal::PACING_LIMITED),
                    bits_of(false), false) ||
                wrote;
    }
    return wrote;
}

bool SystemTags::put_states(ChannelEngine& engine,
                            const CompiledChannel& channel, std::int64_t now_ns,
                            bool all) noexcept {
    bool wrote = put(index_of(ChannelSignal::STATE),
                     bits_of(static_cast<std::int64_t>(engine.link())), all);
    // No phase 2 channel is authenticated and encrypted (reference 13.3).
    wrote = put(index_of(ChannelSignal::SECURED), bits_of(false), all) || wrote;
    for (std::size_t device = 0;
         device < handles_.devices && device < channel.devices.size();
         ++device) {
        const auto index = static_cast<std::uint32_t>(device);
        const DeviceStatus state = engine.failures().state(index);
        wrote = put(device_slot(device, DeviceSignal::STATE),
                    bits_of(static_cast<std::int64_t>(state)), all) ||
                wrote;
        const bool limited = channel.devices[device].enabled &&
                             engine.scheduler().pacing_limited(index, now_ns);
        wrote = put(device_slot(device, DeviceSignal::PACING_LIMITED),
                    bits_of(limited), all) ||
                wrote;
    }
    return put_identity(engine, channel, all) || wrote;
}

bool SystemTags::put_counters(ChannelEngine& engine,
                              const CompiledChannel& channel,
                              std::int64_t now_ns, bool all) noexcept {
    const EngineCounts& counts = engine.counts();
    bool wrote = put(index_of(ChannelSignal::CONNECTS), counts.connects, all);
    wrote =
        put(index_of(ChannelSignal::REFUSED_CONNECTS), counts.refused, all) ||
        wrote;
    wrote = put(index_of(ChannelSignal::REQUESTS_PER_S),
                bits_of(engine.requests_per_s(now_ns)), all) ||
            wrote;
    for (std::size_t group = 0;
         group < handles_.groups && group < channel.poll_groups.size();
         ++group) {
        const double period_ms =
            static_cast<double>(engine.scheduler().achieved_period_ns(
                static_cast<std::uint32_t>(group), now_ns)) /
            NS_PER_MS;
        wrote =
            put(CHANNEL_SIGNALS + group, bits_of(rounded_ms(period_ms)), all) ||
            wrote;
    }
    for (std::size_t device = 0;
         device < handles_.devices && device < channel.devices.size();
         ++device) {
        const auto index = static_cast<std::uint32_t>(device);
        const DeviceCounts& diag = engine.device_counts(index);
        const auto slot = [&](DeviceSignal signal) {
            return device_slot(device, signal);
        };
        wrote = put(slot(DeviceSignal::LAST_OK_TIME),
                    bits_of(diag.last_ok_time_ns), all) ||
                wrote;
        wrote = put(slot(DeviceSignal::REQUESTS), diag.requests, all) || wrote;
        wrote = put(slot(DeviceSignal::ERRORS), diag.errors, all) || wrote;
        wrote = put(slot(DeviceSignal::TIMEOUTS), diag.timeouts, all) || wrote;
        wrote = put(slot(DeviceSignal::RESPONSE_MS),
                    bits_of(rounded_ms(engine.response_ms(index))), all) ||
                wrote;
        wrote = put(slot(DeviceSignal::REQUESTS_PER_SCAN),
                    requests_per_scan(channel.devices[device].plan), all) ||
                wrote;
        wrote = put(slot(DeviceSignal::LAST_STATUS), bits_of(diag.last_status),
                    all) ||
                wrote;
        wrote =
            put(slot(DeviceSignal::EXCEPTIONS), diag.exceptions, all) || wrote;
        wrote = put(slot(DeviceSignal::REFUSALS), diag.refusals, all) || wrote;
        wrote =
            put(slot(DeviceSignal::SENTINELS), diag.sentinels, all) || wrote;
        wrote = put(slot(DeviceSignal::INVALID), diag.invalid, all) || wrote;
        wrote = put(slot(DeviceSignal::POINTS_ABSENT),
                    engine.points_absent(index), all) ||
                wrote;
        wrote =
            put(slot(DeviceSignal::RESTARTS), engine.restarts(index), all) ||
            wrote;
    }
    return wrote;
}

bool SystemTags::publish_idle(const CompiledChannel& channel) noexcept {
    // A channel never connected, its devices DISABLED, no secure channel,
    // every count, time and average 0: all of them are 0 but a device's
    // `requests_per_scan`, which is what a scan of its plan would send.
    static_assert(static_cast<int>(ChannelLink::DISCONNECTED) == 0);
    static_assert(static_cast<int>(DeviceStatus::DISABLED) == 0);
    bool wrote = false;
    for (std::size_t slot = 0; slot < CHANNEL_SIGNALS + handles_.groups;
         ++slot) {
        wrote = put(slot, 0, true) || wrote;
    }
    for (std::size_t device = 0;
         device < handles_.devices && device < channel.devices.size();
         ++device) {
        for (std::size_t signal = 0; signal < DEVICE_SIGNALS; ++signal) {
            const auto which = static_cast<DeviceSignal>(signal);
            if (which == DeviceSignal::IDENTITY_OK ||
                which == DeviceSignal::IDENTITY) {
                continue;  // no verdict: NOT_INIT from the walk of 9.1
            }
            const std::uint64_t bits =
                which == DeviceSignal::REQUESTS_PER_SCAN
                    ? requests_per_scan(channel.devices[device].plan)
                    : 0;
            wrote = put(device_slot(device, which), bits, true) || wrote;
        }
    }
    return wrote;
}

bool SystemTags::publish(ChannelEngine& engine, const CompiledChannel& channel,
                         std::int64_t now_ns, bool all) noexcept {
    bool wrote = put_states(engine, channel, now_ns, all);
    if (all || now_ns >= due_ns_) {
        wrote = put_counters(engine, channel, now_ns, all) || wrote;
        due_ns_ = now_ns > MAX_NS - PERIOD_NS ? MAX_NS : now_ns + PERIOD_NS;
    }
    return wrote;
}

}  // namespace scada::driver::detail
