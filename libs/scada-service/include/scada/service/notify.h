#ifndef SCADA_SERVICE_NOTIFY_H
#define SCADA_SERVICE_NOTIFY_H

// Client side of the systemd service notification protocol (sd_notify(3)):
// readiness, watchdog and stop messages sent as datagrams to the socket named
// by NOTIFY_SOCKET. Implemented directly on the documented protocol, so no
// systemd library is linked (docs/decisions.md, D-17).

#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace scada::service {

// Longest message send() accepts, in bytes.
inline constexpr std::size_t MAX_NOTIFY_MESSAGE_SIZE = 256;

// Longest accepted watchdog interval. systemd sets WATCHDOG_USEC from
// WatchdogSec; anything beyond one day is treated as invalid.
inline constexpr std::chrono::microseconds MAX_WATCHDOG_INTERVAL =
    std::chrono::hours(24);

// The raw values of the environment variables the service manager sets. An
// unset variable is nullopt. The values are untrusted until parsed.
struct NotifyEnvironment {
    std::optional<std::string> notify_socket;  // NOTIFY_SOCKET
    std::optional<std::string> watchdog_usec;  // WATCHDOG_USEC
    std::optional<std::string> watchdog_pid;   // WATCHDOG_PID
};

// Validated notification settings.
struct NotifySettings {
    // Socket to notify: an absolute path, or '@' followed by an
    // abstract-namespace name. Empty if there is no service manager.
    std::string socket_name;

    // Watchdog interval the service manager enforces for this process, or
    // nullopt if no watchdog applies. The service must send a watchdog message
    // more often than this; half the interval is the usual period.
    std::optional<std::chrono::microseconds> watchdog_interval;
};

// Reads the three variables from the process environment. Not thread-safe
// against concurrent changes of the environment; call it at start-up.
[[nodiscard]] NotifyEnvironment read_notify_environment();

// Validates `environment` for the process with id `own_pid`. Returns
// INVALID_ARGUMENT if any variable that is set has an invalid value: a socket
// name that is not an absolute path or '@name', is too long for a socket
// address or contains a NUL byte; an interval or process id that is not a
// positive decimal number; an interval above MAX_WATCHDOG_INTERVAL. The
// message names the variable. A watchdog addressed to another process
// (WATCHDOG_PID differs from `own_pid`) is valid and gives no watchdog
// interval.
[[nodiscard]] Result<NotifySettings> parse_notify_environment(
    const NotifyEnvironment& environment, std::int64_t own_pid);

// Sends state messages to the service manager.
//
// Every send returns OK if the message was handed to the service manager's
// socket, and also if there is no service manager, in which case nothing is
// sent; enabled() tells the two apart. It returns INVALID_ARGUMENT for a
// message that is empty, too long or contains a NUL byte, and IO_ERROR if the
// socket could not be opened or the send failed.
class Notifier {
public:
    // Opens the socket for `settings.socket_name`. With an empty name the
    // notifier is disabled. If the socket cannot be opened, every send returns
    // IO_ERROR. `settings` must come from parse_notify_environment().
    explicit Notifier(const NotifySettings& settings);

    // True if a service manager is to be notified.
    [[nodiscard]] bool enabled() const noexcept {
        return !socket_name_.empty();
    }

    // Reports that start-up is complete ("READY=1").
    Status ready() noexcept;

    // Feeds the watchdog ("WATCHDOG=1").
    Status watchdog() noexcept;

    // Reports that the service is shutting down ("STOPPING=1").
    Status stopping() noexcept;

    // Sets the free-text status line shown by the service manager
    // ("STATUS=<text>"). `text` must be a single line; a line break gives
    // INVALID_ARGUMENT.
    Status set_status_text(std::string_view text);

    // Sends one raw message, for example "READY=1". Does not block: if the
    // service manager's queue is full the result is IO_ERROR.
    Status send(std::string_view message) noexcept;

private:
    std::string socket_name_;
    Fd socket_;
};

}  // namespace scada::service

#endif  // SCADA_SERVICE_NOTIFY_H
