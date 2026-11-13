#include "scada/service/notify.h"

#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "unix_address.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace scada::service {

namespace {

std::optional<std::string> environment_value(const char* name) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): start-up only, see the header
    const char* const value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

// Parses a decimal number in [1, max]. The whole text must be digits.
std::optional<std::int64_t> parse_positive(std::string_view text,
                                           std::int64_t max) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    constexpr std::int64_t BASE = 10;
    std::int64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        const std::int64_t digit = character - '0';
        // Checked before the multiplication, so it cannot overflow.
        if (value > (max - digit) / BASE) {
            return std::nullopt;
        }
        value = value * BASE + digit;
    }
    if (value < 1) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

NotifyEnvironment read_notify_environment() {
    return {environment_value("NOTIFY_SOCKET"),
            environment_value("WATCHDOG_USEC"),
            environment_value("WATCHDOG_PID")};
}

Result<NotifySettings> parse_notify_environment(
    const NotifyEnvironment& environment, std::int64_t own_pid) {
    NotifySettings settings;

    // An empty NOTIFY_SOCKET means the same as an unset one.
    if (environment.notify_socket && !environment.notify_socket->empty()) {
        if (!make_unix_address(*environment.notify_socket)) {
            return Status(StatusCode::INVALID_ARGUMENT,
                          "NOTIFY_SOCKET is not a valid socket name");
        }
        settings.socket_name = *environment.notify_socket;
    }

    bool watchdog_is_for_us = true;
    if (environment.watchdog_pid) {
        const auto pid =
            parse_positive(*environment.watchdog_pid,
                           std::numeric_limits<std::int64_t>::max());
        if (!pid) {
            return Status(StatusCode::INVALID_ARGUMENT,
                          "WATCHDOG_PID is not a process id");
        }
        watchdog_is_for_us = *pid == own_pid;
    }
    if (environment.watchdog_usec) {
        const auto usec = parse_positive(*environment.watchdog_usec,
                                         MAX_WATCHDOG_INTERVAL.count());
        if (!usec) {
            return Status(StatusCode::INVALID_ARGUMENT,
                          "WATCHDOG_USEC is not a valid interval");
        }
        if (watchdog_is_for_us) {
            settings.watchdog_interval = std::chrono::microseconds(*usec);
        }
    }
    return settings;
}

Notifier::Notifier(const NotifySettings& settings)
    : socket_name_(settings.socket_name) {
    if (enabled()) {
        socket_ = Fd(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    }
}

Status Notifier::ready() noexcept {
    return send("READY=1");
}

Status Notifier::watchdog() noexcept {
    return send("WATCHDOG=1");
}

Status Notifier::stopping() noexcept {
    return send("STOPPING=1");
}

Status Notifier::set_status_text(std::string_view text) {
    if (text.find('\n') != std::string_view::npos) {
        return {StatusCode::INVALID_ARGUMENT, "status text has a line break"};
    }
    std::string message = "STATUS=";
    message += text;
    return send(message);
}

Status Notifier::send(std::string_view message) noexcept {
    if (!enabled()) {
        return {};
    }
    if (message.empty() || message.size() > MAX_NOTIFY_MESSAGE_SIZE ||
        message.find('\0') != std::string_view::npos) {
        return {StatusCode::INVALID_ARGUMENT,
                "notification message is empty, too long or contains NUL"};
    }
    std::optional<UnixAddress> address = make_unix_address(socket_name_);
    if (!socket_.valid() || !address) {
        return {StatusCode::IO_ERROR, "notification socket is not open"};
    }

    // sendmsg() takes a writable buffer, so the message is copied; the size
    // was checked against the buffer above.
    std::array<char, MAX_NOTIFY_MESSAGE_SIZE> buffer{};
    std::copy(message.begin(), message.end(), buffer.begin());
    iovec part{};
    part.iov_base = buffer.data();
    part.iov_len = message.size();

    msghdr header{};
    header.msg_name = &address->address;
    header.msg_namelen = address->length;
    header.msg_iov = &part;
    header.msg_iovlen = 1;

    // MSG_DONTWAIT: a service must not hang on a service manager that does
    // not read. MSG_NOSIGNAL: a closed peer is an error value, not a signal.
    const ssize_t sent =
        ::sendmsg(socket_.get(), &header, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent < 0 || static_cast<std::size_t>(sent) != message.size()) {
        return {StatusCode::IO_ERROR, "sending to the service manager failed"};
    }
    return {};
}

}  // namespace scada::service
