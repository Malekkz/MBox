#ifndef SCADA_DRIVER_CONNECTIONS_H
#define SCADA_DRIVER_CONNECTIONS_H

// The real connections of a channel behind `Transport` (transport.h): a
// TCP socket for `tcp` and `rtu_over_tcp`, a serial port for `rtu`, and the
// system's clocks (driver interface contract 7.1 to 7.3, decision D-141).
// The worker of slice 5 builds one per channel from the compiled keys and
// hands it to its ChannelEngine. Every call follows the rules transport.h
// states; what each class adds is stated here. None of it is real-time
// (DI 2.5) except the clock reads.

#include "scada/common/fd.h"
#include "scada/common/realtime.h"
#include "scada/driver/compiled.h"
#include "scada/driver/transport.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace scada::driver {

// Why the last connect of a transport did not return OK, for the channel's
// diagnostics (slice 5): a rate the port refuses has a diagnostic of its own
// (7.2, CS A.1).
enum class ConnectError : std::uint8_t {
    NONE = 0,
    ADDRESS,    // `host` is not a literal IPv4 or IPv6 address, or port 0
    PATH,       // `device` is not an absolute path under /dev/ (CS A.1)
    NO_PORT,    // no such serial port (unplugged, wrong path)
    BUSY,       // the port is open elsewhere (opened exclusively, 7.2)
    ACCESS,     // no permission to open the port
    NOT_A_TTY,  // the path is not a terminal device
    RATE,       // the port refuses the bit rate; never a nearest rate
    FORMAT,     // the port refuses the parity or the stop bits
    REFUSED,    // the peer refused the connection
    TIMEOUT,    // `connect_timeout` passed first
    SELF,       // the socket connected to itself (review M1): nobody there
    SYSTEM,     // any other failure; `last_errno` says which
};

// The system's clocks: CLOCK_MONOTONIC and CLOCK_REALTIME (5.1). A read is
// `clock_gettime`, which the C library answers without entering the kernel
// on the supported targets.
class SystemClock final : public Clock {
public:
    SCADA_RT [[nodiscard]] std::int64_t monotonic_ns() const noexcept override;
    SCADA_RT [[nodiscard]] std::int64_t realtime_ns() const noexcept override;
};

// The socket's user timeout of a channel (7.3): `lost_after_timeouts` times
// the largest request deadline of its devices (`timeout`, `write_timeout`),
// at least 10 s, so that a short loss burst is not a lost connection for
// every unit. Saturates.
[[nodiscard]] std::int64_t tcp_user_timeout_ns(
    const CompiledChannel& channel) noexcept;

inline constexpr std::int64_t MIN_TCP_USER_TIMEOUT_NS = 10'000'000'000;

// Where a TCP channel connects (CS A.1: `host`, `port`).
struct TcpEndpoint {
    std::string host;  // a literal IPv4 or IPv6 address, never a name
    std::uint16_t port = 0;
    // `TCP_USER_TIMEOUT`, from `tcp_user_timeout_ns`.
    std::int64_t user_timeout_ns = MIN_TCP_USER_TIMEOUT_NS;
};

// True when `host` is a literal IPv4 address (dotted quad) or IPv6 address
// as `inet_pton` reads them: no name, no brackets, no zone index.
[[nodiscard]] bool is_ip_literal(std::string_view host) noexcept;

// A TCP connection (7.3). `connect` opens a non-blocking socket with
// TCP_NODELAY (a request is one frame, sent whole) and TCP_USER_TIMEOUT,
// and waits for it up to the deadline in `ppoll`: the engine's connect
// blocks the worker up to `connect_timeout` (D-140, Q14). REFUSED is a
// refusal by the peer; a socket that connected to itself (no peer at
// all, review M1) is FAILED with ConnectError::SELF; a host that is not a
// literal, or port 0, is FAILED with
// ConnectError::ADDRESS and makes no system call. `send` never blocks: a
// frame the kernel does not take whole is PARTIAL, nothing taken (a full
// send buffer) FAILED, a reset or a closed peer CLOSED. `receive` waits in
// `ppoll` up to the deadline, then reads once without blocking (review
// G4). `discard_input` reads and drops what is readable now, for the quiet
// time of `rtu_over_tcp` (7.3). The clock gives "now" for every deadline and
// must outlive the transport.
class TcpTransport final : public Transport {
public:
    TcpTransport(TcpEndpoint endpoint, const Clock& clock);
    // The clock is kept by reference: never a temporary (S5).
    TcpTransport(TcpEndpoint endpoint, const Clock&& clock) = delete;
    ~TcpTransport() override;
    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;
    TcpTransport(TcpTransport&&) = delete;
    TcpTransport& operator=(TcpTransport&&) = delete;

