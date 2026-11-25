// The channel engine over the loopback (driver interface contract 7.1 to
// 7.5; conformance tests 15, 16, 17, the dead-device part of 12, and the
// gateway and busy parts of 19). The bus answers after a fixed latency on
// an injected clock; faults are set per unit.

#include "scada/driver/channel_engine.h"
#include "channel_builder.h"
#include "loopback.h"
#include "scada/driver/compiled.h"
#include "scada/driver/failure.h"
#include "scada/driver/plan.h"
#include "scada/driver/point_rules.h"
#include "scada/driver/timing.h"
#include "scada/tag/types.h"
#include "sim_transport.h"
#include "test_protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace scada::driver::test {
namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): the addresses, counts and times are the test data, each worked out by hand beside the rule it checks

constexpr std::int64_t LATENCY = MILLISECOND;
constexpr std::uint64_t LARGE = 64;
constexpr std::uint32_t FAST = 0;
constexpr std::uint32_t NORMAL = 1;
constexpr std::uint32_t SLOW = 2;
using Fault = SimBus::Fault;
using Flag = tag::QualityFlag;

SimBus bus_of_units(std::uint8_t units) {
    SimBus bus;
    for (std::uint8_t unit = 1; unit <= units; ++unit) {
        bus.map(unit, 0, 0, 199);
    }
    return bus;
}

FailureRules quick_failure(std::uint8_t retries, std::uint8_t demote_after) {
    return FailureRules{.timeout_ns = 100 * MILLISECOND,
                        .write_timeout_ns = 100 * MILLISECOND,
                        .retries = retries,
                        .demote_after = demote_after,
                        .backoff_ns = SECOND,
                        .backoff_max_ns = 4 * SECOND};
}

// A channel of `units` devices of one point each (unit ids 1 to `units`,
// register 0), all with `rules`, on `transport`.
CompiledChannel channel_of(std::uint8_t units, const FailureRules& rules,
                           TransportKind transport,
                           std::uint32_t group = FAST) {
    std::vector<DeviceSpec> specs;
    for (std::uint8_t unit = 1; unit <= units; ++unit) {
        specs.push_back({.name = "d" + std::to_string(unit),
                         .unit_id = unit,
                         .points = adjacent(0, 1, group)});
    }
    CompiledChannel channel =
        make_channel(specs, ReadRules{.max_units = LARGE});
    channel.transport = transport;
    channel.connection.reconnect_min_ns = SECOND;
    channel.connection.reconnect_max_ns = 8 * SECOND;
    for (CompiledDevice& device : channel.devices) {
        device.failure = rules;
    }
    return channel;
}

// The requests unit `unit` received.
std::size_t requests_of(const SimBus& bus, std::uint8_t unit) {
    return static_cast<std::size_t>(std::count_if(
        bus.log().begin(), bus.log().end(),
        [&](const LoggedRequest& request) { return request.unit_id == unit; }));
}

std::vector<std::int64_t> times_of(const SimBus& bus, std::uint8_t unit) {
    std::vector<std::int64_t> out;
    for (const LoggedRequest& request : bus.log()) {
        if (request.unit_id == unit) {
            out.push_back(request.time_ns);
        }
    }
    return out;
}

// The longest time between two requests of `times` from `after_ns` on.
std::int64_t longest_gap_after(const std::vector<std::int64_t>& times,
                               std::int64_t after_ns) {
    std::int64_t worst = 0;
    for (std::size_t index = 1; index < times.size(); ++index) {
        if (times[index - 1] > after_ns) {
            worst = std::max(worst, times[index] - times[index - 1]);
        }
    }
    return worst;
}

// Everything one test drives: its parts are what the tests look at.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes): a test rig whose parts are its interface
struct Rig {
    explicit Rig(CompiledChannel compiled, std::uint8_t units)
        : channel(std::move(compiled)),
          bus(bus_of_units(units)),
          transport(bus, clock, LATENCY),
          engine(channel, protocol, transport, clock, tags) {}

    void run_until(std::int64_t end_ns) {
        (void)run_engine(engine, clock, end_ns);
    }
    [[nodiscard]] DeviceStatus state(std::uint32_t device) const {
        return engine.failures().state(device);
    }

    CompiledChannel channel;
    SimBus bus;
    SimClock clock;
    SimTransport transport;
    RecordingTags tags;
    TestProtocol protocol;
    ChannelEngine engine;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

// The device is CONNECTING and its point BAD with COMM_FAIL.
bool connecting_with_comm_fail(const Rig& rig, std::uint32_t device) {
    return rig.state(device) == DeviceStatus::CONNECTING &&
           rig.tags.point(device, 0).quality == tag::Quality::BAD &&
           rig.tags.has_flag(device, 0, Flag::COMM_FAIL);
}

// --- Conformance test 15: demotion, probes, recovery ----------------------

TEST(ChannelEngine, ADeadDeviceIsDemotedAfterItsRetriesAndProbedThenRecovers) {
    // Serial, retries 2, demote_after 3: three requests of three attempts
    // each, then DEMOTED with its point BAD and COMM_FAIL. Probes follow at
    // 1 s, 2 s, 4 s (backoff doubling to 4 s); the unit comes back, a probe
    // answers, and the device is polled again and RUNNING.
    Rig rig(channel_of(1, quick_failure(2, 3), TransportKind::SERIAL), 1);
    rig.bus.fault(1, Fault::SILENT);
    ASSERT_TRUE(rig.engine.status().ok());
    rig.run_until(2 * SECOND);
    EXPECT_EQ(rig.state(0), DeviceStatus::DEMOTED);
    EXPECT_EQ(rig.engine.counts().retries, 6U);
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::BAD);
    EXPECT_TRUE(rig.tags.has_flag(0, 0, Flag::COMM_FAIL));
    const std::size_t before_probes = requests_of(rig.bus, 1);
    EXPECT_EQ(before_probes, 9U);
    const std::int64_t demoted_at =
        times_of(rig.bus, 1).back() + 100 * MILLISECOND;
    // Probes only, each a back-off after the last failed one: 1, 2, then 4
    // s (the maximum), each failing after its 100 ms time-out.
    rig.run_until(demoted_at + 7 * SECOND + 400 * MILLISECOND);
    const std::vector<std::int64_t> sent = times_of(rig.bus, 1);
    ASSERT_EQ(sent.size(), before_probes + 3);
    EXPECT_GE(sent[9] - demoted_at, SECOND);
    EXPECT_GE(sent[10] - sent[9], 2 * SECOND + 100 * MILLISECOND);
    EXPECT_GE(sent[11] - sent[10], 4 * SECOND + 100 * MILLISECOND);
    rig.bus.fault(1, Fault::NONE);
    rig.bus.set(1, 0, 0, 42);
    rig.run_until(sent[11] + 5 * SECOND);
    EXPECT_EQ(rig.state(0), DeviceStatus::RUNNING);
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
    EXPECT_EQ(rig.tags.point(0, 0).value, 42);
    EXPECT_FALSE(rig.tags.has_flag(0, 0, Flag::COMM_FAIL));
}

TEST(ChannelEngine, TimeOutsWithinStartupGraceDoNotCount) {
    FailureRules rules = quick_failure(0, 1);
    rules.startup_grace_ns = SECOND;
    Rig rig(channel_of(1, rules, TransportKind::SERIAL), 1);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(900 * MILLISECOND);
    EXPECT_EQ(rig.state(0), DeviceStatus::INITIALISING);
    EXPECT_GT(requests_of(rig.bus, 1), 3U);
    rig.run_until(2 * SECOND);
    EXPECT_EQ(rig.state(0), DeviceStatus::DEMOTED);
}

TEST(ChannelEngine, DevicesSharingAUnitIdShareTheirCount) {
    // Two slices of one dead unit, demote_after 2: two failed requests in
    // all, one of each slice, demote both.
    CompiledChannel two =
        make_channel({{.name = "a", .unit_id = 1, .points = adjacent(0, 1)},
                      {.name = "b", .unit_id = 1, .points = adjacent(10, 1)}},
                     ReadRules{.max_units = LARGE});
    two.transport = TransportKind::SERIAL;
    for (CompiledDevice& device : two.devices) {
        device.failure = quick_failure(0, 2);
    }
    Rig rig(std::move(two), 1);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(300 * MILLISECOND);
    EXPECT_EQ(rig.state(0), DeviceStatus::DEMOTED);
    EXPECT_EQ(rig.state(1), DeviceStatus::DEMOTED);
    EXPECT_EQ(requests_of(rig.bus, 1), 2U);
}

// --- Conformance test 16: a lost connection; reconnect back-off -----------

TEST(ChannelEngine, ALostConnectionFailsEveryDeviceAtOnce) {
    Rig rig(channel_of(2, quick_failure(2, 3), TransportKind::TCP), 2);
    rig.run_until(300 * MILLISECOND);
    ASSERT_EQ(rig.state(0), DeviceStatus::RUNNING);
    ASSERT_EQ(rig.tags.point(1, 0).quality, tag::Quality::GOOD);
    const std::uint64_t requests = rig.engine.counts().requests;
    rig.transport.close_by_peer();
    rig.run_until(400 * MILLISECOND);
    EXPECT_TRUE(connecting_with_comm_fail(rig, 0));
    EXPECT_TRUE(connecting_with_comm_fail(rig, 1));
    // No retries spent, and no request sent: the engine asks the transport
    // before a write and at each boundary (7.3 "Idle close").
    EXPECT_EQ(rig.engine.counts().requests, requests);
    EXPECT_EQ(rig.engine.counts().retries, 0U);
}

TEST(ChannelEngine, ARefusedConnectionIsConnectingWithCommFail) {
    // Three refused connects: CONNECTING with COMM_FAIL, retried after 1,
    // 2 and 4 s (doubling from `min`), then connected.
    Rig rig(channel_of(1, quick_failure(2, 3), TransportKind::TCP), 1);
    rig.transport.refuse_connects(3, 10 * MILLISECOND);
    rig.run_until(500 * MILLISECOND);
    EXPECT_EQ(rig.state(0), DeviceStatus::CONNECTING);
    EXPECT_TRUE(rig.tags.has_flag(0, 0, Flag::COMM_FAIL));
    rig.run_until(8 * SECOND);
    EXPECT_EQ(
        rig.transport.connect_times(),
        (std::vector<std::int64_t>{0, 1010 * MILLISECOND, 3020 * MILLISECOND,
                                   7030 * MILLISECOND}));
    EXPECT_EQ(rig.state(0), DeviceStatus::RUNNING);
}

TEST(ChannelEngine, TheBackOffResetsAfterTheFirstValidAnswerNotAfterConnect) {
    // A peer that accepts and closes at once, twice: the reconnects wait 1 s
    // then 2 s, since no answer came. After an answer, the next loss
    // reconnects after `min` (1 s) again.
    Rig rig(channel_of(1, quick_failure(2, 3), TransportKind::TCP), 1);
    rig.transport.close_on_connect(2);
    rig.run_until(4 * SECOND);
    EXPECT_EQ(rig.transport.connect_times(),
              (std::vector<std::int64_t>{0, SECOND, 3 * SECOND}));
    ASSERT_EQ(rig.state(0), DeviceStatus::RUNNING);  // answered: reset
    rig.transport.close_by_peer();
    const std::int64_t closed_at = rig.clock.monotonic_ns();
    rig.run_until(closed_at + 2 * SECOND);
    ASSERT_EQ(rig.transport.connect_times().size(), 4U);
    // Lost at the next poll after the close, then 1 s.
    EXPECT_LE(rig.transport.connect_times()[3] - closed_at,
              SECOND + FAST_PERIOD);
}

// --- 7.3: closes the driver makes itself ----------------------------------

TEST(ChannelEngine, ABrokenFrameOnTcpClosesAndAQuickReconnectKeepsTheTags) {
    // Unit 2 answers garbage once: an abortive close, an immediate
    // reconnect, and unit 1's tags and state are kept (R-17).
    Rig rig(channel_of(2, quick_failure(2, 3), TransportKind::TCP), 2);
    rig.run_until(300 * MILLISECOND);
    ASSERT_EQ(rig.state(0), DeviceStatus::RUNNING);
    const int writes = rig.tags.point(0, 0).writes;
    rig.bus.fault(2, Fault::GARBAGE);
    rig.run_until(450 * MILLISECOND);
    rig.bus.fault(2, Fault::NONE);
    rig.run_until(700 * MILLISECOND);
    EXPECT_GE(rig.transport.abortive_closes(), 1);
    EXPECT_EQ(rig.engine.counts().resumed,
              static_cast<std::uint64_t>(rig.transport.abortive_closes()));
    EXPECT_EQ(rig.engine.counts().losses, 0U);
    EXPECT_EQ(rig.state(0), DeviceStatus::RUNNING);
    EXPECT_FALSE(rig.tags.has_flag(0, 0, Flag::COMM_FAIL));
    EXPECT_GT(rig.tags.point(0, 0).writes, writes);
}

