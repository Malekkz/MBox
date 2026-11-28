// The log records of a device's failure rules and of a channel's connection,
// through a running driver, its tag database and the simulator (driver
// interface contract 11.3; decision D-149): demoted, recovered, connection
// lost, refused and closed by the driver, and points absent.

#include "driver_fixture.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace scada::test::driver_it {
namespace {

TEST_F(DriverProcess, ADeviceThatDiesAndComesBackIsDemotedAndRecovered) {
    // d1's unit is silent for its first three requests, then answers: with
    // no retries, three time-outs demote it, and the first probe, a
    // back-off (10 s) later, recovers it. d2 keeps the connection up. The
    // engine's unit tests show that failed probes are no new demotion.
    start_tagd({{.name = "line1", .tags = {"d1.v", "d2.v"}}});
    std::optional<ChildProcess> sim =
        start_sim({"--set", "1", "0", "0", "5", "--set", "2", "0", "0", "7",
                   "--silent-for", "1", "3"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    std::optional<ChildProcess> driver = start_driver(joined(
        tcp_channel("line1", port_of(*sim)), device("d1", 1, "0", "100"),
        std::vector<std::string>{"--retries", "0"}, device("d2", 2, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(logs(*driver, R"(MESSAGE_ID="driver-device-demoted")"));
    EXPECT_TRUE(logs(*driver, R"(MESSAGE_ID="driver-device-recovered")"));
    EXPECT_TRUE(has(driver->err(), R"(DEVICE="d1")"));
    EXPECT_FALSE(has(driver->err(),
                     R"(driver-device-demoted" CHANNEL="line1" DEVICE="d2")"));
    EXPECT_TRUE(stops_cleanly(*driver));
    // One of each.
    EXPECT_EQ(count_of(driver->err(), "driver-device-demoted"), 1U)
        << driver->err();
    EXPECT_EQ(count_of(driver->err(), "driver-device-recovered"), 1U);
}

TEST_F(DriverProcess, APeerThatClosesTheConnectionIsALoss) {
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    std::optional<ChildProcess> sim =
        start_sim({"--set", "1", "0", "0", "5", "--close-after", "5"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    std::optional<ChildProcess> driver = start_driver(
        joined(tcp_channel("line1", port_of(*sim)), device("d1", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(logs(*driver,
                     R"(MESSAGE_ID="driver-connection-lost" CHANNEL="line1" )"
                     R"(REASON="peer")"));
    EXPECT_TRUE(stops_cleanly(*driver));
}

TEST_F(DriverProcess, ARefusedConnectIsLogged) {
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    const LocalPort gone(false);
    std::optional<ChildProcess> driver = start_driver(
        joined(tcp_channel("line1", gone.port()), device("d1", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(logs(
        *driver, R"(MESSAGE_ID="driver-connection-refused" CHANNEL="line1")"));
    EXPECT_TRUE(stops_cleanly(*driver));
}

TEST_F(DriverProcess, ARunOfTimeOutsIsClosedByTheDriverAndLost) {
    // The only unit is silent: its time-outs span every live unit, so the
    // driver closes the connection itself, and its rules of a loss apply.
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    std::optional<ChildProcess> sim = start_sim({"--silent", "1"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    std::optional<ChildProcess> driver = start_driver(joined(
        tcp_channel("line1", port_of(*sim)), device("d1", 1, "0", "100")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(logs(*driver,
                     R"(MESSAGE_ID="driver-connection-closed" CHANNEL="line1" )"
                     R"(REASON="time-outs")"));
    EXPECT_TRUE(logs(*driver,
                     R"(MESSAGE_ID="driver-connection-lost" CHANNEL="line1" )"
                     R"(REASON="time-outs")"));
    EXPECT_TRUE(stops_cleanly(*driver));
}

TEST_F(DriverProcess, AbsentPointsGiveOneRecordPerConnection) {
    // d1's point is at register 10, which the simulator does not serve: an
    // illegal address at every poll, one record for the connection.
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    std::optional<ChildProcess> sim = start_sim({});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    std::optional<ChildProcess> driver = start_driver(
        joined(tcp_channel("line1", port_of(*sim)),
               std::vector<std::string>{"--device", "d1", "1", "--point", "v",
                                        "10", "0"}));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_TRUE(logs(*driver, R"(MESSAGE_ID="driver-point-absent" CHANNEL=)"
                              R"("line1" DEVICE="d1" POINTS="1" COUNT="1")"));
    // Its absent block is not asked again (6.3); one record, not one per
    // answer, either way.
    EXPECT_TRUE(stops_cleanly(*driver));
    EXPECT_EQ(count_of(driver->err(), "driver-point-absent"), 1U)
        << driver->err();
}

}  // namespace
}  // namespace scada::test::driver_it
