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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
    // READY (the watchdog is never fed before it), then the watchdog at
    // half its interval.
    ASSERT_EQ(listener().receive(WAIT_LIMIT), "READY=1") << driver->err();
    for (int fed = 0; fed < 3; ++fed) {
        EXPECT_EQ(listener().receive(WAIT_LIMIT), "WATCHDOG=1");
    }
    EXPECT_TRUE(stops_on(*driver, listener(), SIGTERM, EXIT_OK));
}

TEST_F(DriverProcess, TheWatchdogIsNotFedBeforeReady) {
    // The service manager's watchdog runs from READY=1 on (D-142). The
    // resolve of 256 tags beats once before READY, and with a watchdog of
    // 1 ms that beat would feed it; READY=1 still comes first.
    std::vector<std::string> tags;
    std::vector<std::string> args =
        tcp_channel("line1", LocalPort(false).port());
    args.emplace_back("--disabled");
    for (const std::string& part : device("d1", 1, "0")) {
        if (part == "--point") {
            break;
        }
        args.push_back(part);
    }
    for (int point = 0; point < 256; ++point) {
        tags.push_back("d1.p" + std::to_string(point));
        for (std::string part :
             {std::string("--point"), "p" + std::to_string(point),
              std::to_string(point), std::string("0")}) {
            args.push_back(std::move(part));
        }
    }
    start_tagd({{.name = "line1", .tags = tags}});
    auto driver =
        start_driver(args, {.watchdog = std::chrono::microseconds{1000}});
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_EQ(listener().receive(WAIT_LIMIT), "READY=1") << driver->err();
    EXPECT_EQ(listener().receive(WAIT_LIMIT), "WATCHDOG=1");
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
    // A short absolute path, whatever TMPDIR is: a socket path is at most
    // 107 bytes.
    auto dead =
        start_driver(args, {.socket = "/nonexistent-scada-notify.sock"});
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
    // The worker waits for its next poll in 10 s, its wait capped at 1 s;
    // the peer goes (the simulator is killed) during that wait: POLLRDHUP
    // on the connection ends it, and the tag goes COMM_FAIL well before the
    // cap.
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
    const std::int64_t killed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            SteadyClock::now().time_since_epoch())
            .count();
    ASSERT_TRUE(end_with(*sim, SIGKILL).has_value());
    const std::optional<scada_change> failed =
        next_change_where([](const scada_change& change) {
            return (change.quality_flags & SCADA_FLAG_COMM_FAIL) != 0;
        });
    if (!failed.has_value()) {
        FAIL() << driver->err();
    }
    EXPECT_LT(failed->update_mono_ns - killed_ns, 600'000'000);
    EXPECT_TRUE(stops_cleanly(*driver));
}

// Channel a reads unit 1 of its own simulator; channel b hangs in its first
// receive (`--hang`), deaf to the stop, on another. `start` returns once
// b's request has reached its simulator, so that b is in its hang.
class HungChannel : public DriverProcess {
protected:
    ::testing::AssertionResult start(const std::string& connect_ms,
                                     const std::string& timeout_ms) {
        start_tagd(tags_);
        std::optional<ChildProcess> live =
            start_sim({"--set", "1", "0", "0", "1"});
        std::optional<ChildProcess> hung = start_sim({});
        if (!live.has_value() || !hung.has_value()) {
            return ::testing::AssertionFailure() << "no simulator";
        }
        live_ = std::make_unique<ChildProcess>(std::move(*live));
        hung_ = std::make_unique<ChildProcess>(std::move(*hung));
        std::optional<ChildProcess> driver = start_driver(
            joined(tcp_channel("a", port_of(*live_)), device("d1", 1, "0"),
                   faulty("b", port_of(*hung_), "--hang", connect_ms),
                   device("d2", 1, "0", timeout_ms)));
        if (!driver.has_value()) {
            return ::testing::AssertionFailure() << "no driver";
        }
        driver_ = std::make_unique<ChildProcess>(std::move(*driver));
        const ::testing::AssertionResult ready =
            receives(listener(), "READY=1");
        return ready ? prints(*hung_, "request ") : ready;
    }

    ChildProcess& driver() { return *driver_; }

