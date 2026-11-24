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
#include "scada/driver/timing.h"
#include "scada/tag/types.h"
#include "sim_transport.h"
#include "test_protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace scada::driver::test {
namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): the addresses, counts and times are the test data, each worked out by hand beside the rule it checks

constexpr std::int64_t LATENCY = MILLISECOND;
constexpr std::uint64_t LARGE = 64;
constexpr std::uint32_t FAST = 0;
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

TEST(ChannelEngine, AGatewayWithADeadBusLosesTheConnection) {
    // The gateway answers "did not respond" for every unit: each counts as
    // a time-out for the run (D-12), which loses the connection; at 150 s no
    // tag is GOOD.
    Rig rig(channel_of(3, quick_failure(2, 5), TransportKind::TCP), 3);
    for (std::uint8_t unit = 1; unit <= 3; ++unit) {
        rig.bus.fault(unit, Fault::UNREACHABLE);
    }
    rig.run_until(150 * SECOND);
    EXPECT_GT(rig.engine.counts().losses, 0U);
    EXPECT_TRUE(all_comm_failed(rig, 3));
    EXPECT_EQ(rig.engine.counts().retries, 0U);  // never retried
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
    // between unit 1's first attempt and its retry.
    Rig rig(channel_of(2, quick_failure(2, 5), TransportKind::TCP), 2);
    rig.bus.fault(1, Fault::SILENT);
    rig.run_until(FAST_PERIOD - MILLISECOND);
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

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

}  // namespace
}  // namespace scada::driver::test
