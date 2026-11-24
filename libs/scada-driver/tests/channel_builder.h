#ifndef SCADA_DRIVER_TESTS_CHANNEL_BUILDER_H
#define SCADA_DRIVER_TESTS_CHANNEL_BUILDER_H

// Compiled channels for the tests of the test protocol, built in code (the
// compile step from a configuration tree arrives with slice 8).

#include "scada/common/status.h"
#include "scada/driver/compiled.h"
#include "scada/driver/plan.h"
#include "test_protocol.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace scada::driver::test {

inline constexpr std::int64_t MILLISECOND = 1'000'000;
inline constexpr std::int64_t SECOND = 1'000 * MILLISECOND;

struct PointSpec {
    std::uint32_t first = 0;
    std::uint8_t count = 1;
    std::optional<std::uint32_t> group = 0;
    std::uint8_t space = 0;
    // NOLINTBEGIN(readability-redundant-member-init): designated initializers may omit them (-Wmissing-field-initializers)
    std::vector<std::uint32_t> companions{};
    // NOLINTEND(readability-redundant-member-init)
};

struct DeviceSpec {
    // NOLINTBEGIN(readability-redundant-member-init): designated initializers may omit them (-Wmissing-field-initializers)
    std::string name{};
    std::uint8_t unit_id = 1;
    std::vector<PointSpec> points{};
    Pacing pacing{};
    // NOLINTEND(readability-redundant-member-init)
    std::int64_t keepalive_ns = 0;
    bool enabled = true;
};

// The CS default poll groups: fast 200 ms, normal 1 s, slow 10 s.
inline constexpr std::int64_t FAST_PERIOD = 200 * MILLISECOND;
inline constexpr std::int64_t SLOW_PERIOD = 10 * SECOND;
inline constexpr std::uint8_t FAST_PRIORITY = 1;
inline constexpr std::uint8_t NORMAL_PRIORITY = 2;
inline constexpr std::uint8_t SLOW_PRIORITY = 3;

inline std::vector<CompiledPollGroup> default_groups() {
    return {
        {.name = "fast", .period_ns = FAST_PERIOD, .priority = FAST_PRIORITY},
        {.name = "normal", .period_ns = SECOND, .priority = NORMAL_PRIORITY},
        {.name = "slow", .period_ns = SLOW_PERIOD, .priority = SLOW_PRIORITY}};
}

inline CompiledDevice make_device(const DeviceSpec& spec) {
    CompiledDevice device;
    device.name = spec.name;
    device.enabled = spec.enabled;
    device.unit = spec.unit_id;
    device.pacing = spec.pacing;
    device.keepalive_ns = spec.keepalive_ns;
    auto binding = std::make_unique<TestDevice>();
    binding->unit_id = spec.unit_id;
    device.protocol = std::move(binding);
    for (std::size_t index = 0; index < spec.points.size(); ++index) {
        const PointSpec& point_spec = spec.points[index];
        CompiledPoint point;
        point.name = spec.name + ".p" + std::to_string(index);
        point.poll_group = point_spec.group;
        point.rules.companions = point_spec.companions;
        auto point_binding = std::make_unique<TestPoint>();
        point_binding->space = point_spec.space;
        point_binding->first = point_spec.first;
        point_binding->count = point_spec.count;
        point.protocol = std::move(point_binding);
        device.points.push_back(std::move(point));
    }
    return device;
}

// A channel of `devices`, each planned by the test protocol within `rules`.
inline CompiledChannel make_channel(
    const std::vector<DeviceSpec>& devices, const ReadRules& rules,
    Pacing pacing = {},
    std::vector<CompiledPollGroup> groups = default_groups()) {
    CompiledChannel channel;
    channel.name = "line1";
    channel.pacing = pacing;
    channel.poll_groups = std::move(groups);
    const TestProtocol protocol;
    for (const DeviceSpec& spec : devices) {
        CompiledDevice device = make_device(spec);
        device.plan.by_group.resize(channel.poll_groups.size());
        const Status status = protocol.plan(device, rules, device.plan);
        EXPECT_TRUE(status.ok()) << spec.name << ": " << status.message();
        channel.devices.push_back(std::move(device));
    }
    return channel;
}

// `count` points of one unit each from `first`, in `group`.
inline std::vector<PointSpec> adjacent(std::uint32_t first, std::uint32_t count,
                                       std::uint32_t group = 0) {
    std::vector<PointSpec> points;
    for (std::uint32_t index = 0; index < count; ++index) {
        points.push_back(PointSpec{.first = first + index, .group = group});
    }
    return points;
}

}  // namespace scada::driver::test

#endif  // SCADA_DRIVER_TESTS_CHANNEL_BUILDER_H