TEST(ChannelEngine, ARunOfTimeOutsOverEveryUnitClosesTheConnection) {
    // TCP, every unit silent (a dead gateway), lost_after_timeouts 3,
    // demote_after 3: the third time-out closes the connection and nobody
    // is demoted (D-139).
    Rig rig(channel_of(2, quick_failure(0, 3), TransportKind::TCP), 2);
    rig.bus.fault(1, Fault::SILENT);
    rig.bus.fault(2, Fault::SILENT);
    rig.run_until(250 * MILLISECOND);
    EXPECT_GE(rig.engine.counts().own_closes, 1U);
    EXPECT_NE(rig.state(0), DeviceStatus::DEMOTED);
    EXPECT_NE(rig.state(1), DeviceStatus::DEMOTED);
}

TEST(ChannelEngine, ALostSideEffectReadOnTcpClosesAndIsNotReadAgain) {
    // Conformance test 14's last clause: a lost answer to a side-effect read
    // is not followed by a second read; on tcp the connection closes.
    ReadRules rules{.max_units = LARGE};
    rules.ranges = {ReadRange{.kind = ReadRange::Kind::SIDE_EFFECT,
                              .unit_id = 1,
                              .first = 0,
                              .last = 0}};
    CompiledChannel side = make_channel(
        {{.name = "d1", .unit_id = 1, .points = adjacent(0, 1, SLOW)}}, rules);
    side.devices[0].failure = quick_failure(2, 3);
    Rig rig(std::move(side), 1);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(500 * MILLISECOND);
    EXPECT_EQ(requests_of(rig.bus, 1), 1U);
    EXPECT_EQ(rig.engine.counts().retries, 0U);
    EXPECT_EQ(rig.transport.abortive_closes(), 1);
}

TEST(ChannelEngine, ALostSideEffectReadOnASerialLineIsNotReadAgain) {
    // The same on a serial line, where nothing closes: one read per poll,
    // no retry (6.6), however many `retries` the device has.
    ReadRules rules{.max_units = LARGE};
    rules.ranges = {ReadRange{.kind = ReadRange::Kind::SIDE_EFFECT,
                              .unit_id = 1,
                              .first = 0,
                              .last = 0}};
    CompiledChannel side = make_channel(
        {{.name = "d1", .unit_id = 1, .points = adjacent(0, 1)}}, rules);
    side.transport = TransportKind::SERIAL;
    side.devices[0].failure = quick_failure(2, 10);
    Rig rig(std::move(side), 1);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(FAST_PERIOD * 3 - MILLISECOND);  // polls at 0, 200, 400
    EXPECT_EQ(requests_of(rig.bus, 1), 3U);
    EXPECT_EQ(rig.engine.counts().retries, 0U);
}

TEST(ChannelEngine, AReadThatTimesOutIsRetriedWithinItsTurn) {
    // retries 2: a silent unit gets three attempts for one request, each
    // after the last one's 100 ms time-out and the 20 ms quiet time, and
    // the request counts once.
    Rig rig(channel_of(1, quick_failure(2, 10), TransportKind::SERIAL), 1);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(345 * MILLISECOND);
    EXPECT_EQ(
        times_of(rig.bus, 1),
        (std::vector<std::int64_t>{0, 120 * MILLISECOND, 240 * MILLISECOND}));
    EXPECT_EQ(rig.engine.counts().retries, 2U);
    EXPECT_EQ(rig.engine.failures().failures(0), 1U);
}

// --- 7.1, 7.2: retries and the serial quiet time --------------------------

TEST(ChannelEngine, ATimeOutOnASerialLineKeepsTheQuietTime) {
    // 19,200 bit/s 8E1: after unit 1 times out, no request to any unit for
    // the quiet time (20 ms here, the floor), measured from the deadline.
    CompiledChannel channel =
        channel_of(2, quick_failure(0, 5), TransportKind::SERIAL);
    channel.line = LineTiming{.bit_rate = 19200,
                              .bits_per_char = bits_per_char(8, true, 1)};
    Rig rig(std::move(channel), 2);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(150 * MILLISECOND);
    const std::vector<std::int64_t> first = times_of(rig.bus, 1);
    const std::vector<std::int64_t> second = times_of(rig.bus, 2);
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());
    const std::int64_t deadline =
        first.front() + first_byte_deadline_ns(rig.channel.line, 0,
                                               REQUEST_BYTES,
                                               100 * MILLISECOND);
    EXPECT_GE(second.front(), deadline + margin_ns(rig.channel.line));
}

// --- Conformance test 12, the dead-device part: the probe budget ----------

TEST(ChannelEngine, DeadDevicesDoNotStarveALiveOne) {
    // Unit 1 live in the fast group (200 ms); units 2 to 6 dead, demoted
    // after one time-out each. From then on unit 1 keeps its period: a
    // probe (at most 100 ms here) goes only in its idle time.
    Rig rig(channel_of(6, quick_failure(0, 1), TransportKind::SERIAL), 6);
    for (std::uint8_t unit = 2; unit <= 6; ++unit) {
        rig.bus.fault(unit, Fault::SILENT);
    }
    rig.run_until(30 * SECOND);
    for (std::uint32_t device = 1; device < 6; ++device) {
        EXPECT_EQ(rig.state(device), DeviceStatus::DEMOTED);
    }
    const std::int64_t worst =
        longest_gap_after(times_of(rig.bus, 1), 2 * SECOND);
    // The period, plus at most one probe that was in flight when it fell
    // due, plus the quiet time after that probe's time-out.
    EXPECT_LE(worst, FAST_PERIOD + 100 * MILLISECOND + 3 * MILLISECOND +
                         MIN_MARGIN_NS);
    EXPECT_GT(requests_of(rig.bus, 2), 1U);  // probed
}

// Devices `first` to `end` (exclusive) are all DEMOTED.
bool all_demoted(const Rig& rig, std::uint32_t first, std::uint32_t end) {
    for (std::uint32_t device = first; device < end; ++device) {
        if (rig.state(device) != DeviceStatus::DEMOTED) {
            return false;
        }
    }
    return true;
}

// The requests the bus received that start at register `first`.
std::size_t reads_of(const SimBus& bus, std::uint32_t first) {
    return static_cast<std::size_t>(std::count_if(
        bus.log().begin(), bus.log().end(),
        [&](const LoggedRequest& request) { return request.first == first; }));
}

// The point's quality comes from each of its companions (CS 4.7): it is
// read from them, so a failed companion fails it (D-131).
void quality_from_companions(CompiledChannel& channel, std::uint32_t device,
                             std::uint32_t point) {
    PointRules& rules = channel.devices[device].points[point].rules;
    for (std::size_t slot = 0; slot < rules.companions.size(); ++slot) {
        rules.quality_rules.push_back(
            QualityRule{.companion = static_cast<std::uint8_t>(slot),
                        .entries = {},
                        .otherwise = {.quality = tag::Quality::GOOD}});
    }
}

// --- Conformance test 19, the gateway and busy parts ----------------------

TEST(ChannelEngine, AGatewayThatCannotReachAUnitDemotesThatUnitOnly) {
    Rig rig(channel_of(2, quick_failure(0, 2), TransportKind::TCP), 2);
    rig.bus.fault(1, Fault::UNREACHABLE);
    rig.run_until(SECOND);
    EXPECT_EQ(rig.state(0), DeviceStatus::DEMOTED);
    EXPECT_EQ(rig.state(1), DeviceStatus::RUNNING);
    EXPECT_EQ(rig.engine.counts().own_closes, 0U);
    EXPECT_EQ(rig.engine.counts().losses, 0U);
}

TEST(ChannelEngine, BusyAnswersNeverDemoteAndAreNotRetried) {
    // Conformance test 19, DI 7.4 and the lead's ruling on D-12: busy
    // neither fails nor answers, whatever `busy_retries` says; one request
    // per poll, no retry, no demotion, the tag untouched.
    FailureRules rules = quick_failure(2, 1);
    rules.busy_retries = 3;
    Rig rig(channel_of(1, rules, TransportKind::TCP), 1);
    rig.run_until(FAST_PERIOD - MILLISECOND);
    ASSERT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
    rig.bus.fault(1, Fault::BUSY);
    rig.run_until(20 * FAST_PERIOD - MILLISECOND);
    EXPECT_EQ(requests_of(rig.bus, 1), 20U);
    EXPECT_EQ(rig.engine.counts().retries, 0U);
    EXPECT_EQ(rig.state(0), DeviceStatus::RUNNING);
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
    EXPECT_FALSE(rig.tags.has_flag(0, 0, Flag::COMM_FAIL));
}

// --- Conformance test 17: STALE -------------------------------------------

TEST(ChannelEngine, StaleAfterMaxAgeClearedByTheNextGoodRead) {
    // A slow group (10 s) and `max_age` 2 s: read at 0, STALE at 2 s while
    // the worker is idle (it wakes for it), cleared by the read at 10 s.
    CompiledChannel channel =
        channel_of(1, quick_failure(2, 3), TransportKind::TCP, SLOW);
    channel.devices[0].points[0].rules.max_age_ns = 2 * SECOND;
    Rig rig(std::move(channel), 1);
    rig.run_until(SECOND);
    EXPECT_FALSE(rig.tags.has_flag(0, 0, Flag::STALE));
    rig.run_until(2 * SECOND + 2 * MILLISECOND);
    EXPECT_TRUE(rig.tags.has_flag(0, 0, Flag::STALE));
    rig.run_until(10 * SECOND + 10 * MILLISECOND);
    EXPECT_FALSE(rig.tags.has_flag(0, 0, Flag::STALE));
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
}

// --- PR 137 review: P1 a lost connection is never skipped -----------------

// True when every point of the first `devices` devices is BAD with
// COMM_FAIL.
bool all_comm_failed(const Rig& rig, std::uint32_t devices) {
    for (std::uint32_t device = 0; device < devices; ++device) {
        if (rig.tags.point(device, 0).quality != tag::Quality::BAD ||
            !rig.tags.has_flag(device, 0, Flag::COMM_FAIL)) {
            return false;
        }
    }
    return true;
}

TEST(ChannelEngine, ThreeSilentUnitsBehindAnAcceptingPeerGoBad) {
    // TCP, three silent units behind a peer that accepts every connect,
    // demote_after 5: the run of time-outs loses the connection, the values
    // go bad, and the counts kept across reconnects demote the units.
    Rig rig(channel_of(3, quick_failure(0, 5), TransportKind::TCP), 3);
    for (std::uint8_t unit = 1; unit <= 3; ++unit) {
        rig.bus.fault(unit, Fault::SILENT);
    }
    rig.run_until(60 * SECOND);
    EXPECT_GT(rig.engine.counts().losses, 0U);
    EXPECT_TRUE(all_comm_failed(rig, 3));
    EXPECT_EQ(rig.state(0), DeviceStatus::DEMOTED);
}

TEST(ChannelEngine, AGatewayWithADeadBusKeepsTheConnectionAndDemotesAll) {
    // The gateway answers "did not respond" for every unit: a valid frame,
    // which empties the run, so the connection is never lost (the lead's
    // revised ruling on D-12, 7.3); each counts toward its unit's demotion,
    // with no retry. At 150 s every device is DEMOTED and no tag is GOOD.
    Rig rig(channel_of(3, quick_failure(2, 5), TransportKind::TCP), 3);
    for (std::uint8_t unit = 1; unit <= 3; ++unit) {
        rig.bus.fault(unit, Fault::UNREACHABLE);
    }
    rig.run_until(150 * SECOND);
    // One connect, never closed, the run empty.
    EXPECT_EQ(rig.engine.counts().connects + rig.engine.counts().own_closes +
                  rig.engine.failures().timeout_run(),
              1U);
    EXPECT_TRUE(all_demoted(rig, 0, 3));
    EXPECT_TRUE(all_comm_failed(rig, 3));
    EXPECT_EQ(rig.engine.counts().retries, 0U);  // never retried
}

TEST(ChannelEngine, AGatewayAnswerBetweenTimeOutsEmptiesTheRun) {
    // Unit 1 is dead, unit 2 behind a gateway that cannot reach it: the
    // gateway's answers keep emptying the run, so the time-outs of unit 1
    // never lose the connection (lost_after_timeouts 3).
    Rig rig(channel_of(2, quick_failure(0, 100), TransportKind::TCP), 2);
    rig.bus.fault(1, Fault::SILENT);
    rig.bus.fault(2, Fault::UNREACHABLE);
    rig.run_until(10 * SECOND);
    EXPECT_EQ(rig.engine.counts().losses, 0U);
    EXPECT_GT(rig.engine.counts().timeouts, 3U);
}

TEST(ChannelEngine, ASingleDeadUnitGoesBadAndIsDemotedAcrossReconnects) {
    // One dead unit, demote_after 10, lost_after_timeouts 3: every third
    // time-out loses the connection, the two before it count, and the count
    // survives the reconnects: demoted, and BAD long before 200 s.
    Rig rig(channel_of(1, quick_failure(0, 10), TransportKind::TCP), 1);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(20 * SECOND);
    EXPECT_TRUE(all_comm_failed(rig, 1));
    rig.run_until(200 * SECOND);
    EXPECT_EQ(rig.state(0), DeviceStatus::DEMOTED);
    EXPECT_TRUE(all_comm_failed(rig, 1));
}

