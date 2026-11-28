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
    // One failed read (after its retries) per cycle, a cycle per slow
    // period: the third at 20 s demotes it.
    rig.run_until(19 * SECOND);
    EXPECT_EQ(rig.state(), DeviceStatus::INITIALISING);
    rig.run_until(21 * SECOND);
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

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

}  // namespace
}  // namespace scada::driver::test
