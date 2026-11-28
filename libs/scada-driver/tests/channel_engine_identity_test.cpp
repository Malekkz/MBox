// The channel engine's identity check and restart rules over the loopback
// (driver interface contract 3.6, 7.6; decision D-147): the verdict of each
// outcome, the deadline, busy reads, MISMATCH with `refuse` and `warn`, the
// re-check, and a restart.

#include "channel_builder.h"
#include "loopback.h"
#include "scada/driver/channel_engine.h"
#include "scada/driver/compiled.h"
#include "scada/driver/failure.h"
#include "scada/driver/identity.h"
#include "scada/driver/identity_run.h"
#include "scada/driver/plan.h"
#include "scada/driver/point_rules.h"
#include "scada/tag/types.h"
#include "sim_transport.h"
#include "test_protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::driver::test {
namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): the addresses, values and times are the test data

constexpr std::int64_t LATENCY = MILLISECOND;
constexpr std::uint32_t FAST = 0;
constexpr std::uint32_t SLOW = 2;
constexpr std::uint32_t DATA = 0;       // register of the data point, fast
constexpr std::uint32_t IDENTITY = 10;  // register of the identity point
constexpr std::uint16_t MODEL = 7;
using Flag = tag::QualityFlag;

FailureRules rules() {
    return FailureRules{.timeout_ns = 100 * MILLISECOND,
                        .write_timeout_ns = 100 * MILLISECOND,
                        .retries = 2,
                        .demote_after = 3,
                        .backoff_ns = SECOND,
                        .backoff_max_ns = 4 * SECOND};
}

// d1 on unit 1: point 0 at DATA in the fast group, point 1 at `identity`
// in the slow group (in no group when `groupless`), checked to equal MODEL.
CompiledChannel identity_channel(std::uint32_t identity = IDENTITY,
                                 bool groupless = false) {
    const std::vector<PointSpec> points{
        PointSpec{.first = DATA, .group = FAST},
        PointSpec{.first = identity,
                  .group = groupless ? std::nullopt
                                     : std::optional<std::uint32_t>{SLOW}}};
    const DeviceSpec spec{.name = "d1", .unit_id = 1, .points = points};
    // The checks go in before the plan: the plan step reads them.
    CompiledChannel channel;
    channel.name = "line1";
    channel.poll_groups = default_groups();
    channel.transport = TransportKind::SERIAL;
    channel.connection.reconnect_min_ns = SECOND;
    channel.connection.reconnect_max_ns = 8 * SECOND;
    CompiledDevice device = make_device(spec);
    device.failure = rules();
    IdentityCheck check;
    check.point = 1;
    check.kind = IdentityCheck::Kind::EQUALS;
    check.values = {ExpectedValue{
        .type = device.points[1].rules.tag_type, .i = MODEL, .u = MODEL}};
    device.identify = {check};
    device.plan.by_group.resize(channel.poll_groups.size());
    const TestProtocol protocol;
    EXPECT_TRUE(
        protocol.plan(device, ReadRules{.max_units = 4}, device.plan).ok());
    channel.devices.push_back(std::move(device));
    return channel;
}

SimBus identity_bus(std::uint16_t model = MODEL) {
    SimBus bus;
    bus.map(1, 0, 0, 199);
    bus.set(1, 0, IDENTITY, model);
    bus.set(1, 0, DATA, 5);
    return bus;
}

// NOLINTBEGIN(misc-non-private-member-variables-in-classes): a test rig whose parts are its interface
struct Rig {
    Rig(CompiledChannel compiled, SimBus simulated)
        : channel(std::move(compiled)),
          bus(std::move(simulated)),
          transport(bus, clock, LATENCY),
          engine(channel, protocol, transport, clock, tags) {}