TEST(ChannelEngine, EveryTimeOutLosesTheConnectionWithLostAfterOne) {
    CompiledChannel channel =
        channel_of(1, quick_failure(0, 10), TransportKind::TCP);
    channel.connection.lost_after_timeouts = 1;
    Rig rig(std::move(channel), 1);
    rig.run_until(FAST_PERIOD - MILLISECOND);
    ASSERT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(SECOND);
    EXPECT_GT(rig.engine.counts().losses, 0U);
    EXPECT_TRUE(all_comm_failed(rig, 1));
}

TEST(ChannelEngine, AnOwnCloseNotBackWithinConnectTimeoutIsALoss) {
    // connect_timeout 500 ms, back-off from 1 s: a garbage frame closes the
    // connection twice; the second time no answer came since the connect,
    // so the reconnect waits 1 s, and at 500 ms the loss applies: COMM_FAIL
    // while the reconnect waits, though it then succeeds.
    CompiledChannel channel =
        channel_of(1, quick_failure(2, 10), TransportKind::TCP);
    channel.connection.connect_timeout_ns = 500 * MILLISECOND;
    Rig rig(std::move(channel), 1);
    rig.run_until(FAST_PERIOD - MILLISECOND);
    ASSERT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
    rig.bus.fault(1, Fault::GARBAGE);
    rig.run_until(FAST_PERIOD + 100 * MILLISECOND);
    EXPECT_EQ(rig.engine.counts().resumed, 1U);  // the first: back at once
    EXPECT_EQ(rig.engine.counts().losses, 0U);
    // The second close, at the next poll (400 ms), with no answer since the
    // connect: the reconnect waits 1 s, the loss applies after 500 ms.
    rig.run_until(2 * FAST_PERIOD + 499 * MILLISECOND);
    EXPECT_EQ(rig.engine.counts().losses, 0U);
    rig.run_until(2 * FAST_PERIOD + 600 * MILLISECOND);
    EXPECT_EQ(rig.engine.counts().losses, 1U);
    EXPECT_TRUE(all_comm_failed(rig, 1));
}

TEST(ChannelEngine, AReconnectKeepsADemotedDeviceDemoted) {
    // Q2: a demoted device is not polled again after a lost connection: no
    // burst of its retries after the reconnect.
    Rig rig(channel_of(2, quick_failure(2, 1), TransportKind::SERIAL), 2);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(SECOND);
    ASSERT_EQ(rig.state(0), DeviceStatus::DEMOTED);
    const std::size_t before = requests_of(rig.bus, 1);
    rig.transport.close_by_peer();
    rig.run_until(3 * SECOND);
    EXPECT_EQ(rig.state(0), DeviceStatus::DEMOTED);
    EXPECT_LE(requests_of(rig.bus, 1), before + 1);  // a probe at most
}

// --- P2: a request that fails writes its points bad -----------------------

TEST(ChannelEngine, APartOfADeviceThatNeverAnswersGoesBad) {
    // A device of two blocks: registers 0 and 150, the second never
    // answered. The first stays GOOD; the second is BAD with COMM_FAIL
    // after its retries, though the device answers and is never demoted.
    CompiledChannel channel =
        make_channel({{.name = "d1",
                       .unit_id = 1,
                       .points = {{.first = 0}, {.first = 150}}}},
                     ReadRules{.max_units = 4});
    channel.transport = TransportKind::SERIAL;
    channel.devices[0].failure = quick_failure(1, 3);
    Rig rig(std::move(channel), 1);
    rig.bus.silent_from(1, 100);
    rig.run_until(SECOND);
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
    EXPECT_EQ(rig.tags.point(0, 1).quality, tag::Quality::BAD);
    EXPECT_TRUE(rig.tags.has_flag(0, 1, Flag::COMM_FAIL));
    EXPECT_EQ(rig.state(0), DeviceStatus::RUNNING);
}

// --- P3: a stale frame does not start the second stage --------------------

TEST(ChannelEngine, ALateAnswerIsDroppedAndTheRetryWaitsItsOwnDeadline) {
    // TCP, time-out 100 ms, the unit answers after 150 ms: each attempt's
    // answer arrives during the next attempt as a stale frame and is
    // dropped; the attempt waits to its own first deadline and times out.
    // No broken frame, no close (`lost_after_timeouts` 10 keeps the run of
    // time-outs out of it).
    CompiledChannel channel =
        channel_of(1, quick_failure(2, 10), TransportKind::TCP);
    channel.connection.lost_after_timeouts = 10;
    Rig rig(std::move(channel), 1);
    rig.transport.latency(1, 150 * MILLISECOND);
    rig.run_until(SECOND);
    EXPECT_GT(rig.engine.counts().stale_frames, 0U);
    EXPECT_EQ(rig.engine.counts().broken, 0U);
    EXPECT_EQ(rig.transport.abortive_closes(), 0);
    EXPECT_GT(rig.engine.counts().timeouts, 0U);
}

TEST(ChannelEngine, AFrameWithAnotherTransactionIdIsDroppedAndTheAnswerTaken) {
    // A stray frame of another transaction arrives before the answer: it is
    // dropped, and the answer that follows is read GOOD.
    Rig rig(channel_of(1, quick_failure(2, 10), TransportKind::TCP), 1);
    rig.bus.set(1, 0, 0, 7);
    rig.transport.latency(1, 5 * MILLISECOND);
    // tid 0x7FFF, unit 1, OK, 1 unit, value 99: no request has that id.
    rig.transport.inject(
        {std::byte{0x7F}, std::byte{0xFF}, std::byte{1}, std::byte{0},
         std::byte{0}, std::byte{1}, std::byte{0}, std::byte{99}},
        MILLISECOND);
    rig.run_until(FAST_PERIOD - MILLISECOND);
    EXPECT_EQ(rig.engine.counts().stale_frames, 1U);
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
    EXPECT_EQ(rig.tags.point(0, 0).value, 7);
}

// --- P6: the silence of a serial line -------------------------------------

TEST(ChannelEngine, ASerialLineKeepsThreeAndAHalfCharactersAfterAGoodAnswer) {
    // 9,600 bit/s 8E1: 3.5 characters are 4.01 ms; with `turnaround` 2 ms
    // the next request waits 6.01 ms after the last byte of the answer.
    CompiledChannel channel =
        channel_of(2, quick_failure(0, 5), TransportKind::SERIAL);
    channel.line = LineTiming{.bit_rate = 9600,
                              .bits_per_char = bits_per_char(8, true, 1),
                              .turnaround_ns = 2 * MILLISECOND};
    Rig rig(std::move(channel), 2);
    rig.run_until(50 * MILLISECOND);
    const std::vector<std::int64_t> first = times_of(rig.bus, 1);
    const std::vector<std::int64_t> second = times_of(rig.bus, 2);
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());
    const std::int64_t gap = inter_frame_gap_ns(rig.channel.line);
    EXPECT_EQ(gap, 4'010'417 + 2 * MILLISECOND);
    EXPECT_GE(second.front() - (first.front() + LATENCY), gap);
}

TEST(ChannelEngine, BytesDuringTheQuietTimeAreNotTheNextAnswer) {
    // A time-out of unit 1, then noise arrives during the quiet time: it is
    // discarded before the next request, whose answer is read GOOD.
    Rig rig(channel_of(2, quick_failure(0, 5), TransportKind::SERIAL), 2);
    rig.bus.fault(1, Fault::SILENT);
    rig.bus.set(2, 0, 0, 5);
    rig.transport.inject({std::byte{0x55}, std::byte{0x55}, std::byte{0x55}},
                         110 * MILLISECOND);
    rig.run_until(200 * MILLISECOND);
    EXPECT_EQ(rig.engine.counts().broken, 0U);
    EXPECT_EQ(rig.tags.point(1, 0).quality, tag::Quality::GOOD);
    EXPECT_EQ(rig.tags.point(1, 0).value, 5);
}

TEST(ChannelEngine, APartialSendOnASerialLineIsATimeOut) {
    // Q7: not a close; the request is tried again.
    Rig rig(channel_of(1, quick_failure(2, 5), TransportKind::SERIAL), 1);
    rig.transport.partial_sends(1);
    rig.run_until(FAST_PERIOD - MILLISECOND);
    EXPECT_EQ(rig.transport.abortive_closes(), 0);
    EXPECT_EQ(rig.engine.counts().retries, 1U);
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
}

// --- Q1: a retry takes its turn -------------------------------------------

TEST(ChannelEngine, ARetryWaitsForTheOtherDevicesDueAtTheSameTime) {
    // Dead unit 1 (retries 2) and live unit 2 in one group: unit 2 is read
    // between unit 1's first attempt and its retry, which waits for a gap
    // longer than its time-out (K3).
    Rig rig(channel_of(2, quick_failure(2, 5), TransportKind::TCP), 2);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(2 * FAST_PERIOD);
    const std::vector<std::int64_t> dead = times_of(rig.bus, 1);
    const std::vector<std::int64_t> live = times_of(rig.bus, 2);
    ASSERT_GE(dead.size(), 2U);
    ASSERT_FALSE(live.empty());
    EXPECT_LT(live.front(), dead[1]);
}

// --- Q4: the probe budget, measured ---------------------------------------

// The share of line time the probes of `dead` units took from `from_ns` to
// `to_ns`, each probe holding the line for its time-out.
double probe_share(const SimBus& bus, std::uint8_t dead, std::int64_t from_ns,
                   std::int64_t to_ns, std::int64_t timeout_ns) {
    std::int64_t busy = 0;
    for (const LoggedRequest& request : bus.log()) {
        if (request.unit_id >= 2 && request.unit_id <= dead + 1 &&
            request.time_ns >= from_ns && request.time_ns < to_ns) {
            busy += timeout_ns;
        }
    }
    return static_cast<double>(busy) / static_cast<double>(to_ns - from_ns);
}

TEST(ChannelEngine, TheProbeBudgetKeepsTheLivePeriod) {
    // One live unit in a 1 s group and 20 dead ones behind a serial line,
    // time-out 100 ms, back-off 1 s to 4 s. A probe goes only when no poll
    // falls due within its time-out, unless it waited 10 time-outs: the
    // live unit's period stays within one probe of 1 s (measured: 1.003 s),
    // and the probes, which use the idle time only, take 49 % of the line
    // here (D-138).
    CompiledChannel channel =
        channel_of(21, quick_failure(0, 1), TransportKind::SERIAL, 1);
    Rig rig(std::move(channel), 21);
    for (std::uint8_t unit = 2; unit <= 21; ++unit) {
        rig.bus.fault(unit, Fault::SILENT);
    }
    rig.run_until(310 * SECOND);
    const std::int64_t worst =
        longest_gap_after(times_of(rig.bus, 1), 10 * SECOND);
    const double share =
        probe_share(rig.bus, 20, 10 * SECOND, 310 * SECOND, 100 * MILLISECOND);
    EXPECT_LE(worst, SECOND + 100 * MILLISECOND + 25 * MILLISECOND);
    EXPECT_LE(share, 0.60);
}

// --- C++ review: R8, P3, Q11 to Q15 ---------------------------------------

// A one-device channel whose only point lies in a side-effect range, so its
// plan has no probe request.
CompiledChannel side_effect_only(TransportKind transport) {
    ReadRules rules{.max_units = LARGE};
    rules.ranges = {ReadRange{.kind = ReadRange::Kind::SIDE_EFFECT,
                              .unit_id = 1,
                              .first = 0,
                              .last = 0}};
    CompiledChannel channel = make_channel(
        {{.name = "d1", .unit_id = 1, .points = adjacent(0, 1)}}, rules);
    channel.transport = transport;
    channel.devices[0].failure = quick_failure(0, 1);
    return channel;
}

TEST(ChannelEngine, ADeviceWithoutAProbeRequestIsProbedByAWholePollCycle) {
    // R8: its only request reads a side-effect range, so no probe request
    // exists. It is demoted, probed by one poll cycle per back-off (the
    // range read once each time), and recovers when the unit answers.
    Rig rig(side_effect_only(TransportKind::SERIAL), 1);
    ASSERT_FALSE(rig.channel.devices[0].plan.probe.has_value());
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(500 * MILLISECOND);
    ASSERT_EQ(rig.state(0), DeviceStatus::DEMOTED);
    const std::size_t before = requests_of(rig.bus, 1);
    rig.run_until(8 * SECOND);
    // Probes at about 1, 3 and 7 s after the demotion: one read each.
    EXPECT_EQ(requests_of(rig.bus, 1), before + 3);
    rig.bus.fault(1, Fault::NONE);
    rig.bus.set(1, 0, 0, 3);
    rig.run_until(13 * SECOND);
    EXPECT_EQ(rig.state(0), DeviceStatus::RUNNING);
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
    EXPECT_EQ(rig.tags.point(0, 0).value, 3);
}

