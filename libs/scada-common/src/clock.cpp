#include "scada/common/clock.h"

#include "scada/common/panic.h"

#include <cstdint>

#include <ctime>

namespace scada {

namespace {

// The POSIX clock names below come from <ctime> on every supported system,
// but the include check knows only the C standard names of that header;
// hence the three suppressions.
std::int64_t read_clock_ns(auto clock) noexcept {
    timespec now{};
    // NOLINTNEXTLINE(misc-include-cleaner): POSIX name, provided by <ctime>
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
    // NOLINTNEXTLINE(misc-include-cleaner): POSIX name, provided by <ctime>
    return read_clock_ns(CLOCK_REALTIME);
}

std::int64_t monotonic_ns() noexcept {
    // NOLINTNEXTLINE(misc-include-cleaner): POSIX name, provided by <ctime>
    return read_clock_ns(CLOCK_MONOTONIC);
}

}  // namespace scada
