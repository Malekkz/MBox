#ifndef SCADA_SERVICE_SRC_UNIX_ADDRESS_H
#define SCADA_SERVICE_SRC_UNIX_ADDRESS_H

#include <sys/socket.h>
#include <sys/un.h>

#include <optional>
#include <string_view>

namespace scada::service {

// A filled-in AF_UNIX socket address and its length.
struct UnixAddress {
    sockaddr_un address{};
    socklen_t length = 0;
};

// Builds the address for `name`: an absolute path, or '@' followed by an
// abstract-namespace name. Returns nullopt if `name` has neither form, is
// longer than a socket address allows, or contains a NUL byte.
[[nodiscard]] std::optional<UnixAddress> make_unix_address(
    std::string_view name) noexcept;

// Binds `fd` to `address`. Returns true on success.
[[nodiscard]] bool bind_unix(int fd, const UnixAddress& address) noexcept;

}  // namespace scada::service

#endif  // SCADA_SERVICE_SRC_UNIX_ADDRESS_H
