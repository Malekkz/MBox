// The pure parts of the driver process (driver.h, D-142): the stall limit
// of a channel (2.6), the tag name of a point (CS 12.4) and a processed
// value as the tag library takes it. The threads, the signals and the
// watchdog run across the boundary in tests/integration.

#include "scada/driver/driver.h"
#include "../src/tag_writer.h"
#include "channel_builder.h"
#include "scada/common/fd.h"
#include "scada/common/log.h"
#include "scada/common/status.h"
#include "scada/driver/compiled.h"
#include "scada/driver/plan.h"
#include "scada/driver/sample_processing.h"
#include "scada/driver/tag_value.h"
#include "scada/driver/timing.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/types.h"
#include "sim_transport.h"
#include "test_protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal functions are declared here, not in <csignal>
#include <sys/types.h>
#include <unistd.h>

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

// True when `value` comes back as itself through the library's form.
bool round_trips(const TagValue& value) {
    const TagValue back = detail::from_scada_value(
        detail::to_scada_value(TagWrite{.value = value}), {});
    return back.type == value.type && back.i == value.i && back.u == value.u &&
           back.b == value.b &&
           std::bit_cast<std::uint64_t>(back.f) ==
               std::bit_cast<std::uint64_t>(value.f);
}

TEST(Driver, AStoredValueReadsBackAsTheTagValueItWasWrittenFrom) {
    using detail::from_scada_value;
    EXPECT_TRUE(round_trips(int64_value(-7)));
    EXPECT_TRUE(round_trips(uint64_value(9)));
    EXPECT_TRUE(round_trips(bool_value(true)));
    EXPECT_TRUE(round_trips(TagValue{.type = tag::TagType::FLOAT64, .f = 2.5}));
    const std::array<char, 3> text{'a', 'b', 'c'};
    scada_value stored{};
    stored.type = SCADA_TYPE_STRING;
    const TagValue string = from_scada_value(stored, text);
    EXPECT_EQ(string.type, tag::TagType::STRING);
    EXPECT_EQ(string.text, "abc");
}

TEST(Driver, ASeededStringKeepsItsTextAndAnotherTypeIsIgnored) {
    PointState state;
    init_point_state(state, tag::TagType::STRING);
    reset_point_state(state, tag::TagType::STRING, 0);
    seed_point_state(state,
                     TagValue{.type = tag::TagType::STRING, .text = "kept"});
    EXPECT_EQ(std::string_view(state.text.data(), state.text.size()), "kept");
    seed_point_state(state, int64_value(3));  // another type: ignored
    EXPECT_EQ(state.value.type, tag::TagType::STRING);
    EXPECT_EQ(state.quality, tag::Quality::BAD);  // the reset's quality stays
}

TEST(Driver, EachEndHasItsExitStatus) {
    EXPECT_EQ(exit_status(RunEnd::STOPPED), EXIT_OK);
    EXPECT_EQ(exit_status(RunEnd::FAILED), EXIT_ERROR);
    EXPECT_EQ(exit_status(RunEnd::STALLED), EXIT_STALL);
}

// PR 182 M2: one summary per kind and device fits a burst of every device.
TEST(Driver, AWorkersLogQueueIsSizedFromItsDevices) {
    EXPECT_EQ(worker_log_queue_size(0, 0), MIN_WORKER_LOG_QUEUE);
    EXPECT_EQ(worker_log_queue_size(0, 32), MIN_WORKER_LOG_QUEUE);
    EXPECT_EQ(worker_log_queue_size(0, 33), 66U);
    EXPECT_EQ(worker_log_queue_size(0, 200), 400U);
    EXPECT_EQ(worker_log_queue_size(0, LOG_MAX_QUEUE_CAPACITY / 2),
              LOG_MAX_QUEUE_CAPACITY);
    EXPECT_EQ(worker_log_queue_size(0, LOG_MAX_QUEUE_CAPACITY),
              LOG_MAX_QUEUE_CAPACITY);
    EXPECT_EQ(worker_log_queue_size(0, std::numeric_limits<std::size_t>::max()),
              LOG_MAX_QUEUE_CAPACITY);
    // A setting wins.
    EXPECT_EQ(worker_log_queue_size(1, 200), 1U);
}

