#include "fd_io.h"

#include "scada/driver/transport.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iterator>
#include <span>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace scada::driver::detail {
namespace {

constexpr std::int64_t NS_PER_S = 1'000'000'000;

timespec to_timespec(std::int64_t duration_ns) noexcept {
    const std::int64_t clamped = std::max<std::int64_t>(duration_ns, 0);
    return timespec{.tv_sec = static_cast<time_t>(clamped / NS_PER_S),
                    .tv_nsec = static_cast<long>(clamped % NS_PER_S)};
}

// One non-blocking read call, again when interrupted.
ssize_t read_call(int descriptor, bool socket,
                  std::span<std::byte> into) noexcept {
    for (;;) {
        const ssize_t got =
            socket ? ::recv(descriptor, into.data(), into.size(), MSG_DONTWAIT)
                   : ::read(descriptor, into.data(), into.size());
        if (got >= 0 || errno != EINTR) {
            return got;
        }
    }
}

}  // namespace

Readiness wait_for(int descriptor, short events, std::int64_t deadline_ns,
                   const Clock& clock, short& revents, int& error) noexcept {
    pollfd entry{.fd = descriptor, .events = events, .revents = 0};
    for (;;) {
        const timespec left = to_timespec(time_left(deadline_ns, clock));
        const int ready = ::ppoll(&entry, 1, &left, nullptr);
        if (ready > 0) {
            revents = entry.revents;
            return Readiness::READY;
        }
        if (ready == 0) {
            revents = 0;
            return Readiness::TIMEOUT;
        }
        if (errno != EINTR) {
            error = errno;
            return Readiness::FAILED;
        }
    }
}

bool hung_up(int descriptor, bool stream) noexcept {
    // POLLHUP and POLLERR are reported whatever `events` asks.
    const auto watched = static_cast<short>(stream ? POLLRDHUP : 0);
    pollfd entry{.fd = descriptor, .events = watched, .revents = 0};
    int ready = ::poll(&entry, 1, 0);
    while (ready < 0 && errno == EINTR) {
        ready = ::poll(&entry, 1, 0);
    }
    if (ready < 0) {
        return true;  // the descriptor is unusable: as good as closed
    }
    return (entry.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) != 0;
}

IoStatus read_once(int descriptor, bool socket, std::span<std::byte> into,
                   std::size_t& received, int& error) noexcept {
    received = 0;
    if (into.empty()) {
        return IoStatus::FAILED;
    }
    const ssize_t got = read_call(descriptor, socket, into);
    if (got > 0) {
        received = static_cast<std::size_t>(got);
        return IoStatus::OK;
    }
    if (got == 0) {
        return IoStatus::CLOSED;  // end of file: the peer or the port is gone
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return IoStatus::TIMEOUT;
    }
    error = errno;
    return IoStatus::CLOSED;  // a reset, a user time-out, EIO of a lost port
}

std::int64_t time_left(std::int64_t deadline_ns, const Clock& clock) noexcept {
    const std::int64_t now = clock.monotonic_ns();
    // No subtraction that can overflow: a deadline at or before now leaves
    // nothing, whatever its value (review M2).
    return deadline_ns <= now ? 0 : deadline_ns - now;
}

IoStatus receive_by(int descriptor, bool socket, std::span<std::byte> into,
                    std::int64_t deadline_ns, const Clock& clock,
                    std::size_t& received, int& error) noexcept {
    return receive_loop(
        [&] {
            short revents = 0;
            return wait_for(descriptor, POLLIN, deadline_ns, clock, revents,
                            error);
        },
        [&](std::size_t& got) {
            return read_once(descriptor, socket, into, got, error);
        },
        deadline_ns, clock, received);
}

namespace {

// One end's address of `descriptor`, read into `out` (sockaddr_in6 holds
// either family's); false if it has none or it does not fit.
template <class Address>
bool end_address(int descriptor, bool peer, Address& out) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,scada-banned): the socket interface takes every address type through a sockaddr pointer
    auto* generic = reinterpret_cast<sockaddr*>(&out);
    socklen_t length = sizeof(out);
    const int result = peer ? ::getpeername(descriptor, generic, &length)
                            : ::getsockname(descriptor, generic, &length);
    return result == 0 && length <= sizeof(out);
}

}  // namespace

bool connected_to_itself(int descriptor) noexcept {
    sockaddr_in6 local{};
    sockaddr_in6 peer{};
    if (!end_address(descriptor, false, local) ||
        !end_address(descriptor, true, peer) ||
        local.sin6_family != peer.sin6_family) {
        return false;
    }
    if (local.sin6_family == AF_INET6) {
        return local.sin6_port == peer.sin6_port &&
               std::equal(std::begin(local.sin6_addr.s6_addr),
                          std::end(local.sin6_addr.s6_addr),
                          std::begin(peer.sin6_addr.s6_addr));
    }
    sockaddr_in local4{};
    sockaddr_in peer4{};
    return local.sin6_family == AF_INET &&
           end_address(descriptor, false, local4) &&
           end_address(descriptor, true, peer4) &&
           local4.sin_port == peer4.sin_port &&
           local4.sin_addr.s_addr == peer4.sin_addr.s_addr;
}

bool drop_readable(int descriptor, bool socket) noexcept {
    constexpr std::size_t CHUNK = 256;
    std::array<std::byte, CHUNK> sink{};
    std::size_t dropped = 0;
    while (dropped < DISCARD_LIMIT) {
        const ssize_t got = read_call(descriptor, socket, sink);
        if (got <= 0) {
            break;  // nothing more now, an end of file, or an error
        }
        dropped += static_cast<std::size_t>(got);
    }
    return dropped > 0;
}

IoStatus write_once(int descriptor, bool socket,
                    std::span<const std::byte> frame, int& error) noexcept {
    for (;;) {
        const ssize_t sent =
            socket ? ::send(descriptor, frame.data(), frame.size(),
                            MSG_NOSIGNAL | MSG_DONTWAIT)
                   : ::write(descriptor, frame.data(), frame.size());
        if (sent == 0 && !frame.empty()) {
            return IoStatus::FAILED;  // nothing taken
        }
        if (sent >= 0) {
            return static_cast<std::size_t>(sent) == frame.size()
                       ? IoStatus::OK
                       : IoStatus::PARTIAL;
        }
        if (errno == EINTR) {
            continue;
        }
        error = errno;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return IoStatus::FAILED;  // nothing sent: the buffer is full
        }
        return IoStatus::CLOSED;
    }
}

}  // namespace scada::driver::detail
