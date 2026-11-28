// The test protocol's simulator as a process of its own, for the
// integration tests of the real transports (driver interface contract 14:
// "a separate process that serves simulated devices over the real
// transport (a TCP port, a pseudo-terminal pair)"). It answers with the
// in-process SimBus (loopback.h) and reports, on standard output, its
// endpoint and every request and answer with its CLOCK_MONOTONIC time
// (item 11), which the test reads; the clock is the same for both
// processes. The Modbus simulators of #105 and #106 add the control socket
// and the faults of items 2 to 10; this one has the few the transport
// tests need, set on the command line:
//
//   scada-driver-test-sim (--tcp | --pty)
//       [--map UNIT SPACE FIRST LAST] [--set UNIT SPACE ADDRESS VALUE]
//       [--silent UNIT] [--silent-for UNIT REQUESTS] [--close-after ANSWERS]
//
// Output lines: `tcp PORT` or `pty PATH` once ready; `request TIME UNIT
// FIRST` when a request's last byte was read; `answer TIME` just before its
// answer was written; `closed` when it closed a connection after
// `--close-after` answers (it then accepts the next one); `peer closed`
// or `peer reset` when the driver ended a TCP connection with a FIN or a
// reset (the stop's orderly close, DI 10). A connection the driver resets
// while an answer is being written ends as any other (`peer reset` or `peer
// closed`): SIGPIPE is blocked, so the write fails instead of ending the
// simulator.

#include "loopback.h"
#include "scada/common/clock.h"
#include "scada/common/fd.h"
#include "test_protocol.h"

#include <signal.h>  // NOLINT(modernize-deprecated-headers): pthread_sigmask is declared here, not in <csignal>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

using scada::driver::test::REQUEST_BYTES;
using scada::driver::test::SimBus;

constexpr int EXIT_USAGE = 2;
constexpr std::size_t READ_CHUNK = 512;
constexpr std::size_t PATH_CAPACITY = 128;

// Writes one line to standard output at once, so that the test sees it
// when it is written.
void say(std::string_view line) {
    std::string text(line);
    text.push_back('\n');
    std::span<const char> rest(text);
    while (!rest.empty()) {
        const ssize_t wrote = ::write(STDOUT_FILENO, rest.data(), rest.size());
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote <= 0) {
            std::_Exit(1);  // the test has gone
        }
        rest = rest.subspan(static_cast<std::size_t>(wrote));
    }
}