TEST(ChannelEngine, BytesThatStartNoAnswerEndTheSecondStage) {
    // A serial unit sends three bytes 5 ms after the request and nothing
    // more: the answer is broken 20 ms (the margin) after its first byte,
    // not at the 100 ms time-out.
    Rig rig(channel_of(1, quick_failure(0, 5), TransportKind::SERIAL), 1);
    rig.bus.fault(1, Fault::SILENT);
    rig.transport.inject({std::byte{0}, std::byte{0}, std::byte{1}},
                         5 * MILLISECOND);
    rig.run_until(30 * MILLISECOND);
    EXPECT_EQ(rig.engine.counts().broken, 1U);
    EXPECT_EQ(rig.engine.counts().timeouts, 0U);
}

TEST(ChannelEngine, AStaleFrameNeverMakesTheWaitLonger) {
    // P3, the other direction: 1,200 bit/s, time-out 100 ms, the unit
    // answers only after 500 ms, and a stale frame arrives at 90 ms. The
    // attempt still times out at its first deadline (173 ms); the late
    // answer is not taken.
    CompiledChannel channel =
        channel_of(1, quick_failure(0, 5), TransportKind::SERIAL);
    channel.line = LineTiming{.bit_rate = 1200,
                              .bits_per_char = bits_per_char(8, true, 1)};
    Rig rig(std::move(channel), 1);
    rig.transport.latency(1, 500 * MILLISECOND);
    rig.transport.inject(
        {std::byte{0x7F}, std::byte{0xFF}, std::byte{1}, std::byte{0},
         std::byte{0}, std::byte{1}, std::byte{0}, std::byte{9}},
        90 * MILLISECOND);
    rig.run_until(190 * MILLISECOND);
    EXPECT_EQ(rig.engine.counts().stale_frames, 1U);
    EXPECT_EQ(rig.engine.counts().timeouts, 1U);
    EXPECT_NE(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
}

TEST(ChannelEngine, ASecondOwnCloseBeforeAnyAnswerWaitsTheBackOff) {
    // 7.3: the second close comes before any valid answer: the reconnect
    // waits `reconnect.min` (1 s), not at once.
    Rig rig(channel_of(1, quick_failure(2, 10), TransportKind::TCP), 1);
    rig.run_until(FAST_PERIOD - MILLISECOND);
    rig.bus.fault(1, Fault::GARBAGE);
    rig.run_until(2 * FAST_PERIOD + 10 * MILLISECOND);
    const std::vector<std::int64_t>& connects = rig.transport.connect_times();
    ASSERT_EQ(connects.size(), 2U);  // the first close reconnected at once
    rig.run_until(4 * SECOND);
    ASSERT_GE(connects.size(), 3U);
    EXPECT_GE(connects[2] - connects[1], SECOND);
}

TEST(ChannelEngine, ASendThatFailsWithNothingSentLosesTheConnection) {
    Rig rig(channel_of(1, quick_failure(2, 10), TransportKind::TCP), 1);
    rig.run_until(FAST_PERIOD - MILLISECOND);
    rig.transport.failed_sends(1);
    rig.run_until(FAST_PERIOD + MILLISECOND);
    EXPECT_EQ(rig.engine.counts().losses, 1U);
    EXPECT_TRUE(all_comm_failed(rig, 1));
}

TEST(ChannelEngine, AProbeWaitsAtMostOneSecond) {
    // A device time-out of 3 s; its probe waits 1 s (7.4): the second
    // probe follows the first by 1 s plus the doubled back-off (2 s).
    FailureRules rules = quick_failure(0, 1);
    rules.timeout_ns = 3 * SECOND;
    CompiledChannel channel = channel_of(1, rules, TransportKind::TCP);
    channel.connection.lost_after_timeouts = 10;
    Rig rig(std::move(channel), 1);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(20 * SECOND);
    const std::vector<std::int64_t> sent = times_of(rig.bus, 1);
    ASSERT_GE(sent.size(), 3U);  // the poll, then two probes
    EXPECT_EQ(sent[2] - sent[1], SECOND + 2 * SECOND);
}

TEST(ChannelEngine, AChainIsPublishedWholeAndItsDependentsFollowAFailure) {
    // A value at 0 with its companion at 100 (4 units per request): nothing
    // is written after the first request, both after the second. When the
    // companion's request fails, the companion goes BAD with COMM_FAIL and
    // the value goes BAD with it, through its companion's outcome (DI 5.5,
    // conformance test 8; review G1), and stays BAD.
    CompiledChannel channel = make_channel(
        {{.name = "d1",
          .unit_id = 1,
          .points = {{.first = 0, .companions = {1}}, {.first = 100}}}},
        ReadRules{.max_units = 4});
    channel.transport = TransportKind::TCP;
    channel.devices[0].failure = quick_failure(0, 10);
    quality_from_companions(channel, 0, 0);
    Rig rig(std::move(channel), 1);
    rig.run_until(3 * LATENCY);
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
    EXPECT_EQ(rig.tags.point(0, 0).writes, 1);
    EXPECT_EQ(rig.tags.point(0, 1).writes, 1);
    // Both written when the second answer was in, not the value first.
    EXPECT_EQ(rig.tags.point(0, 0).time_ns, rig.tags.point(0, 1).time_ns);
    // The companion written before its dependent (P8's order).
    ASSERT_GE(rig.tags.order().size(), 2U);
    EXPECT_EQ(rig.tags.order()[0].second, 1U);
    rig.bus.silent_from(1, 100);
    rig.run_until(FAST_PERIOD + 200 * MILLISECOND);
    EXPECT_TRUE(rig.tags.has_flag(0, 1, Flag::COMM_FAIL));
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::BAD);
    // The companion still before its dependent.
    EXPECT_EQ(rig.tags.order().back().second, 0U);
    rig.run_until(60 * SECOND);
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::BAD);
    EXPECT_EQ(rig.tags.point(0, 1).quality, tag::Quality::BAD);
}

TEST(ChannelEngine, AChainWhoseMiddlePartFailsWritesAllItsPoints) {
    // A value at 0 with companions at 100 and 150, one request each. The
    // request at 100 times out: when the chain's last request is in, the
    // companion at 100 is BAD with COMM_FAIL, the one at 150 GOOD, and the
    // value BAD through its failed companion, all at once (G1, P9).
    CompiledChannel channel =
        make_channel({{.name = "d1",
                       .unit_id = 1,
                       .points = {{.first = 0, .companions = {1, 2}},
                                  {.first = 100},
                                  {.first = 150}}}},
                     ReadRules{.max_units = 4});
    channel.transport = TransportKind::TCP;
    channel.devices[0].failure = quick_failure(0, 10);
    quality_from_companions(channel, 0, 0);
    Rig rig(std::move(channel), 1);
    rig.bus.silent_from(1, 100, 100);
    rig.run_until(FAST_PERIOD - MILLISECOND);
    ASSERT_EQ(rig.bus.log().size(), 3U);  // the chain read to its end
    EXPECT_TRUE(rig.tags.has_flag(0, 1, Flag::COMM_FAIL));
    EXPECT_EQ(rig.tags.point(0, 2).quality, tag::Quality::GOOD);
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::BAD);
    EXPECT_EQ(rig.tags.point(0, 0).writes, 1);
    EXPECT_EQ(rig.tags.point(0, 0).time_ns, rig.tags.point(0, 2).time_ns);
    EXPECT_EQ(rig.tags.point(0, 1).time_ns, rig.tags.point(0, 2).time_ns);
}

TEST(ChannelEngine, AChainWithABusyPartPublishesNothing) {
    // P9: as above, the request at 100 answered busy: nothing of the chain
    // is written, and busy writes nothing either.
    CompiledChannel channel =
        make_channel({{.name = "d1",
                       .unit_id = 1,
                       .points = {{.first = 0, .companions = {1, 2}},
                                  {.first = 100},
                                  {.first = 150}}}},
                     ReadRules{.max_units = 4});
    channel.transport = TransportKind::TCP;
    channel.devices[0].failure = quick_failure(0, 10);
    Rig rig(std::move(channel), 1);
    rig.bus.busy_at(1, 100);
    rig.run_until(FAST_PERIOD - MILLISECOND);
    ASSERT_EQ(rig.bus.log().size(), 3U);
    EXPECT_EQ(rig.tags.point(0, 0).writes, 0);
    EXPECT_EQ(rig.tags.point(0, 1).writes, 0);
    EXPECT_EQ(rig.tags.point(0, 2).writes, 0);
}

TEST(ChannelEngine, ADeviceDemotedTwiceIsWrittenCommFailTwice) {
    Rig rig(channel_of(1, quick_failure(0, 1), TransportKind::SERIAL), 1);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(500 * MILLISECOND);
    ASSERT_EQ(rig.state(0), DeviceStatus::DEMOTED);
    rig.bus.fault(1, Fault::NONE);
    rig.run_until(3 * SECOND);
    ASSERT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(4 * SECOND);
    EXPECT_EQ(rig.state(0), DeviceStatus::DEMOTED);
    EXPECT_TRUE(rig.tags.has_flag(0, 0, Flag::COMM_FAIL));
}