    IoStatus connect(std::int64_t deadline_ns) noexcept override;
    void close(bool abortive) noexcept override;
    [[nodiscard]] int poll_fd() const noexcept override { return fd_.get(); }
    [[nodiscard]] bool closed_by_peer() noexcept override;
    IoStatus send(std::span<const std::byte> frame) noexcept override;
    IoStatus receive(std::span<std::byte> into, std::int64_t deadline_ns,
                     std::size_t& received) noexcept override;
    bool discard_input() noexcept override;

    // Why the last connect failed; NONE after one that returned OK.
    [[nodiscard]] ConnectError last_error() const noexcept { return error_; }
    // The `errno` of the last system call that failed, in any call since
    // the last connect, which sets it to 0 first (S1). 0 when no system call
    // failed: after a connect that returned OK, and after one the transport
    // itself refused or that ran out of time (ADDRESS, TIMEOUT, SELF).
    [[nodiscard]] int last_errno() const noexcept { return errno_; }

private:
    IoStatus fail_connect(ConnectError error, int number,
                          IoStatus status) noexcept;

    TcpEndpoint endpoint_;
    const Clock* clock_;
    Fd fd_;
    ConnectError error_ = ConnectError::NONE;
    int errno_ = 0;
};

// The character format of a serial port (CS A.1: `baud`, `parity`,
// `stop_bits`; `data_bits` is 8 on RTU).
enum class Parity : std::uint8_t { NONE = 1, EVEN, ODD };

struct SerialSettings {
    std::string device;  // the port's path under /dev/
    std::uint32_t baud = 0;
    Parity parity = Parity::EVEN;
    std::uint8_t stop_bits = 1;  // 1 or 2
};

// True when `baud` is a rate the serial transport can set: the standard
// rates of the terminal interface from 300 to 4,000,000 bit/s (D-141). Any
// other rate of CS's range is refused at connect with ConnectError::RATE.
[[nodiscard]] bool is_standard_baud(std::uint32_t baud) noexcept;

// A serial port (7.2). `connect` opens the port by its configured path,
// non-blocking, without a controlling terminal, and exclusively (`flock`,
// which binds every process, and `TIOCEXCL`, which binds every open
// without CAP_SYS_ADMIN);
// sets raw 8-bit characters with the parity and stop bits, no flow control
// of any kind, the receiver on and the modem lines ignored; checks by
// reading the settings back that the port took the rate and the format;
// asks the driver for low latency where it has the setting (best effort:
// a pseudo-terminal has none); and drops what was queued before. It never
// waits: the deadline is not used. `close` drops both queues (`tcflush`)
// before it closes, so that a close never waits for output held off (7.2).
// `close` also clears the exclusive mode, which belongs to the terminal and
// would refuse the next open while another descriptor holds it.
// `closed_by_peer` is a `poll` with time-out 0 for POLLHUP or POLLERR: a
// port that went away. `send` writes the frame whole or reports PARTIAL
// (the engine takes it as a time-out); `receive` and `discard_input` as on
// TCP; an end of file or an error on read is CLOSED, so that a replugged
// adapter is reopened by its path. The clock must outlive the transport.
class SerialTransport final : public Transport {
public:
    SerialTransport(SerialSettings settings, const Clock& clock);
    SerialTransport(SerialSettings settings, const Clock&& clock) = delete;
    ~SerialTransport() override;
    SerialTransport(const SerialTransport&) = delete;
    SerialTransport& operator=(const SerialTransport&) = delete;
    SerialTransport(SerialTransport&&) = delete;
    SerialTransport& operator=(SerialTransport&&) = delete;

    IoStatus connect(std::int64_t deadline_ns) noexcept override;
    void close(bool abortive) noexcept override;
    [[nodiscard]] int poll_fd() const noexcept override { return fd_.get(); }
    [[nodiscard]] bool closed_by_peer() noexcept override;
    IoStatus send(std::span<const std::byte> frame) noexcept override;
    IoStatus receive(std::span<std::byte> into, std::int64_t deadline_ns,
                     std::size_t& received) noexcept override;
    bool discard_input() noexcept override;

    // As TcpTransport's: why the last connect failed (NONE after OK), and
    // the `errno` of the last failed system call since the last connect, 0
    // when none failed (PATH, RATE and FORMAT are the transport's own).
    [[nodiscard]] ConnectError last_error() const noexcept { return error_; }
    [[nodiscard]] int last_errno() const noexcept { return errno_; }
    // True when the last connect set the port's low-latency mode.
    [[nodiscard]] bool low_latency() const noexcept { return low_latency_; }

private:
    IoStatus fail_connect(ConnectError error, int number) noexcept;
    [[nodiscard]] ConnectError configure() noexcept;

    SerialSettings settings_;
    const Clock* clock_;
    Fd fd_;
    ConnectError error_ = ConnectError::NONE;
    int errno_ = 0;
    bool low_latency_ = false;
};

}  // namespace scada::driver

#endif  // SCADA_DRIVER_CONNECTIONS_H