template <class T>
std::optional<T> number(std::string_view text) {
    T value{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// A register that changes once unit `unit` received `after` requests: a
// device that restarts during the test (`--set-after`).
struct LaterSet {
    std::uint8_t unit = 0;
    std::uint8_t space = 0;
    std::uint32_t address = 0;
    std::uint16_t value = 0;
    std::uint32_t after = 0;
    bool done = false;
};

// A unit silent for its first `left` requests, then answering again: a
// device that dies and comes back (`--silent-for`).
struct SilentFor {
    std::uint8_t unit = 0;
    std::uint32_t left = 0;
};

struct Options {
    bool tcp = false;
    bool pty = false;
    std::optional<std::size_t> close_after;
    std::vector<LaterSet> later;
    std::vector<SilentFor> silent_for;
};

// The `count` numbers after `args[index]`; empty when they are not there.
std::vector<std::uint32_t> numbers_after(std::span<char*> args,
                                         std::size_t index, std::size_t count) {
    std::vector<std::uint32_t> values;
    if (args.size() - index <= count) {
        return values;
    }
    for (char* const arg : args.subspan(index + 1, count)) {
        const auto value = number<std::uint32_t>(arg);
        if (!value) {
            return {};
        }
        values.push_back(*value);
    }
    return values;
}

// Applies the option at `args[index]` that takes numbers, and moves `index`
// past them; false on a malformed one.
bool apply(std::span<char*> args, std::size_t& index, SimBus& bus,
           Options& options) {
    const std::string_view arg = args[index];
    // The numbers each option takes: unit, space, address or first, and
    // value or last; `--set-after` adds its count of requests.
    constexpr std::size_t REGISTER_NUMBERS = 4;
    constexpr std::size_t LATER_NUMBERS = REGISTER_NUMBERS + 1;
    std::size_t count = 1;
    if (arg == "--map" || arg == "--set") {
        count = REGISTER_NUMBERS;
    } else if (arg == "--set-after") {
        count = LATER_NUMBERS;
    } else if (arg == "--silent-for") {
        count = 2;
    }
    const std::vector<std::uint32_t> values = numbers_after(args, index, count);
    if (values.empty()) {
        return false;
    }
    index += count;
    const auto unit = static_cast<std::uint8_t>(values[0]);
    if (arg == "--map") {
        bus.map(unit, static_cast<std::uint8_t>(values[1]), values[2],
                values[3]);
    } else if (arg == "--set") {
        bus.set(unit, static_cast<std::uint8_t>(values[1]), values[2],
                static_cast<std::uint16_t>(values[3]));
    } else if (arg == "--set-after") {
        options.later.push_back(
            LaterSet{.unit = unit,
                     .space = static_cast<std::uint8_t>(values[1]),
                     .address = values[2],
                     .value = static_cast<std::uint16_t>(values[3]),
                     .after = values[4]});
    } else if (arg == "--silent") {
        bus.fault(unit, SimBus::Fault::SILENT);
    } else if (arg == "--silent-for") {
        bus.fault(unit, SimBus::Fault::SILENT);
        options.silent_for.push_back(
            SilentFor{.unit = unit, .left = values[1]});
    } else if (arg == "--close-after") {
        options.close_after = values[0];
    } else {
        return false;
    }
    return true;
}

// Reads the options into `bus`; false on a malformed command line.
bool parse(std::span<char*> args, SimBus& bus, Options& options) {
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string_view arg = args[index];
        if (arg == "--tcp") {
            options.tcp = true;
        } else if (arg == "--pty") {
            options.pty = true;
        } else if (!apply(args, index, bus, options)) {
            return false;
        }
    }
    return options.tcp != options.pty;
}

bool write_all(int descriptor, std::span<const std::byte> bytes) {
    std::span<const std::byte> rest = bytes;
    while (!rest.empty()) {
        const ssize_t wrote = ::write(descriptor, rest.data(), rest.size());
        if (wrote < 0 && (errno == EINTR || errno == EAGAIN)) {
            pollfd entry{.fd = descriptor, .events = POLLOUT, .revents = 0};
            (void)::poll(&entry, 1, -1);
            continue;
        }
        if (wrote <= 0) {
            return false;
        }
        rest = rest.subspan(static_cast<std::size_t>(wrote));
    }
    return true;
}

// Takes the whole requests out of `pending` and answers each on `fd`.
// Returns the answers written.
std::size_t serve(std::vector<std::byte>& pending, SimBus& bus, int descriptor,
                  std::vector<LaterSet>& later,
                  std::vector<SilentFor>& silent_for) {
    std::size_t answered = 0;
    while (pending.size() >= REQUEST_BYTES) {
        const std::span<const std::byte> request =
            std::span(pending).first(REQUEST_BYTES);
        const std::int64_t now = scada::monotonic_ns();
        const auto unit = std::to_integer<unsigned>(request[2]);
        const auto first = (std::to_integer<std::uint32_t>(request[5]) << 24U) |
                           (std::to_integer<std::uint32_t>(request[6]) << 16U) |
                           (std::to_integer<std::uint32_t>(request[7]) << 8U) |
                           std::to_integer<std::uint32_t>(request[8]);
        say("request " + std::to_string(now) + " " + std::to_string(unit) +
            " " + std::to_string(first));
        // A change due after this many requests to its unit, before the
        // answer: the answer to request `after` + 1 has the new value.
        for (LaterSet& change : later) {
            if (change.unit != unit || change.done) {
                continue;
            }
            if (change.after == 0) {
                bus.set(change.unit, change.space, change.address,
                        change.value);
                change.done = true;
            } else {
                --change.after;
            }
        }
        const std::vector<std::byte> answer = bus.answer(request, now);
        // The last silent request of a `--silent-for` unit: it answers from
        // the next one on.
        for (SilentFor& silent : silent_for) {
            if (silent.unit == unit && silent.left > 0 && --silent.left == 0) {
                bus.fault(silent.unit, SimBus::Fault::NONE);
            }
        }
        pending.erase(
            pending.begin(),
            pending.begin() + static_cast<std::ptrdiff_t>(REQUEST_BYTES));
        if (answer.empty()) {
            continue;  // a silent unit
        }
        // Stamped before the write: the driver cannot have read a byte of
        // the answer before this time.
        const std::int64_t written = scada::monotonic_ns();
        if (!write_all(descriptor, answer)) {
            return answered;
        }
        say("answer " + std::to_string(written));
        ++answered;
    }
    return answered;
}

// How the peer ended a connection.
enum class Input : std::uint8_t { MORE = 1, END, RESET };

// Reads what `descriptor` has: MORE, or how the connection ended.
Input take_input(int descriptor, std::vector<std::byte>& pending) {
    std::array<std::byte, READ_CHUNK> chunk{};
    const ssize_t got = ::read(descriptor, chunk.data(), chunk.size());
    if (got < 0 && (errno == EINTR || errno == EAGAIN)) {
        return Input::MORE;
    }
    if (got < 0) {
        return errno == ECONNRESET ? Input::RESET : Input::END;
    }
    if (got == 0) {
        return Input::END;
    }
    pending.insert(pending.end(), chunk.begin(),
                   chunk.begin() + static_cast<std::ptrdiff_t>(got));
    return Input::MORE;
}

int run_tcp(SimBus& bus, Options& options) {
    const scada::Fd listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!listener.valid()) {
        return 1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    (void)::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    address.sin_port = 0;
    socklen_t length = sizeof(address);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,scada-banned): the socket interface takes every address type through a sockaddr pointer
    auto* generic = reinterpret_cast<sockaddr*>(&address);
    if (::bind(listener.get(), generic, sizeof(address)) != 0 ||
        ::listen(listener.get(), 1) != 0 ||
        ::getsockname(listener.get(), generic, &length) != 0) {
        return 1;
    }
    say("tcp " + std::to_string(ntohs(address.sin_port)));
    for (;;) {
        scada::Fd client(
            ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC));
        if (!client.valid()) {
            if (errno == EINTR) {
                continue;
            }
            return 1;
        }
        std::vector<std::byte> pending;
        std::size_t answered = 0;
        Input input = Input::MORE;
        while ((input = take_input(client.get(), pending)) == Input::MORE) {
            answered += serve(pending, bus, client.get(), options.later,
                              options.silent_for);
            if (options.close_after && answered >= *options.close_after) {
                break;
            }
        }
        client.reset();  // closed before it says so
        if (input == Input::END) {
            say("peer closed");
        } else if (input == Input::RESET) {
            say("peer reset");
        } else {
            say("closed");
        }
    }
}

