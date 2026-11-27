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

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
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

TEST_F(DriverProcess, The91WritesComeBeforeTheResolve) {
    // Review M6: 9.1 is written by the owner's thread before the main
    // thread resolves the names. A start that then fails on a point
    // without a tag has already written the channel's tags NOT_INIT.
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto sim = start_sim({"--set", "1", "0", "0", "22"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    const std::vector<std::string> channel =
        tcp_channel("line1", port_of(*sim));
    ASSERT_TRUE(run_until(
        joined(channel, device("d1", 1, "0")), "d1.v",
        [](const scada_value& value) { return good_with(value, 22); }));
    auto failing = start_driver(
        joined(channel, device("d1", 1, "0"), device("d9", 1, "0")));
    if (!failing.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(fails_start(*failing, "the tag set has no tag d9.v"));
    EXPECT_TRUE(holds(
        read(handle("d1.v")), SCADA_QUALITY_BAD,
        SCADA_FLAG_NOT_INIT | SCADA_FLAG_LOCAL_TIME | SCADA_FLAG_OWNER_DOWN,
        22));
}

// --- The owners of the tags (review M3) -------------------------------------

TEST_F(DriverProcess, AnOwnerNoChannelWritesFailsTheStart) {
    // Delta 3: the tag set has owners drv-test.a and drv-test.b; a driver
    // with channel b only would leave a's tags as the previous life left
    // them. The start fails, naming the owner.
    start_tagd(
        {{.name = "a", .tags = {"d1.v"}}, {.name = "b", .tags = {"d2.v"}}});
    const LocalPort port(false);
    auto driver = start_driver(
        joined(tcp_channel("b", port.port()), device("d2", 2, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(fails_start(
        *driver, "owner drv-test.a of this process is written by no channel"));
}

TEST_F(DriverProcess, TwoChannelsOnOneOwnerFailTheStart) {
    start_tagd({{.name = "a", .tags = {"d1.v"}, .owner = "drv-test.ab"},
                {.name = "b", .tags = {"d2.v"}, .owner = "drv-test.ab"}});
    const LocalPort port(false);
    auto driver = start_driver(
        joined(tcp_channel("a", port.port()), device("d1", 1, "0"),
               tcp_channel("b", port.port()), device("d2", 2, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(
        fails_start(*driver, "channels a and b both write owner drv-test.ab"));
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
    // The fourth change is written before its notify: wait for the counter.
    EXPECT_TRUE(cycles_since(before, 4));
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

// --- 11.1: the system tags --------------------------------------------------

bool good(const scada_value& value) {
    return value.quality == SCADA_QUALITY_GOOD;
}
std::int64_t as_i64(const scada_value& value) {
    return std::bit_cast<std::int64_t>(value.value);
}
double as_f64(const scada_value& value) {
    return std::bit_cast<double>(value.value);
}

// Reads the system tags by name: each check one assertion.
class SystemTags : public DriverProcess {
protected:
    // GOOD, with the value bits `bits`.
    ::testing::AssertionResult is(const std::string& name, std::uint64_t bits) {
        const scada_value value = read(handle(name));
        if (good(value) && value.value == bits) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure()
               << name << ": quality " << int{value.quality} << ", value "
               << value.value;
    }
    ::testing::AssertionResult is_i64(const std::string& name,
                                      std::int64_t number) {
        return is(name, std::bit_cast<std::uint64_t>(number));
    }
    // GOOD, a FLOAT64 above `low`.
    ::testing::AssertionResult above(const std::string& name, double low) {
        const scada_value value = read(handle(name));
        if (good(value) && as_f64(value) > low) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure()
               << name << ": quality " << int{value.quality} << ", value "
               << as_f64(value);
    }
    // GOOD, a FLOAT64 of at least `low`.
    ::testing::AssertionResult at_least(const std::string& name, double low) {
        const scada_value value = read(handle(name));
        if (good(value) && as_f64(value) >= low) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure()
               << name << ": quality " << int{value.quality} << ", value "
               << as_f64(value);
    }
    // GOOD, an INT64 above 0.
    ::testing::AssertionResult positive(const std::string& name) {
        const scada_value value = read(handle(name));
        if (good(value) && as_i64(value) > 0) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure()
               << name << ": quality " << int{value.quality} << ", value "
               << as_i64(value);
    }
    // The first of `results` that failed, else success.
    static ::testing::AssertionResult all(
        std::initializer_list<::testing::AssertionResult> results) {
        for (const ::testing::AssertionResult& result : results) {
            if (!result) {
                return result;
            }
        }
        return ::testing::AssertionSuccess();
    }
    // Each of `names` GOOD and 0.
    ::testing::AssertionResult zero(std::initializer_list<const char*> names) {
        for (const char* name : names) {
            const ::testing::AssertionResult held = is(name, 0);
            if (!held) {
                return held;
            }
        }
        return ::testing::AssertionSuccess();
    }
};

TEST_F(SystemTags, ShowTheChannelAndItsDevices) {
    // A device read in the fast group and a disabled one: the counters of
    // 11.1 move for the first, and the second shows DISABLED (3.5).
    start_tagd({{.name = "line1", .tags = {"d1.v", "d2.v"}}});
    auto sim = start_sim({"--set", "1", "0", "0", "7"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    std::vector<std::string> disabled = device("d2", 2, "0");
    disabled.emplace_back("--disabled");
    auto driver = start_driver(joined(tcp_channel("line1", port_of(*sim)),
                                      device("d1", 1, "0"), disabled));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    // Counters are written at most once a second: three requests counted
    // means at least one write after the first reads.
    ASSERT_TRUE(eventually(
        handle("sys.dev.d1.requests"),
        [](const auto& value) { return good(value) && value.value >= 3; }))
        << driver->err();
    // The channel: connected (2) once, never refused.
    EXPECT_TRUE(
        all({is_i64("sys.drv.line1.state", 2), is("sys.drv.line1.connects", 1),
             zero({"sys.drv.line1.refused_connects", "sys.drv.line1.secured"}),
             above("sys.drv.line1.requests_per_s", 0.0),
             above("sys.drv.line1.period.fast", 0.0)}));
    // d1 RUNNING (3) and answering; d2 DISABLED (0), never asked.
    EXPECT_TRUE(
        all({is_i64("sys.dev.d1.state", 3), positive("sys.dev.d1.last_ok_time"),
             at_least("sys.dev.d1.response_ms", 0.0),
             is("sys.dev.d1.requests_per_scan", 1),
             zero({"sys.dev.d1.errors", "sys.dev.d1.timeouts",
                   "sys.dev.d1.exceptions", "sys.dev.d1.refusals",
                   "sys.dev.d1.points_absent", "sys.dev.d1.pacing_limited",
                   "sys.dev.d2.requests"}),
             is_i64("sys.dev.d2.state", 0)}));
    ASSERT_TRUE(stops_cleanly(*driver));
    // The stop closed the connection, and the state says so before the
    // process ended; the read adds OWNER_DOWN only.
    EXPECT_EQ(as_i64(read(handle("sys.drv.line1.state"))), 0);
}

TEST_F(SystemTags, TheStopWritesTheCountersLastValues) {
    // The period holds back a counter's newest value up to a second; the
    // stop writes it, so that `requests` after the stop is every request
    // the simulator received from the driver (D-145).
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto sim = start_sim({"--set", "1", "0", "0", "7"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    auto driver = start_driver(
        joined(tcp_channel("line1", port_of(*sim)), device("d1", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    ASSERT_TRUE(eventually(
        handle("sys.dev.d1.requests"),
        [](const auto& value) { return good(value) && value.value >= 3; }))
        << driver->err();
    // Stopped while the tag is behind the simulator: a value held back.
    const std::uint64_t shown = read(handle("sys.dev.d1.requests")).value;
    ASSERT_TRUE(prints_times(*sim, "request ", shown + 1));
    ASSERT_TRUE(stops_cleanly(*driver));
    // Every request is printed before the simulator sees the close.
    ASSERT_TRUE(prints(*sim, "peer closed"));
    const std::size_t received = count_of(sim->out(), "request ");
    EXPECT_EQ(read(handle("sys.dev.d1.requests")).value, received)
        << sim->out();
}

// The point of the simulator's unit 1, read: GOOD, 7.
bool read_seven(const scada_value& value) {
    return good_with(value, 7);
}

// A counter written at least once after a request: GOOD, at least 1.
bool counted_one(const scada_value& value) {
    return good(value) && value.value >= 1;
}

TEST_F(SystemTags, AStateIsWrittenInTheStepThatChangedIt) {
    // One device in the slow group (10 s): its state RUNNING is written in
    // the step of its first answer, with the point, not at the next second
    // (D-145).
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto sim = start_sim({"--set", "1", "0", "0", "7"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    auto driver = start_driver(
        joined(tcp_channel("line1", port_of(*sim)), device("d1", 1, "2")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    ASSERT_TRUE(eventually(handle("d1.v"), read_seven)) << driver->err();
    const std::int64_t answered = read(handle("d1.v")).update_time_ns;
    const scada_value state = read(handle("sys.dev.d1.state"));
    EXPECT_EQ(as_i64(state), 3);  // RUNNING
    EXPECT_LT(state.update_time_ns - answered, 500'000'000);
    EXPECT_TRUE(stops_cleanly(*driver));
}

TEST_F(SystemTags, ACounterIsWrittenWithinItsPeriodOfAChange) {
    // One device in the slow group (10 s): nothing else wakes the worker.
    // Its `requests` is written within a second of its first answer: the
    // worker wakes for it, and waits neither for the next poll, 10 s later,
    // nor for the cap of every wait, a third of the stall limit: 5 s with a
    // connect timeout of 5 s (2.6; D-145).
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto sim = start_sim({"--set", "1", "0", "0", "7"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    auto driver = start_driver(joined(
        tcp_channel("line1", port_of(*sim), "5000"), device("d1", 1, "2")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    ASSERT_TRUE(eventually(handle("d1.v"), read_seven)) << driver->err();
    const std::int64_t answered = read(handle("d1.v")).update_time_ns;
    ASSERT_TRUE(eventually(handle("sys.dev.d1.requests"), counted_one));
    EXPECT_LT(read(handle("sys.dev.d1.requests")).update_time_ns - answered,
              1'500'000'000);
    EXPECT_TRUE(stops_cleanly(*driver));
}

TEST_F(SystemTags, ACounterIsWrittenAtMostOnceASecond) {
    // The fast group (200 ms) counts five requests a second: two writes of
    // `requests` in a row are a second apart, less the scheduling slack
    // (11.1).
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto sim = start_sim({"--set", "1", "0", "0", "7"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    auto driver = start_driver(
        joined(tcp_channel("line1", port_of(*sim)), device("d1", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    const std::uint32_t tag = handle("sys.dev.d1.requests");
    ASSERT_TRUE(eventually(tag, [](const auto& value) {
        return good(value) && value.value >= 1;
    }));
    std::vector<std::int64_t> times;
    for (int write = 0; write < 3; ++write) {
        const std::uint64_t before = read(tag).value;
        ASSERT_TRUE(eventually(tag, [before](const scada_value& value) {
            return value.value != before;
        }));
        times.push_back(read(tag).update_time_ns);
    }
    for (std::size_t index = 1; index < times.size(); ++index) {
        EXPECT_GE(times[index] - times[index - 1], 900'000'000) << index;
    }
    EXPECT_TRUE(stops_cleanly(*driver));
}

TEST_F(DriverProcess, ARefusedConnectIsCountedOnTheChannel) {
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    const LocalPort refusing(false);
    auto driver = start_driver(
        joined(tcp_channel("line1", refusing.port()), device("d1", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    EXPECT_TRUE(eventually(
        handle("sys.drv.line1.refused_connects"),
        [](const auto& value) { return good(value) && value.value >= 1; }));
    EXPECT_EQ(read(handle("sys.drv.line1.connects")).value, 0U);
    EXPECT_NE(as_i64(read(handle("sys.drv.line1.state"))), 2);
    EXPECT_EQ(as_i64(read(handle("sys.dev.d1.state"))), 1);  // CONNECTING
    EXPECT_EQ(read(handle("sys.dev.d1.requests")).value, 0U);
    EXPECT_TRUE(stops_cleanly(*driver));
}

TEST_F(SystemTags, OfADisabledChannelShowItWasNeverConnected) {
    // A disabled channel has no engine: its system tags are written once
    // at its start (9.1, D-145), the channel disconnected, its device
    // DISABLED, every count 0 but what a scan of the plan would send.
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    std::vector<std::string> off =
        tcp_channel("line1", LocalPort(false).port());
    off.emplace_back("--disabled");
    auto driver = start_driver(joined(off, device("d1", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    ASSERT_TRUE(receives(listener(), "READY=1")) << driver->err();
    observe();
    EXPECT_TRUE(is_i64("sys.drv.line1.state", 0));  // disconnected
    EXPECT_TRUE(is_i64("sys.dev.d1.state", 0));     // DISABLED
    EXPECT_TRUE(is("sys.dev.d1.requests_per_scan", 1));
    EXPECT_TRUE(
        zero({"sys.drv.line1.connects", "sys.drv.line1.refused_connects",
              "sys.drv.line1.secured", "sys.drv.line1.requests_per_s",
              "sys.drv.line1.period.fast", "sys.dev.d1.requests",
              "sys.dev.d1.last_ok_time", "sys.dev.d1.pacing_limited"}));
    EXPECT_TRUE(stops_cleanly(*driver));
}

TEST_F(DriverProcess, AChannelWithoutDevicesIsOwnedByItsSystemTags) {
    // No point tag to learn the owner from: the channel's state tag gives
    // it (D-145).
    start_tagd({{.name = "line1", .tags = {}}});
    auto driver = start_driver(tcp_channel("line1", LocalPort(false).port()));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    ASSERT_TRUE(receives(listener(), "READY=1")) << driver->err();
    observe();
    EXPECT_TRUE(good(read(handle("sys.drv.line1.state"))));
    EXPECT_TRUE(stops_cleanly(*driver));
}

TEST_F(DriverProcess, AMissingOrMistypedSystemTagFailsTheStart) {
    // CS 12.5: a tag set lists the system tags; one that lacks one, or
    // gives one another type, does not run the driver (D-145).
    const std::vector<std::string> args = joined(
        tcp_channel("line1", LocalPort(false).port()), device("d1", 1, "0"));
    start_tagd({{.name = "line1", .tags = {"d1.v"}, .system_tags = false}});
    auto missing = start_driver(args);
    if (!missing.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(
        fails_start(*missing, "the tag set has no tag sys.drv.line1.state"));
    // Every channel system tag, all INT64: `connects` is UINT64.
    std::vector<std::string> all_int64;
    all_int64.reserve(CHANNEL_SYSTEM_TAGS.size());
    for (const SystemTag& tag : CHANNEL_SYSTEM_TAGS) {
        all_int64.push_back("sys.drv.line1." + std::string(tag.signal));
    }
    const std::vector<ChannelTags> mistyped{
        {.name = "line1", .tags = {"d1.v"}, .system_tags = false}};
    restart_tagd(&mistyped, all_int64);
    auto wrong = start_driver(args);
    if (!wrong.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(fails_start(
        *wrong, "tag sys.drv.line1.connects is not of its type (11.1)"));
}

TEST_F(SystemTags, OfTheProcessAreTheMainThreads) {
    // `sys.proc.drv-test.*`, owner `drv-test`, written by the main thread
    // (D-145): no real-time thread, no `config_id` until the compile step,
    // APPLIED (1) once READY=1 is sent, every count 0. The owner has no
    // channel and is no unused owner.
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto driver = start_driver(joined(
        tcp_channel("line1", LocalPort(false).port()), device("d1", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    ASSERT_TRUE(receives(listener(), "READY=1")) << driver->err();
    observe();
    ASSERT_TRUE(eventually(handle("sys.proc.drv-test.config_state"),
                           [](const scada_value& value) {
                               return good(value) && as_i64(value) == 1;
                           }));
    EXPECT_TRUE(
        zero({"sys.proc.drv-test.realtime", "sys.proc.drv-test.config_id",
              "sys.proc.drv-test.stale_outcomes",
              "sys.proc.drv-test.answers_dropped",
              "sys.proc.drv-test.safe_unjournalled",
              "sys.proc.drv-test.log_dropped"}));
    EXPECT_TRUE(stops_cleanly(*driver));
}

TEST_F(DriverProcess, TheMainThreadsOwnerIsNoChannels) {
    // CS 12.5: the owner named like the process is the main thread's; a
    // channel on it would be its second writer (TD 13.2).
    start_tagd({{.name = "line1", .tags = {"d1.v"}, .owner = "drv-test"}});
    auto driver = start_driver(joined(
        tcp_channel("line1", LocalPort(false).port()), device("d1", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(fails_start(
        *driver, "channel line1 writes owner drv-test, the main thread's"));
}

TEST_F(DriverProcess, ATagSetWithoutTheProcessOwnerFailsTheStart) {
    without_process_tags();
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto driver = start_driver(joined(
        tcp_channel("line1", LocalPort(false).port()), device("d1", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(fails_start(
        *driver, "the tag set has no owner drv-test for the sys.proc tags"));
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
