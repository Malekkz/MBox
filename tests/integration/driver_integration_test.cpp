// The driver process through real tags (driver interface contract 2.4,
// 9.1, 5.2; conformance tests 1, 16, 17; decision D-142): the writes after
// attach, a kill and a restart, retained values, the owners of the tags,
// the wake of the consumers, lost and refused connections and staleness.
// The stop, the watchdog and the service manager are in
// driver_stop_integration_test.cpp.

#include "driver_fixture.h"

#include "scada/tag/scada_tag.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal numbers are declared here, not in <csignal>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace scada::test::driver_it {
namespace {

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers): the test data

// Kills `process` and waits for its end.
::testing::AssertionResult killed(ChildProcess& process) {
    if (!process.send_signal(SIGKILL) ||
        !process.wait_for_ending(WAIT_LIMIT).has_value()) {
        return ::testing::AssertionFailure() << "did not end on SIGKILL";
    }
    return ::testing::AssertionSuccess();
}

std::vector<std::string> disabled_device(const std::string& name, int unit) {
    std::vector<std::string> args = device(name, unit, "0");
    args.emplace_back("--disabled");
    return args;
}

// --- 9.1, conformance test 1 ------------------------------------------------

TEST_F(DriverProcess, EveryTagIsNotInitialisedBeforeTheFirstRequest) {
    // 9.1: the value as stored, BAD with NOT_INIT, no source time. A
    // disabled device's tag gets only that write, so it shows what the
    // worker wrote before its first request; the first run gives it a value.
    // Every call of the protocol runs on its thread (conformance test 1):
    // `run_until` fails on the driver's thread check.
    start_tagd({{.name = "line1", .tags = {"d1.v", "d2.v"}}});
    auto sim =
        start_sim({"--set", "1", "0", "0", "11", "--set", "2", "0", "0", "22"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    const std::vector<std::string> channel =
        tcp_channel("line1", port_of(*sim));
    ASSERT_TRUE(run_until(
        joined(channel, device("d1", 1, "0"), device("d2", 2, "0")), "d2.v",
        [](const scada_value& value) { return good_with(value, 22); }));
    ASSERT_TRUE(run_until(
        joined(channel, device("d1", 1, "0"), disabled_device("d2", 2)), "d1.v",
        [](const scada_value& value) { return good_with(value, 11); }));

    const scada_value stored = read(handle("d2.v"));
    // Source time 0 is "no device time": the library stores its own update
    // time with LOCAL_TIME (TD 13, scada_tag_write). The driver has stopped,
    // and the read says so with OWNER_DOWN.
    EXPECT_TRUE(holds(
        stored, SCADA_QUALITY_BAD,
        SCADA_FLAG_NOT_INIT | SCADA_FLAG_LOCAL_TIME | SCADA_FLAG_OWNER_DOWN,
        22));
    EXPECT_EQ(stored.source_time_ns, stored.update_time_ns);
}

TEST_F(DriverProcess, TheWritesOf91ComeFirstAndBeforeReady) {
    // Review M6: after a restart the first write of the tag is that of 9.1
    // (NOT_INIT, the value of the previous life), before the first request
    // and before READY; at READY the tag never reads GOOD with the old
    // value.
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto first = start_sim({"--set", "1", "0", "0", "22"});
    if (!first.has_value()) {
        FAIL() << "no simulator";
    }
    ASSERT_TRUE(run_until(
        joined(tcp_channel("line1", port_of(*first)), device("d1", 1, "0")),
        "d1.v", [](const scada_value& value) { return good_with(value, 22); }));
    // The second life: the device does not answer within its 2 s
    // time-out, so at READY the tag can only show what 9.1 wrote.
    auto silent = start_sim({"--silent", "1"});
    if (!silent.has_value()) {
        FAIL() << "no simulator";
    }
    auto driver = start_driver(joined(tcp_channel("line1", port_of(*silent)),
                                      device("d1", 1, "0", "2000")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    ASSERT_TRUE(receives(listener(), "READY=1")) << driver->err();
    // Exactly these flags: neither OWNER_DOWN nor COMM_FAIL.
    EXPECT_TRUE(holds(read(handle("d1.v")), SCADA_QUALITY_BAD,
                      SCADA_FLAG_NOT_INIT | SCADA_FLAG_LOCAL_TIME, 22));
    EXPECT_TRUE(stops_cleanly(*driver));
}

TEST_F(DriverProcess, AKilledDriverIsOwnerDownAndItsRestartKeepsTheValue) {
    // Review M4 and M6: a kill leaves the tag with its value and OWNER_DOWN
    // (asserted, not masked out); a restart with the peer gone writes it
    // BAD with COMM_FAIL and keeps the value as stored (5.2 "keeps its last
    // value", 9.1 "value as stored"), never 0.
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto sim = start_sim({"--set", "1", "0", "0", "11"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    auto driver = start_driver(
        joined(tcp_channel("line1", port_of(*sim)), device("d1", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    const std::uint32_t tag = handle("d1.v");
    ASSERT_TRUE(eventually(
        tag, [](const scada_value& value) { return good_with(value, 11); }));
    ASSERT_TRUE(killed(*driver));
    EXPECT_TRUE(eventually(tag, [](const scada_value& value) {
        return owner_down(value) && holds_value(value, SCADA_QUALITY_BAD, 11);
    }));
    const LocalPort gone(false);
    auto again = start_driver(
        joined(tcp_channel("line1", gone.port()), device("d1", 1, "0")));
    if (!again.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(eventually(tag, [](const scada_value& value) {
        return comm_fail(value) &&
               holds_value(value, SCADA_QUALITY_BAD, 11, SCADA_FLAG_OWNER_DOWN);
    })) << again->err();
    EXPECT_TRUE(stops_cleanly(*again));
}

// A tag restored from a retain file (UNCERTAIN with RESTORED, TD 6.3):
// d1.v read as 11 and retained, then a changed tag set, a new generation in
// which the driver's attach restores it (TD 12).
class RestoredTag : public DriverProcess {
protected:
    void SetUp() override {
        DriverProcess::SetUp();
        start_tagd(tags_);
        std::optional<ChildProcess> sim =
            start_sim({"--set", "1", "0", "0", "11"});
        if (!sim.has_value()) {
            FAIL() << "no simulator";
        }
        sim_.emplace(std::move(*sim));
        channel_ = tcp_channel("line1", port_of(*sim_));
        ASSERT_TRUE(run_until(
            joined(channel_, device("d1", 1, "0")), "d1.v",
            [](const scada_value& value) { return good_with(value, 11); }));
        restart_tagd(&tags_, {"spare.v"});
    }

    static bool restored_11(const scada_value& value) {
        return (value.quality_flags & SCADA_FLAG_RESTORED) != 0 &&
               holds_value(value, SCADA_QUALITY_UNCERTAIN, 11);
    }

    [[nodiscard]] const std::vector<std::string>& channel() const {
        return channel_;
    }

private:
    const std::vector<ChannelTags> tags_{
        {.name = "line1", .tags = {"d1.v"}, .retain = true}};
    std::optional<ChildProcess> sim_;
    std::vector<std::string> channel_;
};

TEST_F(RestoredTag, TheWritesAfterAttachLeaveItAlone) {
    // 9.1: a disabled device gets only the writes of 9.1, which keep a
    // restored value until the device is read.
    auto off = start_driver(joined(channel(), disabled_device("d1", 1)));
    if (!off.has_value()) {
        FAIL() << "no driver";
    }
    ASSERT_TRUE(receives(listener(), "READY=1")) << off->err();
    observe();
    EXPECT_TRUE(restored_11(read(handle("d1.v"))));
    EXPECT_TRUE(stops_cleanly(*off));
}

TEST_F(RestoredTag, ACommFailKeepsItsValue) {
    const LocalPort gone(false);
    auto failing = start_driver(
        joined(tcp_channel("line1", gone.port()), device("d1", 1, "0")));
    if (!failing.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    EXPECT_TRUE(eventually(handle("d1.v"), [](const scada_value& value) {
        return comm_fail(value) && holds_value(value, SCADA_QUALITY_BAD, 11);
    })) << failing->err();
    EXPECT_TRUE(stops_cleanly(*failing));
}

TEST_F(DriverProcess, ADisabledChannelOwnsAndWritesItsTags) {
    // Review M2: a disabled channel's worker writes 9.1 for its tags, so
    // that the value of the previous life never reads GOOD; it never
    // connects (CS 4.6).
    start_tagd(
        {{.name = "on", .tags = {"d1.v"}}, {.name = "off", .tags = {"d2.v"}}});
    // One simulator per channel: a simulator serves one connection at a
    // time.
    auto sim = start_sim({"--set", "1", "0", "0", "5"});
    auto other = start_sim({"--set", "2", "0", "0", "6"});
    if (!sim.has_value() || !other.has_value()) {
        FAIL() << "no simulator";
    }
    const std::vector<std::string> enabled =
        joined(tcp_channel("on", port_of(*sim)), device("d1", 1, "0"));
    const std::vector<std::string> off_channel =
        tcp_channel("off", port_of(*other));
    auto both =
        start_driver(joined(enabled, off_channel, device("d2", 2, "0")));
    if (!both.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    const std::uint32_t tag = handle("d2.v");
    ASSERT_TRUE(eventually(
        tag, [](const scada_value& value) { return good_with(value, 6); }));
    ASSERT_TRUE(stops_cleanly(*both));
    std::vector<std::string> disabled = off_channel;
    disabled.emplace_back("--disabled");
    auto driver = start_driver(joined(enabled, disabled, device("d2", 2, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(eventually(tag, [](const scada_value& value) {
        return holds(value, SCADA_QUALITY_BAD,
                     SCADA_FLAG_NOT_INIT | SCADA_FLAG_LOCAL_TIME, 6);
    })) << driver->err();
    EXPECT_TRUE(stops_cleanly(*driver));
}

// --- The owners of the tags (review M3) -------------------------------------

TEST_F(DriverProcess, TwoChannelsOnOneOwnerFailTheStart) {
    start_tagd({{.name = "a", .tags = {"d1.v"}, .owner = "drv-test"},
                {.name = "b", .tags = {"d2.v"}, .owner = "drv-test"}});
    const LocalPort port(false);
    auto driver = start_driver(
        joined(tcp_channel("a", port.port()), device("d1", 1, "0"),
               tcp_channel("b", port.port()), device("d2", 2, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(
        fails_start(*driver, "channels a and b both write owner drv-test"));
}

TEST_F(DriverProcess, ATagOfAnotherChannelsOwnerFailsTheStart) {
    // Channel a's second device has its tag in channel b's owner.
    start_tagd({{.name = "a", .tags = {"d1.v"}},
                {.name = "b", .tags = {"d2.v", "d3.v"}}});
    const LocalPort port(false);
    auto driver =
        start_driver(joined(tcp_channel("a", port.port()), device("d1", 1, "0"),
                            device("d3", 1, "0"), tcp_channel("b", port.port()),
                            device("d2", 2, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(
        fails_start(*driver, "tag d3.v of channel a is owned by drv-test.b"));
}

// --- One notify per delivery (reference 5.3) --------------------------------

TEST_F(DriverProcess, EveryDeliveryWakesTheConsumers) {
    // `scada_tag_notify` advances the process's cycle counter: at least
    // once per delivery that wrote a change. A point of the 1 s group with
    // `max_age` 300 ms changes twice a second (fresh, STALE); an unchanged
    // value is not written again, so it is no delivery to count.
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto sim = start_sim({"--set", "1", "0", "0", "4"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    std::vector<std::string> point = device("d1", 1, "1");
    point.insert(point.end(), {"--max-age-ms", "300"});
    auto driver =
        start_driver(joined(tcp_channel("line1", port_of(*sim)), point));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    ASSERT_TRUE(eventually(handle("d1.v"), [](const scada_value& value) {
        return good_with(value, 4);
    }));
    drain();
    const std::uint32_t before = driver_status().cycle_seq;
    ASSERT_TRUE(changes(4));
    EXPECT_GE(driver_status().cycle_seq - before, 4U);
    EXPECT_TRUE(stops_cleanly(*driver));
}

// --- Conformance tests 16 and 17 --------------------------------------------

TEST_F(DriverProcess, ALostConnectionSetsEveryDeviceBadWithCommFail) {
    start_tagd({{.name = "line1", .tags = {"d1.v", "d2.v"}}});
    auto sim =
        start_sim({"--set", "1", "0", "0", "5", "--set", "2", "0", "0", "6"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    auto driver =
        start_driver(joined(tcp_channel("line1", port_of(*sim)),
                            device("d1", 1, "0"), device("d2", 2, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    const std::uint32_t one = handle("d1.v");
    const std::uint32_t two = handle("d2.v");
    ASSERT_TRUE(eventually(one, [&](const auto& value) {
        return good_with(value, 5) && good_with(read(two), 6);
    }));
    // The peer goes: every device of the channel BAD with COMM_FAIL; the
    // reconnects are refused and the tags stay so, with their values.
    ASSERT_TRUE(killed(*sim));
    EXPECT_TRUE(eventually(one, [&](const auto& value) {
        return comm_fail(value) && comm_fail(read(two)) &&
               holds_value(value, SCADA_QUALITY_BAD, 5);
    }));
    EXPECT_TRUE(stops_cleanly(*driver));
}

TEST_F(DriverProcess, ARefusedConnectionIsCommFail) {
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    const LocalPort refusing(false);
    auto driver = start_driver(
        joined(tcp_channel("line1", refusing.port()), device("d1", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    EXPECT_TRUE(eventually(handle("d1.v"), comm_fail));
    EXPECT_TRUE(stops_cleanly(*driver));
}

TEST_F(DriverProcess, StaleAfterMaxAgeAndClearedByTheNextRead) {
    // 7.5: a point of the 1 s group with `max_age` 300 ms goes STALE
    // between its reads; the next read clears it.
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto sim = start_sim({"--set", "1", "0", "0", "9"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    std::vector<std::string> point = device("d1", 1, "1");
    point.insert(point.end(), {"--max-age-ms", "300"});
    auto driver =
        start_driver(joined(tcp_channel("line1", port_of(*sim)), point));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    const std::uint32_t tag = handle("d1.v");
    const auto fresh = [](const auto& value) { return good_with(value, 9); };
    ASSERT_TRUE(eventually(tag, fresh));
    EXPECT_TRUE(eventually(tag, stale));
    EXPECT_TRUE(eventually(tag, fresh));
    EXPECT_TRUE(eventually(tag, stale));
    EXPECT_TRUE(stops_cleanly(*driver));
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

}  // namespace
}  // namespace scada::test::driver_it