TEST(ChannelEngine, AnAbsentAnswerWritesThePointsAbsentNotCommFail) {
    Rig rig(channel_of(1, quick_failure(0, 1), TransportKind::TCP), 1);
    rig.bus.fault(1, Fault::ABSENT);
    rig.run_until(FAST_PERIOD - MILLISECOND);
    EXPECT_EQ(rig.state(0), DeviceStatus::RUNNING);  // an answer
    EXPECT_EQ(rig.tags.point(0, 0).writes, 1);
    EXPECT_FALSE(rig.tags.has_flag(0, 0, Flag::COMM_FAIL));
    EXPECT_NE(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
}

TEST(ChannelEngine, OnRtuOverTcpAReadTimeOutNeverClosesTheConnection) {
    // 7.3: no close for read time-outs, the quiet time after each, and the
    // device demoted by its count.
    Rig rig(channel_of(1, quick_failure(0, 3), TransportKind::RTU_OVER_TCP), 1);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(2 * SECOND);
    EXPECT_EQ(rig.engine.counts().own_closes, 0U);
    EXPECT_EQ(rig.engine.counts().losses, 0U);
    EXPECT_EQ(rig.state(0), DeviceStatus::DEMOTED);
    const std::vector<std::int64_t> sent = times_of(rig.bus, 1);
    ASSERT_GE(sent.size(), 2U);
    EXPECT_GE(sent[1] - sent[0], 100 * MILLISECOND + MIN_MARGIN_NS);
}

TEST(ChannelEngine, ARefusedEncodeEndsTheRequestAndTheEngineMovesOn) {
    // Q11: a request the protocol cannot encode (more units than the test
    // protocol reads) fails without I/O; the clock moves on.
    CompiledChannel channel =
        channel_of(1, quick_failure(0, 5), TransportKind::TCP);
    channel.devices[0].plan.requests[0].count = 1000;
    Rig rig(std::move(channel), 1);
    const std::size_t steps = run_engine(rig.engine, rig.clock, SECOND);
    EXPECT_LT(steps, 100U);
    EXPECT_EQ(rig.engine.counts().requests, 0U);
    EXPECT_TRUE(rig.tags.has_flag(0, 0, Flag::COMM_FAIL));
}

TEST(ChannelEngine, OnAStreamBytesAfterAFrameStartTheNextOne) {
    // Q12: a stale frame and the start of the answer arrive in one read on
    // a stream; the stale one is dropped and the rest read as the answer.
    Rig rig(channel_of(1, quick_failure(0, 5), TransportKind::TCP), 1);
    rig.bus.set(1, 0, 0, 4);
    rig.transport.latency(1, 2 * MILLISECOND);
    rig.transport.inject(
        {std::byte{0x7F}, std::byte{0xFF}, std::byte{1}, std::byte{0},
         std::byte{0}, std::byte{1}, std::byte{0}, std::byte{9}},
        2 * MILLISECOND);
    rig.run_until(FAST_PERIOD - MILLISECOND);
    EXPECT_EQ(rig.engine.counts().stale_frames, 1U);
    EXPECT_EQ(rig.tags.point(0, 0).value, 4);
}

TEST(ChannelEngine, KeysOutOfRangeAreRefusedWhenTheEngineIsBuilt) {
    // Q13: a back-off of 0 would spin.
    CompiledChannel channel =
        channel_of(1, quick_failure(0, 5), TransportKind::TCP);
    channel.connection.reconnect_min_ns = 0;
    Rig rig(std::move(channel), 1);
    EXPECT_FALSE(rig.engine.status().ok());
    EXPECT_EQ(rig.engine.step(), ChannelEngine::NEVER);
    EXPECT_EQ(rig.transport.connects(), 0);
}

TEST(ChannelEngine, AProbeAnswerLeavesTheReconnectBackOffAndTheRun) {
    // R10: a probe's answer neither empties the run of time-outs nor
    // resets the reconnect back-off (only a poll's answer does).
    Rig rig(channel_of(1, quick_failure(0, 1), TransportKind::TCP), 1);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(500 * MILLISECOND);
    ASSERT_EQ(rig.state(0), DeviceStatus::DEMOTED);
    EXPECT_EQ(rig.engine.failures().timeout_run(), 1U);
    rig.bus.fault(1, Fault::BUSY);  // the probe will be answered
    // The probe falls due 1 s after the demotion at 100 ms; stop right
    // after its exchange, before the recovered device's own poll.
    rig.run_until(1100 * MILLISECOND);
    EXPECT_EQ(rig.state(0), DeviceStatus::INITIALISING);
    EXPECT_EQ(rig.engine.failures().timeout_run(), 1U);
}

// --- Tests the mutation pass of Q15 asked for -----------------------------

TEST(ChannelEngine, AStepWhileIdleLearnsOfThePeersClose) {
    // 7.3 "Idle close": a slow group, the peer closes while the engine waits;
    // the next boundary finds it without sending anything.
    Rig rig(channel_of(1, quick_failure(0, 5), TransportKind::TCP, SLOW), 1);
    rig.run_until(SECOND);
    const std::uint64_t requests = rig.engine.counts().requests;
    rig.transport.close_by_peer();
    (void)rig.engine.step();
    EXPECT_EQ(rig.engine.counts().losses, 1U);
    EXPECT_EQ(rig.engine.counts().requests, requests);
}

TEST(ChannelEngine, AConnectThatSucceedsAfterTheWindowIsStillALoss) {
    // R-17, P1: connect_timeout 500 ms, each connect taking 300 ms. Connected
    // at 300 ms, a broken answer at 301 ms closes the connection; with no
    // answer since the connect the reconnect waits 400 ms and starts at
    // 701 ms, inside the window, and succeeds at 1001 ms, after it: the
    // rules of a lost connection apply then.
    CompiledChannel channel =
        channel_of(1, quick_failure(0, 10), TransportKind::TCP);
    channel.connection.connect_timeout_ns = 500 * MILLISECOND;
    channel.connection.reconnect_min_ns = 400 * MILLISECOND;
    Rig rig(std::move(channel), 1);
    rig.bus.fault(1, Fault::GARBAGE);
    rig.transport.connect_delay(300 * MILLISECOND);
    rig.run_until(1001 * MILLISECOND + MILLISECOND / 2);
    EXPECT_EQ(rig.transport.connect_times(),
              (std::vector<std::int64_t>{0, 701 * MILLISECOND}));
    EXPECT_EQ(rig.engine.counts().losses, 1U);
    EXPECT_EQ(rig.engine.counts().resumed, 0U);
    EXPECT_TRUE(all_comm_failed(rig, 1));
}

TEST(ChannelEngine, OneProbeServesTwoSlicesOfOneUnit) {
    // Q5: two devices on one dead unit, demoted together: one probe per
    // back-off, not two.
    CompiledChannel channel =
        make_channel({{.name = "a", .unit_id = 1, .points = adjacent(0, 1)},
                      {.name = "b", .unit_id = 1, .points = adjacent(10, 1)}},
                     ReadRules{.max_units = LARGE});
    channel.transport = TransportKind::SERIAL;
    for (CompiledDevice& device : channel.devices) {
        device.failure = quick_failure(0, 1);
    }
    // The first device, which probes for the unit, waits 4 s; the second
    // would probe after 1 s.
    channel.devices[0].failure.backoff_ns = 4 * SECOND;
    channel.devices[0].failure.backoff_max_ns = 8 * SECOND;
    Rig rig(std::move(channel), 1);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(500 * MILLISECOND);
    ASSERT_EQ(rig.state(0), DeviceStatus::DEMOTED);
    ASSERT_EQ(rig.state(1), DeviceStatus::DEMOTED);
    const std::size_t before = requests_of(rig.bus, 1);
    rig.run_until(4 * SECOND);
    EXPECT_EQ(requests_of(rig.bus, 1), before);  // no probe of its own
    rig.run_until(4300 * MILLISECOND);
    EXPECT_EQ(requests_of(rig.bus, 1), before + 1);  // one, for both
}

TEST(ChannelEngine, CommFailIsWrittenOnceNotAtEveryRefusedConnect) {
    Rig rig(channel_of(1, quick_failure(0, 5), TransportKind::TCP), 1);
    rig.transport.refuse_connects(3, MILLISECOND);
    // Refused at 0, 1, 3 and connected at 7 s: one COMM_FAIL write until
    // then.
    rig.run_until(7 * SECOND);
    EXPECT_EQ(rig.state(0), DeviceStatus::CONNECTING);
    EXPECT_EQ(rig.tags.point(0, 0).writes, 1);
}

TEST(ChannelEngine, AFrameThatCameWithThePreviousAnswerIsReadNext) {
    // Q12: on a stream the bytes after a frame are kept: the next request's
    // whole answer came in the same read as the previous one (transaction
    // id 1, value 77), and is taken without waiting.
    Rig rig(channel_of(1, quick_failure(0, 5), TransportKind::TCP), 1);
    rig.bus.set(1, 0, 0, 5);
    rig.transport.trailing({std::byte{0}, std::byte{1}, std::byte{1},
                            std::byte{0}, std::byte{0}, std::byte{1},
                            std::byte{0}, std::byte{77}});
    rig.run_until(FAST_PERIOD + MILLISECOND / 2);
    EXPECT_EQ(rig.tags.point(0, 0).value, 77);
}

TEST(ChannelEngine, AnEarlyStaleFrameDoesNotShortenTheWait) {
    // P3: a stale frame at 10 ms, the answer at 50 ms, time-out 100 ms:
    // after the stale frame the first deadline holds, so the answer is
    // taken.
    Rig rig(channel_of(1, quick_failure(0, 5), TransportKind::SERIAL), 1);
    rig.bus.set(1, 0, 0, 6);
    rig.transport.latency(1, 50 * MILLISECOND);
    // The stale frame arrives in two reads: its start begins the second
    // stage, and once it is dropped the first deadline holds again.
    rig.transport.inject({std::byte{0x7F}, std::byte{0xFF}}, 10 * MILLISECOND);
    rig.transport.inject({std::byte{1}, std::byte{0}, std::byte{0},
                          std::byte{1}, std::byte{0}, std::byte{9}},
                         11 * MILLISECOND);
    rig.run_until(60 * MILLISECOND);
    EXPECT_EQ(rig.engine.counts().timeouts, 0U);
    EXPECT_EQ(rig.tags.point(0, 0).value, 6);
}

TEST(ChannelEngine, ABrokenFrameOnASerialLineIsRetried) {
    // D-12: on a serial line a broken answer is tried again.
    Rig rig(channel_of(1, quick_failure(1, 5), TransportKind::SERIAL), 1);
    rig.bus.fault(1, Fault::GARBAGE);
    rig.run_until(FAST_PERIOD - MILLISECOND);
    EXPECT_EQ(rig.engine.counts().retries, 1U);
    EXPECT_EQ(rig.engine.counts().broken, 2U);
}

TEST(ChannelEngine, AWorkerThatReadsLateStillEndsTheAnswerByTheCap) {
    // P3: the answer's start arrives at 50 ms, inside the first stage of
    // 100 ms, but the worker reads it only at 250 ms. The answer must end
    // by the second stage counted from the first deadline, not from the
    // late read: its rest at 260 ms is too late, and the answer is broken.
    Rig rig(channel_of(1, quick_failure(0, 5), TransportKind::TCP), 1);
    rig.bus.fault(1, Fault::SILENT);
    rig.transport.inject(
        {std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0}},
        50 * MILLISECOND);
    rig.transport.inject(
        {std::byte{0}, std::byte{1}, std::byte{0}, std::byte{9}},
        260 * MILLISECOND);
    rig.transport.stall_next_receive(250 * MILLISECOND);
    rig.run_until(255 * MILLISECOND);
    EXPECT_EQ(rig.engine.counts().broken, 1U);
    EXPECT_NE(rig.tags.point(0, 0).value, 9);
}

TEST(ChannelEngine, BytesFoundBeforeASendRestartTheQuietTime) {
    // G3: at 9,600 bit/s unit 2 times out, then starts a 255-byte answer
    // (292 ms) 5 ms after its deadline. The next request, to the healthy
    // unit 1, waits until the line is quiet instead of going out over it:
    // every time-out is unit 2's, and no request collided.
    CompiledChannel channel =
        channel_of(2, quick_failure(0, 100), TransportKind::SERIAL);
    channel.line.bit_rate = 9'600;
    Rig rig(std::move(channel), 2);
    rig.bus.fault(2, Fault::SILENT);
    rig.transport.collisions(true);
    // Its deadline: the request on the wire (11 bytes, 11.5 ms) plus 100 ms.
    rig.transport.late_burst(2, 117 * MILLISECOND, 292 * MILLISECOND);
    rig.run_until(2 * SECOND);
    EXPECT_EQ(rig.transport.collided(), 0U);
    EXPECT_EQ(rig.engine.counts().timeouts, requests_of(rig.bus, 2));
    EXPECT_EQ(rig.engine.failures().failures(0), 0U);
    EXPECT_EQ(rig.engine.counts().jams, 0U);
}

TEST(ChannelEngine, ALineThatIsNeverQuietIsJammedAndStillPolled) {
    // G3, J6: noise for 10 s on a serial line with `turnaround` 1 s: after
    // 5 s of quiet times that each found bytes the line counts as jammed,
    // once, and requests go out again; the rule is in time, not in rounds.
    CompiledChannel channel =
        channel_of(1, quick_failure(0, 100), TransportKind::SERIAL);
    channel.line = LineTiming{.bit_rate = 300, .turnaround_ns = SECOND};
    Rig rig(std::move(channel), 1);
    rig.run_until(FAST_PERIOD - MILLISECOND);
    rig.bus.fault(1, Fault::GARBAGE);  // a broken answer starts a quiet time
    rig.transport.noise(FAST_PERIOD, 20 * SECOND);
    rig.run_until(4 * SECOND);
    const std::size_t before = requests_of(rig.bus, 1);
    EXPECT_EQ(rig.engine.counts().jams, 0U);
    rig.run_until(9 * SECOND);
    EXPECT_EQ(rig.engine.counts().jams, 1U);
    EXPECT_GT(requests_of(rig.bus, 1), before);
}

TEST(ChannelEngine, ADeadBlockOfALiveDeviceRestsWithItsOwnBackOff) {
    // G8: the block at 100 never answers, the one at 0 always does, so the
    // device is never demoted. After 3 failures in a row (demote_after) the
    // block rests 1 s, then 2 s, then 4 s (back-off, doubling to its
    // maximum), tried once at the end of each; its point stays BAD with
    // COMM_FAIL. When it answers again, it is polled at its period.
    CompiledChannel channel =
        make_channel({{.name = "d1",
                       .unit_id = 1,
                       .points = {{.first = 0}, {.first = 100}}}},
                     ReadRules{.max_units = 4});
    channel.transport = TransportKind::SERIAL;
    channel.devices[0].failure = quick_failure(0, 3);
    Rig rig(std::move(channel), 1);
    rig.bus.silent_from(1, 100, 100);
    rig.run_until(10 * SECOND);
    // 3 at 0, 200, 400 ms; then at about 1.6, 3.8 and 8.0 s.
    const std::size_t dead_reads = reads_of(rig.bus, 100);
    EXPECT_TRUE(dead_reads == 5 || dead_reads == 6);
    EXPECT_GE(reads_of(rig.bus, 0), 45U);
    EXPECT_GE(rig.engine.counts().rests, 3U);
    EXPECT_EQ(rig.state(0), DeviceStatus::RUNNING);
    EXPECT_TRUE(rig.tags.has_flag(0, 1, Flag::COMM_FAIL));
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
    rig.bus.silent_from(1, 1000, 1000);  // the block answers again
    rig.run_until(20 * SECOND);
    EXPECT_EQ(rig.tags.point(0, 1).quality, tag::Quality::GOOD);
    const std::size_t before = reads_of(rig.bus, 100);
    rig.run_until(21 * SECOND);
    EXPECT_GE(reads_of(rig.bus, 100) - before, 4U);  // its period again
}

TEST(ChannelEngine, AProbeEndsWhenTheNextPollFallsDue) {
    // G7: default time-outs (1 s), a live unit in a 1 s group and a demoted
    // one, back-off 1 s. A poll falls due within every probe's 1 s; the
    // probe goes in the window before it, its time-out ending there, so the
    // returned unit is found at once and the live period is kept.
    FailureRules rules = quick_failure(0, 1);
    rules.timeout_ns = SECOND;
    rules.backoff_max_ns = SECOND;
    Rig rig(channel_of(2, rules, TransportKind::SERIAL, 1), 2);
    rig.bus.fault(2, Fault::SILENT);
    rig.run_until(1500 * MILLISECOND);  // demoted at 1 s, probe due at 2 s
    ASSERT_EQ(rig.state(1), DeviceStatus::DEMOTED);
    rig.bus.fault(2, Fault::NONE);
    const std::int64_t due = rig.engine.failures().probe_due_ns(1);
    rig.run_until(due + SECOND);
    EXPECT_NE(rig.state(1), DeviceStatus::DEMOTED);
    EXPECT_LE(longest_gap_after(times_of(rig.bus, 1), 3 * SECOND),
              SECOND + 5 * MILLISECOND);
}

