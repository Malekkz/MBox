#include "fd_io.h"
#include "scada/common/fd.h"
#include "scada/driver/compiled.h"
#include "scada/driver/connections.h"
#include "scada/driver/transport.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>

namespace scada::driver {
namespace {

constexpr std::int64_t NS_PER_MS = 1'000'000;

// The longest literal `inet_pton` reads, with its terminating NUL.
constexpr std::size_t LITERAL_CAPACITY = INET6_ADDRSTRLEN;

// `host` as a NUL-terminated copy for `inet_pton`; false when it cannot be
// a literal (empty, too long, an embedded NUL).
bool terminated(std::string_view host,
                std::array<char, LITERAL_CAPACITY>& out) noexcept {
    if (host.empty() || host.size() >= out.size() ||
        host.find('\0') != std::string_view::npos) {
        return false;
    }
    // `out` is all zeros: the copy leaves its terminating NUL.
    std::copy(host.begin(), host.end(), out.begin());
    return true;
}

// The C library defines these in kernel headers (asm/...) that the POSIX
// headers above include; the names are POSIX's.
// NOLINTBEGIN(misc-include-cleaner): reached through <sys/socket.h>
constexpr int SOCKET_LEVEL = SOL_SOCKET;
constexpr int PENDING_ERROR = SO_ERROR;
constexpr int LINGER = SO_LINGER;
using AddressLength = socklen_t;
// NOLINTEND(misc-include-cleaner)

// A socket address of either family, and its length.
struct SocketAddress {
    sockaddr_in v4{};
    sockaddr_in6 v6{};
    AddressLength length = 0;
    int family = AF_UNSPEC;
};

const sockaddr* generic_of(const SocketAddress& address) noexcept {
    if (address.family == AF_INET) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,scada-banned): the socket interface takes every address type through a sockaddr pointer
        return reinterpret_cast<const sockaddr*>(&address.v4);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,scada-banned): the socket interface takes every address type through a sockaddr pointer
    return reinterpret_cast<const sockaddr*>(&address.v6);
}

bool parse(std::string_view host, std::uint16_t port,
           SocketAddress& out) noexcept {
    std::array<char, LITERAL_CAPACITY> text{};
    if (!terminated(host, text)) {
        return false;
    }
    if (::inet_pton(AF_INET, text.data(), &out.v4.sin_addr) == 1) {
        out.v4.sin_family = AF_INET;
        out.v4.sin_port = htons(port);
        out.length = sizeof(out.v4);
        out.family = AF_INET;
        return true;
    }
    if (::inet_pton(AF_INET6, text.data(), &out.v6.sin6_addr) == 1) {
        out.v6.sin6_family = AF_INET6;
        out.v6.sin6_port = htons(port);
        out.length = sizeof(out.v6);
        out.family = AF_INET6;
        return true;
    }
    return false;
}

bool set_option(int descriptor, int level, int name, int value) noexcept {
    return ::setsockopt(descriptor, level, name, &value, sizeof(value)) == 0;
}

// `TCP_USER_TIMEOUT` takes milliseconds in an unsigned int; rounded up.
unsigned int user_timeout_ms(std::int64_t duration_ns) noexcept {
    const std::int64_t at_least =
        std::max(duration_ns, MIN_TCP_USER_TIMEOUT_NS);
    const std::int64_t millis =
        (at_least / NS_PER_MS) + (at_least % NS_PER_MS != 0 ? 1 : 0);
    return static_cast<unsigned int>(
        std::min<std::int64_t>(millis, std::numeric_limits<int>::max()));
}

}  // namespace

bool is_ip_literal(std::string_view host) noexcept {
    SocketAddress unused;
    return parse(host, 0, unused);
}

std::int64_t tcp_user_timeout_ns(const CompiledChannel& channel) noexcept {
    std::int64_t largest = 0;
    for (const CompiledDevice& device : channel.devices) {
        largest = std::max({largest, device.failure.timeout_ns,
                            device.failure.write_timeout_ns});
    }
    const std::int64_t times = channel.connection.lost_after_timeouts;
    const std::int64_t product =
        largest > std::numeric_limits<std::int64_t>::max() /
                      std::max<std::int64_t>(times, 1)
            ? std::numeric_limits<std::int64_t>::max()
            : largest * times;
    return std::max(product, MIN_TCP_USER_TIMEOUT_NS);
}

TcpTransport::TcpTransport(TcpEndpoint endpoint, const Clock& clock)
    : endpoint_(std::move(endpoint)), clock_(&clock) {}

