#ifndef SCADA_DRIVER_TRANSPORT_H
#define SCADA_DRIVER_TRANSPORT_H

// What the channel engine needs of a connection and of time (driver
// interface contract 7.1 to 7.3). The real TCP and serial transports come
// with slice 4; the conformance tests drive a simulated one on an injected
// clock, so every deadline is exact and nothing sleeps.

#include "scada/common/realtime.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::driver {

// The result of one transport call.
enum class IoStatus : std::uint8_t {
    OK = 1,
    TIMEOUT,  // the deadline passed first
    CLOSED,   // the peer closed the connection, or the port went away
    FAILED,   // refused, a partial send, an error
};

// One connection of a channel: a TCP socket or a serial port. Written by
// the channel's worker only.
class Transport {
public:
    Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;
    virtual ~Transport() = default;

    // Opens the connection, giving up at `deadline_ns` (the channel's
    // `connect_timeout`): OK, TIMEOUT or FAILED.
    virtual IoStatus connect(std::int64_t deadline_ns) noexcept = 0;

    // Closes the connection. `abortive`: unsent and unacknowledged bytes
    // are discarded (`SO_LINGER` 0, 7.3), and output is flushed first on a
    // serial port.
    virtual void close(bool abortive) noexcept = 0;

    // Sends one whole frame: OK, CLOSED, or FAILED when the frame was not
    // taken whole (7.2, 7.3).
    SCADA_RT virtual IoStatus send(
        std::span<const std::byte> frame) noexcept = 0;

    // Waits until some bytes arrived or `deadline_ns` passed, and copies at
    // most `into.size()` of them into `into`, setting `received`: OK with
    // `received` > 0, TIMEOUT with 0, or CLOSED.
    SCADA_RT virtual IoStatus receive(std::span<std::byte> into,
                                      std::int64_t deadline_ns,
                                      std::size_t& received) noexcept = 0;

    // Drops every byte received and not yet read (7.2).
    SCADA_RT virtual void discard_input() noexcept = 0;
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