TEST(ChannelEngine, AWindowedProbeThatTimesOutIsNoFailedProbe) {
    // J1: a dead unit's windowed probe gets no answer: its back-off does not
    // double and its due time stands; the floor probe, with the whole
    // time-out, follows 10 time-outs after the due time.
    FailureRules rules = quick_failure(0, 1);
    rules.timeout_ns = SECOND;
    rules.backoff_max_ns = 8 * SECOND;
    Rig rig(channel_of(2, rules, TransportKind::SERIAL, 1), 2);
    rig.bus.fault(2, Fault::SILENT);
    rig.run_until(1500 * MILLISECOND);  // demoted at 1 s, probe due at 2 s
    ASSERT_EQ(rig.state(1), DeviceStatus::DEMOTED);
    const std::int64_t due = rig.engine.failures().probe_due_ns(1);
    const std::size_t before = requests_of(rig.bus, 2);
    rig.run_until(due + 5 * SECOND);
    EXPECT_EQ(requests_of(rig.bus, 2), before + 1);  // the windowed one
    EXPECT_EQ(rig.engine.failures().probe_due_ns(1), due);
    rig.run_until(due + 10 * SECOND + 1100 * MILLISECOND);
    EXPECT_EQ(requests_of(rig.bus, 2), before + 2);  // and the floor probe
    EXPECT_GT(rig.engine.failures().probe_due_ns(1), due + 10 * SECOND);
}

// The reviewer's run of J1 on `transport`: unit 2 answers in 300 ms (its
// time-out 1 s) on a channel with a 200 ms group; silent from 5 s to 60 s.
DeviceStatus slow_unit_after_its_return(TransportKind transport) {
    CompiledChannel channel = channel_of(2, FailureRules{}, transport);
    channel.connection.lost_after_timeouts = 100;
    Rig rig(std::move(channel), 2);
    rig.transport.latency(2, 300 * MILLISECOND);
    rig.run_until(5 * SECOND);
    rig.bus.fault(2, Fault::SILENT);
    rig.run_until(60 * SECOND);
    EXPECT_EQ(rig.state(1), DeviceStatus::DEMOTED);
    rig.bus.fault(2, Fault::NONE);
    rig.run_until(400 * SECOND);
    return rig.state(1);
}

TEST(ChannelEngine, AUnitSlowerThanTheProbeWindowIsStillRecovered) {
    // J1, DI 7.4: no window is long enough for its 300 ms, but the floor
    // probe waits its whole time-out and finds it.
    EXPECT_EQ(slow_unit_after_its_return(TransportKind::SERIAL),
              DeviceStatus::RUNNING);
    EXPECT_EQ(slow_unit_after_its_return(TransportKind::TCP),
              DeviceStatus::RUNNING);
}

// --- The C++ delta check: H1 to H7 ------------------------------------------

// A device with a chain: a value at 0 read with its companion at 100 (and
// 150 when `three`), one request each, the value's quality from them.
CompiledChannel chained_device(TransportKind transport, bool three,
                               std::uint8_t demote_after) {
    std::vector<PointSpec> points{{.first = 0, .companions = {1}},
                                  {.first = 100}};
    if (three) {
        points[0].companions = {1, 2};
        points.push_back({.first = 150});
    }
    CompiledChannel channel =
        make_channel({{.name = "d1", .unit_id = 1, .points = points}},
                     ReadRules{.max_units = 4});
    channel.transport = transport;
    channel.devices[0].failure = quick_failure(0, demote_after);
    quality_from_companions(channel, 0, 0);
    return channel;
}

TEST(ChannelEngine, AChainWhoseFirstPartFailsStillWritesItsOtherParts) {
    // H1: a value at 0, its companion at 20 and the companion's own at 40
    // (0 <- 20 <- 40). The unit stops answering register 0 only: 0 is BAD
    // with COMM_FAIL, and 20 and 40 are written GOOD at every cycle, never
    // frozen; no part of a chain rests (K1).
    CompiledChannel channel =
        make_channel({{.name = "d1",
                       .unit_id = 1,
                       .points = {{.first = 0, .companions = {1}},
                                  {.first = 20, .companions = {2}},
                                  {.first = 40}}}},
                     ReadRules{.max_units = 4});
    channel.transport = TransportKind::SERIAL;
    channel.devices[0].failure = quick_failure(0, 3);
    quality_from_companions(channel, 0, 0);
    quality_from_companions(channel, 0, 1);
    Rig rig(std::move(channel), 1);
    rig.run_until(SECOND);
    rig.bus.silent_from(1, 0, 0);
    rig.run_until(10 * SECOND);
    EXPECT_TRUE(rig.tags.has_flag(0, 0, Flag::COMM_FAIL));
    for (std::uint32_t point = 1; point <= 2; ++point) {
        EXPECT_EQ(rig.tags.point(0, point).quality, tag::Quality::GOOD);
        EXPECT_GE(rig.tags.point(0, point).time_ns,
                  SimClock::REALTIME_OFFSET_NS + 9 * SECOND);
    }
    EXPECT_EQ(rig.engine.counts().rests, 0U);
    EXPECT_EQ(rig.state(0), DeviceStatus::RUNNING);
}

TEST(ChannelEngine, TheQuietTimeAfterATimeOutIncludesTheTurnaround) {
    // H2: 9,600 bit/s 8N1, `turnaround` 18 ms: 3.65 ms plus 18 ms is more
    // than the 20 ms floor, so the next request waits 21.65 ms after the
    // deadline.
    CompiledChannel channel =
        channel_of(2, quick_failure(0, 5), TransportKind::SERIAL);
    channel.line =
        LineTiming{.bit_rate = 9600, .turnaround_ns = 18 * MILLISECOND};
    Rig rig(std::move(channel), 2);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(200 * MILLISECOND);
    EXPECT_EQ(quiet_time_ns(rig.channel.line), 3'645'834 + 18 * MILLISECOND);
    const std::vector<std::int64_t> first = times_of(rig.bus, 1);
    const std::vector<std::int64_t> second = times_of(rig.bus, 2);
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());
    const std::int64_t deadline =
        first.front() + first_byte_deadline_ns(rig.channel.line, 0,
                                               REQUEST_BYTES,
                                               100 * MILLISECOND);
    EXPECT_GE(second.front(), deadline + quiet_time_ns(rig.channel.line));
}

// The retries one broken answer gets on `transport`, retries 2.
std::uint64_t retries_of_a_broken_answer(TransportKind transport) {
    Rig rig(channel_of(1, quick_failure(2, 10), transport), 1);
    rig.bus.fault(1, Fault::GARBAGE);
    rig.run_until(FAST_PERIOD - MILLISECOND);
    return rig.engine.counts().retries;
}

TEST(ChannelEngine, ABrokenAnswerIsRetriedExceptOnTcp) {
    // H3, the ruling on D-12: a broken frame is retried on a serial line and
    // on `rtu_over_tcp`; on `tcp` it closes the connection instead.
    EXPECT_EQ(retries_of_a_broken_answer(TransportKind::SERIAL), 2U);
    EXPECT_EQ(retries_of_a_broken_answer(TransportKind::RTU_OVER_TCP), 2U);
    EXPECT_EQ(retries_of_a_broken_answer(TransportKind::TCP), 0U);
}

TEST(ChannelEngine, ABusyCycleProbeCutsItsChain) {
    // H4: a chain in a side-effect range, so no probe request: the probe is
    // a poll cycle. Its first part answers busy: the unit recovers, but
    // nothing of that chain is written.
    ReadRules rules{.max_units = 4};
    rules.ranges = {ReadRange{.kind = ReadRange::Kind::SIDE_EFFECT,
                              .unit_id = 1,
                              .first = 100,
                              .last = 100}};
    CompiledChannel side = make_channel(
        {{.name = "d1",
          .unit_id = 1,
          .points = {{.first = 0, .companions = {1}}, {.first = 100}}}},
        rules);
    side.transport = TransportKind::SERIAL;
    side.devices[0].failure = quick_failure(0, 1);
    quality_from_companions(side, 0, 0);
    ASSERT_FALSE(side.devices[0].plan.probe.has_value());
    Rig rig(std::move(side), 1);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(500 * MILLISECOND);
    ASSERT_EQ(rig.state(0), DeviceStatus::DEMOTED);
    const int written = rig.tags.point(0, 1).writes;
    rig.bus.fault(1, Fault::NONE);
    // The chain's first request.
    rig.bus.busy_at(1, static_cast<std::uint32_t>(
                           rig.channel.devices[0].plan.requests[0].first));
    rig.run_until(3 * SECOND);
    EXPECT_NE(rig.state(0), DeviceStatus::DEMOTED);
    EXPECT_EQ(rig.tags.point(0, 1).writes, written);
}

TEST(ChannelEngine, DevicesOfTheirOwnUnitsCountApart) {
    // H5: two devices with no shared unit (`OWN_UNIT`): the silent one is
    // demoted, the live one keeps RUNNING.
    CompiledChannel channel =
        channel_of(2, quick_failure(0, 3), TransportKind::SERIAL);
    for (CompiledDevice& device : channel.devices) {
        device.unit = CompiledDevice::OWN_UNIT;
    }
    Rig rig(std::move(channel), 2);
    rig.bus.fault(2, Fault::SILENT);
    rig.run_until(2 * SECOND);
    EXPECT_EQ(rig.state(0), DeviceStatus::RUNNING);
    EXPECT_EQ(rig.state(1), DeviceStatus::DEMOTED);
}

TEST(ChannelEngine, AChainCutByADemotionIsNotPublishedAfterTheRecovery) {
    // H6: chain 0, 100, 150 with demote_after 1. The part at 100 times out:
    // the device is demoted with the chain half read, and `publish_states`
    // drops what it held. After the recovery the first chain is published
    // whole; nothing left over makes it overflow.
    Rig rig(chained_device(TransportKind::SERIAL, true, 1), 1);
    rig.run_until(FAST_PERIOD - MILLISECOND);
    rig.bus.silent_from(1, 100, 100);
    rig.run_until(FAST_PERIOD + 200 * MILLISECOND);
    ASSERT_EQ(rig.state(0), DeviceStatus::DEMOTED);
    rig.bus.silent_from(1, 1000, 1000);
    rig.run_until(4 * SECOND);
    ASSERT_EQ(rig.state(0), DeviceStatus::RUNNING);
    EXPECT_EQ(rig.engine.counts().chain_overflows, 0U);
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
}

TEST(ChannelEngine, TheGapAfterAGoodAnswerIsExact) {
    // H6: 9,600 bit/s 8E1 with `turnaround` 2 ms: the next request goes out
    // exactly 6.01 ms after the answer's last byte, not earlier.
    CompiledChannel channel =
        channel_of(2, quick_failure(0, 5), TransportKind::SERIAL);
    channel.line = LineTiming{.bit_rate = 9600,
                              .bits_per_char = bits_per_char(8, true, 1),
                              .turnaround_ns = 2 * MILLISECOND};
    Rig rig(std::move(channel), 2);
    rig.run_until(50 * MILLISECOND);
    const std::vector<std::int64_t> first = times_of(rig.bus, 1);
    const std::vector<std::int64_t> second = times_of(rig.bus, 2);
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());
    EXPECT_EQ(second.front() - (first.front() + LATENCY),
              4'010'417 + 2 * MILLISECOND);
}

TEST(ChannelEngine, BytesAfterAFrameOnASerialLineAreDropped) {
    // H6, Q12: on a serial line the bytes after a frame are noise, even when
    // they look like the next answer (transaction id 1, value 77): the next
    // request reads its own answer.
    Rig rig(channel_of(1, quick_failure(0, 5), TransportKind::SERIAL), 1);
    rig.bus.set(1, 0, 0, 5);
    rig.transport.trailing({std::byte{0}, std::byte{1}, std::byte{1},
                            std::byte{0}, std::byte{0}, std::byte{1},
                            std::byte{0}, std::byte{77}});
    rig.run_until(FAST_PERIOD + 50 * MILLISECOND);
    EXPECT_EQ(rig.tags.point(0, 0).value, 5);
    EXPECT_EQ(rig.engine.counts().stale_frames, 0U);
}

TEST(ChannelEngine, AReadOfNothingIsAClose) {
    // H6, Q14: a read that returns OK with 0 bytes is the peer's close.
    Rig rig(channel_of(1, quick_failure(0, 5), TransportKind::TCP), 1);
    rig.transport.empty_read();
    rig.run_until(10 * MILLISECOND);
    EXPECT_EQ(rig.engine.counts().losses, 1U);
}