TcpTransport::~TcpTransport() {
    close(false);
}

IoStatus TcpTransport::fail_connect(ConnectError error, int number,
                                    IoStatus status) noexcept {
    fd_.reset();
    error_ = error;
    errno_ = number;
    return status;
}

IoStatus TcpTransport::connect(std::int64_t deadline_ns) noexcept {
    close(false);
    error_ = ConnectError::NONE;
    errno_ = 0;
    SocketAddress address;
    // Port 0 connects nowhere: refused before any system call (S5).
    if (endpoint_.port == 0 ||
        !parse(endpoint_.host, endpoint_.port, address)) {
        return fail_connect(ConnectError::ADDRESS, 0, IoStatus::FAILED);
    }
    fd_ = Fd(::socket(address.family,
                      SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP));
    if (!fd_.valid()) {
        return fail_connect(ConnectError::SYSTEM, errno, IoStatus::FAILED);
    }
    const int user_timeout =
        static_cast<int>(user_timeout_ms(endpoint_.user_timeout_ns));
    if (!set_option(fd_.get(), IPPROTO_TCP, TCP_NODELAY, 1) ||
        !set_option(fd_.get(), IPPROTO_TCP, TCP_USER_TIMEOUT, user_timeout)) {
        return fail_connect(ConnectError::SYSTEM, errno, IoStatus::FAILED);
    }
    const sockaddr* generic = generic_of(address);
    // An interrupted non-blocking connect goes on in the background, like
    // one in progress (EINTR; a second connect would give EALREADY, S4).
    const int result = ::connect(fd_.get(), generic, address.length);
    int error = 0;
    if (result != 0) {
        if (errno != EINPROGRESS && errno != EINTR) {
            error = errno;
        } else {
            short revents = 0;
            const detail::Readiness ready = detail::wait_for(
                fd_.get(), POLLOUT, deadline_ns, *clock_, revents, error);
            if (ready == detail::Readiness::TIMEOUT) {
                return fail_connect(ConnectError::TIMEOUT, 0,
                                    IoStatus::TIMEOUT);
            }
            if (ready == detail::Readiness::FAILED) {
                return fail_connect(ConnectError::SYSTEM, error,
                                    IoStatus::FAILED);
            }
            AddressLength length = sizeof(error);
            if (::getsockopt(fd_.get(), SOCKET_LEVEL, PENDING_ERROR, &error,
                             &length) != 0) {
                error = errno;
            }
        }
    }
    if (error == ECONNREFUSED) {
        return fail_connect(ConnectError::REFUSED, error, IoStatus::REFUSED);
    }
    if (error != 0) {
        return fail_connect(ConnectError::SYSTEM, error, IoStatus::FAILED);
    }
    // A connect to one of the host's own addresses on a free port of the
    // local range can meet itself with nobody listening; every frame would
    // come back as its own answer, a write "confirmed" with nothing done
    // (review M1; command safety).
    if (detail::connected_to_itself(fd_.get())) {
        return fail_connect(ConnectError::SELF, 0, IoStatus::FAILED);
    }
    return IoStatus::OK;
}

void TcpTransport::close(bool abortive) noexcept {
    if (!fd_.valid()) {
        return;
    }
    if (abortive) {
        // A reset: unsent and unacknowledged bytes are discarded (7.3).
        const linger reset{.l_onoff = 1, .l_linger = 0};
        (void)::setsockopt(fd_.get(), SOCKET_LEVEL, LINGER, &reset,
                           sizeof(reset));
    }
    fd_.reset();
}

bool TcpTransport::closed_by_peer() noexcept {
    return !fd_.valid() || detail::hung_up(fd_.get(), true);
}

IoStatus TcpTransport::send(std::span<const std::byte> frame) noexcept {
    if (!fd_.valid()) {
        return IoStatus::CLOSED;
    }
    return detail::write_once(fd_.get(), true, frame, errno_);
}

IoStatus TcpTransport::receive(std::span<std::byte> into,
                               std::int64_t deadline_ns,
                               std::size_t& received) noexcept {
    received = 0;
    if (!fd_.valid()) {
        return IoStatus::CLOSED;
    }
    return detail::receive_by(fd_.get(), true, into, deadline_ns, *clock_,
                              received, errno_);
}

bool TcpTransport::discard_input() noexcept {
    return fd_.valid() && detail::drop_readable(fd_.get(), true);
}

}  // namespace scada::driver
