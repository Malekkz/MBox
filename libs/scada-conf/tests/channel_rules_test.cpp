// Stage 5: the channels against the limits and each other (configuration
// schemas contract 4.1, 10.6 and Annex A.1; decision D-123; slice 7d-3 of
// #100). Addresses are from the documentation ranges (RFC 5737, RFC 3849).

#include "expand_cases.h"
#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/validate.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace scada::conf {
namespace {

constexpr std::string_view METER =
    "template: meter_a\nprotocol: modbus\npoints:\n"
    "  - { name: flow, address: { table: holding, register: 1, type: uint16 "
    "} }\n";

// `count` devices of meter_a named <prefix>1, <prefix>2, ...
std::string devices(std::string_view prefix, std::size_t count) {
    std::string text;
    for (std::size_t i = 1; i <= count; ++i) {
        text += "  - { name: " + std::string(prefix) + std::to_string(i) +
                ", template: meter_a, unit_id: " + std::to_string(i) + " }\n";
    }
    return text;
}

// An RTU channel `name` on `path`, line 2 its name, line 4 its path.
std::string rtu(std::string_view name, std::string_view path,
                std::string_view prefix, std::size_t count = 1) {
    return "channel:\n  name: " + std::string(name) +
           "\n  type: rtu\n  device: " + std::string(path) + "\ndevices:\n" +
           devices(prefix, count);
}

// A channel of `type` to `host`, line 4 its host, with `port` on line 5
// when given.
std::string tcp(std::string_view name, std::string_view host,
                std::string_view prefix, std::string_view port = {},
                std::string_view type = "tcp") {
    return "channel:\n  name: " + std::string(name) +
           "\n  type: " + std::string(type) + "\n  host: " + std::string(host) +
           "\n" +
           (port.empty() ? std::string()
                         : "  port: " + std::string(port) + "\n") +
           "devices:\n" + devices(prefix, 1);
}

// The diagnostics of the tree of meter_a and `files`: its errors, else its
// warnings.
Lines diagnostics_of(Files files) {
    files.try_emplace("templates/meter_a.yaml", std::string(METER));
    const Result<Validation> validation = validated(files);
    EXPECT_TRUE(validation.ok());
    if (!validation.ok()) {
        return {"not validated"};
    }
    const Report& report = validation.value().report;
    Lines found;
    for (const Diagnostic& diagnostic :
         report.errors.empty() ? report.warnings : report.errors) {
        found.push_back(format(diagnostic));
    }
    return found;
}

// --- CF-605: devices per channel -----------------------------------------

TEST(ChannelRules, TooManyDevicesOnAChannel) {
    Files files = {
        {"drivers/modbus/line1.yaml", rtu("line1", "/dev/ttyS1", "a", 3)},
        {"drivers/modbus/line2.yaml", rtu("line2", "/dev/ttyS2", "b", 2)}};
    files.emplace(
        "system.yaml",
        std::string(SYSTEM) + "limits: { max_devices_per_channel: 2 }\n");
    EXPECT_EQ(diagnostics_of(files),
              Lines{"drivers/modbus/line1.yaml:2:9: error CF-605: channel "
                    "line1 has 3 devices, the limit is 2"});
}

// The default limit is 247 (4.1).
TEST(ChannelRules, TheDefaultLimitIs247) {
    EXPECT_EQ(diagnostics_of({{"drivers/modbus/line1.yaml",
                               rtu("line1", "/dev/ttyS1", "a", 247)}}),
              Lines{});
    EXPECT_EQ(diagnostics_of({{"drivers/modbus/line1.yaml",
                               rtu("line1", "/dev/ttyS1", "a", 248)}}),
              Lines{"drivers/modbus/line1.yaml:2:9: error CF-605: channel "
                    "line1 has 248 devices, the limit is 247"});
}

// --- CF-620: one serial device path --------------------------------------

TEST(ChannelRules, TwoChannelsOnOneSerialPath) {
    EXPECT_EQ(
        diagnostics_of(
            {{"drivers/modbus/line1.yaml",
              rtu("line1", "/dev/serial/by-path/platform-serial1", "a")},
             {"drivers/modbus/line2.yaml",
              rtu("line2", "/dev/serial/by-path/platform-serial1", "b")},
             {"drivers/modbus/line3.yaml",
              rtu("line3", "/dev/serial/by-path/platform-serial1", "c")},
             {"drivers/modbus/line4.yaml",
              rtu("line4", "/dev/serial/by-path/platform-serial2", "d")}}),
        (Lines{"drivers/modbus/line2.yaml:4:11: error CF-620: channels line1 "
               "and line2 use /dev/serial/by-path/platform-serial1",
               "drivers/modbus/line3.yaml:4:11: error CF-620: channels line1 "
               "and line3 use /dev/serial/by-path/platform-serial1"}));
}

// --- CW-621: one host and port -------------------------------------------

TEST(ChannelRules, TwoChannelsToOneEndpoint) {
    EXPECT_EQ(
        diagnostics_of(
            {{"drivers/modbus/gw1.yaml", tcp("gw1", "192.0.2.10", "a")},
             {"drivers/modbus/gw2.yaml",
              tcp("gw2", "192.0.2.10", "b", "502", "rtu_over_tcp")},
             {"drivers/modbus/gw3.yaml", tcp("gw3", "192.0.2.10", "c", "1502")},
             {"drivers/modbus/gw4.yaml", tcp("gw4", "192.0.2.11", "d")}}),
        Lines{"drivers/modbus/gw2.yaml:4:9: warning CW-621: channels gw1 and "
              "gw2 both connect to 192.0.2.10:502; most devices allow one "
              "client"});
}

// Hosts are compared as addresses: two spellings of one IPv6 address, and
// an IPv4 address and its mapped IPv6 form, meet.
TEST(ChannelRules, HostsAreComparedAsAddresses) {
    EXPECT_EQ(
        diagnostics_of(
            {{"drivers/modbus/gw1.yaml",
              tcp("gw1", "2001:db8::a", "a", "1502")},
             {"drivers/modbus/gw2.yaml",
              tcp("gw2", "2001:DB8:0:0::A", "b", "1502")},
             {"drivers/modbus/gw3.yaml", tcp("gw3", "192.0.2.10", "c")},
             {"drivers/modbus/gw4.yaml",
              tcp("gw4", "\"::ffff:192.0.2.10\"", "d")}}),
        (Lines{"drivers/modbus/gw2.yaml:4:9: warning CW-621: channels gw1 and "
               "gw2 both connect to [2001:DB8:0:0::A]:1502; most devices "
               "allow one client",
               "drivers/modbus/gw4.yaml:4:9: warning CW-621: channels gw3 and "
               "gw4 both connect to [::ffff:192.0.2.10]:502; most devices "
               "allow one client"}));
}

}  // namespace
}  // namespace scada::conf