    // Stops the driver for 150 ms every 500 ms until it ends, at most 8 s;
    // its end, if it came.
    std::optional<ChildEnding> pause_repeatedly() {
        const auto started = SteadyClock::now();
        std::optional<ChildEnding> ending;
        while (since(started) < milliseconds{8000} &&
               driver_->send_signal(SIGSTOP)) {
            // A stopped driver does not run, but one that was already
            // exiting may still end here: keep that end, or the next signal
            // fails and the end is lost.
            ending = driver_->wait_for_ending(milliseconds{150});
            if (ending.has_value() || !driver_->send_signal(SIGCONT)) {
                break;
            }
            ending = driver_->wait_for_ending(milliseconds{350});
            if (ending.has_value()) {
                break;
            }
        }
        return ending;
    }
    [[nodiscard]] const std::vector<ChannelTags>& tags() const { return tags_; }

private:
    const std::vector<ChannelTags> tags_{{.name = "a", .tags = {"d1.v"}},
                                         {.name = "b", .tags = {"d2.v"}}};
    std::unique_ptr<ChildProcess> live_;
    std::unique_ptr<ChildProcess> hung_;
    std::unique_ptr<ChildProcess> driver_;
};

TEST_F(HungChannel, AStopKeepsTheHeartbeatAndEndsInTheBudget) {
    // b's stall limit (3 s) is longer than the stop budget (2 s). The stop
    // waits for the workers with the heartbeat going on (the process is
    // never down while it runs), then ends with the stall status within the
    // budget.
    ASSERT_TRUE(start("1000", "500"));
    observe();
    const auto stop = SteadyClock::now();
    ASSERT_TRUE(driver().send_signal(SIGTERM));
    bool down = false;
    const std::optional<ChildEnding> ending = end_watching(driver(), down);
    if (!ending.has_value()) {
        FAIL() << "did not end";
    }
    EXPECT_FALSE(down);
    EXPECT_TRUE(
        !ending->by_signal && ending->number == EXIT_STALL &&
        has(driver().err(), R"(MESSAGE_ID="driver-stop-overrun" CHANNEL="b")"))
        << driver().err();
    EXPECT_TRUE(between(since(stop), milliseconds{1900}, milliseconds{2900}));
}

TEST_F(HungChannel, TheStallCheckGoesOnWhileStopping) {
    // b's stall limit is 1 s, below the stop budget: a stop once b hangs
    // ends with the stall status when its limit passes, not at the end of
    // the budget. (Without waiting for b's request, a stop that came before
    // b's first step ended the run with 0: the race of the earlier version.)
    ASSERT_TRUE(start("300", "300"));
    ASSERT_TRUE(driver().send_signal(SIGTERM));
    EXPECT_TRUE(ends_with(driver(), EXIT_STALL, milliseconds{1700}));
}

