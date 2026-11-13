#include "scada/common/fd.h"

#include <utility>

#include <unistd.h>

namespace scada {

Fd::Fd(int descriptor) noexcept : fd_(descriptor >= 0 ? descriptor : -1) {}

Fd::~Fd() {
    reset();
}

Fd::Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) {
        reset();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

void Fd::reset() noexcept {
    if (fd_ >= 0) {
        // Not retried on EINTR: on Linux the descriptor is released even
        // then, and a retry could close a descriptor another thread has just
        // been given.
        static_cast<void>(::close(fd_));
        fd_ = -1;
    }
}

}  // namespace scada
