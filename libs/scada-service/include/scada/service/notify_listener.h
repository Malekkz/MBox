#ifndef SCADA_SERVICE_NOTIFY_LISTENER_H
#define SCADA_SERVICE_NOTIFY_LISTENER_H

// The service manager's side of the notification protocol, for tests: it
// stands in for systemd so that a service can be started and observed without
// one. Not used by any service.

#include "scada/common/fd.h"
#include "scada/common/result.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace scada::service {

// Largest datagram receive() returns; longer ones are truncated to this.
inline constexpr std::size_t MAX_RECEIVED_NOTIFY_SIZE = 4096;

// Receives notification datagrams on a socket of its own.
class NotifyListener {
public:
    // Binds a datagram socket to a unique abstract-namespace name, so nothing
    // is created in the filesystem. Returns IO_ERROR if that fails.
    [[nodiscard]] static Result<NotifyListener> create();

    // The value to pass to the service as NOTIFY_SOCKET.
    [[nodiscard]] const std::string& socket_name() const noexcept {
        return socket_name_;
    }

    // Waits up to `timeout` for one message and returns it. Returns nullopt on
    // timeout or on a receive error.
    [[nodiscard]] std::optional<std::string> receive(
        std::chrono::milliseconds timeout);

private:
    NotifyListener(std::string socket_name, Fd socket) noexcept;

    std::string socket_name_;
    Fd socket_;
};

}  // namespace scada::service

#endif  // SCADA_SERVICE_NOTIFY_LISTENER_H