TEST_F(HungChannel, ARetiringGenerationEndsInTheBudget) {
    // Review M1: the generation retires (a changed tag set) while b hangs:
    // the run fails, and the bounded stop ends the process instead of a
    // join that would never return.
    ASSERT_TRUE(start("1000", "500"));
    restart_tagd(&tags(), {"spare.v"});
    EXPECT_TRUE(ends_with(driver(), EXIT_ERROR, milliseconds{10'000}));
    EXPECT_TRUE(has(driver().err(), R"(MESSAGE_ID="driver-retiring")") &&
                has(driver().err(), R"(MESSAGE_ID="driver-stop-overrun")"));
}

TEST_F(HungChannel, ShortPausesOfTheProcessDoNotHideAStall) {
    // Delta 1: the process is stopped for 150 ms every 500 ms while b
    // hangs (limit 1 s). Only the time the main thread lost is credited,
    // so the stall is still found: within 1 s of b's own time, about
    // 1.3 s, well under 4 s.
    ASSERT_TRUE(start("300", "300"));
    const auto started = SteadyClock::now();
    const std::optional<ChildEnding> ending = pause_repeatedly();
    if (!ending.has_value()) {
        FAIL() << "the stall was never found: " << driver().err();
    }
    EXPECT_TRUE(!ending->by_signal && ending->number == EXIT_STALL)
        << driver().err();
    EXPECT_LT(since(started), milliseconds{4000});
    EXPECT_LE(count_of(driver().err(), R"(MESSAGE_ID="driver-paused")"), 1U);
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

// Success if `err` shows the path of AStallLogsTheEventsItHeld: two
// restart records and one stall, the stall within the first record's 10 s
// period, and the second record logged after the stall, by its exit.
::testing::AssertionResult held_until_the_stall(std::string_view err) {
    constexpr std::string_view RESTART =
        R"(MESSAGE_ID="driver-device-restart")";
    constexpr std::string_view STALL = R"(MESSAGE_ID="driver-stall")";
    constexpr std::int64_t PERIOD_MS = 10'000;
    const std::vector<std::int64_t> restarts = record_times_ms(err, RESTART);
    const std::vector<std::int64_t> stalls = record_times_ms(err, STALL);
    if (restarts.size() != 2 || stalls.size() != 1) {
        return ::testing::AssertionFailure()
               << restarts.size() << " restart records, " << stalls.size()
               << " stalls";
    }
    if (stalls[0] - restarts[0] >= PERIOD_MS || restarts[1] < stalls[0] ||
        err.find(STALL) > err.rfind(RESTART)) {
        return ::testing::AssertionFailure()
               << "restarts at " << restarts[0] << " and " << restarts[1]
               << " ms, the stall at " << stalls[0] << " ms";
    }
    return ::testing::AssertionSuccess();
}

TEST_F(DriverProcess, AStallLogsTheEventsItHeld) {
    // Channel a's device restarts at its 3rd and 6th request, about a
    // second apart: the first record at once, the second held for its 10 s
    // period. Channel b hangs and stalls about 4.5 s in (its 1.5 s time-out
    // makes its stall limit three times that), well before the period
    // ends; the stall's exit logs the held record (PR 177 S6). The record
    // times show that path was taken: the second after the stall, the stall
    // within the period of the first.
    start_tagd(
        {{.name = "a", .tags = {"d1.v"}}, {.name = "b", .tags = {"d2.v"}}});
    auto sim =
        start_sim({"--set", "1", "0", "0", "5", "--set-after", "1", "0", "0",
                   "3", "3", "--set-after", "1", "0", "0", "1", "6"});
    auto other = start_sim({});
    if (!sim.has_value() || !other.has_value()) {
        FAIL() << "no simulator";
    }
    observe();
    auto driver = start_driver(joined(
        tcp_channel("a", port_of(*sim)), device("d1", 1, "0"),
        std::vector<std::string>{"--restart-decreases", "0"},
        faulty("b", port_of(*other), "--hang"), device("d2", 2, "0", "1500")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    ASSERT_TRUE(
        eventually(handle("sys.dev.d1.restarts"), [](const scada_value& value) {
            return value.quality == SCADA_QUALITY_GOOD && value.value == 2;
        }));
    EXPECT_TRUE(ends_with(*driver, EXIT_STALL));
    const std::string err = driver->err();
    EXPECT_EQ(restart_counts(err), 2U) << err;
    EXPECT_TRUE(held_until_the_stall(err)) << err;
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
    // READY first, so that the pause comes after the start; under load it
    // can still come before the first check, which is credited too (the
    // start-time hole, seen by this test under asan-ubsan).
    ASSERT_TRUE(receives(listener(), "READY=1")) << driver->err();
    observe();
    const std::uint32_t tag = handle("d1.v");
    ASSERT_TRUE(eventually(
        tag, [](const scada_value& value) { return good_with(value, 6); }));
    ASSERT_TRUE(pause_until_down(*driver, tag));  // 1.5 s heartbeat timeout
    const bool back = eventually(tag, [](const scada_value& value) {
        return !owner_down(value) && good_with(value, 6);
    });
    const scada_value last = read(tag);
    ASSERT_TRUE(back) << "quality " << int{last.quality} << " flags "
                      << last.quality_flags << " ended "
                      << driver->wait_for_ending(milliseconds{0}).has_value()
                      << "\ndriver:\n"
                      << driver->err() << "\nsimulator:\n"
                      << sim->out();
    EXPECT_TRUE(logs(*driver, R"(MESSAGE_ID="driver-paused")") &&
                stops_cleanly(*driver) && !has(driver->err(), "stalled"))
        << driver->err();
}

TEST_F(DriverProcess, APauseWithOnlyADisabledChannelIsNotAStall) {
    // A disabled channel's worker waits for the stop, waking every third of
    // its 1 s stall limit to move its counter (2.6). The process is stopped
    // from its 9.1 write until the library sees it down, past that limit,
    // and continued: the pause is logged and nothing stalls. (Whether the
    // stop comes before the first check, the start-time hole of D-142,
    // cannot be chosen from outside the process; the unit test
    // StallWatch.APauseBeforeTheFirstCheckIsCredited pins that case.)
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    observe();
    const std::uint32_t tag = handle("d1.v");
    std::vector<std::string> off =
        tcp_channel("line1", LocalPort(false).port(), "300");
    off.emplace_back("--disabled");
    auto driver = start_driver(joined(off, device("d1", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    // The written flags, whether or not the owner reads as up yet.
    constexpr std::uint16_t WRITTEN =
        SCADA_FLAG_NOT_INIT | SCADA_FLAG_LOCAL_TIME;
    ASSERT_TRUE(eventually(tag, [](const scada_value& value) {
        return (value.quality_flags & WRITTEN) == WRITTEN;
    })) << driver->err();
    ASSERT_TRUE(pause_until_down(*driver, tag));
    EXPECT_TRUE(logs(*driver, R"(MESSAGE_ID="driver-paused")") &&
                stops_cleanly(*driver) && !has(driver->err(), "stalled"))
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
