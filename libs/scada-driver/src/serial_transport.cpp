#include "fd_io.h"
#include "scada/common/fd.h"
#include "scada/driver/connections.h"
#include "scada/driver/transport.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <termios.h>

#if __has_include(<linux/serial.h>)
#include <linux/serial.h>
#endif

namespace scada::driver {
namespace {

// The standard rates of the terminal interface (D-141). The rates above
// 38,400 bit/s are not in POSIX; each is taken where the C library has it.
struct Rate {
    std::uint32_t baud;
    speed_t speed;
};

constexpr std::array RATES{
    Rate{300, B300},           Rate{600, B600},      Rate{1'200, B1200},
    Rate{1'800, B1800},        Rate{2'400, B2400},   Rate{4'800, B4800},
    Rate{9'600, B9600},        Rate{19'200, B19200}, Rate{38'400, B38400},
#ifdef B57600
    Rate{57'600, B57600},
#endif
#ifdef B115200
    Rate{115'200, B115200},
#endif
#ifdef B230400
    Rate{230'400, B230400},
#endif
#ifdef B460800
    Rate{460'800, B460800},
#endif
#ifdef B500000
    Rate{500'000, B500000},
#endif
#ifdef B576000
    Rate{576'000, B576000},
#endif
#ifdef B921600
    Rate{921'600, B921600},
#endif
#ifdef B1000000
    Rate{1'000'000, B1000000},
#endif
#ifdef B1152000
    Rate{1'152'000, B1152000},
#endif
#ifdef B1500000
    Rate{1'500'000, B1500000},
#endif
#ifdef B2000000
    Rate{2'000'000, B2000000},
#endif
#ifdef B2500000
    Rate{2'500'000, B2500000},
#endif
#ifdef B3000000
    Rate{3'000'000, B3000000},
#endif
#ifdef B3500000
    Rate{3'500'000, B3500000},
#endif
#ifdef B4000000
    Rate{4'000'000, B4000000},
#endif
};

bool speed_of(std::uint32_t baud, speed_t& out) noexcept {
    for (const Rate& rate : RATES) {
        if (rate.baud == baud) {
            out = rate.speed;
            return true;
        }
    }
    return false;
}

// CS A.1 `device`: an absolute path under /dev/, at most 255 bytes, no
// empty, `.` or `..` component, no NUL. The compile step checks it too;
// the transport takes nothing on trust (CLAUDE.md).
bool valid_path(std::string_view path) noexcept {
    constexpr std::string_view PREFIX = "/dev/";
    constexpr std::size_t MAX_PATH_BYTES = 255;
    if (path.size() <= PREFIX.size() || path.size() > MAX_PATH_BYTES ||
        !path.starts_with(PREFIX) ||
        path.find('\0') != std::string_view::npos) {
        return false;
    }
    std::string_view rest = path.substr(1);
    while (!rest.empty()) {
        const std::size_t slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        rest = rest.substr(slash + 1);
        if (rest.empty()) {
            return false;  // a trailing slash: an empty last component
        }
    }
    return true;
}

ConnectError open_error(int number) noexcept {
    switch (number) {
        case ENOENT:
        case ENODEV:
        case ENXIO:
            return ConnectError::NO_PORT;
        case EBUSY:
            return ConnectError::BUSY;
        case EACCES:
        case EPERM:
            return ConnectError::ACCESS;
        default:
            return ConnectError::SYSTEM;
    }
}

// Mark or space parity (CMSPAR, where the C library has it) is never set:
// it is cleared with the rest and checked in the read-back, so that one
// inherited from an earlier user of the port is not kept (review M5).
#ifdef CMSPAR
constexpr tcflag_t MARK_OR_SPACE = CMSPAR;
#else
constexpr tcflag_t MARK_OR_SPACE = 0;
#endif
constexpr tcflag_t FORMAT_BITS =
    CSIZE | PARENB | PARODD | CSTOPB | MARK_OR_SPACE;

// The C library defines these in kernel headers (asm/..., linux/...) that
// <sys/ioctl.h> and <linux/serial.h> include.
// NOLINTBEGIN(misc-include-cleaner): reached through <sys/ioctl.h>
constexpr unsigned long EXCLUSIVE = TIOCEXCL;
constexpr unsigned long NOT_EXCLUSIVE = TIOCNXCL;
#if __has_include(<linux/serial.h>)
constexpr unsigned long GET_SERIAL = TIOCGSERIAL;
constexpr unsigned long SET_SERIAL = TIOCSSERIAL;
constexpr unsigned int LOW_LATENCY = ASYNC_LOW_LATENCY;
#endif
// NOLINTEND(misc-include-cleaner)

}  // namespace

bool is_standard_baud(std::uint32_t baud) noexcept {
    speed_t unused = 0;
    return speed_of(baud, unused);
}

SerialTransport::SerialTransport(SerialSettings settings, const Clock& clock)
    : settings_(std::move(settings)), clock_(&clock) {}

SerialTransport::~SerialTransport() {
    close(false);
}

IoStatus SerialTransport::fail_connect(ConnectError error,
                                       int number) noexcept {
    fd_.reset();
    error_ = error;
    errno_ = number;
    return IoStatus::FAILED;
}

ConnectError SerialTransport::configure() noexcept {
    const int port = fd_.get();
    termios settings{};
    if (::tcgetattr(port, &settings) != 0) {
        errno_ = errno;
        return errno_ == ENOTTY ? ConnectError::NOT_A_TTY
                                : ConnectError::SYSTEM;
    }
    speed_t speed = 0;
    if (!speed_of(settings_.baud, speed)) {
        return ConnectError::RATE;  // never the nearest rate (7.2)
    }
    if (settings_.stop_bits != 1 && settings_.stop_bits != 2) {
        return ConnectError::FORMAT;
    }
    // Raw 8-bit characters: no line editing, no echo, no signals, no
    // translation, no software or hardware flow control (7.2).
    settings.c_iflag &= ~static_cast<tcflag_t>(
        IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON |
        IXOFF | IXANY | INPCK | IGNPAR);
    settings.c_oflag &= ~static_cast<tcflag_t>(OPOST);
    settings.c_lflag &=
        ~static_cast<tcflag_t>(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    settings.c_cflag &= ~FORMAT_BITS;
#ifdef CRTSCTS
    settings.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);
#endif
    settings.c_cflag |= CS8 | CREAD | CLOCAL;
    if (settings_.parity != Parity::NONE) {
        // A character with a parity error is read as a 0 byte: the frame's
        // check catches it.
        settings.c_iflag |= INPCK;
        settings.c_cflag |= PARENB;
        if (settings_.parity == Parity::ODD) {
            settings.c_cflag |= PARODD;
        }
    }
    if (settings_.stop_bits == 2) {
        settings.c_cflag |= CSTOPB;
    }
    // VMIN 1: a read of an empty port is EAGAIN, not 0, which is the end of
    // file of a port that went away (O_NONBLOCK keeps it from waiting).
    settings.c_cc[VMIN] = 1;
    settings.c_cc[VTIME] = 0;
    if (::cfsetispeed(&settings, speed) != 0 ||
        ::cfsetospeed(&settings, speed) != 0) {
        errno_ = errno;
        return ConnectError::RATE;
    }
    if (::tcsetattr(port, TCSANOW, &settings) != 0) {
        errno_ = errno;
        return errno_ == EINVAL ? ConnectError::RATE : ConnectError::SYSTEM;
    }
    // `tcsetattr` succeeds when any part was taken: read back what the
    // port holds.
    termios held{};
    if (::tcgetattr(port, &held) != 0) {
        errno_ = errno;
        return ConnectError::SYSTEM;
    }
    if (::cfgetospeed(&held) != speed || ::cfgetispeed(&held) != speed) {
        return ConnectError::RATE;
    }
    const tcflag_t wanted = settings.c_cflag & FORMAT_BITS;
    if ((held.c_cflag & FORMAT_BITS) != wanted) {
        return ConnectError::FORMAT;
    }
    return ConnectError::NONE;
}

IoStatus SerialTransport::connect(std::int64_t /*deadline_ns*/) noexcept {
    close(false);
    error_ = ConnectError::NONE;
    errno_ = 0;
    low_latency_ = false;
    if (!valid_path(settings_.device)) {
        return fail_connect(ConnectError::PATH, 0);
    }
    constexpr int FLAGS = O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open has no other form
    int opened = ::open(settings_.device.c_str(), FLAGS);
    while (opened < 0 && errno == EINTR) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open has no other form
        opened = ::open(settings_.device.c_str(), FLAGS);
    }
    fd_ = Fd(opened);
    if (!fd_.valid()) {
        const int number = errno;
        return fail_connect(open_error(number), number);
    }
    // A terminal first: a wrong `device` (a file, /dev/null) is refused
    // before it is locked, never locked as if it were a port (review M4).
    termios probe{};
    if (::tcgetattr(fd_.get(), &probe) != 0) {
        const int number = errno;
        return fail_connect(
            number == ENOTTY ? ConnectError::NOT_A_TTY : ConnectError::SYSTEM,
            number);
    }
    // Exclusive: `flock` binds every process that asks, root included;
    // TIOCEXCL refuses any later open by another user (7.2).
    if (::flock(fd_.get(), LOCK_EX | LOCK_NB) != 0) {
        const int number = errno;
        return fail_connect(
            number == EWOULDBLOCK ? ConnectError::BUSY : ConnectError::SYSTEM,
            number);
    }
    const ConnectError configured = configure();
    if (configured != ConnectError::NONE) {
        return fail_connect(configured, errno_);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): ioctl has no other form
    if (::ioctl(fd_.get(), EXCLUSIVE) != 0) {
        const int number = errno;
        return fail_connect(ConnectError::SYSTEM, number);
    }
#if __has_include(<linux/serial.h>)
    // Low latency where the driver has it: a USB adapter then delivers at
    // once instead of at the end of its latency timer (7.2). A port
    // without the setting (a pseudo-terminal) keeps its own.
    serial_struct info{};
    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg): ioctl has no other form
    if (::ioctl(fd_.get(), GET_SERIAL, &info) == 0) {
        info.flags = static_cast<int>(static_cast<unsigned int>(info.flags) |
                                      LOW_LATENCY);
        low_latency_ = ::ioctl(fd_.get(), SET_SERIAL, &info) == 0;
    }
    // NOLINTEND(cppcoreguidelines-pro-type-vararg)
#endif
    // Nothing queued before this connect belongs to an exchange of it.
    (void)::tcflush(fd_.get(), TCIOFLUSH);
    return IoStatus::OK;
}

void SerialTransport::close(bool /*abortive*/) noexcept {
    if (!fd_.valid()) {
        return;
    }
    // Always: pending output dropped first, so that `close` never waits
    // for output a held-off line does not take (7.2).
    (void)::tcflush(fd_.get(), TCIOFLUSH);
    // The exclusive mode is the terminal's, not the descriptor's: it would
    // outlive this close while another descriptor holds the terminal (the
    // master of a pseudo-terminal), and refuse our own reopen.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): ioctl has no other form
    (void)::ioctl(fd_.get(), NOT_EXCLUSIVE);
    fd_.reset();
}

bool SerialTransport::closed_by_peer() noexcept {
    return !fd_.valid() || detail::hung_up(fd_.get(), false);
}

IoStatus SerialTransport::send(std::span<const std::byte> frame) noexcept {
    if (!fd_.valid()) {
        return IoStatus::CLOSED;
    }
    return detail::write_once(fd_.get(), false, frame, errno_);
}

IoStatus SerialTransport::receive(std::span<std::byte> into,
                                  std::int64_t deadline_ns,
                                  std::size_t& received) noexcept {
    received = 0;
    if (!fd_.valid()) {
        return IoStatus::CLOSED;
    }
    return detail::receive_by(fd_.get(), false, into, deadline_ns, *clock_,
                              received, errno_);
}

bool SerialTransport::discard_input() noexcept {
    return fd_.valid() && detail::drop_readable(fd_.get(), false);
}

}  // namespace scada::driver