TEST(Driver, OnlyTheStopSignalsAreBlocked) {
    // Review M8: SIGABRT and the fault signals keep their default.
    sigset_t before{};
    ASSERT_EQ(::pthread_sigmask(SIG_SETMASK, nullptr, &before), 0);
    ASSERT_TRUE(block_stop_signals().ok());
    sigset_t now{};
    ASSERT_EQ(::pthread_sigmask(SIG_SETMASK, nullptr, &now), 0);
    const auto all_blocked = [&](std::initializer_list<int> signals) {
        return std::all_of(signals.begin(), signals.end(), [&](int number) {
            return ::sigismember(&now, number) == 1;
        });
    };
    const auto unchanged = [&](std::initializer_list<int> signals) {
        return std::all_of(signals.begin(), signals.end(), [&](int number) {
            return ::sigismember(&now, number) ==
                   ::sigismember(&before, number);
        });
    };
    EXPECT_TRUE(all_blocked({SIGTERM, SIGINT, SIGHUP, SIGPIPE}));
    EXPECT_TRUE(unchanged({SIGABRT, SIGSEGV, SIGBUS, SIGFPE, SIGILL}));
    ASSERT_EQ(::pthread_sigmask(SIG_SETMASK, &before, nullptr), 0);
}

TEST(Driver, AWriteToAPipeNobodyReadsFailsAndTheProcessRunsOn) {
    // Delta 2: with SIGPIPE blocked, a write to a standard error whose
    // reader has gone gives EPIPE instead of ending the process.
    sigset_t before{};
    ASSERT_EQ(::pthread_sigmask(SIG_SETMASK, nullptr, &before), 0);
    ASSERT_TRUE(block_stop_signals().ok());
    std::array<int, 2> ends{-1, -1};
    ASSERT_EQ(::pipe2(ends.data(), O_CLOEXEC), 0);
    Fd reader(ends[0]);
    const Fd writer(ends[1]);
    reader.reset();  // the reader has gone
    const char byte = 'x';
    const ssize_t wrote = ::write(writer.get(), &byte, 1);
    const int error = errno;
    EXPECT_TRUE(wrote == -1 && error == EPIPE) << wrote << " " << error;
    // Take the pending SIGPIPE, so that restoring the mask delivers none.
    sigset_t pipe{};
    ASSERT_EQ(::sigemptyset(&pipe), 0);
    ASSERT_EQ(::sigaddset(&pipe, SIGPIPE), 0);
    const timespec now{};
    EXPECT_EQ(::sigtimedwait(&pipe, nullptr, &now), SIGPIPE);
    ASSERT_EQ(::pthread_sigmask(SIG_SETMASK, &before, nullptr), 0);
}

// A Driver that never reaches the tag library: no channel, and settings
// that fail before the attach.
RunEnd run_with(DriverSettings settings, Status& failure) {
    const CompiledConfig config;
    const TestProtocol protocol;
    SimClock clock;
    Driver driver(config, protocol, {}, clock, std::move(settings), nullptr);
    const RunEnd end = driver.run();
    failure = driver.failure();
    EXPECT_TRUE(driver.joined());
    return end;
}

DriverSettings settings(std::string process, std::int64_t period_ns) {
    DriverSettings out;
    out.process = std::move(process);
    out.heartbeat_period_ns = period_ns;
    return out;
}

TEST(Driver, SettingsOutOfRangeFailTheRunAtOnce) {
    Status failure;
    EXPECT_EQ(run_with(settings("drv-test", 0), failure), RunEnd::FAILED);
    EXPECT_EQ(failure.code(), StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(
        run_with(settings("drv-test", MAX_HEARTBEAT_PERIOD_NS + 1), failure),
        RunEnd::FAILED);
    EXPECT_EQ(failure.code(), StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(run_with(settings("", DEFAULT_HEARTBEAT_PERIOD_NS), failure),
              RunEnd::FAILED);
    EXPECT_EQ(failure.code(), StatusCode::INVALID_ARGUMENT);
    DriverSettings big = settings("drv-test", DEFAULT_HEARTBEAT_PERIOD_NS);
    big.worker_log_queue = LOG_MAX_QUEUE_CAPACITY + 1;
    EXPECT_EQ(run_with(std::move(big), failure), RunEnd::FAILED);
    EXPECT_EQ(failure.code(), StatusCode::INVALID_ARGUMENT);
}

TEST(Driver, ASecondRunIsRefused) {
    const CompiledConfig config;
    const TestProtocol protocol;
    SimClock clock;
    Driver driver(config, protocol, {}, clock, settings("", 1), nullptr);
    EXPECT_EQ(driver.run(), RunEnd::FAILED);
    EXPECT_EQ(driver.run(), RunEnd::FAILED);
    EXPECT_EQ(driver.failure().code(), StatusCode::FAILED_PRECONDITION);
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

}  // namespace
}  // namespace scada::driver::test
