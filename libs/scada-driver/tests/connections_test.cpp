// The pure parts of the real transports (connections.h, D-141): which
// hosts are literals, which rates the serial port can set, and the
// socket's user timeout of a channel (7.3). Their system calls are tested
// across the boundary in transport_integration_test.cpp.

#include "scada/driver/connections.h"
#include "channel_builder.h"
#include "scada/driver/compiled.h"
#include "scada/driver/plan.h"
#include "test_protocol.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

namespace scada::driver::test {
namespace {

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers): the test data

TEST(Connections, OnlyLiteralAddressesAreHosts) {
    // 7.3 "Addresses", ruling O-7: a name would need a resolver call with
    // no deadline the driver can enforce.
    EXPECT_TRUE(is_ip_literal("192.0.2.10"));
    EXPECT_TRUE(is_ip_literal("2001:db8::10"));
    EXPECT_TRUE(is_ip_literal("::ffff:192.0.2.10"));
    EXPECT_FALSE(is_ip_literal("meter.example"));
    EXPECT_FALSE(is_ip_literal("localhost"));
    EXPECT_FALSE(is_ip_literal(""));
    EXPECT_FALSE(is_ip_literal("[2001:db8::10]"));
    EXPECT_FALSE(is_ip_literal("fe80::1%eth0"));
    EXPECT_FALSE(is_ip_literal("192.0.2.10:502"));
    EXPECT_FALSE(is_ip_literal(std::string("192.0.2.1\0x", 11)));
    EXPECT_FALSE(is_ip_literal(std::string(200, '1')));
}

TEST(Connections, TheStandardRatesAreTheOnlyOnesSet) {
    for (const std::uint32_t baud :
         {300U, 1'200U, 9'600U, 19'200U, 38'400U, 115'200U}) {
        EXPECT_TRUE(is_standard_baud(baud)) << baud;
    }
    for (const std::uint32_t baud : {0U, 299U, 14'400U, 250'000U, 4'000'001U}) {
        EXPECT_FALSE(is_standard_baud(baud)) << baud;
    }
}

CompiledChannel timed(std::int64_t timeout_ns, std::int64_t write_timeout_ns,
                      std::uint32_t lost_after) {
    CompiledChannel channel =
        make_channel({{.name = "a", .unit_id = 1, .points = adjacent(0, 1)},
                      {.name = "b", .unit_id = 2, .points = adjacent(0, 1)}},
                     ReadRules{.max_units = MAX_UNITS});
    channel.connection.lost_after_timeouts = lost_after;
    channel.devices[1].failure.timeout_ns = timeout_ns;
    channel.devices[1].failure.write_timeout_ns = write_timeout_ns;
    return channel;
}

TEST(Connections, TheUserTimeoutCoversTheLossRuleAndAtLeastTenSeconds) {
    // 7.3: `lost_after_timeouts` times the largest request deadline, at
    // least 10 s.
    EXPECT_EQ(tcp_user_timeout_ns(timed(SECOND, SECOND, 3)), 10 * SECOND);
    EXPECT_EQ(tcp_user_timeout_ns(timed(4 * SECOND, 2 * SECOND, 3)),
              12 * SECOND);
    EXPECT_EQ(tcp_user_timeout_ns(timed(SECOND, 6 * SECOND, 2)), 12 * SECOND);
    EXPECT_EQ(tcp_user_timeout_ns(timed(
                  std::numeric_limits<std::int64_t>::max() / 2, SECOND, 10)),
              std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(tcp_user_timeout_ns(CompiledChannel{}), MIN_TCP_USER_TIMEOUT_NS);
}

TEST(Connections, TheSystemClockIsMonotonic) {
    const SystemClock clock;
    const std::int64_t first = clock.monotonic_ns();
    const std::int64_t second = clock.monotonic_ns();
    EXPECT_GE(second, first);
    EXPECT_GT(clock.realtime_ns(), 0);
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

}  // namespace
}  // namespace scada::driver::test
