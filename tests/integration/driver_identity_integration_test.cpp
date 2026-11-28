// A device's identity check and restart rule through a running driver, its
// tag database and the simulator (driver interface contract 3.6, 7.6, 11.1;
// decision D-147): a match, a mismatch with `refuse` and with `warn`, a hot
// swap, and a restart, each in its tags and its log record.

#include "driver_fixture.h"

#include "scada/tag/scada_tag.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::test::driver_it {
namespace {

// d1 on unit 1: `v` at register 0 and `model` at register 1, both fast;
// the check is that `model` is 7. The simulator holds 5 and `model`.
class Identity : public DriverProcess {
protected:
    struct Running {
        ChildProcess sim;
        ChildProcess driver;
    };

    std::optional<Running> run(const std::string& model,
                               const std::vector<std::string>& sim_extra,
                               const std::vector<std::string>& device_extra) {
        start_tagd({{.name = "line1",
                     .tags = {"d1.v", "d1.model"},
                     .identified = {"d1"}}});
        std::vector<std::string> sim_args{"--set", "1", "0", "0", "5",
                                          "--set", "1", "0", "1", model};
        sim_args.insert(sim_args.end(), sim_extra.begin(), sim_extra.end());
        std::optional<ChildProcess> sim = start_sim(sim_args);
        if (!sim.has_value()) {
            return std::nullopt;
        }
        std::vector<std::string> checked =
            joined(device("d1", 1, "0"),
                   std::vector<std::string>{"--point", "model", "1", "0",
                                            "--identify-equals", "1", "7"});
        checked.insert(checked.end(), device_extra.begin(), device_extra.end());
        std::optional<ChildProcess> driver =
            start_driver(joined(tcp_channel("line1", port_of(*sim)), checked));
        if (!driver.has_value()) {
            return std::nullopt;
        }
        observe();
        return Running{.sim = std::move(*sim), .driver = std::move(*driver)};
    }

