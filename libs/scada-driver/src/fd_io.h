#ifndef SCADA_DRIVER_SRC_FD_IO_H
#define SCADA_DRIVER_SRC_FD_IO_H

// The descriptor calls the TCP and serial transports share (connections.h):
// a wait on one descriptor up to a monotonic deadline, one non-blocking
// read, and the drop of what is readable now. Not real-time (DI 2.5).

#include "scada/driver/transport.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::driver::detail {

// What a wait saw.
enum class Readiness : std::uint8_t {
    READY = 1,  // one of `events`, or a hang-up or an error, is pending
    TIMEOUT,    // the deadline passed first
    FAILED,     // `ppoll` failed; `error` says why
};

// Waits in `ppoll` until `descriptor` has one of `events` (or POLLHUP,
// POLLERR), or `deadline_ns` of `clock` passed. A deadline already past polls
// once with time-out 0. An interrupted wait goes on to the same deadline.
// `revents` gets the events seen.
[[nodiscard]] Readiness wait_for(int descriptor, short events,
                                 std::int64_t deadline_ns, const Clock& clock,
                                 short& revents, int& error) noexcept;

// True when `descriptor` has POLLRDHUP (a stream), POLLHUP or POLLERR now: a
// `poll` with time-out 0 that consumes nothing.
[[nodiscard]] bool hung_up(int descriptor, bool stream) noexcept;

// One read that does not block: `recv` with MSG_DONTWAIT on a socket,
// `read` on a descriptor opened O_NONBLOCK. OK with `received` > 0;
// TIMEOUT when nothing is readable; CLOSED on an end of file or an error,
// with `error` set for an error. An empty `into` is FAILED.
[[nodiscard]] IoStatus read_once(int descriptor, bool socket,
                                 std::span<std::byte> into,
                                 std::size_t& received, int& error) noexcept;

// The time from now to `deadline_ns`, 0 when it has passed, for any
// deadline (review M2).
[[nodiscard]] std::int64_t time_left(std::int64_t deadline_ns,
                                     const Clock& clock) noexcept;

// The loop of `receive_by`, apart from its system calls so that a test can
// drive it: `wait()` waits for input up to the deadline and returns a
// Readiness; `read(received)` makes one read that does not block and
// returns OK, TIMEOUT (nothing readable), CLOSED or FAILED. After every
// wait, whatever it saw, one read: a worker stalled past its deadline still
// reads what arrived. A wait that saw input whose read then finds nothing
// (another reader took it, a wake without data) waits again while the
// deadline has not passed (review M3): only the deadline makes a TIMEOUT.
template <class Wait, class Read>
[[nodiscard]] IoStatus receive_loop(const Wait& wait, const Read& read,
                                    std::int64_t deadline_ns,
                                    const Clock& clock,
                                    std::size_t& received) noexcept {
    received = 0;
    for (;;) {
        const Readiness ready = wait();
        if (ready == Readiness::FAILED) {
            return IoStatus::CLOSED;
        }
        const IoStatus status = read(received);
        if (status != IoStatus::TIMEOUT || ready == Readiness::TIMEOUT ||
            time_left(deadline_ns, clock) == 0) {
            return status;
        }
    }
}

// True when the connected socket `descriptor` has the same local and peer
// address and port: a TCP self-connection, which a connect to one of the
// host's own addresses on a free port of the local port range can make
// with nobody listening, and which echoes every frame back as its answer
// (review M1). False on a socket that is not connected.
[[nodiscard]] bool connected_to_itself(int descriptor) noexcept;

// `receive_loop` over `wait_for` POLLIN and `read_once` on `descriptor`.
[[nodiscard]] IoStatus receive_by(int descriptor, bool socket,
                                  std::span<std::byte> into,
                                  std::int64_t deadline_ns, const Clock& clock,
                                  std::size_t& received, int& error) noexcept;

// Reads and drops what is readable now, at most DISCARD_LIMIT bytes, so
// that a line that never falls quiet cannot hold the worker here. True
// when there was any.
inline constexpr std::size_t DISCARD_LIMIT = 65'536;
[[nodiscard]] bool drop_readable(int descriptor, bool socket) noexcept;

// Writes `frame` with one call that does not block (`send` with
// MSG_NOSIGNAL and MSG_DONTWAIT on a socket, `write` on a descriptor opened
// O_NONBLOCK), again only when interrupted before any byte. OK: all of it;
// PARTIAL: some; FAILED: none, the buffer is full; CLOSED: the peer reset
// or the port went away. `error` gets the `errno` of a failure.
[[nodiscard]] IoStatus write_once(int descriptor, bool socket,
                                  std::span<const std::byte> frame,
                                  int& error) noexcept;

}  // namespace scada::driver::detail

#endif  // SCADA_DRIVER_SRC_FD_IO_H