    void run_until(std::int64_t end_ns) {
        (void)run_engine(engine, clock, end_ns);
    }
    [[nodiscard]] DeviceStatus state() const {
        return engine.failures().state(0);
    }
    // Requests that read register `first`.
    [[nodiscard]] std::size_t reads_of(std::uint32_t first) const {
        return static_cast<std::size_t>(
            std::count_if(bus.log().begin(), bus.log().end(),
                          [&](const LoggedRequest& request) {
                              return request.first == first;
                          }));
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

TEST(EngineIdentity, AMatchRunsTheDeviceAndPublishesItsPoints) {
    Rig rig(identity_channel(), identity_bus());
    ASSERT_TRUE(rig.engine.status().ok());
    rig.run_until(2 * SECOND);
    EXPECT_EQ(rig.state(), DeviceStatus::RUNNING);
    EXPECT_EQ(rig.engine.identity_ok(0), std::optional<bool>{true});
    EXPECT_EQ(rig.engine.identity_text(0), "7");
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
    EXPECT_EQ(rig.tags.point(0, 0).value, 5);
    // The identity read came first, the data after it.
    ASSERT_FALSE(rig.bus.log().empty());
    EXPECT_EQ(rig.bus.log().front().first, IDENTITY);
}

TEST(EngineIdentity, AMismatchWithRefuseIsMismatchAndReadsOnlyTheCycle) {
    Rig rig(identity_channel(), identity_bus(8));
    rig.run_until(25 * SECOND);
    EXPECT_EQ(rig.state(), DeviceStatus::MISMATCH);
    EXPECT_EQ(rig.engine.identity_ok(0), std::optional<bool>{false});
    EXPECT_EQ(rig.engine.identity_text(0), "8");
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::BAD);
    EXPECT_FALSE(rig.tags.has_flag(0, 0, Flag::COMM_FAIL));
    EXPECT_EQ(rig.reads_of(DATA), 0U);
    // Re-checked at the back-off (1 s), but no sooner than one identity
    // period (the slow group, 10 s) after the last cycle started (PR 171
    // M3): at 0, 10 and 20 s.
    EXPECT_EQ(rig.reads_of(IDENTITY), 3U);
    // The device is fixed: the next re-check passes, the polls start.
    rig.bus.set(1, 0, IDENTITY, MODEL);
    rig.run_until(32 * SECOND);
    EXPECT_EQ(rig.state(), DeviceStatus::RUNNING);
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
}

TEST(EngineIdentity, AMismatchWithWarnRunsTheDevice) {
    CompiledChannel channel = identity_channel();
    channel.devices[0].refuse_on_mismatch = false;
    Rig rig(std::move(channel), identity_bus(8));
    rig.run_until(2 * SECOND);
    EXPECT_EQ(rig.state(), DeviceStatus::RUNNING);
    EXPECT_EQ(rig.engine.identity_ok(0), std::optional<bool>{false});
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
}

TEST(EngineIdentity, AnAbsentAddressIsAMismatchAtOnce) {
    // Register 300 is not mapped: an illegal address, ABSENT (ruling 3).
    Rig rig(identity_channel(300, true), identity_bus());
    rig.run_until(500 * MILLISECOND);
    EXPECT_EQ(rig.state(), DeviceStatus::MISMATCH);
    EXPECT_EQ(rig.reads_of(DATA), 0U);
}

TEST(EngineIdentity, AnUnreadableValueIsAMismatchAtItsDeadlineOnly) {
    // The marker makes 0xFFFF BAD: unreadable. The deadline is 3 periods of
    // the slow group, 30 s (ruling item 2).
    CompiledChannel channel = identity_channel();
    channel.devices[0].points[1].rules.markers.push_back(
        InvalidMarker{.pattern = 0xFFFF, .action = InvalidAction::BAD});
    Rig rig(std::move(channel), identity_bus(0xFFFF));
    rig.run_until(29 * SECOND);
    EXPECT_EQ(rig.state(), DeviceStatus::INITIALISING);
    EXPECT_EQ(rig.reads_of(DATA), 0U);
    rig.run_until(31 * SECOND);
    EXPECT_EQ(rig.state(), DeviceStatus::MISMATCH);
}

TEST(EngineIdentity, ABusyIdentityReadIsReadAgainWithinItsRetries) {
    // Busy for ever: each cycle reads 1 + 2 retries, and the next cycle
    // comes a slow period (10 s) after the last started; no verdict.
    Rig rig(identity_channel(), identity_bus());
    rig.bus.busy_at(1, IDENTITY);
    rig.run_until(25 * SECOND);
    EXPECT_EQ(rig.state(), DeviceStatus::INITIALISING);
    EXPECT_EQ(rig.reads_of(IDENTITY), 9U);
    EXPECT_EQ(rig.reads_of(DATA), 0U);
    EXPECT_FALSE(rig.engine.identity_ok(0).has_value());
}

TEST(EngineIdentity, ARestartChecksTheDeviceAgain) {
    // `decreases` on the data point: 5, then 3, is a restart.
    CompiledChannel channel = identity_channel();
    channel.devices[0].restart_indication = {
        RestartRule{.point = 0, .kind = RestartRule::Kind::DECREASES}};
    Rig rig(std::move(channel), identity_bus());
    rig.run_until(2 * SECOND);
    ASSERT_EQ(rig.state(), DeviceStatus::RUNNING);
    const std::size_t checks = rig.reads_of(IDENTITY);
    rig.bus.set(1, 0, DATA, 3);
    // Checked again a slow period after the last check, at 10 s.
    rig.run_until(9 * SECOND);
    EXPECT_EQ(rig.state(), DeviceStatus::INITIALISING);
    EXPECT_EQ(rig.reads_of(IDENTITY), checks);
    rig.run_until(12 * SECOND);
    EXPECT_EQ(rig.engine.restarts(0), 1U);
    EXPECT_GT(rig.reads_of(IDENTITY), checks);
    EXPECT_EQ(rig.state(), DeviceStatus::RUNNING);
    // The 9.1 writes between: NOT_INIT once, then the value again.
    EXPECT_EQ(rig.tags.point(0, 0).value, 3);
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
}

// The index in the bus log of the first read of register `first` from
// `from` on; the log's size when there is none.
std::size_t first_read(const SimBus& bus, std::uint32_t first,
                       std::size_t from) {
    for (std::size_t index = from; index < bus.log().size(); ++index) {
        if (bus.log()[index].first == first) {
            return index;
        }
    }
    return bus.log().size();
}

TEST(EngineIdentity, ARecoveredDeviceIsCheckedBeforeItIsPolled) {
    // Dead from the start: its identity reads fail and count, so it is
    // DEMOTED, never MISMATCH. Back again, a probe recovers it, and the
    // check is read before any poll of the data.
    Rig rig(identity_channel(), identity_bus());
    rig.bus.fault(1, SimBus::Fault::SILENT);
    // One failed read (after its retries) per cycle. A cycle that nothing
    // answered waits the fastest group's period only (200 ms), not the
    // identity period (10 s; the lead's ruling on 5b-2b): DEMOTED within
    // demote_after periods of 200 ms plus the time-outs of 3 attempts of
    // 100 ms each, 1.5 s.
    rig.run_until(1'500 * MILLISECOND);
    ASSERT_EQ(rig.state(), DeviceStatus::DEMOTED);
    rig.bus.fault(1, SimBus::Fault::NONE);
    const std::size_t before = rig.bus.log().size();
    rig.run_until(50 * SECOND);
    EXPECT_EQ(rig.state(), DeviceStatus::RUNNING);
    EXPECT_EQ(rig.engine.identity_ok(0), std::optional<bool>{true});
    // After the probe, the identity read comes before the first data read.
    EXPECT_LT(first_read(rig.bus, IDENTITY, before + 1),
              first_read(rig.bus, DATA, before + 1));
}

TEST(EngineIdentity, AnIdentityPointInNoGroupIsNeverPublished) {
    // A check on a protocol address: read by the identity cycle only, its
    // value goes to the check, never to a tag.
    Rig rig(identity_channel(IDENTITY, true), identity_bus());
    rig.run_until(2 * SECOND);
    EXPECT_EQ(rig.state(), DeviceStatus::RUNNING);
    EXPECT_EQ(rig.engine.identity_text(0), "7");
    EXPECT_NE(rig.tags.point(0, 1).quality, tag::Quality::GOOD);
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
}

TEST(EngineIdentity, AMalformedCheckOrRuleRefusesTheChannel) {
    // The engine checks the keys again (D-147, S3): a check whose value has
    // another type than its point, and a `decreases` rule on a STRING.
    CompiledChannel wrong_type = identity_channel();
    wrong_type.devices[0].identify[0].values[0].type = tag::TagType::BOOL;
    const Rig check(std::move(wrong_type), identity_bus());
    EXPECT_FALSE(check.engine.status().ok());
    CompiledChannel on_string = identity_channel();
    on_string.devices[0].points[0].rules.tag_type = tag::TagType::STRING;
    on_string.devices[0].restart_indication = {
        RestartRule{.point = 0, .kind = RestartRule::Kind::DECREASES}};
    const Rig rule(std::move(on_string), identity_bus());
    EXPECT_FALSE(rule.engine.status().ok());
}

TEST(EngineIdentity, EachEventIsNotedForTheLog) {
    // A mismatch with its reason, check and value; the match that ends it;
    // a restart. Each is taken once.
    CompiledChannel channel = identity_channel();
    channel.devices[0].restart_indication = {
        RestartRule{.point = 0, .kind = RestartRule::Kind::DECREASES}};
    Rig rig(std::move(channel), identity_bus(8));
    rig.run_until(SECOND);
    EventRecord record;
    ASSERT_TRUE(rig.engine.take_event(0, DeviceEvent::MISMATCH, record));
    EXPECT_EQ(record.count, 1U);
    EXPECT_EQ(record.reason, static_cast<std::uint8_t>(MismatchReason::VALUE));
    EXPECT_EQ(record.check, 0U);
    EXPECT_EQ(std::string_view(record.text.data(), record.length), "8");
    EXPECT_FALSE(rig.engine.take_event(0, DeviceEvent::MISMATCH, record));
    EXPECT_FALSE(rig.engine.take_event(0, DeviceEvent::MATCH, record));
    rig.bus.set(1, 0, IDENTITY, MODEL);
    rig.run_until(12 * SECOND);
    ASSERT_EQ(rig.state(), DeviceStatus::RUNNING);
    ASSERT_TRUE(rig.engine.take_event(0, DeviceEvent::MATCH, record));
    EXPECT_EQ(std::string_view(record.text.data(), record.length), "7");
    rig.bus.set(1, 0, DATA, 3);
    rig.run_until(13 * SECOND);
    ASSERT_TRUE(rig.engine.take_event(0, DeviceEvent::RESTART, record));
    EXPECT_EQ(record.count, 1U);
    EXPECT_FALSE(rig.engine.take_event(5, DeviceEvent::RESTART, record));
}

// PR 177 M1: d1 as identity_channel, with a third point at `restart` and a
// `decreases` rule on it. At register 1, in the fast group, it shares the
// data point's request, first in the identity cycle; at 50, in the slow
// group after the identity point, it has a request of its own, last in the
// cycle.
constexpr std::uint32_t RESTART_FIRST = 1;
constexpr std::uint32_t RESTART_LAST = 50;

CompiledChannel restart_channel(std::uint32_t restart) {
    const std::vector<PointSpec> points{
        PointSpec{.first = DATA, .group = FAST},
        PointSpec{.first = IDENTITY, .group = SLOW},
        PointSpec{.first = restart,
                  .group = restart == RESTART_LAST ? SLOW : FAST}};
    const DeviceSpec spec{.name = "d1", .unit_id = 1, .points = points};
    CompiledChannel channel;
    channel.name = "line1";
    channel.poll_groups = default_groups();
    channel.transport = TransportKind::SERIAL;
    channel.connection.reconnect_min_ns = SECOND;
    channel.connection.reconnect_max_ns = 8 * SECOND;
    CompiledDevice device = make_device(spec);
    device.failure = rules();
    IdentityCheck check;
    check.point = 1;
    check.kind = IdentityCheck::Kind::EQUALS;
    check.values = {ExpectedValue{
        .type = device.points[1].rules.tag_type, .i = MODEL, .u = MODEL}};
    device.identify = {check};
    device.restart_indication = {
        RestartRule{.point = 2, .kind = RestartRule::Kind::DECREASES}};
    device.plan.by_group.resize(channel.poll_groups.size());
    const TestProtocol protocol;
    EXPECT_TRUE(
        protocol.plan(device, ReadRules{.max_units = 4}, device.plan).ok());
    channel.devices.push_back(std::move(device));
    return channel;
}

SimBus restart_bus(std::uint32_t restart, std::uint16_t model) {
    SimBus bus = identity_bus(model);
    bus.set(1, 0, restart, 5);
    return bus;
}

// The cycle order the tests rely on: the restart point's request before
// or after the identity point's.
TEST(EngineIdentity, TheRestartPointIsFirstOrLastInTheCycle) {
    for (const std::uint32_t restart : {RESTART_FIRST, RESTART_LAST}) {
        Rig rig(restart_channel(restart), restart_bus(restart, MODEL));
        rig.run_until(100 * MILLISECOND);
        ASSERT_GE(rig.bus.log().size(), 2U);
        const bool first =
            rig.bus.log()[0].first <= restart && restart < IDENTITY;
        EXPECT_EQ(first, restart == RESTART_FIRST);
        EXPECT_EQ(rig.bus.log()[restart == RESTART_FIRST ? 1 : 0].first,
                  IDENTITY);
    }
}

// A restart seen in an identity cycle while the check is pending: the
// device is read again and reaches its verdict (PR 177 M1).
TEST(EngineIdentity, ARestartInAPendingCycleIsReadAgain) {
    for (const std::uint32_t restart : {RESTART_FIRST, RESTART_LAST}) {
        SCOPED_TRACE(restart);
        // Unreadable at first: the check stays pending (deadline 30 s).
        CompiledChannel channel = restart_channel(restart);
        channel.devices[0].points[1].rules.markers.push_back(
            InvalidMarker{.pattern = 0xFFFF, .action = InvalidAction::BAD});
        Rig rig(std::move(channel), restart_bus(restart, 0xFFFF));
        rig.run_until(SECOND);
        ASSERT_EQ(rig.state(), DeviceStatus::INITIALISING);
        // Before the next cycle (10 s): the model readable, and the
        // restart point lower, which that cycle sees.
        rig.bus.set(1, 0, IDENTITY, MODEL);
        rig.bus.set(1, 0, restart, 3);
        rig.run_until(15 * SECOND);
        EXPECT_EQ(rig.engine.restarts(0), 1U);
        EXPECT_EQ(rig.state(), DeviceStatus::RUNNING);
        EXPECT_GT(rig.reads_of(DATA), 0U);
    }
}

TEST(EngineIdentity, ARestartInAMismatchReCheckIsReadAgain) {
    for (const std::uint32_t restart : {RESTART_FIRST, RESTART_LAST}) {
        SCOPED_TRACE(restart);
        Rig rig(restart_channel(restart), restart_bus(restart, 8));
        rig.run_until(SECOND);
        ASSERT_EQ(rig.state(), DeviceStatus::MISMATCH);
        // The re-check at 10 s finds the right model and a restart.
        rig.bus.set(1, 0, IDENTITY, MODEL);
        rig.bus.set(1, 0, restart, 3);
        rig.run_until(15 * SECOND);
        EXPECT_EQ(rig.engine.restarts(0), 1U);
        EXPECT_EQ(rig.state(), DeviceStatus::RUNNING);
    }
}

// The probes' case: a device lost, back with its restart point lower; the
// check after the recovery sees the restart and must still end.
TEST(EngineIdentity, ARestartSeenAfterALostDeviceIsReadAgain) {
    for (const std::uint32_t restart : {RESTART_FIRST, RESTART_LAST}) {
        SCOPED_TRACE(restart);
        Rig rig(restart_channel(restart), restart_bus(restart, MODEL));
        rig.run_until(SECOND);
        ASSERT_EQ(rig.state(), DeviceStatus::RUNNING);
        rig.bus.fault(1, SimBus::Fault::SILENT);
        rig.run_until(4 * SECOND);
        ASSERT_EQ(rig.state(), DeviceStatus::DEMOTED);
        rig.bus.set(1, 0, restart, 3);
        rig.bus.fault(1, SimBus::Fault::NONE);
        rig.run_until(40 * SECOND);
        EXPECT_EQ(rig.engine.restarts(0), 1U);
        EXPECT_EQ(rig.state(), DeviceStatus::RUNNING);
        // Read up to the end, not stuck after the restart.
        ASSERT_FALSE(rig.bus.log().empty());
        EXPECT_GT(rig.bus.log().back().time_ns, 39 * SECOND);
    }
}

// PR 177 M2: a restart point in one block with another point. The answer
// that shows the restart writes no value: every write after it, until the
// device is checked again, is BAD with NOT_INIT.
TEST(EngineIdentity, NoValueOfARestartAnswerIsPublished) {
    Rig rig(restart_channel(RESTART_FIRST), restart_bus(RESTART_FIRST, MODEL));
    rig.run_until(SECOND);
    ASSERT_EQ(rig.state(), DeviceStatus::RUNNING);
    ASSERT_EQ(rig.tags.point(0, 0).value, 5);
    const std::size_t before = rig.tags.history().size();
    // One answer: the data point 6 and the restart point 3.
    rig.bus.set(1, 0, DATA, 6);
    rig.bus.set(1, 0, RESTART_FIRST, 3);
    rig.run_until(1'300 * MILLISECOND);
    ASSERT_EQ(rig.engine.restarts(0), 1U);
    ASSERT_EQ(rig.state(), DeviceStatus::INITIALISING);
    const std::vector<RecordingTags::Logged>& history = rig.tags.history();
    ASSERT_GT(history.size(), before);
    for (std::size_t index = before; index < history.size(); ++index) {
        const RecordingTags::Logged& write = history[index];
        EXPECT_EQ(write.written.quality, tag::Quality::BAD) << index;
        EXPECT_NE(
            write.written.flags & static_cast<std::uint16_t>(Flag::NOT_INIT), 0)
            << index;
        EXPECT_EQ(write.written.time_ns, 0) << index;
    }
    // Checked again at 10 s: the values return.
    rig.run_until(11 * SECOND);
    EXPECT_EQ(rig.state(), DeviceStatus::RUNNING);
    EXPECT_EQ(rig.tags.point(0, 0).value, 6);
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
}

// Every rule sees the whole answer (M2): two rules fire in one answer, and
// the next answer, the same values, fires none.
TEST(EngineIdentity, EveryRestartRuleSeesTheWholeAnswer) {
    CompiledChannel channel = restart_channel(RESTART_FIRST);
    channel.devices[0].restart_indication.push_back(
        RestartRule{.point = 0, .kind = RestartRule::Kind::DECREASES});
    Rig rig(std::move(channel), restart_bus(RESTART_FIRST, MODEL));
    rig.run_until(SECOND);
    ASSERT_EQ(rig.state(), DeviceStatus::RUNNING);
    rig.bus.set(1, 0, DATA, 2);
    rig.bus.set(1, 0, RESTART_FIRST, 3);
    rig.run_until(25 * SECOND);
    EXPECT_EQ(rig.engine.restarts(0), 1U);
    EXPECT_EQ(rig.state(), DeviceStatus::RUNNING);
}

// The same in an identity cycle: both rules' points are in its first
// request, and both keep their baselines when they fire together.
TEST(EngineIdentity, EveryRestartRuleSeesTheWholeIdentityAnswer) {
    CompiledChannel channel = restart_channel(RESTART_FIRST);
    channel.devices[0].restart_indication.push_back(
        RestartRule{.point = 0, .kind = RestartRule::Kind::DECREASES});
    channel.devices[0].points[1].rules.markers.push_back(
        InvalidMarker{.pattern = 0xFFFF, .action = InvalidAction::BAD});
    Rig rig(std::move(channel), restart_bus(RESTART_FIRST, 0xFFFF));
    rig.run_until(SECOND);
    ASSERT_EQ(rig.state(), DeviceStatus::INITIALISING);
    // Both lower while the check is pending: the next cycle (every 200 ms,
    // the fast group's period) sees both; the model readable after that.
    rig.bus.set(1, 0, DATA, 2);
    rig.bus.set(1, 0, RESTART_FIRST, 3);
    rig.run_until(1'500 * MILLISECOND);
    ASSERT_EQ(rig.engine.restarts(0), 1U);
    rig.bus.set(1, 0, IDENTITY, MODEL);
    rig.run_until(25 * SECOND);
    EXPECT_EQ(rig.engine.restarts(0), 1U);
    EXPECT_EQ(rig.state(), DeviceStatus::RUNNING);
}

// PR 177 M3: the guards that keep a possibly wrong device's values off its
// point tags.

// Whether any write of point `point` of d1 from history index `from` on was
// GOOD.
bool written_good(const RecordingTags& tags, std::uint32_t point,
                  std::size_t from = 0) {
    const std::vector<RecordingTags::Logged>& history = tags.history();
    return std::any_of(history.begin() + static_cast<std::ptrdiff_t>(from),
                       history.end(), [&](const RecordingTags::Logged& write) {
                           return write.device == 0 && write.point == point &&
                                  write.written.quality == tag::Quality::GOOD;
                       });
}

bool written_with(const RecordingTags& tags, std::uint32_t point, Flag flag,
                  std::size_t from = 0) {
    const std::vector<RecordingTags::Logged>& history = tags.history();
    return std::any_of(history.begin() + static_cast<std::ptrdiff_t>(from),
                       history.end(), [&](const RecordingTags::Logged& write) {
                           return write.device == 0 && write.point == point &&
                                  (write.written.flags &
                                   static_cast<std::uint16_t>(flag)) != 0;
                       });
}

TEST(EngineIdentity, AKeepAlivesSamplesNeverReachThePointTags) {
    // In MISMATCH the keep-alive (1 s) is the identity request (S8); its
    // answer, the wrong model, goes to the check, never to the identity
    // point's own tag.
    CompiledChannel channel = identity_channel();
    channel.devices[0].keepalive_ns = SECOND;
    Rig rig(std::move(channel), identity_bus(8));
    rig.run_until(9 * SECOND);
    ASSERT_EQ(rig.state(), DeviceStatus::MISMATCH);
    EXPECT_GT(rig.reads_of(IDENTITY), 3U);  // the keep-alives
    EXPECT_FALSE(written_good(rig.tags, 1));
    EXPECT_FALSE(written_good(rig.tags, 0));
}

TEST(EngineIdentity, AKeepAliveFailureArmsNoVerdict) {
    // A keep-alive that times out is an identity read: it counts toward
    // demotion, and no early verdict writes COMM_FAIL (one failure, below
    // demote_after).
    CompiledChannel channel = identity_channel();
    channel.devices[0].keepalive_ns = SECOND;
    Rig rig(std::move(channel), identity_bus(8));
    rig.run_until(900 * MILLISECOND);
    ASSERT_EQ(rig.state(), DeviceStatus::MISMATCH);
    rig.bus.fault(1, SimBus::Fault::SILENT);
    rig.run_until(1'400 * MILLISECOND);
    rig.bus.fault(1, SimBus::Fault::NONE);
    // A verdict would wait one period of the request's group (slow, 10 s).
    rig.run_until(15 * SECOND);
    EXPECT_GT(rig.engine.counts().timeouts, 0U);
    EXPECT_EQ(rig.engine.counts().early_verdicts, 0U);
    EXPECT_FALSE(written_with(rig.tags, 0, Flag::COMM_FAIL));
    EXPECT_EQ(rig.state(), DeviceStatus::MISMATCH);
}

TEST(EngineIdentity, AnIdentityProbesAnswerIsNeverPublished) {
    // No probe request: the identity cycle probes (S4). The identity point
    // is in no group, so no write of it may ever be GOOD. The recovered
    // device is checked and runs: before the fix the probe left the cycle
    // read and nothing asked for the next, so it stayed INITIALISING.
    CompiledChannel channel = identity_channel(IDENTITY, true);
    channel.devices[0].plan.probe.reset();
    Rig rig(std::move(channel), identity_bus());
    rig.bus.fault(1, SimBus::Fault::SILENT);
    rig.run_until(5 * SECOND);
    ASSERT_EQ(rig.state(), DeviceStatus::DEMOTED);
    const std::uint64_t probes = rig.engine.counts().requests;
    rig.bus.fault(1, SimBus::Fault::NONE);
    rig.run_until(30 * SECOND);
    ASSERT_EQ(rig.state(), DeviceStatus::RUNNING);
    EXPECT_GT(rig.engine.counts().requests, probes);
    EXPECT_FALSE(written_good(rig.tags, 1));
}

// The hot swap: a running device with GOOD values is unplugged and another
// model answers on the new connection. No GOOD value after the swap, and
// entering MISMATCH writes every point BAD, without COMM_FAIL, the value
// kept.
TEST(EngineIdentity, AHotSwappedDeviceIsWithheldInMismatch) {
    CompiledChannel channel = identity_channel();
    channel.transport = TransportKind::TCP;
    Rig rig(std::move(channel), identity_bus());
    rig.run_until(SECOND);
    ASSERT_EQ(rig.state(), DeviceStatus::RUNNING);
    ASSERT_EQ(rig.tags.point(0, 0).quality, tag::Quality::GOOD);
    const std::size_t before = rig.tags.history().size();
    rig.bus.set(1, 0, IDENTITY, 8);
    rig.bus.set(1, 0, DATA, 9);
    rig.transport.close_by_peer();
    // Reconnected after 1 s; the check is due one identity period (10 s)
    // after the last cycle started (PR 171 M3).
    rig.run_until(12 * SECOND);
    ASSERT_EQ(rig.state(), DeviceStatus::MISMATCH);
    EXPECT_GT(rig.transport.connects(), 1);
    EXPECT_FALSE(written_good(rig.tags, 0, before));
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::BAD);
    EXPECT_FALSE(rig.tags.has_flag(0, 0, Flag::COMM_FAIL));
    EXPECT_FALSE(rig.tags.has_flag(0, 0, Flag::NOT_INIT));
    EXPECT_EQ(rig.tags.point(0, 0).value, 5);  // the old value, kept
}

// After a loss (COMM_FAIL written), a MISMATCH clears COMM_FAIL, and a
// later loss writes it again.
TEST(EngineIdentity, AMismatchClearsCommFailAndALaterLossWritesItAgain) {
    Rig rig(identity_channel(), identity_bus());
    rig.run_until(SECOND);
    ASSERT_EQ(rig.state(), DeviceStatus::RUNNING);
    rig.bus.fault(1, SimBus::Fault::SILENT);
    rig.run_until(3 * SECOND);
    ASSERT_EQ(rig.state(), DeviceStatus::DEMOTED);
    ASSERT_TRUE(rig.tags.has_flag(0, 0, Flag::COMM_FAIL));
    rig.bus.set(1, 0, IDENTITY, 8);
    rig.bus.fault(1, SimBus::Fault::NONE);
    rig.run_until(15 * SECOND);
    ASSERT_EQ(rig.state(), DeviceStatus::MISMATCH);
    EXPECT_EQ(rig.tags.point(0, 0).quality, tag::Quality::BAD);
    EXPECT_FALSE(rig.tags.has_flag(0, 0, Flag::COMM_FAIL));
    rig.bus.fault(1, SimBus::Fault::SILENT);
    rig.run_until(60 * SECOND);
    ASSERT_EQ(rig.state(), DeviceStatus::DEMOTED);
    EXPECT_TRUE(rig.tags.has_flag(0, 0, Flag::COMM_FAIL));
}

TEST(EngineIdentity, NoStalenessIsWrittenInMismatch) {
    // `max_age` 1 s on the data point: in MISMATCH nothing confirms it, but
    // no point tag is written while the device reads its cycle only.
    CompiledChannel channel = identity_channel();
    channel.devices[0].points[0].rules.max_age_ns = SECOND;
    Rig rig(std::move(channel), identity_bus(8));
    rig.run_until(500 * MILLISECOND);
    ASSERT_EQ(rig.state(), DeviceStatus::MISMATCH);
    const std::size_t before = rig.tags.history().size();
    rig.run_until(5 * SECOND);
    EXPECT_FALSE(written_with(rig.tags, 0, Flag::STALE, before));
    EXPECT_EQ(rig.tags.history().size(), before);
}

TEST(EngineIdentity, NoStalenessIsWrittenWhileTheCheckIsPending) {
    CompiledChannel channel = identity_channel();
    channel.devices[0].points[0].rules.max_age_ns = SECOND;
    channel.devices[0].points[1].rules.markers.push_back(
        InvalidMarker{.pattern = 0xFFFF, .action = InvalidAction::BAD});
    Rig rig(std::move(channel), identity_bus(0xFFFF));
    rig.run_until(5 * SECOND);
    ASSERT_EQ(rig.state(), DeviceStatus::INITIALISING);
    EXPECT_FALSE(written_with(rig.tags, 0, Flag::STALE));
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

}  // namespace
}  // namespace scada::driver::test
