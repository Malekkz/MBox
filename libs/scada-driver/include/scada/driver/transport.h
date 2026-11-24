#ifndef SCADA_DRIVER_TRANSPORT_H
#define SCADA_DRIVER_TRANSPORT_H

// What the channel engine needs of a connection and of time (driver
// interface contract 7.1 to 7.3). The real TCP and serial transports come
// with slice 4 and keep the rules stated here; the conformance tests drive a
// simulated one on an injected clock, so every deadline is exact and
// nothing sleeps.

#include "scada/common/realtime.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::driver {

// The result of one transport call.
enum class IoStatus : std::uint8_t {
    OK = 1,
    TIMEOUT,  // the deadline passed first; nothing readable, nothing sent
    CLOSED,   // the peer closed the connection, or the port went away
    REFUSED,  // connect: the peer refused (a diagnostic of its own, 7.3)
    FAILED,   // connect or send: failed with nothing sent
    PARTIAL,  // send: the frame was not taken whole (7.2, 7.3)
};

// One connection of a channel: a TCP socket or a serial port, opened
// non-blocking (7.2). Written by the channel's worker only. None of these
// calls is real-time: each may block up to its deadline and makes system
// calls (DI 2.5); the engine calls them outside every SCADA_RT function.
//
// Waiting: the engine does not wait while idle. The worker's loop (slice 5)
// waits on the connection (POLLIN, POLLRDHUP), on the order eventfd of the
// command thread (slices 6 and 7) and on the time `ChannelEngine::step`
// returned, in one `poll`, then calls `step` again; `closed_by_peer` is how
// the engine learns of an idle close (7.3 "Idle close").
class Transport {
public:
    Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;
    virtual ~Transport() = default;

    // Opens the connection, giving up at `deadline_ns` (the channel's
    // `connect_timeout`): OK, TIMEOUT, REFUSED or FAILED. A connect that
    // does not return OK leaves the transport closed.
    virtual IoStatus connect(std::int64_t deadline_ns) noexcept = 0;

    // Closes the connection. `abortive`: unsent and unacknowledged bytes
    // are discarded (`SO_LINGER` 0, 7.3), and output is flushed first on a
    // serial port.
    virtual void close(bool abortive) noexcept = 0;

    // True when the peer has closed the connection or the port went away,
    // learnt without blocking (a socket check: POLLRDHUP, or a read of 0).
    // The engine asks before every send and at every idle boundary, since
    // a send is accepted after the peer's FIN.
    [[nodiscard]] virtual bool closed_by_peer() noexcept = 0;

    // Sends one whole frame: OK, CLOSED, FAILED (nothing sent) or PARTIAL
    // (some bytes left: on `tcp` and `rtu_over_tcp` the engine closes the
    // connection, on a serial line it treats the request as timed out).
    virtual IoStatus send(std::span<const std::byte> frame) noexcept = 0;

    // Waits until some bytes are readable or `deadline_ns` passed, then
    // reads once without blocking: at most `into.size()` bytes into `into`,
    // setting `received`. OK with `received` > 0; TIMEOUT only when nothing
    // is readable after the wait (a worker stalled past its deadline still
    // reads what arrived); CLOSED.
    virtual IoStatus receive(std::span<std::byte> into,
                             std::int64_t deadline_ns,
                             std::size_t& received) noexcept = 0;

    // Drops every byte received and not yet read (7.2). Bytes that arrive
    // later are not affected: the engine discards again before its next
    // send after a quiet time.
    virtual void discard_input() noexcept = 0;
};

// The two clocks of the worker: CLOCK_MONOTONIC for deadlines and
// staleness, CLOCK_REALTIME for the receive time of samples (5.1).
class Clock {
public:
    Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    Clock(Clock&&) = delete;
    Clock& operator=(Clock&&) = delete;
    virtual ~Clock() = default;

    SCADA_RT [[nodiscard]] virtual std::int64_t monotonic_ns()
        const noexcept = 0;
    SCADA_RT [[nodiscard]] virtual std::int64_t realtime_ns()
        const noexcept = 0;
};

}  // namespace scada::driver

#endif  // SCADA_DRIVER_TRANSPORT_H
