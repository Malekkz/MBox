// The real transports (connections.h) across a process or device boundary
// (decision D-141): a ChannelEngine over TcpTransport against the test
// simulator in its own process, over SerialTransport against it on a
// pseudo-terminal pair, and each transport's rules against peers the test
// holds itself (a socket, a pty master). Every wait is on a condition with
// a time-out: the engine's own wait times, as the worker of slice 5 waits
// (transport.h), or `poll` on the peer. Nothing sleeps.
//
// A pseudo-terminal has no bit rate and no parity (7.2): the serial tests
// check framing, the silence between requests and the port's settings, not
// line timing, which is tested on the target board (#41).

#include "channel_builder.h"
#include "scada/common/fd.h"
#include "scada/driver/channel_engine.h"
#include "scada/driver/compiled.h"
#include "scada/driver/connections.h"
#include "scada/driver/failure.h"
#include "scada/driver/plan.h"
#include "scada/driver/timing.h"
#include "scada/driver/transport.h"
#include "scada/tag/types.h"
#include "scada/test/child_process.h"
#include "sim_transport.h"
#include "test_protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal numbers are declared here, not in <csignal>
#include <sys/socket.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

namespace scada::driver::test {
namespace {

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers): the test data

using std::chrono::milliseconds;
using Flag = tag::QualityFlag;

constexpr milliseconds START_LIMIT{10'000};
constexpr std::int64_t RUN_LIMIT = 10 * SECOND;

// SCADA_DRIVER_TEST_SIM_COMMAND, set by CTest: elements separated by `|`
// (the emulator of a cross build, then the program).
std::vector<std::string> sim_command(std::vector<std::string> args) {
    std::vector<std::string> command;
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing changes the environment while the tests run
    const char* const value = std::getenv("SCADA_DRIVER_TEST_SIM_COMMAND");
    if (value == nullptr) {
        return command;
    }
    std::string_view rest(value);
    while (!rest.empty()) {
        const std::size_t bar = rest.find('|');
        command.emplace_back(rest.substr(0, bar));
        rest = bar == std::string_view::npos ? std::string_view{}
                                             : rest.substr(bar + 1);
    }
    command.insert(command.end(), args.begin(), args.end());
    return command;
}

// NOLINTBEGIN(misc-non-private-member-variables-in-classes): the parts are what the tests look at

// The simulator, started with `args`, once it has said where it serves.
struct Sim {
    scada::test::ChildProcess process;
    std::string where;  // the port or the path

    // Waits until its output holds `count` lines that start with `word`.
    bool await(std::string_view word, std::size_t count) {
        const std::string start = std::string(word) + " ";
        return process.wait_for_output(
            [&](std::string_view out) {
                std::size_t seen = 0;
                for (std::size_t at = 0; at < out.size();) {
                    const std::size_t end = out.find('\n', at);
                    if (end == std::string_view::npos) {
                        break;
                    }
                    const std::string_view line = out.substr(at, end - at);
                    if (line == word || line.starts_with(start)) {
                        ++seen;
                    }
                    at = end + 1;
                }
                return seen >= count;
            },
            START_LIMIT);
    }

    // The lines of its output that start with `word`.
    [[nodiscard]] std::vector<std::vector<std::string>> lines(
        std::string_view word) const {
        std::vector<std::vector<std::string>> out;
        std::istringstream text(process.out());
        std::string line;
        while (std::getline(text, line)) {
            std::istringstream fields(line);
            std::vector<std::string> parts;
            std::string part;
            while (fields >> part) {
                parts.push_back(part);
            }
            if (!parts.empty() && parts[0] == word) {
                out.push_back(std::move(parts));
            }
        }
        return out;
    }
};

// NOLINTEND(misc-non-private-member-variables-in-classes)

std::optional<Sim> start_sim(const std::vector<std::string>& args,
                             std::string_view kind) {
    const std::vector<std::string> command = sim_command(args);
    if (command.size() <= args.size()) {
        ADD_FAILURE() << "SCADA_DRIVER_TEST_SIM_COMMAND is not set";
        return std::nullopt;
    }
    auto process = scada::test::ChildProcess::start(command);
    if (!process) {
        ADD_FAILURE() << "the simulator did not start";
        return std::nullopt;
    }
    const std::string prefix = std::string(kind) + " ";
    if (!process->wait_for_output(
            [&](std::string_view out) {
                return out.find('\n') != std::string_view::npos;
            },
            START_LIMIT) ||
        !process->out().starts_with(prefix)) {
        ADD_FAILURE() << "the simulator said: " << process->out()
                      << process->err();
        return std::nullopt;
    }
    const std::string& out = process->out();
    std::string where =
        out.substr(prefix.size(), out.find('\n') - prefix.size());
    return Sim{.process = std::move(*process), .where = std::move(where)};
}

std::uint16_t port_of(const Sim& sim) {
    std::istringstream text(sim.where);
    std::uint16_t port = 0;
    text >> port;
    return port;
}

// Steps `engine` until `done` or `limit_ns` of the system clock passed,
// waiting between steps as the worker does: in `ppoll` up to the time the
// step returned, on the transport's descriptor for POLLRDHUP (transport.h).
bool run_until(ChannelEngine& engine, Transport& transport,
               const SystemClock& clock, std::int64_t limit_ns,
               const std::function<bool()>& done) {
    const std::int64_t end = clock.monotonic_ns() + limit_ns;
    while (!done()) {
        const std::int64_t next = engine.step();
        const std::int64_t now = clock.monotonic_ns();
        if (now >= end) {
            return done();
        }
        const std::int64_t until = std::min(next, end);
        if (until > now) {
            pollfd entry{
                .fd = transport.poll_fd(), .events = POLLRDHUP, .revents = 0};
            const std::int64_t left = until - now;
            const timespec wait{.tv_sec = left / SECOND,
                                .tv_nsec = left % SECOND};
            (void)::ppoll(&entry, 1, &wait, nullptr);
        }
    }
    return true;
}

// One device of unit 1 with one point at register 0 in the fast group.
CompiledChannel one_device(TransportKind kind) {
    CompiledChannel channel =
        make_channel({{.name = "d1", .unit_id = 1, .points = adjacent(0, 1)}},
                     ReadRules{.max_units = MAX_UNITS});
    channel.transport = kind;
    channel.connection.reconnect_min_ns = 100 * MILLISECOND;
    channel.connection.reconnect_max_ns = 400 * MILLISECOND;
    channel.connection.connect_timeout_ns = SECOND;
    for (CompiledDevice& device : channel.devices) {
        device.failure.timeout_ns = 500 * MILLISECOND;
        device.failure.write_timeout_ns = 500 * MILLISECOND;
    }
    return channel;
}

// A read request of the test protocol: `count` units of space 0 from
// `first` on `unit`.
std::array<std::byte, REQUEST_BYTES> read_request(std::uint8_t unit,
                                                  std::uint32_t first,
                                                  std::uint16_t count) {
    return {std::byte{0},
            std::byte{1},
            std::byte{unit},
            std::byte{'R'},
            std::byte{0},
            static_cast<std::byte>(first >> 24U),
            static_cast<std::byte>(first >> 16U),
            static_cast<std::byte>(first >> 8U),
            static_cast<std::byte>(first),
            static_cast<std::byte>(count >> 8U),
            static_cast<std::byte>(count)};
}

// Waits up to `limit` for `events` on `fd`.
bool wait_ready(int descriptor, short events, milliseconds limit) {
    pollfd entry{.fd = descriptor, .events = events, .revents = 0};
    const auto wait = static_cast<int>(limit.count());
    int ready = ::poll(&entry, 1, wait);
    while (ready < 0 && errno == EINTR) {
        ready = ::poll(&entry, 1, wait);
    }
    return ready > 0;
}

// NOLINTBEGIN(misc-non-private-member-variables-in-classes): the parts are what the tests look at

// A listening socket on 127.0.0.1 with an ephemeral port, for the tests
// whose peer is the test itself.
struct Listener {
    Fd fd;
    std::uint16_t port = 0;

    static Listener open(int backlog) {
        Listener out;
        out.fd = Fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        (void)::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        socklen_t length = sizeof(address);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,scada-banned): the socket interface takes every address type through a sockaddr pointer
        auto* generic = reinterpret_cast<sockaddr*>(&address);
        EXPECT_EQ(::bind(out.fd.get(), generic, sizeof(address)), 0);
        if (backlog >= 0) {
            EXPECT_EQ(::listen(out.fd.get(), backlog), 0);
        }
        EXPECT_EQ(::getsockname(out.fd.get(), generic, &length), 0);
        out.port = ntohs(address.sin_port);
        return out;
    }

    // The next connection, once it is there.
    [[nodiscard]] Fd accept() const {
        EXPECT_TRUE(wait_ready(fd.get(), POLLIN, milliseconds{5'000}));
        return Fd(::accept4(fd.get(), nullptr, nullptr, SOCK_CLOEXEC));
    }
};

// NOLINTEND(misc-non-private-member-variables-in-classes)

TcpEndpoint loopback(std::uint16_t port) {
    return TcpEndpoint{.host = "127.0.0.1", .port = port};
}

// NOLINTBEGIN(misc-non-private-member-variables-in-classes): the parts are what the tests look at

// A pseudo-terminal pair the test holds: the master, and the path of the
// port the transport opens.
struct Pty {
    Fd master;
    std::string path;

    static Pty open() {
        Pty out;
        out.master = Fd(::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC));
        std::array<char, 128> name{};
        EXPECT_TRUE(out.master.valid());
        EXPECT_EQ(::grantpt(out.master.get()), 0);
        EXPECT_EQ(::unlockpt(out.master.get()), 0);
        EXPECT_EQ(::ptsname_r(out.master.get(), name.data(), name.size()), 0);
        out.path = name.data();
        return out;
    }
};

// NOLINTEND(misc-non-private-member-variables-in-classes)

SerialSettings port(std::string path, Parity parity = Parity::NONE,
                    std::uint32_t baud = 19'200) {
    return SerialSettings{.device = std::move(path),
                          .baud = baud,
                          .parity = parity,
                          .stop_bits = 1};
}

std::int64_t deadline_in(const SystemClock& clock, std::int64_t duration_ns) {
    return clock.monotonic_ns() + duration_ns;
}

// --- TCP, against the simulator process ------------------------------------

TEST(TcpTransport, AnEngineReadsADeviceThroughTheSimulator) {
    auto sim = start_sim(
        {"--tcp", "--map", "1", "0", "0", "3", "--set", "1", "0", "0", "1234"},
        "tcp");
    if (!sim.has_value()) {
        FAIL() << "the simulator did not start";
    }
    const CompiledChannel channel = one_device(TransportKind::TCP);
    SystemClock clock;  // the engine takes it to write
    TcpTransport transport(
        TcpEndpoint{.host = "127.0.0.1",
                    .port = port_of(*sim),
                    .user_timeout_ns = tcp_user_timeout_ns(channel)},
        clock);
    RecordingTags tags;
    const TestProtocol protocol;
    ChannelEngine engine(channel, protocol, transport, clock, tags);
    ASSERT_TRUE(engine.status().ok());

    EXPECT_TRUE(run_until(engine, transport, clock, RUN_LIMIT, [&] {
        return tags.point(0, 0).quality == tag::Quality::GOOD;
    }));
    EXPECT_EQ(tags.point(0, 0).value, 1234);
    EXPECT_EQ(engine.failures().state(0), DeviceStatus::RUNNING);
    EXPECT_TRUE(sim->await("request", 1));
}

TEST(TcpTransport, APeerCloseIsSeenWhileIdleAndTheEngineReconnects) {
    // 7.3 "Idle close" and "Lost connection": the simulator closes after
    // its first answer; the engine learns of it at a boundary through
    // `closed_by_peer`, without reading, and connects again after its
    // back-off (100 ms here).
    auto sim = start_sim({"--tcp", "--map", "1", "0", "0", "3", "--set", "1",
                          "0", "0", "7", "--close-after", "1"},
                         "tcp");
    if (!sim.has_value()) {
        FAIL() << "the simulator did not start";
    }
    const CompiledChannel channel = one_device(TransportKind::TCP);
    SystemClock clock;  // the engine takes it to write
    TcpTransport transport(loopback(port_of(*sim)), clock);
    RecordingTags tags;
    const TestProtocol protocol;
    ChannelEngine engine(channel, protocol, transport, clock, tags);

    EXPECT_TRUE(run_until(engine, transport, clock, RUN_LIMIT,
                          [&] { return engine.counts().connects >= 3; }));
    EXPECT_GE(engine.counts().losses, 2U);
    EXPECT_TRUE(sim->await("closed", 2));
}

TEST(TcpTransport, AnAnswerThatDoesNotComeIsATimeOutAtItsDeadline) {
    auto sim = start_sim(
        {"--tcp", "--map", "1", "0", "0", "3", "--silent", "1"}, "tcp");
    if (!sim.has_value()) {
        FAIL() << "the simulator did not start";
    }
    const SystemClock clock;
    TcpTransport transport(loopback(port_of(*sim)), clock);
    ASSERT_EQ(transport.connect(deadline_in(clock, SECOND)), IoStatus::OK);
    const auto request = read_request(1, 0, 1);
    ASSERT_EQ(transport.send(request), IoStatus::OK);
    std::array<std::byte, 64> into{};
    std::size_t got = 1;
    const std::int64_t started = clock.monotonic_ns();
    EXPECT_EQ(transport.receive(into, started + 100 * MILLISECOND, got),
              IoStatus::TIMEOUT);
    EXPECT_EQ(got, 0U);
    EXPECT_GE(clock.monotonic_ns() - started, 100 * MILLISECOND);
}

// --- TCP, against a peer the test holds ------------------------------------

TEST(TcpTransport, AClosedPortRefusesAndTheTransportStaysClosed) {
    const Listener bound = Listener::open(-1);  // bound, never listening
    const SystemClock clock;
    TcpTransport transport(loopback(bound.port), clock);
    EXPECT_EQ(transport.connect(deadline_in(clock, SECOND)), IoStatus::REFUSED);
    EXPECT_EQ(transport.last_error(), ConnectError::REFUSED);
    EXPECT_EQ(transport.poll_fd(), -1);
    EXPECT_TRUE(transport.closed_by_peer());
    const auto request = read_request(1, 0, 1);
    EXPECT_EQ(transport.send(request), IoStatus::CLOSED);
}

TEST(TcpTransport, AHostNameIsRefusedWithoutAnyConnect) {
    const SystemClock clock;
    TcpTransport transport(TcpEndpoint{.host = "localhost", .port = 502},
                           clock);
    EXPECT_EQ(transport.connect(deadline_in(clock, SECOND)), IoStatus::FAILED);
    EXPECT_EQ(transport.last_error(), ConnectError::ADDRESS);
    EXPECT_EQ(transport.poll_fd(), -1);
}

TEST(TcpTransport, TheSocketHasNoDelayAndTheChannelsUserTimeout) {
    const Listener listener = Listener::open(1);
    const SystemClock clock;
    TcpTransport transport(TcpEndpoint{.host = "127.0.0.1",
                                       .port = listener.port,
                                       .user_timeout_ns = 12'345'678'901},
                           clock);
    ASSERT_EQ(transport.connect(deadline_in(clock, SECOND)), IoStatus::OK);
    EXPECT_EQ(transport.last_error(), ConnectError::NONE);
    int value = 0;
    socklen_t length = sizeof(value);
    ASSERT_EQ(::getsockopt(transport.poll_fd(), IPPROTO_TCP, TCP_NODELAY,
                           &value, &length),
              0);
    EXPECT_NE(value, 0);
    ASSERT_EQ(::getsockopt(transport.poll_fd(), IPPROTO_TCP, TCP_USER_TIMEOUT,
                           &value, &length),
              0);
    EXPECT_EQ(value, 12'346);  // milliseconds, rounded up
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl has no other form
    EXPECT_NE(::fcntl(transport.poll_fd(), F_GETFL) & O_NONBLOCK, 0);
}

TEST(TcpTransport, ReceiveReadsWhatArrivedEvenPastItsDeadline) {
    // A worker stalled past its deadline still reads what arrived; the read
    // after the wait is one call that does not block (transport.h; G4).
    const Listener listener = Listener::open(1);
    const SystemClock clock;
    TcpTransport transport(loopback(listener.port), clock);
    ASSERT_EQ(transport.connect(deadline_in(clock, SECOND)), IoStatus::OK);
    const Fd peer = listener.accept();
    std::array<std::byte, 64> into{};
    std::size_t got = 0;
    EXPECT_EQ(transport.receive(into, clock.monotonic_ns() - 1, got),
              IoStatus::TIMEOUT);

    const std::array<std::byte, 5> bytes{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
    ASSERT_EQ(::write(peer.get(), bytes.data(), bytes.size()), 5);
    ASSERT_TRUE(wait_ready(transport.poll_fd(), POLLIN, milliseconds{5'000}));
    EXPECT_EQ(transport.receive(into, clock.monotonic_ns() - 1, got),
              IoStatus::OK);
    EXPECT_EQ(got, 5U);
}

TEST(TcpTransport, APeersCloseIsSeenWithoutConsumingItsLastBytes) {
    const Listener listener = Listener::open(1);
    const SystemClock clock;
    TcpTransport transport(loopback(listener.port), clock);
    ASSERT_EQ(transport.connect(deadline_in(clock, SECOND)), IoStatus::OK);
    Fd peer = listener.accept();
    EXPECT_FALSE(transport.closed_by_peer());
    const std::array<std::byte, 4> bytes{};
    ASSERT_EQ(::write(peer.get(), bytes.data(), bytes.size()), 4);
    peer.reset();
    ASSERT_TRUE(
        wait_ready(transport.poll_fd(), POLLRDHUP, milliseconds{5'000}));
    EXPECT_TRUE(transport.closed_by_peer());
    EXPECT_TRUE(transport.closed_by_peer());  // asking again changes nothing
    std::array<std::byte, 64> into{};
    std::size_t got = 0;
    EXPECT_EQ(transport.receive(into, deadline_in(clock, SECOND), got),
              IoStatus::OK);
    EXPECT_EQ(got, 4U);
    EXPECT_EQ(transport.receive(into, deadline_in(clock, SECOND), got),
              IoStatus::CLOSED);
}

TEST(TcpTransport, AFrameTheKernelDoesNotTakeWholeIsPartial) {
    // 7.3: a partial send closes the connection; nothing taken at all is
    // FAILED. The peer never reads, so the buffers fill.
    const Listener listener = Listener::open(1);
    const SystemClock clock;
    TcpTransport transport(loopback(listener.port), clock);
    ASSERT_EQ(transport.connect(deadline_in(clock, SECOND)), IoStatus::OK);
    const Fd peer = listener.accept();
    const std::vector<std::byte> large(64U << 20U);
    EXPECT_EQ(transport.send(large), IoStatus::PARTIAL);
    const std::array<std::byte, 1> one{};
    IoStatus status = IoStatus::OK;
    for (int attempt = 0; attempt < 1'000'000 && status == IoStatus::OK;
         ++attempt) {
        status = transport.send(one);
    }
    EXPECT_EQ(status, IoStatus::FAILED);
    EXPECT_EQ(transport.last_errno(), EAGAIN);
}

TEST(TcpTransport, AnAbortiveCloseResetsAndAnOrderlyOneEnds) {
    // 7.3: an abortive close discards what is unsent (SO_LINGER 0): the
    // peer sees a reset. An orderly close is an end of file.
    const Listener listener = Listener::open(2);
    const SystemClock clock;
    TcpTransport transport(loopback(listener.port), clock);
    std::array<std::byte, 8> into{};

    ASSERT_EQ(transport.connect(deadline_in(clock, SECOND)), IoStatus::OK);
    const Fd first = listener.accept();
    transport.close(true);
    EXPECT_EQ(transport.poll_fd(), -1);
    ASSERT_TRUE(wait_ready(first.get(), POLLIN, milliseconds{5'000}));
    EXPECT_EQ(::recv(first.get(), into.data(), into.size(), 0), -1);
    EXPECT_EQ(errno, ECONNRESET);

    ASSERT_EQ(transport.connect(deadline_in(clock, SECOND)), IoStatus::OK);
    const Fd second = listener.accept();
    transport.close(false);
    ASSERT_TRUE(wait_ready(second.get(), POLLIN, milliseconds{5'000}));
    EXPECT_EQ(::recv(second.get(), into.data(), into.size(), 0), 0);
}

TEST(TcpTransport, DiscardDropsWhatIsReadableNow) {
    const Listener listener = Listener::open(1);
    const SystemClock clock;
    TcpTransport transport(loopback(listener.port), clock);
    ASSERT_EQ(transport.connect(deadline_in(clock, SECOND)), IoStatus::OK);
    const Fd peer = listener.accept();
    EXPECT_FALSE(transport.discard_input());
    const std::array<std::byte, 10> bytes{};
    ASSERT_EQ(::write(peer.get(), bytes.data(), bytes.size()), 10);
    ASSERT_TRUE(wait_ready(transport.poll_fd(), POLLIN, milliseconds{5'000}));
    EXPECT_TRUE(transport.discard_input());
    EXPECT_FALSE(transport.discard_input());
}

// --- Serial, against the simulator on a pseudo-terminal ---------------------

CompiledChannel serial_channel(std::int64_t turnaround_ns) {
    CompiledChannel channel = one_device(TransportKind::SERIAL);
    channel.line = LineTiming{.bit_rate = 19'200,
                              .bits_per_char = bits_per_char(8, false, 1),
                              .turnaround_ns = turnaround_ns};
    return channel;
}

TEST(SerialTransport, AnEngineReadsADeviceThroughTheSimulator) {
    auto sim = start_sim(
        {"--pty", "--map", "1", "0", "0", "3", "--set", "1", "0", "0", "4321"},
        "pty");
    if (!sim.has_value()) {
        FAIL() << "the simulator did not start";
    }
    const CompiledChannel channel = serial_channel(0);
    SystemClock clock;  // the engine takes it to write
    SerialTransport transport(port(sim->where), clock);
    RecordingTags tags;
    const TestProtocol protocol;
    ChannelEngine engine(channel, protocol, transport, clock, tags);
    ASSERT_TRUE(engine.status().ok());

    EXPECT_TRUE(run_until(engine, transport, clock, RUN_LIMIT, [&] {
        return tags.point(0, 0).quality == tag::Quality::GOOD;
    }));
    EXPECT_EQ(tags.point(0, 0).value, 4321);
    EXPECT_EQ(transport.last_error(), ConnectError::NONE);
}

// The requests of `sim` that came at least `gap_ns` after the answer
// before them; a request that came sooner fails the test.
std::size_t gaps_kept(const Sim& sim, std::int64_t gap_ns) {
    const auto requests = sim.lines("request");
    const auto answers = sim.lines("answer");
    std::size_t kept = 0;
    for (std::size_t index = 1;
         index < requests.size() && index - 1 < answers.size(); ++index) {
        const std::int64_t answered = std::stoll(answers[index - 1][1]);
        const std::int64_t asked = std::stoll(requests[index][1]);
        EXPECT_GE(asked - answered, gap_ns) << "request " << index;
        kept += asked - answered >= gap_ns ? 1 : 0;
    }
    return kept;
}

TEST(SerialTransport, EachRequestKeepsTheSilenceAfterTheLastAnswer) {
    // 7.2: the 3.5-character silence plus `turnaround`, from the last byte
    // received. The simulator stamps each answer before it writes it, so
    // the driver read it later: the gap it measures is a lower bound.
    auto sim = start_sim({"--pty", "--map", "1", "0", "0", "3"}, "pty");
    if (!sim.has_value()) {
        FAIL() << "the simulator did not start";
    }
    const CompiledChannel channel = serial_channel(30 * MILLISECOND);
    SystemClock clock;  // the engine takes it to write
    SerialTransport transport(port(sim->where), clock);
    RecordingTags tags;
    const TestProtocol protocol;
    ChannelEngine engine(channel, protocol, transport, clock, tags);

    EXPECT_TRUE(run_until(engine, transport, clock, RUN_LIMIT,
                          [&] { return engine.counts().requests >= 6; }));
    ASSERT_TRUE(sim->await("answer", 6));
    const std::int64_t gap = inter_frame_gap_ns(channel.line);
    EXPECT_GT(gap, 30 * MILLISECOND);
    EXPECT_GE(gaps_kept(*sim, gap), 5U);
}

// Kills the simulator and waits for its end: its pty master closes.
bool kill(Sim& sim) {
    return sim.process.send_signal(SIGKILL) &&
           sim.process.wait_for_ending(START_LIMIT).has_value();
}

TEST(SerialTransport, APortThatGoesAwayIsClosed) {
    // 7.2: an error or end of file on read closes the port; the engine then
    // reopens it by its path.
    auto sim = start_sim({"--pty", "--map", "1", "0", "0", "3"}, "pty");
    if (!sim.has_value()) {
        FAIL() << "the simulator did not start";
    }
    const SystemClock clock;
    SerialTransport transport(port(sim->where), clock);
    ASSERT_EQ(transport.connect(0), IoStatus::OK);
    EXPECT_FALSE(transport.closed_by_peer());
    ASSERT_TRUE(kill(*sim));
    ASSERT_TRUE(wait_ready(transport.poll_fd(), POLLIN, milliseconds{5'000}));
    EXPECT_TRUE(transport.closed_by_peer());
    std::array<std::byte, 16> into{};
    std::size_t got = 0;
    EXPECT_EQ(transport.receive(into, deadline_in(clock, SECOND), got),
              IoStatus::CLOSED);
}

// --- Serial, against a pseudo-terminal the test holds -----------------------

TEST(SerialTransport, ThePortIsRawEightBitWithoutFlowControl) {
    const Pty pty = Pty::open();
    const SystemClock clock;
    SerialTransport transport(port(pty.path), clock);
    ASSERT_EQ(transport.connect(0), IoStatus::OK);
    termios held{};
    ASSERT_EQ(::tcgetattr(transport.poll_fd(), &held), 0);
    EXPECT_EQ(held.c_lflag & (ICANON | ECHO | ISIG | IEXTEN), 0U);
    EXPECT_EQ(held.c_iflag & (IXON | IXOFF | ICRNL | ISTRIP), 0U);
    EXPECT_EQ(held.c_oflag & OPOST, 0U);
    EXPECT_EQ(held.c_cflag & CSIZE, static_cast<tcflag_t>(CS8));
    EXPECT_NE(held.c_cflag & CLOCAL, 0U);
    EXPECT_NE(held.c_cflag & CREAD, 0U);
#ifdef CRTSCTS
    EXPECT_EQ(held.c_cflag & CRTSCTS, 0U);
#endif
    EXPECT_EQ(::cfgetospeed(&held), static_cast<speed_t>(B19200));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl has no other form
    EXPECT_NE(::fcntl(transport.poll_fd(), F_GETFL) & O_NONBLOCK, 0);
}

TEST(SerialTransport, ASecondOpenOfThePortIsBusy) {
    // Run without CAP_SYS_ADMIN (the pipeline's user), the reopen after the
    // close also checks that the close cleared TIOCEXCL, which the master
    // the test holds would otherwise keep.
    const Pty pty = Pty::open();
    const SystemClock clock;
    SerialTransport first(port(pty.path), clock);
    SerialTransport second(port(pty.path), clock);
    ASSERT_EQ(first.connect(0), IoStatus::OK);
    EXPECT_EQ(second.connect(0), IoStatus::FAILED);
    EXPECT_EQ(second.last_error(), ConnectError::BUSY);
    EXPECT_EQ(second.poll_fd(), -1);
    first.close(false);
    EXPECT_EQ(second.connect(0), IoStatus::OK);  // free again
}

TEST(SerialTransport, ARateThatIsNotStandardIsAConnectErrorOfItsOwn) {
    // 7.2, CS A.1: "never a silent nearest rate".
    const Pty pty = Pty::open();
    const SystemClock clock;
    SerialTransport transport(port(pty.path, Parity::NONE, 250'000), clock);
    EXPECT_EQ(transport.connect(0), IoStatus::FAILED);
    EXPECT_EQ(transport.last_error(), ConnectError::RATE);
    EXPECT_EQ(transport.poll_fd(), -1);
}

TEST(SerialTransport, AParityThePortDoesNotHoldIsAFormatError) {
    // A pseudo-terminal keeps no parity: the settings read back differ
    // from those asked, and the connect fails rather than run without.
    const Pty pty = Pty::open();
    const SystemClock clock;
    SerialTransport transport(port(pty.path, Parity::EVEN), clock);
    EXPECT_EQ(transport.connect(0), IoStatus::FAILED);
    EXPECT_EQ(transport.last_error(), ConnectError::FORMAT);
    EXPECT_EQ(transport.poll_fd(), -1);
}

TEST(SerialTransport, PathsThatAreNotPortsAreRefused) {
    const SystemClock clock;
    const std::vector<std::pair<std::string, ConnectError>> cases{
        {"/tmp/port", ConnectError::PATH},
        {"/dev/../etc/passwd", ConnectError::PATH},
        {"/dev//ttyS0", ConnectError::PATH},
        {"/dev/", ConnectError::PATH},
        {"/dev/scada-no-such-port", ConnectError::NO_PORT},
        {"/dev/null", ConnectError::NOT_A_TTY},
    };
    for (const auto& [path, error] : cases) {
        SerialTransport transport(port(path), clock);
        EXPECT_EQ(transport.connect(0), IoStatus::FAILED) << path;
        EXPECT_EQ(transport.last_error(), error) << path;
        EXPECT_EQ(transport.poll_fd(), -1) << path;
    }
}

TEST(SerialTransport, SendReceiveAndDiscardOverThePort) {
    const Pty pty = Pty::open();
    const SystemClock clock;
    SerialTransport transport(port(pty.path), clock);
    ASSERT_EQ(transport.connect(0), IoStatus::OK);

    const auto request = read_request(1, 0, 1);
    ASSERT_EQ(transport.send(request), IoStatus::OK);
    ASSERT_TRUE(wait_ready(pty.master.get(), POLLIN, milliseconds{5'000}));
    std::array<std::byte, 64> seen{};
    EXPECT_EQ(::read(pty.master.get(), seen.data(), seen.size()),
              static_cast<ssize_t>(REQUEST_BYTES));

    EXPECT_FALSE(transport.discard_input());
    const std::array<std::byte, 3> noise{std::byte{0xFF}, std::byte{0x0D},
                                         std::byte{0x0A}};
    ASSERT_EQ(::write(pty.master.get(), noise.data(), noise.size()), 3);
    ASSERT_TRUE(wait_ready(transport.poll_fd(), POLLIN, milliseconds{5'000}));
    EXPECT_TRUE(transport.discard_input());
    EXPECT_FALSE(transport.discard_input());

    ASSERT_EQ(::write(pty.master.get(), noise.data(), noise.size()), 3);
    std::array<std::byte, 64> into{};
    std::size_t got = 0;
    EXPECT_EQ(transport.receive(into, deadline_in(clock, SECOND), got),
              IoStatus::OK);
    ASSERT_EQ(got, 3U);  // raw: no CR to LF, no stripping
    EXPECT_TRUE(std::equal(noise.begin(), noise.end(), into.begin()));
    EXPECT_EQ(
        transport.receive(into, deadline_in(clock, 20 * MILLISECOND), got),
        IoStatus::TIMEOUT);
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

}  // namespace
}  // namespace scada::driver::test
