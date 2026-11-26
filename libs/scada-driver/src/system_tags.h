#ifndef SCADA_DRIVER_SRC_SYSTEM_TAGS_H
#define SCADA_DRIVER_SRC_SYSTEM_TAGS_H

// The system tags of one channel and its devices (driver interface contract
// 11.1, decision D-144), written by the channel's worker, which owns them
// with the channel's point tags (CS 12.5).

#include "scada/driver/channel_engine.h"
#include "scada/driver/compiled.h"
#include "scada/driver/diagnostics.h"
#include "scada/tag/scada_tag.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scada::driver::detail {

// The signals of `sys.drv.<channel>.<signal>` this slice writes, in slot
// order; `period.<group>` follow, one per poll group.
enum class ChannelSignal : std::uint8_t {
    STATE = 0,
    CONNECTS,
    REFUSED_CONNECTS,
    SECURED,
    REQUESTS_PER_S,
};
inline constexpr std::size_t CHANNEL_SIGNALS = 5;

// The signals of `sys.dev.<device>.<signal>` this slice writes, in slot
// order. `identity`, `identity_ok` and `restarts` come with slice 5b-2,
// `selections_lost` and `last_command_stage` with the command slices,
// `refresh_failures` with phase 5 (D-144).
enum class DeviceSignal : std::uint8_t {
    STATE = 0,
    LAST_OK_TIME,
    REQUESTS,
    ERRORS,
    TIMEOUTS,
    RESPONSE_MS,
    REQUESTS_PER_SCAN,
    PACING_LIMITED,
    LAST_STATUS,
    EXCEPTIONS,
    REFUSALS,
    SENTINELS,
    INVALID,
    POINTS_ABSENT,
};
inline constexpr std::size_t DEVICE_SIGNALS = 14;

// One signal's name and tag type (11.1).
struct SignalSpec {
    std::string_view name;
    std::uint8_t type;  // SCADA_TYPE_*
};

[[nodiscard]] const SignalSpec& channel_signal(ChannelSignal signal) noexcept;
[[nodiscard]] const SignalSpec& device_signal(DeviceSignal signal) noexcept;

// `sys.drv.<channel>.<signal>` and `sys.dev.<device>.<signal>`.
[[nodiscard]] std::string channel_tag_name(std::string_view channel,
                                           std::string_view signal);
[[nodiscard]] std::string device_tag_name(std::string_view device,
                                          std::string_view signal);

// One system tag: its handle and type, and the value last written.
struct SystemSlot {
    std::uint32_t handle = SCADA_INVALID_HANDLE;
    std::uint8_t type = 0;
    bool written = false;
    std::uint64_t last = 0;  // the bits of the value last written
};

// The slots of a channel: CHANNEL_SIGNALS, then one `period.<group>` per
// poll group, then DEVICE_SIGNALS per device of the channel (disabled ones
// too). Filled by the main thread when it resolves the tags.
struct SystemTagHandles {
    std::vector<SystemSlot> slots;
    std::size_t groups = 0;
    std::size_t devices = 0;
};

class SystemTags {
public:
    // How often counters, times and averages are written at most (11.1).
    static constexpr std::int64_t PERIOD_NS = 1'000'000'000;

    // `ctx` is borrowed and used only from the worker's thread.
    SystemTags(scada_ctx* ctx, SystemTagHandles handles) noexcept;

    // Writes what changed: the states at every call, the other signals at
    // most once every PERIOD_NS of `now_ns`; with `all`, every signal at
    // once, changed or not (9.1: "Diagnostic tags are written with their
    // current values"). Returns true when it wrote anything. Not real-time:
    // a worker is not (DI 2.5).
    bool publish(ChannelEngine& engine, const CompiledChannel& channel,
                 std::int64_t now_ns, bool all) noexcept;

    // Writes every signal of a channel that has no engine, a disabled one
    // (CS 4.6), once at its start: never connected, every device DISABLED,
    // every count 0 (9.1: "Diagnostic tags are written with their current
    // values"; D-144). Returns true when it wrote anything. Not real-time.
    bool publish_idle(const CompiledChannel& channel) noexcept;

    // Writes `sys.drv.<channel>.state` at once if it changed; true if it
    // wrote.
    bool link(ChannelLink link) noexcept;

    // The monotonic time at which `publish` next writes the counters.
    [[nodiscard]] std::int64_t next_due_ns() const noexcept { return due_ns_; }

    // Writes the library refused.
    [[nodiscard]] std::uint64_t refused() const noexcept { return refused_; }

private:
    // Writes `bits` of the slot's type if it differs from the last value,
    // or with `all`; true if it wrote.
    bool put(std::size_t slot, std::uint64_t bits, bool all) noexcept;
    bool put_states(ChannelEngine& engine, const CompiledChannel& channel,
                    std::int64_t now_ns, bool all) noexcept;
    bool put_counters(ChannelEngine& engine, const CompiledChannel& channel,
                      std::int64_t now_ns, bool all) noexcept;
    [[nodiscard]] std::size_t device_slot(std::size_t device,
                                          DeviceSignal signal) const noexcept;

    scada_ctx* ctx_;
    SystemTagHandles handles_;
    std::int64_t due_ns_ = 0;
    std::uint64_t refused_ = 0;
};

}  // namespace scada::driver::detail

#endif  // SCADA_DRIVER_SRC_SYSTEM_TAGS_H
