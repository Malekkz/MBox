// The pure parts of the real transports (connections.h, D-141): which
// hosts are literals, which rates the serial port can set, and the
// socket's user timeout of a channel (7.3). Their system calls are tested
// across the boundary in transport_integration_test.cpp.

#include "scada/driver/connections.h"
#include "../src/fd_io.h"
#include "channel_builder.h"
#include "scada/common/fd.h"
#include "scada/driver/compiled.h"
#include "scada/driver/plan.h"
#include "scada/driver/transport.h"
#include "test_protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

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

namespace scada::driver::test {
namespace {

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers): the test data

// A clock that stands still at `now`.
class FixedClock final : public Clock {
public:
    explicit FixedClock(std::int64_t now) : now_(now) {}
    SCADA_RT [[nodiscard]] std::int64_t monotonic_ns() const noexcept override {
        return now_;
    }
    SCADA_RT [[nodiscard]] std::int64_t realtime_ns() const noexcept override {
        return now_;
    }

private:
    std::int64_t now_;
};

TEST(Connections, TheTimeLeftNeverOverflows) {
    // Review M2: a deadline far in the past is 0 left, not an overflow.
    const FixedClock clock(100);
    EXPECT_EQ(
        detail::time_left(std::numeric_limits<std::int64_t>::min(), clock), 0);
    EXPECT_EQ(detail::time_left(50, clock), 0);
    EXPECT_EQ(detail::time_left(100, clock), 0);
    EXPECT_EQ(detail::time_left(150, clock), 50);
    EXPECT_EQ(
        detail::time_left(std::numeric_limits<std::int64_t>::max(), clock),
        std::numeric_limits<std::int64_t>::max() - 100);
}

// NOLINTBEGIN(misc-non-private-member-variables-in-classes): the script is what the tests set and look at

// A scripted wait and read for `receive_loop`.
struct Script {
    std::vector<detail::Readiness> waits;
    std::vector<IoStatus> reads;
    std::size_t waited = 0;
    std::size_t read = 0;

    [[nodiscard]] IoStatus run(std::int64_t deadline_ns, const Clock& clock) {
        std::size_t received = 0;
        return detail::receive_loop(
            [&] { return waits.at(std::min(waited++, waits.size() - 1)); },
            [&](std::size_t& got) {
                const IoStatus status =
                    reads.at(std::min(read++, reads.size() - 1));
                got = status == IoStatus::OK ? 3 : 0;
                return status;
            },
            deadline_ns, clock, received);
    }
};

// NOLINTEND(misc-non-private-member-variables-in-classes)

TEST(Connections, AWokenWaitIsAbandonedWithoutARead) {
    // DI 10: the stop's wake ends the wait; nothing is read after it.
    const FixedClock clock(100);
    Script script{.waits = {detail::Readiness::WOKEN}, .reads = {IoStatus::OK}};
    EXPECT_EQ(script.run(1'000, clock), IoStatus::ABANDONED);
    EXPECT_EQ(script.read, 0U);
}

TEST(Connections, AWakeWithNothingToReadWaitsAgainUntilTheDeadline) {
    // Review M3: input seen, then nothing readable, is no time-out while
    // the deadline has not passed.
    const FixedClock clock(100);
    Script script{
        .waits = {detail::Readiness::READY},
        .reads = {IoStatus::TIMEOUT, IoStatus::TIMEOUT, IoStatus::OK}};
    EXPECT_EQ(script.run(1'000, clock), IoStatus::OK);
    EXPECT_EQ(script.waited, 3U);
}

TEST(Connections, AWaitThatTimedOutStillReadsOnce) {
    // T3: what arrived during a stall is read, though the wait timed out.
    const FixedClock clock(100);
    Script script{.waits = {detail::Readiness::TIMEOUT},
                  .reads = {IoStatus::OK}};
    EXPECT_EQ(script.run(50, clock), IoStatus::OK);
    EXPECT_EQ(script.read, 1U);

    Script nothing{.waits = {detail::Readiness::TIMEOUT},
                   .reads = {IoStatus::TIMEOUT}};
    EXPECT_EQ(nothing.run(50, clock), IoStatus::TIMEOUT);
    EXPECT_EQ(nothing.waited, 1U);
}

TEST(Connections, AWakeAtTheDeadlineWithNothingToReadIsATimeOut) {
    const FixedClock clock(100);
    Script script{.waits = {detail::Readiness::READY},
                  .reads = {IoStatus::TIMEOUT}};
    EXPECT_EQ(script.run(100, clock), IoStatus::TIMEOUT);
    EXPECT_EQ(script.waited, 1U);
    Script failed{.waits = {detail::Readiness::FAILED},
                  .reads = {IoStatus::OK}};
    EXPECT_EQ(failed.run(1'000, clock), IoStatus::CLOSED);
    EXPECT_EQ(failed.read, 0U);
}

TEST(Connections, ASocketConnectedToItselfIsFound) {
    // Review M1: a socket bound to 127.0.0.1:P that connects to 127.0.0.1:P
    // meets itself (a simultaneous open with nobody listening).
    const Fd self(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr), 1);
    socklen_t length = sizeof(address);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,scada-banned): the socket interface takes every address type through a sockaddr pointer
    auto* generic = reinterpret_cast<sockaddr*>(&address);
    ASSERT_EQ(::bind(self.get(), generic, sizeof(address)), 0);
    ASSERT_EQ(::getsockname(self.get(), generic, &length), 0);
    EXPECT_FALSE(detail::connected_to_itself(self.get()));  // not connected
    ASSERT_EQ(::connect(self.get(), generic, sizeof(address)), 0);
    EXPECT_TRUE(detail::connected_to_itself(self.get()));

    // A connection to a listener is not.
    const Fd listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    address.sin_port = 0;
    ASSERT_EQ(::bind(listener.get(), generic, sizeof(address)), 0);
    ASSERT_EQ(::listen(listener.get(), 1), 0);
    length = sizeof(address);
    ASSERT_EQ(::getsockname(listener.get(), generic, &length), 0);
    const Fd client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    ASSERT_EQ(::connect(client.get(), generic, sizeof(address)), 0);
    EXPECT_FALSE(detail::connected_to_itself(client.get()));
}

TEST(Connections, ADiscardStopsAtItsLimit) {
    // T4: at most DISCARD_LIMIT bytes a call, so that a line that never
    // falls quiet cannot hold the worker; a pipe sized to hold more.
    std::array<int, 2> ends{-1, -1};
    ASSERT_EQ(::pipe2(ends.data(), O_NONBLOCK | O_CLOEXEC), 0);
    const Fd reader(ends[0]);
    const Fd writer(ends[1]);
    constexpr int CAPACITY = 256 * 1024;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl has no other form
    ASSERT_GE(::fcntl(writer.get(), F_SETPIPE_SZ, CAPACITY), CAPACITY);
    const std::vector<std::byte> bytes(detail::DISCARD_LIMIT + 1'000);
    ASSERT_EQ(::write(writer.get(), bytes.data(), bytes.size()),
              static_cast<ssize_t>(bytes.size()));
    EXPECT_TRUE(detail::drop_readable(reader.get(), false));
    int left = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,misc-include-cleaner): ioctl has no other form; FIONREAD comes through <sys/ioctl.h>
    ASSERT_EQ(::ioctl(reader.get(), FIONREAD, &left), 0);
    EXPECT_EQ(left, 1'000);
    EXPECT_TRUE(detail::drop_readable(reader.get(), false));
    EXPECT_FALSE(detail::drop_readable(reader.get(), false));
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

}  // namespace
}  // namespace scada::driver::test
