// The name grammars of configuration schemas contract 2.1 and the address,
// zone, time and URL forms the schemas check (names.h). Invented examples
// only.

#include "scada/conf/names.h"

#include <gtest/gtest.h>

#include <string_view>

#include <optional>

#include <cstdint>

#include <array>

#include <string>

namespace scada::conf {
namespace {

TEST(Names, Identifiers) {
    EXPECT_TRUE(is_identifier("pump_1"));
    EXPECT_TRUE(is_identifier(std::string(31, 'a')));
    EXPECT_FALSE(is_identifier(std::string(32, 'a')));
    EXPECT_FALSE(is_identifier(""));
    EXPECT_FALSE(is_identifier("1pump"));
    EXPECT_FALSE(is_identifier("Pump"));
    EXPECT_FALSE(is_identifier("pump-1"));
}

TEST(Names, TagNames) {
    EXPECT_TRUE(is_tag_name("ps1.pump-2.flow_total"));
    EXPECT_TRUE(is_tag_name("0.a"));
    EXPECT_FALSE(is_tag_name("ps1..flow"));
    EXPECT_FALSE(is_tag_name(".ps1"));
    EXPECT_FALSE(is_tag_name("ps1."));
    EXPECT_FALSE(is_tag_name("ps1._x"));
    EXPECT_FALSE(is_tag_name("PS1.flow"));
    EXPECT_TRUE(is_tag_name(std::string(127, 'a')));
    EXPECT_FALSE(is_tag_name(std::string(128, 'a')));
    EXPECT_FALSE(is_tag_name(std::string(101, 'a'), MAX_DEVICE_NAME));
}

TEST(Names, PollGroupsProtocolsUnitsAndProcesses) {
    EXPECT_TRUE(is_poll_group_name("fast"));
    EXPECT_TRUE(is_poll_group_name("15m"));
    EXPECT_FALSE(is_poll_group_name("15"));
    EXPECT_FALSE(is_poll_group_name("very_slow"));
    EXPECT_TRUE(is_protocol_name("modbus"));
    EXPECT_FALSE(is_protocol_name("mod_bus"));
    EXPECT_FALSE(is_protocol_name("protocol123"));
    EXPECT_TRUE(is_unit_id("site-0042"));
    EXPECT_FALSE(is_unit_id("-site"));
    EXPECT_FALSE(is_unit_id("Site"));
    EXPECT_TRUE(is_process_name("tagd"));
    EXPECT_TRUE(is_process_name("drv-modbus"));
    EXPECT_FALSE(is_process_name("drv-"));
    EXPECT_FALSE(is_process_name("web"));
}

TEST(Names, FreeText) {
    EXPECT_TRUE(is_free_text("Pump station 1, north side"));
    EXPECT_TRUE(is_free_text("caf\xc3\xa9"));
    EXPECT_FALSE(is_free_text("a\nb"));
    EXPECT_FALSE(is_free_text("a\tb"));
    EXPECT_FALSE(is_free_text("a\x7f"));
    EXPECT_FALSE(is_free_text("a\xc2\x85"));
}

TEST(Names, Ipv4Addresses) {
    EXPECT_TRUE(is_ipv4_address("192.0.2.1"));
    EXPECT_TRUE(is_ipv4_address("0.0.0.0"));
    EXPECT_TRUE(is_ipv4_address("255.255.255.255"));
    EXPECT_FALSE(is_ipv4_address("256.0.0.1"));
    EXPECT_FALSE(is_ipv4_address("192.0.2"));
    EXPECT_FALSE(is_ipv4_address("192.0.2.1.5"));
    EXPECT_FALSE(is_ipv4_address("192.0.02.1"));
    EXPECT_FALSE(is_ipv4_address("192.0.2."));
    EXPECT_FALSE(is_ipv4_address(" 192.0.2.1"));
}

TEST(Names, Ipv6Addresses) {
    EXPECT_TRUE(is_ipv6_address("2001:db8::1"));
    EXPECT_TRUE(is_ipv6_address("::"));
    EXPECT_TRUE(is_ipv6_address("::1"));
    EXPECT_TRUE(is_ipv6_address("2001:db8:0:0:0:0:0:1"));
    EXPECT_TRUE(is_ipv6_address("::ffff:192.0.2.1"));
    EXPECT_FALSE(is_ipv6_address("2001:db8::1::2"));
    EXPECT_FALSE(is_ipv6_address("2001:db8:0:0:0:0:0:0:1"));
    EXPECT_FALSE(is_ipv6_address("2001:db8::12345"));
    EXPECT_FALSE(is_ipv6_address("fe80::1%eth0"));
    EXPECT_FALSE(is_ipv6_address("2001:db8::g"));
    EXPECT_TRUE(is_ip_address("192.0.2.1"));
    EXPECT_TRUE(is_ip_address("2001:db8::1"));
    EXPECT_FALSE(is_ip_address("example"));
}

TEST(Names, HostNames) {
    EXPECT_TRUE(is_host_name("ntp.example.invalid"));
    EXPECT_TRUE(is_host_name("a-b.example"));
    EXPECT_FALSE(is_host_name("-a.example"));
    EXPECT_FALSE(is_host_name("a-.example"));
    EXPECT_FALSE(is_host_name("a..example"));
    EXPECT_FALSE(is_host_name("a_b.example"));
    EXPECT_FALSE(is_host_name(std::string(64, 'a')));
    // All digits and dots is an address, not a name.
    EXPECT_FALSE(is_host_name("256.0.0.1"));
    EXPECT_FALSE(is_host_name(""));
}

TEST(Names, AddressesWithAPrefixLength) {
    EXPECT_TRUE(is_cidr("192.0.2.0/24"));
    EXPECT_TRUE(is_cidr("192.0.2.0/0"));
    EXPECT_TRUE(is_cidr("2001:db8::/32"));
    EXPECT_TRUE(is_cidr("2001:db8::/128"));
    EXPECT_FALSE(is_cidr("192.0.2.0/33"));
    EXPECT_FALSE(is_cidr("2001:db8::/129"));
    EXPECT_FALSE(is_cidr("192.0.2.0"));
    EXPECT_FALSE(is_cidr("192.0.2.0/"));
    EXPECT_FALSE(is_cidr("192.0.2.0/024"));
}

TEST(Names, ZonesTimesAndUrls) {
    EXPECT_TRUE(is_time_zone_name("Etc/UTC"));
    EXPECT_TRUE(is_time_zone_name("America/Argentina/Buenos_Aires"));
    EXPECT_TRUE(is_time_zone_name("Etc/GMT+5"));
    EXPECT_FALSE(is_time_zone_name("Etc//UTC"));
    EXPECT_FALSE(is_time_zone_name("/UTC"));
    EXPECT_FALSE(is_time_zone_name("Etc/1UTC"));
    EXPECT_FALSE(is_time_zone_name("Europe/Averyveryverylongname"));
    EXPECT_TRUE(is_time_of_day("00:00"));
    EXPECT_TRUE(is_time_of_day("23:59"));
    EXPECT_FALSE(is_time_of_day("24:00"));
    EXPECT_FALSE(is_time_of_day("12:60"));
    EXPECT_FALSE(is_time_of_day("1:30"));
    EXPECT_TRUE(is_https_url("https://upstream.example.invalid/api"));
    EXPECT_FALSE(is_https_url("http://upstream.example.invalid/api"));
    EXPECT_FALSE(is_https_url("https://"));
    EXPECT_FALSE(is_https_url("https://upstream.example.invalid/a b"));
}

TEST(Names, DevicePaths) {
    EXPECT_TRUE(is_device_path("/dev/ttyS1"));
    EXPECT_TRUE(is_device_path("/dev/serial/by-id/usb adapter 1"));
    EXPECT_TRUE(is_device_path("/dev/" + std::string(250, 'a')));
    EXPECT_FALSE(is_device_path("/dev/" + std::string(251, 'a')));
    EXPECT_FALSE(is_device_path("/dev/"));
    EXPECT_FALSE(is_device_path("/dev"));
    EXPECT_FALSE(is_device_path("/dev//a"));
    EXPECT_FALSE(is_device_path("/dev/a/"));
    EXPECT_FALSE(is_device_path("/dev/./a"));
    EXPECT_FALSE(is_device_path("/dev/a/.."));
    EXPECT_FALSE(is_device_path("/devices/a"));
    EXPECT_FALSE(is_device_path("/dev/a\tb"));
    EXPECT_FALSE(is_device_path("/dev/a\nb"));
}

// The bytes of an address, however it is written (B4 of the review of #144).
TEST(Names, AddressBytes) {
    using V4 = std::array<std::uint8_t, IPV4_BYTES>;
    EXPECT_EQ(ipv4_bytes("192.0.2.1"), (V4{192, 0, 2, 1}));
    EXPECT_EQ(ipv4_bytes("192.0.2"), std::nullopt);
}

TEST(Names, Ipv6Bytes) {
    using V6 = std::array<std::uint8_t, IPV6_BYTES>;
    V6 loopback{};
    loopback.back() = 1;
    for (const std::string_view text :
         {"::1", "0::1", "0:0:0:0:0:0:0:1",
          "0000:0000:0000:0000:0000:0000:0000:0001", "::0.0.0.1"}) {
        EXPECT_EQ(ipv6_bytes(text), loopback) << text;
    }
}

TEST(Names, Ipv6BytesOfOtherForms) {
    using V6 = std::array<std::uint8_t, IPV6_BYTES>;
    EXPECT_EQ(ipv6_bytes("ff02::1a"),
              (V6{0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1A}));
    EXPECT_EQ(ipv6_bytes("ff::1"),
              (V6{0x00, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}));
    EXPECT_EQ(ipv6_bytes("::FFFF:127.0.0.1"),
              (V6{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 127, 0, 0, 1}));
    EXPECT_EQ(ipv6_bytes("1::2::3"), std::nullopt);
    EXPECT_EQ(ipv6_bytes("192.0.2.1"), std::nullopt);
}

}  // namespace
}  // namespace scada::conf