int run_pty(SimBus& bus, Options& options) {
    const scada::Fd master(::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC));
    std::array<char, PATH_CAPACITY> path{};
    if (!master.valid() || ::grantpt(master.get()) != 0 ||
        ::unlockpt(master.get()) != 0 ||
        ::ptsname_r(master.get(), path.data(), path.size()) != 0) {
        return 1;
    }
    // The simulator holds the other end open too, so that the master does
    // not see a hang-up before the transport opens it, or between two of
    // its connects.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open has no other form
    const scada::Fd held(::open(path.data(), O_RDWR | O_NOCTTY | O_CLOEXEC));
    if (!held.valid()) {
        return 1;
    }
    say(std::string("pty ") + path.data());
    std::vector<std::byte> pending;
    while (take_input(master.get(), pending) == Input::MORE) {
        (void)serve(pending, bus, master.get(), options.later,
                    options.silent_for);
    }
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    // A driver that gives up a read resets the connection, at any time
    // (DI 10): the write of an answer then fails with EPIPE, and the
    // simulator goes on to the next connection. Blocked, as the driver
    // blocks it (D-142): the simulator has one thread.
    sigset_t pipe{};
    if (::sigemptyset(&pipe) != 0 || ::sigaddset(&pipe, SIGPIPE) != 0 ||
        ::pthread_sigmask(SIG_BLOCK, &pipe, nullptr) != 0) {
        return 1;
    }
    SimBus bus;
    Options options;
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    if (args.size() < 2 || !parse(args.subspan(1), bus, options)) {
        say("usage: scada-driver-test-sim (--tcp | --pty) [--map U S F L] "
            "[--set U S A V] [--set-after U S A V N] [--silent U] "
            "[--silent-for U N] "
            "[--close-after N]");
        return EXIT_USAGE;
    }
    return options.tcp ? run_tcp(bus, options) : run_pty(bus, options);
}
