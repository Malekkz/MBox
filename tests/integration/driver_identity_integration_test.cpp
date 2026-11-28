// A device's identity check and restart rule through a running driver, its
// tag database and the simulator (driver interface contract 3.6, 7.6, 11.1;
// decision D-147): a match, a mismatch with `refuse` and with `warn`, a hot
// swap, and a restart, each in its tags and its log record.

#include "driver_fixture.h"

#include "scada/tag/scada_tag.h"

#include <gtest/gtest.h>

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
    // `restarts` (PR 177 S1).
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
}

}  // namespace
}  // namespace scada::test::driver_it
