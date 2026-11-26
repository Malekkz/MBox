// The pure parts of the driver process (driver.h, D-142): the stall limit
// of a channel (2.6), the tag name of a point (CS 12.4) and a processed
// value as the tag library takes it. The threads, the signals and the
// watchdog run across the boundary in tests/integration.

#include "scada/driver/driver.h"
#include "../src/tag_writer.h"
#include "channel_builder.h"
#include "scada/driver/compiled.h"
#include "scada/driver/plan.h"
#include "scada/driver/sample_processing.h"
#include "scada/driver/tag_value.h"
#include "scada/driver/timing.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/types.h"
#include "test_protocol.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace scada::driver::test {
namespace {

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers): the test data

CompiledChannel two_devices(std::int64_t timeout_ns,
                            std::int64_t write_timeout_ns) {
    CompiledChannel channel =
        make_channel({{.name = "a", .unit_id = 1, .points = adjacent(0, 1)},
                      {.name = "b", .unit_id = 2, .points = adjacent(0, 1)}},
                     ReadRules{.max_units = MAX_UNITS});
    channel.connection.connect_timeout_ns = 100 * MILLISECOND;
    channel.devices[0].failure.timeout_ns = 10 * MILLISECOND;
    channel.devices[0].failure.write_timeout_ns = 10 * MILLISECOND;
    channel.devices[1].failure.timeout_ns = timeout_ns;
    channel.devices[1].failure.write_timeout_ns = write_timeout_ns;
    return channel;
}

TEST(Driver, TheStallLimitIsThreeTimesTheLongestWait) {
    const TestProtocol protocol;
    // A stream: the request deadline is the time-out plus the margin.
    EXPECT_EQ(stall_limit_ns(two_devices(2 * SECOND, SECOND), protocol),
              3 * (2 * SECOND + MIN_MARGIN_NS));
    EXPECT_EQ(stall_limit_ns(two_devices(SECOND, 4 * SECOND), protocol),
              3 * (4 * SECOND + MIN_MARGIN_NS));
    // `connect_timeout` when it is longer.
    CompiledChannel connecting = two_devices(SECOND, SECOND);
    connecting.connection.connect_timeout_ns = 5 * SECOND;
    EXPECT_EQ(stall_limit_ns(connecting, protocol), 15 * SECOND);
    // At least 1 s.
    EXPECT_EQ(stall_limit_ns(two_devices(10 * MILLISECOND, 10 * MILLISECOND),
                             protocol),
              MIN_STALL_LIMIT_NS);
}

TEST(Driver, TheStallLimitCountsTheLineTimeOfTheLongestFrames) {
    // A serial line: both frames of the device's largest size on the wire
    // (7.1), then the margin.
    const TestProtocol protocol;
    CompiledChannel serial = two_devices(SECOND, SECOND);
    serial.transport = TransportKind::SERIAL;
    serial.line = LineTiming{.bit_rate = 1'200, .bits_per_char = 11};
    const std::size_t frame =
        protocol.device_caps(serial.devices[1]).max_frame_bytes;
    const std::int64_t wait = (2 * frame_time_ns(serial.line, frame)) + SECOND +
                              margin_ns(serial.line);
    EXPECT_EQ(stall_limit_ns(serial, protocol), 3 * wait);
}

TEST(Driver, ADisabledDeviceAndSaturationLeaveTheStallLimitSane) {
    const TestProtocol protocol;
    CompiledChannel channel = two_devices(30 * SECOND, 30 * SECOND);
    channel.devices[1].enabled = false;  // only `a`, 10 ms: the least
    EXPECT_EQ(stall_limit_ns(channel, protocol), MIN_STALL_LIMIT_NS);
    channel.devices[1].enabled = true;
    channel.devices[1].failure.timeout_ns =
        std::numeric_limits<std::int64_t>::max();
    EXPECT_EQ(stall_limit_ns(channel, protocol),
              std::numeric_limits<std::int64_t>::max());
}

TEST(Driver, APointsTagIsItsDeviceAndItsName) {
    CompiledDevice device;
    device.name = "ps1.fm1";
    CompiledPoint point;
    point.name = "flow";
    EXPECT_EQ(point_tag_name(device, point), "ps1.fm1.flow");
}

TEST(Driver, AProcessedValueIsWrittenWithItsBitsQualityAndTime) {
    using detail::to_scada_value;
    const TagWrite real{.value = float64_value(-2.5),
                        .quality = tag::Quality::UNCERTAIN,
                        .flags = SCADA_FLAG_STALE,
                        .source_time_ns = 1'234};
    const scada_value written = to_scada_value(real);
    EXPECT_EQ(written.type, SCADA_TYPE_FLOAT64);
    EXPECT_EQ(written.value, std::bit_cast<std::uint64_t>(-2.5));
    EXPECT_EQ(written.quality, SCADA_QUALITY_UNCERTAIN);
    EXPECT_EQ(written.quality_flags, SCADA_FLAG_STALE);
    EXPECT_EQ(written.source_time_ns, 1'234);

    EXPECT_EQ(to_scada_value(TagWrite{.value = int64_value(-7)}).value,
              std::bit_cast<std::uint64_t>(std::int64_t{-7}));
    EXPECT_EQ(to_scada_value(TagWrite{.value = uint64_value(9)}).value, 9U);
    EXPECT_EQ(to_scada_value(TagWrite{.value = bool_value(true)}).value, 1U);
    EXPECT_EQ(to_scada_value(TagWrite{.value = bool_value(false)}).value, 0U);
    EXPECT_EQ(to_scada_value(TagWrite{}).quality, SCADA_QUALITY_BAD);
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

}  // namespace
}  // namespace scada::driver::test
