#include "scada/common/clock.h"

#include "scada/common/panic.h"

#include <ctime>

namespace scada {

namespace {

std::int64_t read_clock_ns(clockid_t clock) noexcept {
    timespec now{};
    if (::clock_gettime(clock, &now) != 0) {
        // Both clocks are mandatory on Linux, so this means a broken system
        // or a sandbox that forbids the call.
        panic("clock_gettime failed");
    }
    // A signed 64-bit count of nanoseconds lasts until the year 2262.
    return static_cast<std::int64_t>(now.tv_sec) * NANOSECONDS_PER_SECOND +
           static_cast<std::int64_t>(now.tv_nsec);
}

}  // namespace

std::int64_t realtime_ns() noexcept {
    return read_clock_ns(CLOCK_REALTIME);
}

std::int64_t monotonic_ns() noexcept {
    return read_clock_ns(CLOCK_MONOTONIC);
}

}  // namespace scada
