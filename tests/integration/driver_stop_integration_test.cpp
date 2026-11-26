// The driver process's stop, channel watchdog, signals and service manager
// (driver interface contract 2.4, 2.6, 10; conformance test 2; decision
// D-142): scada-tagd, the simulator and scada-drv-test as real processes,
// a listener standing in for the service manager, and an observer of the
// tags.

#include "driver_fixture.h"
#include "tagd_fixture.h"

#include "scada/tag/scada_tag.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal numbers are declared here, not in <csignal>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scada::test::driver_it {
namespace {

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers): the test data

using std::chrono::duration_cast;

milliseconds since(SteadyClock::time_point start) {
    return duration_cast<milliseconds>(SteadyClock::now() - start);
}

// The channel `name` on `port` with the fault `fault` (`--hang`, `--spin`).
std::vector<std::string> faulty(const std::string& name, std::uint16_t port,
                                const std::string& fault,
                                const std::string& connect_ms = "1000") {
    std::vector<std::string> args = tcp_channel(name, port, connect_ms);
    args.push_back(fault);
    return args;
}

// Sends `signal_number` to `driver`, which logs `record` and goes on.
::testing::AssertionResult ignores(ChildProcess& driver, int signal_number,
                                   std::string_view record) {
    if (!driver.send_signal(signal_number)) {
        return ::testing::AssertionFailure() << "no signal sent";
    }
    const ::testing::AssertionResult logged = logs(driver, record);
    if (!logged) {
        return logged;
    }
    if (driver.wait_for_ending(milliseconds{200}).has_value()) {
        return ::testing::AssertionFailure() << "ended: " << driver.err();
    }
    return ::testing::AssertionSuccess();
}

// Sends `signal_number`; the service manager hears STOPPING=1 and the
// driver ends with `status`.
::testing::AssertionResult stops_on(ChildProcess& driver,
                                    NotifyListener& listener, int signal_number,
                                    int status) {
    if (!driver.send_signal(signal_number)) {
        return ::testing::AssertionFailure() << "no signal sent";
    }
    const ::testing::AssertionResult stopping =
        receives(listener, "STOPPING=1");
    if (!stopping) {
        return stopping;
    }
    return ends_with(driver, status);
}

// The simulator saw the driver close its connection with a FIN, never a
// reset.
::testing::AssertionResult closed_in_order(ChildProcess& sim) {
    const ::testing::AssertionResult closed = prints(sim, "peer closed");
    if (closed && has(sim.out(), "peer reset")) {
        return ::testing::AssertionFailure() << sim.out();
    }
    return closed;
}

// True when `elapsed` is from `low` to below `high`.
::testing::AssertionResult between(milliseconds elapsed, milliseconds low,
                                   milliseconds high) {
    if (elapsed >= low && elapsed < high) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << elapsed.count() << " ms";
}

// --- The service manager (review M10) ---------------------------------------