    // Waits until the INT64 tag `name` is GOOD with `expected`.
    bool reaches(const std::string& name, std::int64_t expected) {
        return eventually(handle(name), [&](const scada_value& value) {
            return value.quality == SCADA_QUALITY_GOOD &&
                   std::bit_cast<std::int64_t>(value.value) == expected;
        });
    }
    // Waits until the BOOL tag `name` is GOOD with `expected`.
    bool reaches(const std::string& name, bool expected) {
        return eventually(handle(name), [&](const scada_value& value) {
            return value.quality == SCADA_QUALITY_GOOD &&
                   value.value == (expected ? 1U : 0U);
        });
    }
    ::testing::AssertionResult bad_without_comm_fail(const std::string& name) {
        const scada_value value = read(handle(name));
        if (value.quality == SCADA_QUALITY_BAD &&
            (value.quality_flags & SCADA_FLAG_COMM_FAIL) == 0) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure()
               << name << ": quality " << int{value.quality} << ", flags "
               << value.quality_flags;
    }
    // The mismatch's record, with its reason and the value read.
    static ::testing::AssertionResult logged_mismatch(ChildProcess& driver) {
        for (const char* line : {R"(MESSAGE_ID="driver-identity-mismatch")",
                                 R"(REASON="value")", R"(VALUE="8")"}) {
            const ::testing::AssertionResult found = logs(driver, line);
            if (!found) {
                return found;
            }
        }
        return ::testing::AssertionSuccess();
    }
    // The text of the STRING tag `name`, GOOD.
    std::string text_of(const std::string& name) {
        std::uint8_t quality = SCADA_QUALITY_BAD;
        const std::string text = read_text(handle(name), quality);
        return quality == SCADA_QUALITY_GOOD ? text : "<not good>";
    }
};

constexpr std::int64_t RUNNING = 3;
constexpr std::int64_t MISMATCH = 5;

TEST_F(Identity, AMatchingDeviceRunsAndShowsItsIdentity) {
    std::optional<Running> running = run("7", {}, {});
    if (!running.has_value()) {
        FAIL() << "no simulator or no driver";
    }
    EXPECT_TRUE(reaches("sys.dev.d1.state", RUNNING));
    EXPECT_TRUE(reaches("sys.dev.d1.identity_ok", true));
    EXPECT_EQ(text_of("sys.dev.d1.identity"), "7");
    EXPECT_TRUE(reaches("d1.v", std::int64_t{5}));
    EXPECT_TRUE(stops_cleanly(running->driver));
}

TEST_F(Identity, AMismatchRefusesTheDeviceAndIsLogged) {
    std::optional<Running> running = run("8", {}, {});
    if (!running.has_value()) {
        FAIL() << "no simulator or no driver";
    }
    EXPECT_TRUE(reaches("sys.dev.d1.state", MISMATCH));
    EXPECT_TRUE(reaches("sys.dev.d1.identity_ok", false));
    EXPECT_EQ(text_of("sys.dev.d1.identity"), "8");
    // Its points BAD without COMM_FAIL (3.6).
    EXPECT_TRUE(bad_without_comm_fail("d1.v"));
    EXPECT_TRUE(logged_mismatch(running->driver));
    EXPECT_TRUE(stops_cleanly(running->driver));
}

TEST_F(Identity, AMismatchWithWarnRunsTheDevice) {
    std::optional<Running> running = run("8", {}, {"--on-mismatch-warn"});
    if (!running.has_value()) {
        FAIL() << "no simulator or no driver";
    }
    EXPECT_TRUE(reaches("sys.dev.d1.state", RUNNING));
    EXPECT_TRUE(reaches("sys.dev.d1.identity_ok", false));
    EXPECT_TRUE(reaches("d1.v", std::int64_t{5}));
    EXPECT_TRUE(stops_cleanly(running->driver));
}

TEST_F(Identity, AHotSwappedDeviceIsRefusedAndWithheld) {
    // A running device unplugged and another model plugged in: the
    // simulator closes the connection after 20 answers, and from the 20th
    // request on `model` reads 8. The check after the reconnect refuses
    // it, and its points go BAD without COMM_FAIL (PR 177 M3).
    std::optional<Running> running = run(
        "7", {"--set-after", "1", "0", "1", "8", "20", "--close-after", "20"},
        {});
    if (!running.has_value()) {
        FAIL() << "no simulator or no driver";
    }
    EXPECT_TRUE(reaches("sys.dev.d1.state", MISMATCH));
    EXPECT_TRUE(reaches("sys.dev.d1.identity_ok", false));
    EXPECT_EQ(text_of("sys.dev.d1.identity"), "8");
    EXPECT_TRUE(bad_without_comm_fail("d1.v"));
    EXPECT_TRUE(logged_mismatch(running->driver));
    EXPECT_TRUE(stops_cleanly(running->driver));
}

TEST_F(Identity, ARestartIsCountedLoggedAndCheckedAgain) {
    // `v` falls from 5 to 3 after 20 requests to the unit: a restart.
    std::optional<Running> running =
        run("7", {"--set-after", "1", "0", "0", "3", "20"},
            {"--restart-decreases", "0"});
    if (!running.has_value()) {
        FAIL() << "no simulator or no driver";
    }
    EXPECT_TRUE(reaches("sys.dev.d1.restarts", std::int64_t{1}));
    EXPECT_TRUE(logs(running->driver, R"(MESSAGE_ID="driver-device-restart")"));
    // Checked again, then running with the new value.
    EXPECT_TRUE(reaches("d1.v", std::int64_t{3}));
    EXPECT_TRUE(reaches("sys.dev.d1.state", RUNNING));
    EXPECT_TRUE(stops_cleanly(running->driver));
}

TEST_F(Identity, TheRestartRecordsCountEveryRestart) {
    // Two restarts, `v` falling at the 20th and the 40th request, likely in
    // one 10 s period: the first record at once, the second's count held
    // and logged by the stop at the latest. The counts add up to
    // `restarts` (PR 177 S1): no record is lost at the stop, which drains
    // the worker's queue after the worker returned (11.3, D-148).
    std::optional<Running> running =
        run("7",
            {"--set-after", "1", "0", "0", "3", "20", "--set-after", "1", "0",
             "0", "1", "40"},
            {"--restart-decreases", "0"});
    if (!running.has_value()) {
        FAIL() << "no simulator or no driver";
    }
    EXPECT_TRUE(reaches("sys.dev.d1.restarts", std::int64_t{2}));
    EXPECT_TRUE(stops_cleanly(running->driver));
    EXPECT_EQ(restart_counts(running->driver.err()), 2U);
    // The main thread's own last records went through its queue too.
    EXPECT_TRUE(has(running->driver.err(), R"(MESSAGE_ID="driver-stopping")"));
}

TEST_F(Identity, ASecondRestartIsLoggedAfterItsPeriod) {
    // Two restarts a few seconds apart: the first record at once, the
    // second, a summary, only when the 10 s period has passed (PR 177 delta,
    // item 3): a driver that logged each at once would write them seconds
    // apart.
    constexpr std::string_view RECORD = R"(MESSAGE_ID="driver-device-restart")";
    constexpr std::int64_t PERIOD_MS = 10'000;
    constexpr std::int64_t DAY_MS = 86'400'000;
    std::optional<Running> running =
        run("7",
            {"--set-after", "1", "0", "0", "3", "20", "--set-after", "1", "0",
             "0", "1", "40"},
            {"--restart-decreases", "0"});
    if (!running.has_value()) {
        FAIL() << "no simulator or no driver";
    }
    EXPECT_TRUE(reaches("sys.dev.d1.restarts", std::int64_t{2}));
    ASSERT_TRUE(running->driver.wait_for_error(
        [&](std::string_view err) { return count_of(err, RECORD) >= 2; },
        WAIT_LIMIT))
        << running->driver.err();
    const std::vector<std::int64_t> times =
        record_times_ms(running->driver.err(), RECORD);
    ASSERT_EQ(times.size(), 2U);
    // Across midnight too; the clock of the log is the wall clock, so a
    // little less than the monotonic period is allowed.
    const std::int64_t apart = (times[1] - times[0] + DAY_MS) % DAY_MS;
    EXPECT_GE(apart, PERIOD_MS - 100) << running->driver.err();
    EXPECT_EQ(restart_counts(running->driver.err()), 2U);
    EXPECT_TRUE(stops_cleanly(running->driver));
}

TEST_F(DriverProcess, AFullWorkerQueuesDropsShowInLogDropped) {
    // Four devices of one unit, d1 with a restart rule: one restart is the
    // unit's, so the worker pushes four restart records in one step into a
    // queue of one record (`--worker-log-queue 1`). The logger drains the
    // queue at most every 20 ms, so records are dropped, and
    // `sys.proc.drv-test.log_dropped` shows them (11.3, D-148).
    start_tagd({{.name = "line1", .tags = {"d1.v", "d2.v", "d3.v", "d4.v"}}});
    std::optional<ChildProcess> sim = start_sim(
        {"--set", "1", "0", "0", "5", "--set-after", "1", "0", "0", "3", "20"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    std::optional<ChildProcess> driver = start_driver(joined(
        std::vector<std::string>{"--worker-log-queue", "1"},
        tcp_channel("line1", port_of(*sim)), device("d1", 1, "0"),
        std::vector<std::string>{"--restart-decreases", "0"},
        device("d2", 1, "0"), device("d3", 1, "0"), device("d4", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    EXPECT_TRUE(
        eventually(handle("sys.dev.d4.restarts"), [](const scada_value& value) {
            return value.quality == SCADA_QUALITY_GOOD && value.value == 1;
        }));
    EXPECT_TRUE(eventually(
        handle("sys.proc.drv-test.log_dropped"), [](const scada_value& value) {
            return value.quality == SCADA_QUALITY_GOOD && value.value >= 1;
        }));
    EXPECT_TRUE(logs(*driver, R"(log records dropped: the queue was full)"));
    // The notice names the worker's queue by its channel (PR 182 S2).
    EXPECT_TRUE(logs(*driver, R"(QUEUE="line1")"));
    EXPECT_TRUE(stops_cleanly(*driver));
}

// `devices` devices d1, d2, ... of unit 1 on channel line1, d1 with a
// restart rule on `v`: a restart d1 sees is the unit's, so every device
// restarts in the same step. The worker's log queue holds `queue` records.
// d1 and d2 of unit 1, d1 with a restart after 20 requests, logging into a
// slow journal that refuses every record (`--refuse-log MS` writes each to
// standard error and reports it refused MS ms later). After the stop, the
// last `log_dropped` must equal the records written (PR 182 S1, S5).
class RefusedJournal : public DriverProcess {
protected:
    void check(const std::string& delay_ms) {
        start_tagd({{.name = "line1", .tags = {"d1.v", "d2.v"}}});
        std::optional<ChildProcess> sim =
            start_sim({"--set", "1", "0", "0", "5", "--set-after", "1", "0",
                       "0", "3", "20"});
        if (!sim.has_value()) {
            FAIL() << "no simulator";
        }
        std::optional<ChildProcess> driver = start_driver(
            joined(std::vector<std::string>{"--refuse-log", delay_ms},
                   tcp_channel("line1", port_of(*sim)), device("d1", 1, "0"),
                   std::vector<std::string>{"--restart-decreases", "0"},
                   device("d2", 1, "0")));
        if (!driver.has_value()) {
            FAIL() << "no driver";
        }
        observe();
        EXPECT_TRUE(eventually(
            handle("sys.dev.d2.restarts"), [](const scada_value& value) {
                return value.quality == SCADA_QUALITY_GOOD && value.value == 1;
            }));
        EXPECT_TRUE(stops_cleanly(*driver));
        const std::string err = driver->err();
        // A record is a line "<time> <LEVEL> scada-drv-test: ...".
        std::size_t records = 0;
        for (std::size_t at = 0; at < err.size();) {
            const std::size_t end = std::min(err.find('\n', at), err.size());
            const std::string_view line =
                std::string_view(err).substr(at, end - at);
            if (line.starts_with("20") &&
                line.find(" scada-drv-test: ") != std::string_view::npos) {
                ++records;
            }
            at = end + 1;
        }
        EXPECT_GE(records, 4U) << err;  // running, two restarts, stopping
        const scada_value dropped =
            read(handle("sys.proc.drv-test.log_dropped"));
        EXPECT_EQ(std::bit_cast<std::int64_t>(dropped.value),
                  static_cast<std::int64_t>(records))
            << err;
    }
};

TEST_F(RefusedJournal, TheLastLogDroppedCountsTheDrainsRefusals) {
    // 200 ms a record: the stop's records are still on their way when the
    // workers have returned. `log_dropped` is written after the drain of
    // every queue, which waits for them: one written before the drain, or
    // no drain at all (the Logger's destructor delivers them later),
    // counts fewer than the lines.
    check("200");
}

TEST_F(RefusedJournal, RecordsTheDrainLeftUndeliveredCountAsDropped) {
    // 1 s a record: the restart records and the stop's are still queued
    // when the drain's 1 s runs out. Those it left count as dropped (S5);
    // the Logger's destructor then delivers them to the refusing journal,
    // so the lines match the count again.
    check("1000");
}

// The least size of a worker's queue (MIN_WORKER_LOG_QUEUE).
constexpr std::size_t QUEUE = 64;

class UnitRestarts : public DriverProcess {
protected:
    struct Running {
        ChildProcess sim;
        ChildProcess driver;
    };

    std::optional<Running> run(std::size_t devices, std::size_t queue,
                               const std::vector<std::string>& sim_extra) {
        std::vector<std::string> tags;
        std::vector<std::string> args{"--worker-log-queue",
                                      std::to_string(queue)};
        for (std::size_t index = 1; index <= devices; ++index) {
            const std::string name = "d" + std::to_string(index);
            tags.push_back(name + ".v");
        }
        start_tagd({{.name = "line1", .tags = tags}});
        std::vector<std::string> sim_args{"--set", "1", "0", "0", "5"};
        sim_args.insert(sim_args.end(), sim_extra.begin(), sim_extra.end());
        std::optional<ChildProcess> started = start_sim(sim_args);
        if (!started.has_value()) {
            return std::nullopt;
        }
        ChildProcess& sim = *started;
        args = joined(args, tcp_channel("line1", port_of(sim)));
        for (std::size_t index = 1; index <= devices; ++index) {
            args = joined(args, device("d" + std::to_string(index), 1, "0"));
            if (index == 1) {
                args = joined(
                    args, std::vector<std::string>{"--restart-decreases", "0"});
            }
        }
        std::optional<ChildProcess> driver = start_driver(args);
        if (!driver.has_value()) {
            return std::nullopt;
        }
        observe();
        return Running{.sim = std::move(sim), .driver = std::move(*driver)};
    }
    // Waits until every device's `restarts` is GOOD with `expected`; their
    // sum.
    std::uint64_t restarts(std::size_t devices, std::int64_t expected) {
        std::uint64_t sum = 0;
        for (std::size_t index = 1; index <= devices; ++index) {
            const std::string name =
                "sys.dev.d" + std::to_string(index) + ".restarts";
            const bool reached =
                eventually(handle(name), [&](const scada_value& value) {
                    return value.quality == SCADA_QUALITY_GOOD &&
                           std::bit_cast<std::int64_t>(value.value) == expected;
                });
            if (!reached) {
                ADD_FAILURE() << name << " is not " << expected;
                return sum;  // the others would wait as long
            }
            sum += static_cast<std::uint64_t>(expected);
        }
        return sum;
    }
};

TEST_F(UnitRestarts, ABurstLargerThanTheQueueLosesNoCount) {
    // 200 devices restart in one step into a queue of 64 records: 64 go at
    // once, and the queue refuses the rest. Each refused record gives its
    // count back to the limit, so the summaries after the 10 s period
    // carry them, and the counts add up to `restarts` before the stop (PR
    // 182 M2).
    constexpr std::size_t DEVICES = 200;
    std::optional<Running> running =
        run(DEVICES, QUEUE, {"--set-after", "1", "0", "0", "3", "20"});
    if (!running.has_value()) {
        FAIL() << "no simulator or no driver";
    }
    ChildProcess& driver = running->driver;
    const std::uint64_t sum = restarts(DEVICES, 1);
    ASSERT_EQ(sum, DEVICES) << driver.err();
    EXPECT_TRUE(logs(driver, R"(log records dropped: the queue was full)"));
    EXPECT_TRUE(driver.wait_for_error(
        [&](std::string_view err) { return restart_counts(err) >= sum; },
        WAIT_LIMIT))
        << driver.err();
    EXPECT_TRUE(stops_cleanly(driver));
    EXPECT_EQ(restart_counts(driver.err()), sum);
}

TEST_F(UnitRestarts, TheStopWaitsForRoomForEveryHeldCount) {
    // 100 devices restart twice, a few cycles apart, within one 10 s period:
    // the first burst fills the queue of 64, and the second's counts are
    // held. At the stop, 100 summaries wait for a queue of 64: the stop
    // waits for room, and the counts add up to `restarts` (PR 182 M2).
    constexpr std::size_t DEVICES = 100;
    std::optional<Running> running =
        run(DEVICES, QUEUE,
            {"--set-after", "1", "0", "0", "3", "300", "--set-after", "1", "0",
             "0", "1", "800"});
    if (!running.has_value()) {
        FAIL() << "no simulator or no driver";
    }
    ChildProcess& driver = running->driver;
    const std::uint64_t sum = restarts(DEVICES, 2);
    ASSERT_EQ(sum, 2 * DEVICES) << driver.err();
    EXPECT_TRUE(stops_cleanly(driver));
    EXPECT_EQ(restart_counts(driver.err()), sum) << driver.err();
}

}  // namespace
}  // namespace scada::test::driver_it
