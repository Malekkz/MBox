#include "unix_address.h"

#include <algorithm>
#include <cstddef>
#include <span>

namespace scada::service {

std::optional<UnixAddress> make_unix_address(std::string_view name) noexcept {
    UnixAddress result;
    const std::span<char> path(result.address.sun_path);

    // A path needs room for its terminating NUL. An abstract name has none,
    // but its leading NUL takes the place of the '@', so the limit is the same.
    if (name.size() < 2 || name.size() >= path.size()) {
        return std::nullopt;
    }
    const bool abstract = name.front() == '@';
    if (!abstract && name.front() != '/') {
        return std::nullopt;
    }
    if (name.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }

    result.address.sun_family = AF_UNIX;
    // `path` is zero-initialised and longer than `name`, so the copy fits and
    // a path stays terminated.
    std::copy(name.begin(), name.end(), path.begin());
    std::size_t length = offsetof(sockaddr_un, sun_path) + name.size();
    if (abstract) {
        path.front() = '\0';
    } else {
        ++length;
    }
    result.length = static_cast<socklen_t>(length);
    return result;
}

bool bind_unix(int fd, const UnixAddress& address) noexcept {
    // bind() takes the generic `sockaddr*`, which the sockets API defines as
    // the way to pass any address type. The conversion goes through `void*`
    // and happens only here (docs/decisions.md, D-7).
    const void* const generic = &address.address;
    return ::bind(fd, static_cast<const sockaddr*>(generic), address.length) ==
           0;
}

}  // namespace scada::service
