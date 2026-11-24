#ifndef SCADA_DRIVER_COMPILED_H
#define SCADA_DRIVER_COMPILED_H

// The compiled configuration of a driver (driver interface contract 3.1):
// every channel, device, point and poll group of one configuration version,
// built on the main thread by `configure` and replaced whole at a reload
// (3.2). The run-time objects (connections, device states, timers) are not
// here; they refer to the compiled form of the current configuration.
//
// This slice holds what scheduling needs (sections 6.1 to 6.6); the keys of
// failure handling, writes and diagnostics join it with their slices.

#include "scada/driver/plan.h"
#include "scada/driver/point_rules.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace scada::driver {

// A protocol's compiled form of a channel, device or point (3.1). The
// framework holds it in a std::unique_ptr inside the CompiledConfig; a
// protocol derives its own type and reads it back with `protocol_data`.
class ProtocolData {
public:
    ProtocolData() = default;
    virtual ~ProtocolData() = default;
    ProtocolData(const ProtocolData&) = delete;
    ProtocolData& operator=(const ProtocolData&) = delete;
    ProtocolData(ProtocolData&&) = delete;
    ProtocolData& operator=(ProtocolData&&) = delete;
};

// The protocol's own compiled form, as the type the protocol stored. The
// caller must name the type its `configure` created; no other type is ever
// stored, so the downcast is exact.
template <class T>
[[nodiscard]] const T& protocol_data(const ProtocolData& data) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): only T is ever stored (above)
    return static_cast<const T&>(data);
}

// A poll group of a channel (CS 4.6): a period and a priority, 1 highest.
struct CompiledPollGroup {
    std::string name;
    std::int64_t period_ns = 0;
    std::uint8_t priority = 1;
};

// Point access (CS 4.7): a `wo` point is never read.
enum class Access : std::uint8_t { RO = 1, RW, WO };

struct CompiledPoint {
    std::string name;
    Access access = Access::RO;
    // Index into the channel's poll groups; none for a point that is never
    // polled (`access: wo`, a report-only point).
    std::optional<std::uint32_t> poll_group;
    PointRules rules;
    std::unique_ptr<ProtocolData> protocol;
};

// Pacing of a channel or a device (6.4); 0 means none.
struct Pacing {
    std::int64_t min_request_interval_ns = 0;
    std::int64_t min_write_interval_ns = 0;
};

struct CompiledDevice {
    std::string name;
    bool enabled = true;
    // The physical unit: devices of one channel with the same key are one
    // device on the wire (3.4: they share a unit id) and share its pacing
    // and exclusivity (ruling R3). The compile step sets it to the device's
    // default unit id; a device whose points name other unit ids too (X-4)
    // is keyed, and paced, by its default one. OWN_UNIT, the default, is a
    // unit of the device alone.
    static constexpr std::uint32_t OWN_UNIT =
        std::numeric_limits<std::uint32_t>::max();
    std::uint32_t unit = OWN_UNIT;
    Pacing pacing;
    // `keepalive` (6.5); 0: off.
    std::int64_t keepalive_ns = 0;
    std::vector<CompiledPoint> points;
    ReadPlan plan;
    std::unique_ptr<ProtocolData> protocol;
};

struct CompiledChannel {
    std::string name;
    bool enabled = true;
    Pacing pacing;
    std::vector<CompiledPollGroup> poll_groups;
    std::vector<CompiledDevice> devices;
    std::unique_ptr<ProtocolData> protocol;
};

// Every compiled channel of one configuration version. Move-only: the
// protocol parts are owned.
struct CompiledConfig {
    std::vector<CompiledChannel> channels;
};

}  // namespace scada::driver

#endif  // SCADA_DRIVER_COMPILED_H
