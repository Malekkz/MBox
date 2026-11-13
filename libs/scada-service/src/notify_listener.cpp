#include "scada/service/notify_listener.h"

#include "unix_address.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <limits>
#include <utility>

namespace scada::service {

NotifyListener::NotifyListener(std::string socket_name, Fd socket) noexcept
    : socket_name_(std::move(socket_name)), socket_(std::move(socket)) {}

std::optional<NotifyListener> NotifyListener::create() {
    Fd socket(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    if (!socket.valid()) {
        return std::nullopt;
    }
    // Unique among live listeners: the process id separates processes, the
    // descriptor number separates listeners within one.
    std::string name = "@scada-notify-" + std::to_string(::getpid()) + "-" +
                       std::to_string(socket.get());
    const std::optional<UnixAddress> address = make_unix_address(name);
    if (!address || !bind_unix(socket.get(), *address)) {
        return std::nullopt;
    }
    return NotifyListener(std::move(name), std::move(socket));
}

std::optional<std::string> NotifyListener::receive(
    std::chrono::milliseconds timeout) {
    pollfd waiting{};
    waiting.fd = socket_.get();
    waiting.events = POLLIN;

    const auto max_timeout = std::numeric_limits<int>::max();
    const int timeout_ms =
        timeout.count() < 0
            ? 0
            : timeout.count() > max_timeout
                  ? max_timeout
                  : static_cast<int>(timeout.count());
    if (::poll(&waiting, 1, timeout_ms) != 1) {
        return std::nullopt;
    }

    std::array<char, MAX_RECEIVED_NOTIFY_SIZE> buffer{};
    const ssize_t received =
        ::recv(socket_.get(), buffer.data(), buffer.size(), MSG_DONTWAIT);
    if (received < 0) {
        return std::nullopt;
    }
    // recv() returns at most buffer.size() bytes.
    return std::string(buffer.data(), static_cast<std::size_t>(received));
}

}  // namespace scada::service
