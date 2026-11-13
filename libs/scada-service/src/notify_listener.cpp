#include "scada/service/notify_listener.h"

#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "unix_address.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace scada::service {

NotifyListener::NotifyListener(std::string socket_name, Fd socket) noexcept
    : socket_name_(std::move(socket_name)), socket_(std::move(socket)) {}

Result<NotifyListener> NotifyListener::create() {
    Fd socket(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    if (!socket.valid()) {
        return Status(StatusCode::IO_ERROR, "cannot create a socket");
    }
    // Unique among live listeners: the process id separates processes, the
    // descriptor number separates listeners within one.
    std::string name = "@scada-notify-" + std::to_string(::getpid()) + "-" +
                       std::to_string(socket.get());
    const std::optional<UnixAddress> address = make_unix_address(name);
    if (!address || !bind_unix(socket.get(), *address)) {
        return Status(StatusCode::IO_ERROR, "cannot bind the listener socket");
    }
    return NotifyListener(std::move(name), std::move(socket));
}

std::optional<std::string> NotifyListener::receive(
    std::chrono::milliseconds timeout) {
    pollfd waiting{};
    waiting.fd = socket_.get();
    waiting.events = POLLIN;

    // poll() takes the timeout as an int; the clamp makes the cast safe.
    using Count = std::chrono::milliseconds::rep;
    const auto timeout_ms = static_cast<int>(
        std::clamp<Count>(timeout.count(), 0, std::numeric_limits<int>::max()));
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
