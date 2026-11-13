#include "scada/service/fd.h"

#include <unistd.h>

#include <utility>

namespace scada::service {

Fd::Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) {
        reset();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

Fd::~Fd() {
    reset();
}

void Fd::reset() noexcept {
    if (fd_ >= 0) {
        // The result is ignored on purpose: the descriptor is gone whatever
        // close() returns, and a destructor has nobody to report to.
        static_cast<void>(::close(fd_));
        fd_ = -1;
    }
}

}  // namespace scada::service