TEST(ChannelEngine, AProbesAnswerDoesNotResetTheReconnectBackOff) {
    // H6, R10: three refused connects grow the back-off to 8 s; the device
    // is demoted on the new connection, its probe answers, and the peer
    // closes before any poll answered: the reconnect waits the grown
    // back-off, not `reconnect.min`.
    CompiledChannel channel =
        channel_of(1, quick_failure(0, 1), TransportKind::TCP);
    channel.connection.lost_after_timeouts = 100;
    Rig rig(std::move(channel), 1);
    rig.bus.fault(1, Fault::SILENT);
    rig.transport.refuse_connects(3, MILLISECOND);
    rig.run_until(7500 * MILLISECOND);
    ASSERT_EQ(rig.state(0), DeviceStatus::DEMOTED);
    const std::int64_t probe_at = rig.engine.failures().probe_due_ns(0);
    rig.run_until(probe_at - MILLISECOND);
    rig.bus.fault(1, Fault::NONE);
    // The probe step's two checks pass; the next step's check finds it.
    rig.transport.close_by_peer_after_checks(3);
    rig.run_until(probe_at + 20 * SECOND);
    ASSERT_GE(rig.transport.connect_times().size(), 5U);
    EXPECT_GE(rig.transport.connect_times()[4] - probe_at, 8 * SECOND);
}

// The writes of the point of a side-effect-only device (R8) by its cycle
// probe when the unit comes back, `busy` or answering: the difference
// between just after the probe and just before it.
int written_by_a_cycle_probe(bool busy) {
    Rig rig(side_effect_only(TransportKind::SERIAL), 1);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(500 * MILLISECOND);
    EXPECT_EQ(rig.state(0), DeviceStatus::DEMOTED);
    const int before = rig.tags.point(0, 0).writes;
    rig.bus.fault(1, busy ? Fault::BUSY : Fault::NONE);
    rig.bus.set(1, 0, 0, 3);
    const std::int64_t probe_at = rig.engine.failures().probe_due_ns(0);
    rig.run_until(probe_at + 5 * MILLISECOND);
    EXPECT_NE(rig.state(0), DeviceStatus::DEMOTED);
    return rig.tags.point(0, 0).writes - before;
}

TEST(ChannelEngine, ACycleProbePublishesItsSamplesAndABusyOneNothing) {
    // H6, R8: the probe of a device with no probe request is its poll cycle,
    // whose answer is published at once; a busy answer recovers the device
    // and writes nothing.
    EXPECT_EQ(written_by_a_cycle_probe(false), 1);
    EXPECT_EQ(written_by_a_cycle_probe(true), 0);
}

TEST(ChannelEngine, AProbeTheProtocolCannotEncodeCountsAsFailed) {
    // H6, Q11: the probe request cannot be encoded: no byte is sent, and the
    // probe back-off doubles as for a failed probe (1 s, 2 s, 4 s).
    CompiledChannel channel =
        channel_of(1, quick_failure(0, 1), TransportKind::SERIAL);
    ReadPlan& plan = channel.devices[0].plan;
    Request probe = plan.requests[0];
    probe.count = 1000;
    plan.requests.push_back(probe);
    plan.probe = static_cast<std::uint32_t>(plan.requests.size() - 1);
    Rig rig(std::move(channel), 1);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(500 * MILLISECOND);
    ASSERT_EQ(rig.state(0), DeviceStatus::DEMOTED);
    const std::size_t sent = requests_of(rig.bus, 1);
    const std::int64_t first = rig.engine.failures().probe_due_ns(0);
    rig.run_until(first + MILLISECOND);
    EXPECT_EQ(rig.engine.failures().probe_due_ns(0) - first, 2 * SECOND);
    rig.run_until(10 * SECOND);
    EXPECT_EQ(requests_of(rig.bus, 1), sent);
}

TEST(ChannelEngine, ARefusedConnectWithinAnOwnCloseKeepsTheTags) {
    // H6, R-17: a partial send closes the connection (our own close, tags
    // kept); the immediate reconnect is refused, the next one 1 s later is
    // within `connect_timeout` (3 s): resumed, and no tag went bad.
    Rig rig(channel_of(1, quick_failure(0, 5), TransportKind::TCP), 1);
    rig.run_until(FAST_PERIOD - MILLISECOND);
    rig.transport.partial_sends(1);
    rig.transport.refuse_connects(1, MILLISECOND);
    rig.run_until(2 * SECOND);
    EXPECT_EQ(rig.engine.counts().own_closes, 1U);
    EXPECT_EQ(rig.engine.counts().refused, 1U);
    EXPECT_EQ(rig.engine.counts().resumed, 1U);
    EXPECT_EQ(rig.engine.counts().losses, 0U);
    EXPECT_FALSE(rig.tags.has_flag(0, 0, Flag::COMM_FAIL));
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
}

TEST(ChannelEngine, APollsAnswerEmptiesTheRun) {
    // H6, 7.3: unit 1 dead, unit 2 alive: every answer of unit 2 empties the
    // run, so it never holds more than the time-out since.
    CompiledChannel channel =
        channel_of(2, quick_failure(0, 100), TransportKind::TCP);
    channel.connection.lost_after_timeouts = 100;
    Rig rig(std::move(channel), 2);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(3 * SECOND);
    EXPECT_GT(rig.engine.counts().timeouts, 5U);
    EXPECT_LE(rig.engine.failures().timeout_run(), 1U);
}

// Whether an engine accepts a serial line of `bit_rate` and `turnaround`.
bool line_accepted(std::uint32_t bit_rate, std::int64_t turnaround_ns) {
    CompiledChannel channel =
        channel_of(1, quick_failure(0, 5), TransportKind::SERIAL);
    channel.line.bit_rate = bit_rate;
    channel.line.turnaround_ns = turnaround_ns;
    const Rig rig(std::move(channel), 1);
    return rig.engine.status().ok();
}

