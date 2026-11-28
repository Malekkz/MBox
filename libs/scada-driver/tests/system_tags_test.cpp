// The system tags of a channel and its devices on the fake clock (driver
// interface contract 11.1, conformance test 34; decision D-146; PR 159
// review M1 to M3, S1): states at once, counters, times and averages at
// most once a second and only when they changed, their last values at the
// stop, and the states the stop leaves.

#include "../src/system_tags.h"

#include "channel_builder.h"
#include "loopback.h"
#include "scada/driver/channel_engine.h"
#include "scada/driver/compiled.h"
#include "scada/driver/diagnostics.h"
#include "scada/driver/failure.h"
#include "scada/driver/identity.h"
#include "scada/driver/plan.h"
#include "scada/tag/scada_tag.h"
#include "sim_transport.h"
#include "test_protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace scada::driver::test {
namespace {

using detail::CHANNEL_SIGNALS;
using detail::ChannelSignal;
using detail::DEVICE_SIGNALS;
using detail::DeviceSignal;
using detail::ProcessSignal;
using detail::ProcessTags;
using detail::SystemTagHandles;
using detail::SystemTags;

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): the times and counts are the test data

constexpr std::int64_t LATENCY = MILLISECOND;
// 11.1: at most once a second; not PERIOD_NS, so that a shorter period
// fails the tests.
constexpr std::int64_t PERIOD = SECOND;

// One write a SystemTags made: the slot (its handle is its index) and the
// value's bits, at the fake clock's time.
struct Recorded {
    std::uint32_t slot = 0;
    std::uint64_t bits = 0;
    std::int64_t at_ns = 0;
};

// What the recording write saw; a test resets it. The write function is a
// plain function pointer, so it reaches its record through this.
struct Recorder {
    std::vector<Recorded> writes;
    std::int64_t now_ns = 0;
    std::uint32_t refuse = SCADA_INVALID_HANDLE;  // a handle it refuses
    // The STRING writes: the slot and the text.
    std::vector<std::pair<std::uint32_t, std::string>> texts;
};

Recorder& recorder() {
    static Recorder instance;
    return instance;
}

int record(scada_ctx* /*ctx*/, std::uint32_t handle, const scada_value* value) {
    Recorder& seen = recorder();
    if (handle == seen.refuse) {
        return SCADA_E_NOT_OWNER;
    }
    seen.writes.push_back(
        {.slot = handle, .bits = value->value, .at_ns = seen.now_ns});
    return SCADA_OK;
}

int record_text(scada_ctx* /*ctx*/, std::uint32_t handle, const char* text,
                std::size_t length, std::uint8_t /*quality*/,
                std::uint16_t /*flags*/, std::int64_t /*source_time_ns*/) {
    recorder().texts.emplace_back(handle, std::string(text, length));
    return SCADA_OK;
}

std::uint32_t device_slot(std::size_t groups, DeviceSignal signal,
                          std::size_t device = 0) {
    return static_cast<std::uint32_t>(CHANNEL_SIGNALS + groups +
                                      (device * DEVICE_SIGNALS) +
                                      static_cast<std::size_t>(signal));
}

// The writes of `slot`, in order.
std::vector<Recorded> writes_of(std::uint32_t slot) {
    std::vector<Recorded> out;
    for (const Recorded& write : recorder().writes) {
        if (write.slot == slot) {
            out.push_back(write);
        }
    }
    return out;
}

// A channel of `devices` devices in the fast group (200 ms), the last one
// disabled when `disabled_last`, its system tags recorded; the engine runs
// on the fake clock and the tags are published after each step, as the
// worker does.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes): a test rig whose parts are its interface
struct Rig {
    explicit Rig(std::uint8_t devices = 1, bool disabled_last = false)
        : Rig(make(devices, disabled_last), devices) {}

    // `compiled`, its devices on units 1 to `units`.
    Rig(CompiledChannel compiled, std::uint8_t units)
        : channel(std::move(compiled)),
          transport(bus, clock, LATENCY),
          engine(channel, protocol, transport, clock, sink),
          system(nullptr, handles(channel)) {
        for (std::uint8_t unit = 1; unit <= units; ++unit) {
            bus.map(unit, 0, 0, 9);
        }
        recorder() = Recorder{};
    }