TEST_F(DriverProcess, TheServiceManagerHearsReadyTheWatchdogAndStopping) {
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto sim = start_sim({"--set", "1", "0", "0", "1"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    auto driver = start_driver(
        joined(tcp_channel("line1", port_of(*sim)), device("d1", 1, "0")),
        {.watchdog = std::chrono::microseconds{200'000}});
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    // READY (the watchdog may be fed before it), then the watchdog at
    // half its interval.
    ASSERT_TRUE(receives(listener(), "READY=1")) << driver->err();
    for (int fed = 0; fed < 3; ++fed) {
        EXPECT_EQ(listener().receive(WAIT_LIMIT), "WATCHDOG=1");
    }
    EXPECT_TRUE(stops_on(*driver, listener(), SIGTERM, EXIT_OK));
}

TEST_F(DriverProcess, AWrongOrDeadNotifySocketFailsTheStart) {
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    const LocalPort port(false);
    const std::vector<std::string> args =
        joined(tcp_channel("line1", port.port()), device("d1", 1, "0"));
    // Not an absolute path or an abstract name: refused as scada-tagd does.
    auto wrong = start_driver(args, {.socket = "relative/socket"});
    if (!wrong.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(fails_start(*wrong, "invalid environment"));
    // A path nobody listens on: READY cannot be sent.
    auto dead = start_driver(
        args, {.socket = (base().parent_path() / "nobody.sock").string()});
    if (!dead.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(fails_start(*dead, "cannot report readiness"));
}

// --- Signals (review M8) ----------------------------------------------------

TEST_F(DriverProcess, SigintStopsAndSighupDoesNot) {
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto sim = start_sim({"--set", "1", "0", "0", "2"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    auto driver = start_driver(
        joined(tcp_channel("line1", port_of(*sim)), device("d1", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    ASSERT_TRUE(receives(listener(), "READY=1"));
    ASSERT_TRUE(
        ignores(*driver, SIGHUP, R"(MESSAGE_ID="driver-reload-ignored")"));
    EXPECT_TRUE(stops_on(*driver, listener(), SIGINT, EXIT_OK));
}

TEST_F(DriverProcess, SigabrtEndsTheProcessAtOnce) {
    // Only the stop signals are blocked: an abort from the service
    // manager's watchdog ends the process by the signal, with its core.
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    const LocalPort port(false);
    auto driver = start_driver(
        joined(tcp_channel("line1", port.port()), device("d1", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    ASSERT_TRUE(receives(listener(), "READY=1"));
    const std::optional<ChildEnding> ending = end_with(*driver, SIGABRT);
    if (!ending.has_value()) {
        FAIL() << "did not end";
    }
    EXPECT_TRUE(ending->by_signal && ending->number == SIGABRT)
        << scada::test::tagd::describe(*ending);
}

// --- The stop (DI 10, review M5) --------------------------------------------

TEST_F(DriverProcess, AStopAbandonsAReadInFlightAndClosesInOrder) {
    // Unit 1 never answers and its time-out is 10 s: the stop comes while
    // its read is in flight, gives it up at once, and closes the
    // connection with a FIN, not a reset.
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto sim = start_sim({"--silent", "1"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    auto driver =
        start_driver(joined(tcp_channel("line1", port_of(*sim), "10000"),
                            device("d1", 1, "0", "10000")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    ASSERT_TRUE(prints(*sim, "request "));
    ASSERT_TRUE(driver->send_signal(SIGTERM));
    EXPECT_TRUE(ends_with(*driver, EXIT_OK, milliseconds{1000}));
    EXPECT_TRUE(closed_in_order(*sim));
}

TEST_F(DriverProcess, AnIdleWorkerStopsAtOnce) {
    // The 10 s group: the worker waits between polls, at most a third of
    // its 3 s stall limit; its stop eventfd ends the wait at once.
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto sim = start_sim({"--set", "1", "0", "0", "8"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    auto driver = start_driver(
        joined(tcp_channel("line1", port_of(*sim)), device("d1", 1, "2")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    ASSERT_TRUE(eventually(handle("d1.v"), [](const scada_value& value) {
        return good_with(value, 8);
    }));
    ASSERT_TRUE(driver->send_signal(SIGTERM));
    EXPECT_TRUE(ends_with(*driver, EXIT_OK, milliseconds{500}));
}

TEST_F(DriverProcess, APeerCloseOfAnIdleConnectionIsSeenAtOnce) {
    // The peer closes after one answer while the worker waits for its next
    // poll in 10 s: POLLRDHUP on the connection ends the wait, and the
    // tag goes COMM_FAIL well before the wait's cap (1 s).
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto sim = start_sim({"--set", "1", "0", "0", "8", "--close-after", "1"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    auto driver = start_driver(
        joined(tcp_channel("line1", port_of(*sim)), device("d1", 1, "2")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    // The answer and the close come at once: the tag is GOOD for a moment
    // only, so its changes are read from the ring, with their times.
    const std::optional<scada_change> good =
        next_change_where([](const scada_change& change) {
            return change.quality == SCADA_QUALITY_GOOD;
        });
    const std::optional<scada_change> failed = next_change();
    if (!good.has_value() || !failed.has_value()) {
        FAIL() << driver->err();
    }
    EXPECT_NE(failed->quality_flags & SCADA_FLAG_COMM_FAIL, 0);
    EXPECT_LT(failed->update_mono_ns - good->update_mono_ns, 600'000'000);
    EXPECT_TRUE(stops_cleanly(*driver));
}

TEST_F(DriverProcess, AStopWithAHungWorkerKeepsTheHeartbeatAndEndsInTheBudget) {
    // Channel b hangs deaf to the stop; its stall limit (3 s) is longer
    // than the stop budget (2 s). The stop waits for the workers with the
    // heartbeat going on (the process is never down while it runs), then
    // ends with the stall status within the budget.
    start_tagd(
        {{.name = "a", .tags = {"d1.v"}}, {.name = "b", .tags = {"d2.v"}}});
    auto sim = start_sim({"--set", "1", "0", "0", "1"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    auto driver = start_driver(
        joined(tcp_channel("a", port_of(*sim)), device("d1", 1, "0"),
               faulty("b", port_of(*sim), "--hang"), device("d2", 2, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    ASSERT_TRUE(receives(listener(), "READY=1")) << driver->err();
    observe();
    const auto stop = SteadyClock::now();
    ASSERT_TRUE(driver->send_signal(SIGTERM));
    bool down = false;
    const std::optional<ChildEnding> ending = end_watching(*driver, down);
    if (!ending.has_value()) {
        FAIL() << "did not end";
    }
    EXPECT_FALSE(down);
    EXPECT_TRUE(
        !ending->by_signal && ending->number == EXIT_STALL &&
        has(driver->err(), R"(MESSAGE_ID="driver-stop-overrun" CHANNEL="b")"))
        << driver->err();
    EXPECT_TRUE(between(since(stop), milliseconds{1900}, milliseconds{2900}));
}

TEST_F(DriverProcess, TheStallCheckGoesOnWhileStopping) {
    // Channel b hangs with a stall limit of 1 s, below the stop budget: a
    // stop right after READY ends with the stall status when its limit
    // passes, not at the end of the budget.
    start_tagd(
        {{.name = "a", .tags = {"d1.v"}}, {.name = "b", .tags = {"d2.v"}}});
    auto sim = start_sim({"--set", "1", "0", "0", "1"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    auto driver = start_driver(
        joined(tcp_channel("a", port_of(*sim)), device("d1", 1, "0"),
               faulty("b", port_of(*sim), "--hang", "300"),
               device("d2", 2, "0", "300")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    ASSERT_TRUE(receives(listener(), "READY=1")) << driver->err();
    ASSERT_TRUE(driver->send_signal(SIGTERM));
    EXPECT_TRUE(ends_with(*driver, EXIT_STALL, milliseconds{1700}));
}

TEST_F(DriverProcess, ARetiringGenerationWithAHungWorkerEndsInTheBudget) {
    // Review M1: the generation retires (a changed tag set) while channel b
    // hangs: the run fails, and the bounded stop ends the process instead
    // of a join that would never return.
    const std::vector<ChannelTags> tags{{.name = "a", .tags = {"d1.v"}},
                                        {.name = "b", .tags = {"d2.v"}}};
    start_tagd(tags);
    auto sim = start_sim({"--set", "1", "0", "0", "1"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    auto driver = start_driver(
        joined(tcp_channel("a", port_of(*sim)), device("d1", 1, "0"),
               faulty("b", port_of(*sim), "--hang"), device("d2", 2, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    ASSERT_TRUE(receives(listener(), "READY=1")) << driver->err();
    restart_tagd(&tags, {"spare.v"});
    EXPECT_TRUE(ends_with(*driver, EXIT_ERROR, milliseconds{10'000}));
    EXPECT_TRUE(has(driver->err(), R"(MESSAGE_ID="driver-retiring")"));
    EXPECT_TRUE(has(driver->err(), R"(MESSAGE_ID="driver-stop-overrun")"));
}

// --- Conformance test 2: the channel watchdog -------------------------------

TEST_F(DriverProcess, AStalledWorkerEndsTheProcessAtItsLimit) {
    // 2.6: the stall limit of the channel is three times its longest wait,
    // here `connect_timeout` 1 s, so 3 s; the process ends with its own
    // status no sooner and not much later, the stall logged with the
    // channel's name, and no heartbeat after it.
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto sim = start_sim({});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    observe();
    const auto started = SteadyClock::now();
    auto driver = start_driver(
        joined(faulty("line1", port_of(*sim), "--hang"), device("d1", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    ASSERT_TRUE(
        logs(*driver,
             R"(channel stalled MESSAGE_ID="driver-stall" CHANNEL="line1")"));
    const std::uint64_t at_stall = driver_status().heartbeat;
    EXPECT_TRUE(ends_with(*driver, EXIT_STALL));
    EXPECT_TRUE(
        between(since(started), milliseconds{3000}, milliseconds{5000}));
    EXPECT_EQ(driver_status().heartbeat, at_stall);
}

TEST_F(DriverProcess, AStallStopsTheOtherWorkers) {
    start_tagd(
        {{.name = "a", .tags = {"d1.v"}}, {.name = "b", .tags = {"d2.v"}}});
    // One simulator per channel: a simulator serves one connection at a
    // time, and channel a must be connected to be seen closing.
    auto sim = start_sim({"--set", "1", "0", "0", "1"});
    auto other = start_sim({});
    if (!sim.has_value() || !other.has_value()) {
        FAIL() << "no simulator";
    }
    observe();
    auto driver = start_driver(
        joined(tcp_channel("a", port_of(*sim)), device("d1", 1, "0"),
               faulty("b", port_of(*other), "--hang"), device("d2", 2, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    ASSERT_TRUE(eventually(handle("d1.v"), [](const scada_value& value) {
        return good_with(value, 1);
    }));
    EXPECT_TRUE(ends_with(*driver, EXIT_STALL));
    EXPECT_TRUE(has(driver->err(), R"(CHANNEL="b")"));
    EXPECT_TRUE(
        has(driver->err(), R"(MESSAGE_ID="driver-stall-stopped" STOPPED="1")"))
        << driver->err();
    EXPECT_TRUE(prints(*sim, "peer closed"));
}

TEST_F(DriverProcess, ASpinningWorkerIsAStallToo) {
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto sim = start_sim({});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    auto driver = start_driver(
        joined(faulty("line1", port_of(*sim), "--spin"), device("d1", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(ends_with(*driver, EXIT_STALL));
}

TEST_F(DriverProcess, APauseOfTheWholeProcessIsNotAStall) {
    // Review M7: the process is stopped past its 1 s stall limit (until
    // the library sees it down) and continued; the watchdog judges its
    // workers from the continue on, and nothing stalls.
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto sim = start_sim({"--set", "1", "0", "0", "6"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    auto driver =
        start_driver(joined(tcp_channel("line1", port_of(*sim), "300"),
                            device("d1", 1, "0", "300")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    const std::uint32_t tag = handle("d1.v");
    ASSERT_TRUE(eventually(
        tag, [](const scada_value& value) { return good_with(value, 6); }));
    ASSERT_TRUE(pause_until_down(*driver, tag));  // 1.5 s heartbeat timeout
    ASSERT_TRUE(eventually(tag, [](const scada_value& value) {
        return !owner_down(value) && good_with(value, 6);
    }));
    ASSERT_TRUE(logs(*driver, R"(MESSAGE_ID="driver-paused")"));
    EXPECT_TRUE(stops_cleanly(*driver) && !has(driver->err(), "stalled"))
        << driver->err();
}

TEST_F(DriverProcess, AnIdleWorkerAndAConnectingOneDoNotStall) {
    // 2.6: channel `idle` polls every 10 s with a stall limit of 3 s;
    // channel `wait` is in connects of 2 s (a peer whose accept queue is
    // full) with a stall limit of 6 s. Neither ends the process while the
    // idle channel reads twice more (20 s, longer than both limits).
    start_tagd({{.name = "idle", .tags = {"d1.v"}},
                {.name = "wait", .tags = {"d2.v"}}});
    auto sim = start_sim({"--set", "1", "0", "0", "3"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    const LocalPort full(true);
    auto driver = start_driver(
        joined(tcp_channel("idle", port_of(*sim)), device("d1", 1, "2"),
               tcp_channel("wait", full.port(), "2000"), device("d2", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    ASSERT_TRUE(prints_times(*sim, "request ", 3, milliseconds{25'000}))
        << driver->err();
    EXPECT_FALSE(driver->wait_for_ending(milliseconds{0}).has_value());
    EXPECT_TRUE(stops_cleanly(*driver));
    EXPECT_FALSE(has(driver->err(), "stalled"));
}

// --- The start (review M11) -------------------------------------------------

TEST_F(DriverProcess, TheStartEndsHaveTheirExitStatus) {
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    const LocalPort port(false);
    auto help = start_driver({"--help"});
    auto usage = start_driver(
        joined(tcp_channel("line1", port.port()), device("d1", 1, "0", "-5")));
    // A point the tag set has no tag for.
    auto missing = start_driver(
        joined(tcp_channel("line1", port.port()), device("d9", 1, "0")));
    if (!help.has_value() || !usage.has_value() || !missing.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(ends_with(*help, EXIT_OK) &&
                has(help->out(), "usage: scada-drv-test"));
    EXPECT_TRUE(ends_with(*usage, EXIT_USAGE));
    EXPECT_TRUE(fails_start(*missing, "the tag set has no tag d9.v"));
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

}  // namespace
}  // namespace scada::test::driver_it