TEST(ChannelEngine, LineKeysOutsideTheSchemasRangesAreRefused) {
    // H7: CS `baud` 300 to 4,000,000 (0 a stream), `turnaround` 0 to 1 s.
    EXPECT_TRUE(line_accepted(300, 0) && line_accepted(4'000'000, SECOND) &&
                line_accepted(0, 0));
    EXPECT_FALSE(line_accepted(299, 0));
    EXPECT_FALSE(line_accepted(4'000'001, 0));
    EXPECT_FALSE(line_accepted((1U << 31U) + 1U, 0));
    EXPECT_FALSE(line_accepted(9'600, SECOND + 1));
    EXPECT_FALSE(
        line_accepted(9'600, std::numeric_limits<std::int64_t>::max()));
}

TEST(ChannelEngine, BytesAfterTheQuietTimeAreDroppedBeforeTheNextSend) {
    // 7.2, G3: the line fell quiet after a time-out, but the next poll is
    // not due yet; a stray byte arrives in between. When the poll falls due
    // it is found and dropped, a quiet time follows, and the poll goes out
    // at 220 ms: the byte is never read as the start of its answer.
    Rig rig(channel_of(1, quick_failure(0, 5), TransportKind::SERIAL), 1);
    rig.bus.set(1, 0, 0, 8);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(110 * MILLISECOND);  // the time-out, and its quiet time
    rig.bus.fault(1, Fault::NONE);
    rig.transport.inject({std::byte{0xAA}}, 150 * MILLISECOND);
    rig.run_until(FAST_PERIOD + 30 * MILLISECOND);
    EXPECT_EQ(times_of(rig.bus, 1).back(), FAST_PERIOD + MIN_MARGIN_NS);
    EXPECT_EQ(rig.engine.counts().broken, 0U);
    EXPECT_EQ(rig.tags.point(0, 0).value, 8);
}

TEST(ChannelEngine, OnALineWithNoWindowAProbeWaitsTenOfItsCappedTimeOuts) {
    // 7.4, Q4: polls every 50 ms leave no window worth a probe, so a probe
    // waits the floor of 10 time-outs: of the probe's own time-out, capped
    // at 1 s, not of its device's 3 s.
    CompiledChannel channel =
        channel_of(2, quick_failure(0, 1), TransportKind::SERIAL);
    channel.poll_groups[0].period_ns = 50 * MILLISECOND;
    channel.devices[1].failure.timeout_ns = 3 * SECOND;
    Rig rig(std::move(channel), 2);
    rig.bus.fault(2, Fault::SILENT);
    rig.run_until(4 * SECOND);
    ASSERT_EQ(rig.state(1), DeviceStatus::DEMOTED);
    const std::int64_t due = rig.engine.failures().probe_due_ns(1);
    const std::size_t before = requests_of(rig.bus, 2);
    rig.run_until(due + 10 * SECOND + 100 * MILLISECOND);
    EXPECT_EQ(requests_of(rig.bus, 2), before + 1);
}

TEST(ChannelEngine, ACycleProbeWaitsOneSecondAtMost) {
    // 7.4: the first request of a cycle probe is the probe: it waits at most
    // 1 s, though its device's time-out is 3 s.
    CompiledChannel channel = side_effect_only(TransportKind::SERIAL);
    channel.devices[0].failure.timeout_ns = 3 * SECOND;
    Rig rig(std::move(channel), 1);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(4 * SECOND);
    ASSERT_EQ(rig.state(0), DeviceStatus::DEMOTED);
    const std::int64_t due = rig.engine.failures().probe_due_ns(0);
    rig.run_until(due + 1100 * MILLISECOND);
    // The probe failed after 1 s, and the back-off, now 4 s, runs from
    // then: the next probe about 5 s after this one (7 s with a 3 s wait).
    EXPECT_GE(rig.engine.failures().probe_due_ns(0), due + 5 * SECOND);
    EXPECT_LT(rig.engine.failures().probe_due_ns(0),
              due + 5 * SECOND + 100 * MILLISECOND);
}

// When the point of each device `first` to `end` (exclusive) was first seen
// BAD with COMM_FAIL, running `rig` in 10 ms steps to `until_ns`; 0 when
// never. Indexed by device.
std::vector<std::int64_t> first_comm_fail(Rig& rig, std::uint32_t first,
                                          std::uint32_t end,
                                          std::int64_t until_ns) {
    std::vector<std::int64_t> bad_at(end, 0);
    for (std::int64_t now = rig.clock.monotonic_ns(); now <= until_ns;
         now += 10 * MILLISECOND) {
        rig.run_until(now);
        for (std::uint32_t device = first; device < end; ++device) {
            if (bad_at[device] == 0 &&
                rig.tags.has_flag(device, 0, Flag::COMM_FAIL)) {
                bad_at[device] = now;
            }
        }
    }
    return bad_at;
}

TEST(ChannelEngine, DeadUnitsGoBadWithinTheirVerdictTimeOnABusyLine) {
    // J2: 10 live units that take 25 ms each fill a 200 ms group; 5 others,
    // RUNNING, die together at 1 s (time-out 100 ms, retries 2). Their
    // retries are held back behind the live polls (G5), but each dead
    // unit's point is COMM_FAIL at the latest one period (200 ms, here also
    // the retries' 2 x 100 ms) after its first time-out. Without the rule
    // they are still GOOD at 6 s.
    Rig rig(channel_of(15, quick_failure(2, 3), TransportKind::SERIAL), 15);
    for (std::uint8_t unit = 1; unit <= 10; ++unit) {
        rig.transport.latency(unit, 25 * MILLISECOND);
    }
    rig.run_until(SECOND);
    for (std::uint8_t unit = 11; unit <= 15; ++unit) {
        rig.bus.fault(unit, Fault::SILENT);
    }
    const std::vector<std::int64_t> bad_at =
        first_comm_fail(rig, 10, 15, 6 * SECOND);
    // From its first request after the death: one time-out, then at most
    // the verdict time (200 ms) and the end of the exchange on the line then
    // (25 ms), within the sampling (10 ms, and 10 ms more for the step that
    // ends between two samples).
    for (std::uint32_t device = 10; device < 15; ++device) {
        const std::vector<std::int64_t> sent =
            times_of(rig.bus, static_cast<std::uint8_t>(device + 1));
        const auto first =
            std::find_if(sent.begin(), sent.end(),
                         [](std::int64_t time) { return time >= SECOND; });
        ASSERT_NE(first, sent.end());
        ASSERT_GT(bad_at[device], 0);
        EXPECT_LE(bad_at[device] - *first,
                  100 * MILLISECOND + 200 * MILLISECOND + 25 * MILLISECOND +
                      20 * MILLISECOND);
    }
    EXPECT_GT(rig.engine.counts().early_verdicts, 0U);
}

TEST(ChannelEngine, ADemotedUnitIsFoundAgainOnASaturatedLine) {
    // J3: 30 units that take 40 ms each fill a 1 s group, so a request is
    // always due. Units 26 to 30 are dead; unit 30 returns at 1,800 s: its
    // probe, past its floor, takes one turn in the order of service as a
    // due poll of its group, and finds it. In the contract's order a live
    // unit waits its period and the requests due before it, here at most
    // one 1 s request of each dead unit (D-138; the hold is #147).
    CompiledChannel channel =
        channel_of(30, FailureRules{}, TransportKind::SERIAL, 1);
    channel.line.bit_rate = 9'600;
    Rig rig(std::move(channel), 30);
    for (std::uint8_t unit = 1; unit <= 30; ++unit) {
        rig.transport.latency(unit, 40 * MILLISECOND);
    }
    for (std::uint8_t unit = 26; unit <= 30; ++unit) {
        rig.bus.fault(unit, Fault::SILENT);
    }
    rig.run_until(1'800 * SECOND);
    ASSERT_EQ(rig.state(29), DeviceStatus::DEMOTED);
    rig.bus.fault(30, Fault::NONE);
    rig.run_until(2'200 * SECOND);
    EXPECT_EQ(rig.state(29), DeviceStatus::RUNNING);
    std::int64_t worst = 0;
    for (std::uint8_t unit = 1; unit <= 25; ++unit) {
        worst = std::max(
            worst, longest_gap_after(times_of(rig.bus, unit), 60 * SECOND));
    }
    EXPECT_LE(worst, 6 * SECOND + 300 * MILLISECOND);
}

TEST(ChannelEngine, NoPartOfAChainRestsNotEvenTheLast) {
    // K1, the lead's ruling: chain 0 <- 20 <- 40, the request at 40 (the
    // chain's last) unanswered from 1 s. Nothing of the chain rests: it is
    // read whole every period, 0 keeps its fresh value, 20 goes BAD through
    // its companion, 40 is COMM_FAIL, and the chain never overflows.
    CompiledChannel channel =
        make_channel({{.name = "d1",
                       .unit_id = 1,
                       .points = {{.first = 0, .companions = {1}},
                                  {.first = 20, .companions = {2}},
                                  {.first = 40}}}},
                     ReadRules{.max_units = 4});
    channel.transport = TransportKind::SERIAL;
    channel.devices[0].failure = quick_failure(0, 3);
    quality_from_companions(channel, 0, 1);
    Rig rig(std::move(channel), 1);
    rig.run_until(SECOND);
    rig.bus.silent_from(1, 40, 40);
    rig.run_until(30 * SECOND);
    rig.bus.set(1, 0, 0, 21);
    rig.run_until(60 * SECOND);
    EXPECT_EQ(rig.engine.counts().rests, 0U);
    EXPECT_EQ(rig.engine.counts().chain_overflows, 0U);
    EXPECT_EQ(rig.tags.point(0, 0).value, 21);
    EXPECT_GE(rig.tags.point(0, 0).time_ns,
              SimClock::REALTIME_OFFSET_NS + 59 * SECOND);
    EXPECT_EQ(rig.tags.point(0, 1).quality, tag::Quality::BAD);
    EXPECT_TRUE(rig.tags.has_flag(0, 2, Flag::COMM_FAIL));
}

TEST(ChannelEngine, AFailedPartOfABusyCutChainStillGoesBad) {
    // K4: the chain's first request (at 0) answers busy and its second (at
    // 20) is unanswered: nothing of the chain is published, but the point
    // at 20 goes BAD with COMM_FAIL at the end of the chain.
    CompiledChannel channel = make_channel(
        {{.name = "d1",
          .unit_id = 1,
          .points = {{.first = 0, .companions = {1}}, {.first = 20}}}},
        ReadRules{.max_units = 4});
    channel.transport = TransportKind::SERIAL;
    channel.devices[0].failure = quick_failure(0, 100);
    Rig rig(std::move(channel), 1);
    rig.run_until(SECOND);
    rig.bus.busy_at(1, 0);  // every chain from now on is cut
    rig.run_until(2 * SECOND);
    ASSERT_EQ(rig.tags.point(0, 1).quality, tag::Quality::GOOD);
    rig.bus.silent_from(1, 20, 20);
    // Long before the 100 failures that would demote the device.
    rig.run_until(3 * SECOND);
    EXPECT_NE(rig.state(0), DeviceStatus::DEMOTED);
    EXPECT_TRUE(rig.tags.has_flag(0, 1, Flag::COMM_FAIL));
    EXPECT_EQ(rig.tags.point(0, 1).quality, tag::Quality::BAD);
}

// K2: serial; unit 1 live in the 200 ms group, unit 2 with a 500 ms
// time-out answering after `latency_ns`, silent from 1 s to 30 s. Its state
// at `check_ns`.
DeviceStatus unit_answering_after(std::int64_t latency_ns,
                                  std::int64_t check_ns) {
    FailureRules rules;
    rules.timeout_ns = 500 * MILLISECOND;
    Rig rig(channel_of(2, rules, TransportKind::SERIAL), 2);
    rig.transport.latency(2, latency_ns);
    rig.run_until(SECOND);
    rig.bus.fault(2, Fault::SILENT);
    rig.run_until(30 * SECOND);
    EXPECT_EQ(rig.state(1), DeviceStatus::DEMOTED);
    rig.bus.fault(2, Fault::NONE);
    rig.run_until(check_ns);
    return rig.state(1);
}

TEST(ChannelEngine, AProbeCutShortIsNoFailureForASlowUnit) {
    // K2: answering after 190 ms, longer than any window before the next
    // 200 ms poll: found by the floor probe (10 x 500 ms after its due
    // time); answering after 50 ms, by a windowed probe; both within their
    // probe back-off of the return.
    EXPECT_EQ(unit_answering_after(190 * MILLISECOND, 120 * SECOND),
              DeviceStatus::RUNNING);
    EXPECT_EQ(unit_answering_after(50 * MILLISECOND, 90 * SECOND),
              DeviceStatus::RUNNING);
}

// --- K6: the surviving mutants of the final pass --------------------------

TEST(ChannelEngine, AGatewayAnswerProvesTheConnection) {
    // K6, the revised ruling: the gateway's "did not respond" is a valid
    // answer since the connect, so an own close then (a partial send)
    // reconnects at once, not after the 1 s back-off.
    Rig rig(channel_of(1, quick_failure(0, 100), TransportKind::TCP), 1);
    rig.bus.fault(1, Fault::UNREACHABLE);
    rig.run_until(FAST_PERIOD - MILLISECOND);
    rig.transport.partial_sends(1);
    rig.run_until(FAST_PERIOD + 50 * MILLISECOND);
    ASSERT_EQ(rig.transport.connect_times().size(), 2U);
    EXPECT_LE(rig.transport.connect_times()[1], FAST_PERIOD + MILLISECOND);
}

// A device with a live block at 0 and a dead one at 100, serial, demote_after
// 3, back-off 4 s to 8 s.
Rig& dead_block_rig(std::unique_ptr<Rig>& rig) {
    CompiledChannel channel =
        make_channel({{.name = "d1",
                       .unit_id = 1,
                       .points = {{.first = 0}, {.first = 100}}}},
                     ReadRules{.max_units = 4});
    channel.transport = TransportKind::SERIAL;
    channel.devices[0].failure = quick_failure(0, 3);
    channel.devices[0].failure.backoff_ns = 4 * SECOND;
    channel.devices[0].failure.backoff_max_ns = 8 * SECOND;
    rig = std::make_unique<Rig>(std::move(channel), 1);
    rig->bus.silent_from(1, 100, 100);
    return *rig;
}

TEST(ChannelEngine, AReconnectEndsEveryRest) {
    // K6, G8: the dead block rests from about 400 ms for 4 s; the peer closes
    // at 600 ms and the reconnect, 1 s later, ends the rest in the engine
    // and in the scheduler: the block is read again at its period, 3 times
    // before it rests again.
    std::unique_ptr<Rig> holder;
    Rig& rig = dead_block_rig(holder);
    rig.run_until(600 * MILLISECOND);
    ASSERT_EQ(rig.engine.counts().rests, 1U);
    rig.transport.close_by_peer();
    rig.run_until(1500 * MILLISECOND);
    const std::size_t before = reads_of(rig.bus, 100);
    rig.run_until(3 * SECOND);
    EXPECT_GE(rig.engine.counts().connects, 2U);
    EXPECT_GE(reads_of(rig.bus, 100) - before, 3U);
}

TEST(ChannelEngine, AnAnsweredBlockStartsItsRestOver) {
    // K6, G8: the block rests, answers at the end of its rest, and dies
    // again: it is read 3 times more before it rests, with its back-off
    // back at 4 s.
    std::unique_ptr<Rig> holder;
    Rig& rig = dead_block_rig(holder);
    rig.run_until(SECOND);
    ASSERT_EQ(rig.engine.counts().rests, 1U);
    rig.bus.silent_from(1, 1000, 1000);  // it answers
    rig.run_until(6 * SECOND);
    ASSERT_EQ(rig.tags.point(0, 1).quality, tag::Quality::GOOD);
    rig.bus.silent_from(1, 100, 100);  // and dies again
    const std::size_t before = reads_of(rig.bus, 100);
    rig.run_until(7 * SECOND);
    EXPECT_GE(reads_of(rig.bus, 100) - before, 3U);
    EXPECT_EQ(rig.engine.counts().rests, 2U);
}

TEST(ChannelEngine, ACycleProbeHasNoWindow) {
    // K6, G7: a demoted device with no probe request (side-effect only) and a
    // live one in a 1 s group: the cycle probe has no window, so it goes at
    // its floor, 10 time-outs after its due time.
    ReadRules rules{.max_units = LARGE};
    rules.ranges = {ReadRange{.kind = ReadRange::Kind::SIDE_EFFECT,
                              .unit_id = 1,
                              .first = 0,
                              .last = 0}};
    CompiledChannel channel = make_channel(
        {{.name = "d1", .unit_id = 1, .points = adjacent(0, 1, 1)},
         {.name = "d2", .unit_id = 2, .points = adjacent(0, 1, 1)}},
        rules);
    channel.transport = TransportKind::SERIAL;
    for (CompiledDevice& device : channel.devices) {
        device.failure = quick_failure(0, 1);
        device.failure.timeout_ns = SECOND;
    }
    Rig rig(std::move(channel), 2);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(1500 * MILLISECOND);
    ASSERT_EQ(rig.state(0), DeviceStatus::DEMOTED);
    const std::int64_t due = rig.engine.failures().probe_due_ns(0);
    const std::size_t before = requests_of(rig.bus, 1);
    rig.run_until(due + 10 * SECOND - MILLISECOND);
    EXPECT_EQ(requests_of(rig.bus, 1), before);
    rig.run_until(due + 12 * SECOND);
    EXPECT_EQ(requests_of(rig.bus, 1), before + 1);
}

TEST(ChannelEngine, AQuietLineStartsTheJamTimeOver) {
    // K6, J6: noise for 3 s, a quiet second, noise for 3 s again: neither
    // episode lasts the 5 s of a jam.
    Rig rig(channel_of(1, quick_failure(0, 100), TransportKind::SERIAL), 1);
    rig.bus.fault(1, Fault::GARBAGE);  // every answer starts a quiet time
    rig.transport.noise(200 * MILLISECOND, 3200 * MILLISECOND);
    rig.transport.noise(4200 * MILLISECOND, 7200 * MILLISECOND);
    rig.run_until(8 * SECOND);
    EXPECT_EQ(rig.engine.counts().jams, 0U);
}

TEST(ChannelEngine, APeerCloseJustBeforeTheSendSendsNothing) {
    // 7.3: the peer's close is learnt in the instant between the boundary's
    // check and the send; a send would be accepted after the FIN, so the
    // engine asks again before it writes.
    Rig rig(channel_of(1, quick_failure(0, 5), TransportKind::TCP), 1);
    rig.run_until(FAST_PERIOD - MILLISECOND);
    const std::uint64_t requests = rig.engine.counts().requests;
    // One step at the poll's due time: its boundary check answers false.
    rig.clock.advance_to(FAST_PERIOD);
    rig.transport.close_by_peer_after_checks(1);
    (void)rig.engine.step();
    EXPECT_EQ(rig.engine.counts().requests, requests);
    EXPECT_EQ(rig.engine.counts().losses, 1U);
}

TEST(ChannelEngine, AProbeTimeOutAbove1sIsBoundedTo1sInTheBudget) {
    // 7.4: a probe waits at most 1 s, and the budget reckons with that, not
    // with the device's 3 s time-out. The live unit's 2 s group leaves 1 s
    // of idle time in every period: probes go in it, not only after the
    // floor of 10 time-outs.
    CompiledChannel channel =
        channel_of(2, quick_failure(0, 1), TransportKind::SERIAL, 1);
    channel.poll_groups[1].period_ns = 2 * SECOND;
    channel.devices[1].failure.timeout_ns = 3 * SECOND;
    Rig rig(std::move(channel), 2);
    rig.bus.fault(2, Fault::SILENT);
    rig.run_until(4 * SECOND);
    ASSERT_EQ(rig.state(1), DeviceStatus::DEMOTED);
    const std::size_t before = requests_of(rig.bus, 2);
    rig.run_until(20 * SECOND);
    EXPECT_GE(requests_of(rig.bus, 2), before + 3);
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

}  // namespace
}  // namespace scada::driver::test