    static CompiledChannel make(std::uint8_t devices, bool disabled_last) {
        std::vector<DeviceSpec> specs;
        for (std::uint8_t unit = 1; unit <= devices; ++unit) {
            specs.push_back({.name = "d" + std::to_string(unit),
                             .unit_id = unit,
                             .points = adjacent(0, 1, 0)});
        }
        CompiledChannel out = make_channel(specs, ReadRules{.max_units = 4});
        out.transport = TransportKind::TCP;
        if (disabled_last) {
            out.devices.back().enabled = false;
        }
        return out;
    }

    static SystemTagHandles handles(const CompiledChannel& compiled) {
        SystemTagHandles out;
        out.write = &record;
        out.write_text = &record_text;
        out.groups = compiled.poll_groups.size();
        out.devices = compiled.devices.size();
        const std::size_t slots =
            CHANNEL_SIGNALS + out.groups + (out.devices * DEVICE_SIGNALS);
        for (std::size_t slot = 0; slot < slots; ++slot) {
            out.slots.push_back({.handle = static_cast<std::uint32_t>(slot),
                                 .type = SCADA_TYPE_UINT64});
        }
        return out;
    }

    void publish(bool all) {
        recorder().now_ns = clock.monotonic_ns();
        (void)system.publish(engine, channel, clock.monotonic_ns(), all);
    }

    // Steps until `end_ns`, publishing after each step.
    void run_until(std::int64_t end_ns) {
        while (clock.monotonic_ns() <= end_ns) {
            const std::int64_t next = engine.step();
            publish(false);
            clock.advance_to(std::min(next, end_ns + 1));
        }
    }

    [[nodiscard]] std::uint32_t slot(DeviceSignal signal,
                                     std::size_t device = 0) const {
        return device_slot(channel.poll_groups.size(), signal, device);
    }

