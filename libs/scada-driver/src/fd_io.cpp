#include "fd_io.h"

#include "scada/driver/transport.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <span>

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
        const timespec left =
            to_timespec(deadline_ns == std::numeric_limits<std::int64_t>::max()
                            ? deadline_ns
                            : deadline_ns - clock.monotonic_ns());
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

IoStatus receive_by(int descriptor, bool socket, std::span<std::byte> into,
                    std::int64_t deadline_ns, const Clock& clock,
                    std::size_t& received, int& error) noexcept {
    received = 0;
    short revents = 0;
    if (wait_for(descriptor, POLLIN, deadline_ns, clock, revents, error) ==
        Readiness::FAILED) {
        return IoStatus::CLOSED;
    }
    return read_once(descriptor, socket, into, received, error);
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