    CompiledChannel channel;
    SimBus bus;
    SimClock clock;
    SimTransport transport;
    RecordingTags sink;
    TestProtocol protocol;
    ChannelEngine engine;
    SystemTags system;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

// One device whose point 0 (register 0, which the bus holds at 0) its
// identity check reads, equal to `expected`.
CompiledChannel identified(std::int64_t expected) {
    CompiledChannel channel = Rig::make(1, false);
    CompiledDevice& device = channel.devices[0];
    IdentityCheck check;
    check.point = 0;
    check.kind = IdentityCheck::Kind::EQUALS;
    check.values = {
        ExpectedValue{.type = device.points[0].rules.tag_type, .i = expected}};
    device.identify = {check};
    device.plan = ReadPlan{};
    device.plan.by_group.resize(channel.poll_groups.size());
    EXPECT_TRUE(TestProtocol{}
                    .plan(device, ReadRules{.max_units = 4}, device.plan)
                    .ok());
    return channel;
}

TEST(SystemTags, TheIdentityTagsAreWrittenOnceThereIsAVerdict) {
    // D-147: no write before the verdict, then `identity_ok` and the text.
    Rig rig(identified(0), 1);
    rig.publish(true);
    EXPECT_TRUE(writes_of(rig.slot(DeviceSignal::IDENTITY_OK)).empty());
    EXPECT_TRUE(recorder().texts.empty());
    rig.run_until(SECOND);
    const std::vector<Recorded> matched =
        writes_of(rig.slot(DeviceSignal::IDENTITY_OK));
    ASSERT_EQ(matched.size(), 1U);
    EXPECT_EQ(matched.front().bits, 1U);
    ASSERT_EQ(recorder().texts.size(), 1U);
    EXPECT_EQ(
        recorder().texts.front(),
        std::make_pair(rig.slot(DeviceSignal::IDENTITY), std::string("0")));
    // Unchanged: not written again; with `all`, written again.
    rig.run_until(3 * SECOND);
    EXPECT_EQ(recorder().texts.size(), 1U);
    rig.publish(true);
    EXPECT_EQ(recorder().texts.size(), 2U);
}

TEST(SystemTags, AMismatchWritesIdentityOkFalse) {
    Rig rig(identified(5), 1);
    rig.run_until(SECOND);
    const std::vector<Recorded> matched =
        writes_of(rig.slot(DeviceSignal::IDENTITY_OK));
    ASSERT_EQ(matched.size(), 1U);
    EXPECT_EQ(matched.front().bits, 0U);
}

TEST(SystemTags, RestartsAreACounter) {
    // A device without checks: `restarts` 0, written with the counters.
    Rig rig;
    rig.publish(true);
    const std::vector<Recorded> restarts =
        writes_of(rig.slot(DeviceSignal::RESTARTS));
    ASSERT_EQ(restarts.size(), 1U);
    EXPECT_EQ(restarts.front().bits, 0U);
}

TEST(SystemTags, ACounterIsWrittenAtMostOncePerPeriod) {
    Rig rig;
    rig.publish(true);
    rig.run_until(5 * SECOND);
    const std::vector<Recorded> requests =
        writes_of(rig.slot(DeviceSignal::REQUESTS));
    // Five requests a second: written at 0 (9.1), then once a second.
    ASSERT_GE(requests.size(), 5U);
    for (std::size_t index = 1; index < requests.size(); ++index) {
        EXPECT_GE(requests[index].at_ns - requests[index - 1].at_ns, PERIOD)
            << index;
        EXPECT_NE(requests[index].bits, requests[index - 1].bits) << index;
    }
}

TEST(SystemTags, AStateIsWrittenInTheStepThatChangedIt) {
    Rig rig;
    rig.publish(true);
    rig.run_until(2 * SECOND);
    const std::vector<Recorded> states =
        writes_of(rig.slot(DeviceSignal::STATE));
    // CONNECTING at 0, INITIALISING at the connect, RUNNING at the first
    // answer: all inside the first counter period.
    ASSERT_GE(states.size(), 2U);
    EXPECT_EQ(std::bit_cast<std::int64_t>(states.back().bits),
              static_cast<std::int64_t>(DeviceStatus::RUNNING));
    EXPECT_LT(states.back().at_ns, PERIOD / 10);
}

TEST(SystemTags, AnUnchangedValueIsNotWrittenAgain) {
    Rig rig;
    rig.run_until(3 * SECOND);
    recorder().writes.clear();
    // No step: no device count, time or state changed. Only the channel's
    // rate and achieved periods, which move with the time alone, may be
    // written.
    rig.clock.advance_to(rig.system.next_due_ns());
    rig.publish(false);
    const std::size_t first_device =
        CHANNEL_SIGNALS + rig.channel.poll_groups.size();
    for (const Recorded& write : recorder().writes) {
        EXPECT_LT(write.slot, first_device) << "slot " << write.slot;
    }
    recorder().writes.clear();
    rig.publish(false);  // the same instant: nothing at all
    EXPECT_TRUE(recorder().writes.empty());
}

TEST(SystemTags, TheCountersAreDueOnePeriodAfterTheirWrite) {
    Rig rig;
    rig.clock.advance_to(SECOND / 3);
    rig.publish(true);
    EXPECT_EQ(rig.system.next_due_ns(), (SECOND / 3) + PERIOD);
    rig.clock.advance_to((SECOND / 3) + (PERIOD / 2));
    rig.publish(false);  // not due: the due time stays
    EXPECT_EQ(rig.system.next_due_ns(), (SECOND / 3) + PERIOD);
}

TEST(SystemTags, TheStopWritesTheValuesThePeriodHeldBack) {
    // M1: a stop between two counter writes.
    Rig rig;
    rig.publish(true);
    rig.run_until(PERIOD + (PERIOD / 2));
    const std::uint32_t slot = rig.slot(DeviceSignal::REQUESTS);
    const std::vector<Recorded> before = writes_of(slot);
    ASSERT_FALSE(before.empty());
    const std::uint64_t sent = rig.engine.device_counts(0).requests;
    ASSERT_GT(sent, before.back().bits);  // held back by the period
    recorder().writes.clear();
    recorder().now_ns = rig.clock.monotonic_ns();
    EXPECT_TRUE(
        rig.system.flush(rig.engine, rig.channel, rig.clock.monotonic_ns()));
    const std::vector<Recorded> after = writes_of(slot);
    ASSERT_EQ(after.size(), 1U);
    EXPECT_EQ(after.front().bits, sent);
    // No state: the engine still says CONNECTED.
    EXPECT_TRUE(writes_of(rig.slot(DeviceSignal::STATE)).empty());
    EXPECT_TRUE(
        writes_of(static_cast<std::uint32_t>(ChannelSignal::STATE)).empty());
}

TEST(SystemTags, TheStopLeavesEveryStateNotConnected) {
    // M3: the channel DISCONNECTED, an enabled device CONNECTING, a disabled
    // one DISABLED, no pacing limit.
    Rig rig(2, true);
    rig.publish(true);
    rig.run_until(SECOND);
    recorder().writes.clear();
    EXPECT_TRUE(rig.system.stopped(rig.channel));
    const auto last = [](std::uint32_t slot) {
        const std::vector<Recorded> written = writes_of(slot);
        return written.empty()
                   ? -1
                   : std::bit_cast<std::int64_t>(written.back().bits);
    };
    EXPECT_EQ(last(static_cast<std::uint32_t>(ChannelSignal::STATE)),
              static_cast<std::int64_t>(ChannelLink::DISCONNECTED));
    EXPECT_EQ(last(rig.slot(DeviceSignal::STATE, 0)),
              static_cast<std::int64_t>(DeviceStatus::CONNECTING));
    // The disabled device was DISABLED already: not written again.
    EXPECT_EQ(last(rig.slot(DeviceSignal::STATE, 1)), -1);
}

// One device of seven requests in the fast group (200 ms), at most one
// request every 33.3 ms: a cycle takes 233.3 ms, so the group is pacing
// limited and its achieved period is no whole tenth of a ms (6.4).
CompiledChannel paced_channel() {
    CompiledChannel out = make_channel(
        {{.name = "d1", .unit_id = 1, .points = adjacent(0, 7, 0)}},
        ReadRules{.max_units = 1},
        Pacing{.min_request_interval_ns = SECOND / 30});
    out.transport = TransportKind::TCP;
    return out;
}

// The double of the slot's last write; NaN if none.
double last_double(std::uint32_t slot) {
    const std::vector<Recorded> written = writes_of(slot);
    return written.empty() ? std::numeric_limits<double>::quiet_NaN()
                           : std::bit_cast<double>(written.back().bits);
}

TEST(SystemTags, ThePeriodIsWrittenInTenthsAndTheStopEndsAPacingLimit) {
    Rig rig(paced_channel(), 1);
    rig.publish(true);
    rig.run_until(3 * SECOND);
    const std::int64_t now = rig.clock.monotonic_ns();
    ASSERT_TRUE(rig.engine.scheduler().pacing_limited(0, now));
    const std::uint32_t limited = rig.slot(DeviceSignal::PACING_LIMITED);
    ASSERT_FALSE(writes_of(limited).empty());
    EXPECT_EQ(writes_of(limited).back().bits, 1U);
    // The achieved period is no whole tenth; what was written is one.
    const double raw =
        static_cast<double>(rig.engine.scheduler().achieved_period_ns(0, now)) /
        1e6;
    ASSERT_NE(raw * 10.0, std::round(raw * 10.0)) << raw;
    const double period =
        last_double(static_cast<std::uint32_t>(CHANNEL_SIGNALS));
    EXPECT_GT(period, 200.0);
    EXPECT_EQ(period, std::round(period * 10.0) / 10.0) << period;
    // M3: the stop writes `pacing_limited` false.
    recorder().writes.clear();
    EXPECT_TRUE(rig.system.stopped(rig.channel));
    ASSERT_FALSE(writes_of(limited).empty());
    EXPECT_EQ(writes_of(limited).back().bits, 0U);
}

TEST(SystemTags, ATenthIsTheDoubleNearestIt) {
    // round(x * 10) / 10 rounds once: seven tenths is 0.7, not
    // 0.7000000000000001 as round(x / 0.1) * 0.1 gives (the delta check).
    EXPECT_EQ(std::bit_cast<std::uint64_t>(detail::rounded_ms(0.7)),
              std::bit_cast<std::uint64_t>(0.7));
    for (int tenths = 0; tenths <= 100'000; ++tenths) {
        const double exact = static_cast<double>(tenths) / 10.0;
        ASSERT_EQ(
            std::bit_cast<std::uint64_t>(detail::rounded_ms(exact + 0.02)),
            std::bit_cast<std::uint64_t>(exact))
            << tenths;
    }
}

TEST(SystemTags, ARefusedWriteIsCountedAndTriedAgain) {
    // A refused write is counted (the worker stores the count and the main
    // thread logs it, review M4) and not taken as written: the next publish
    // that is due writes it again.
    Rig rig;
    const std::uint32_t slot = rig.slot(DeviceSignal::REQUESTS_PER_SCAN);
    recorder().refuse = slot;
    rig.publish(true);
    EXPECT_EQ(rig.system.refused(), 1U);
    EXPECT_TRUE(writes_of(slot).empty());
    recorder().refuse = SCADA_INVALID_HANDLE;
    rig.clock.advance_to(rig.system.next_due_ns());
    rig.publish(false);
    ASSERT_EQ(writes_of(slot).size(), 1U);
    EXPECT_EQ(rig.system.refused(), 1U);
}

TEST(SystemTags, AnAverageIsRoundedToATenthOfAMillisecond) {
    EXPECT_DOUBLE_EQ(detail::rounded_ms(1.04), 1.0);
    EXPECT_DOUBLE_EQ(detail::rounded_ms(1.06), 1.1);
    EXPECT_DOUBLE_EQ(detail::rounded_ms(0.0), 0.0);
    // Two averages that differ below the resolution are one value.
    EXPECT_EQ(std::bit_cast<std::uint64_t>(detail::rounded_ms(2.01)),
              std::bit_cast<std::uint64_t>(detail::rounded_ms(1.99)));
}

// Process tags whose handles are their slots, recorded.
ProcessTags recorded_process_tags() {
    recorder() = Recorder{};
    ProcessTags::Slots slots{};
    for (std::size_t signal = 0; signal < slots.size(); ++signal) {
        slots.at(signal) = {.handle = static_cast<std::uint32_t>(signal),
                            .type = SCADA_TYPE_UINT64};
    }
    return {nullptr, slots, &record};
}

constexpr auto CONFIG_STATE =
    static_cast<std::uint32_t>(ProcessSignal::CONFIG_STATE);

TEST(ProcessTags, ARefusedWriteIsCounted) {
    ProcessTags tags = recorded_process_tags();
    recorder().refuse = static_cast<std::uint32_t>(ProcessSignal::CONFIG_ID);
    (void)tags.publish_start();
    EXPECT_EQ(tags.refused(), 1U);
    recorder().refuse = CONFIG_STATE;
    EXPECT_FALSE(tags.applied());
    EXPECT_EQ(tags.refused(), 2U);
}

TEST(ProcessTags, TheStopWritesLoadingAgain) {
    // M3: the tags left after the stop do not say "applied".
    ProcessTags tags = recorded_process_tags();
    (void)tags.publish_start();
    EXPECT_TRUE(tags.applied());
    EXPECT_TRUE(tags.stopped());
    const std::vector<Recorded> written = writes_of(CONFIG_STATE);
    ASSERT_EQ(written.size(), 3U);  // LOADING, APPLIED, LOADING
    EXPECT_EQ(written.back().bits, 0U);
    EXPECT_FALSE(tags.stopped());  // LOADING already
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

}  // namespace
}  // namespace scada::driver::test
