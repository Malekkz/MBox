#include "scada/common/clock.h"
#include "scada/driver/connections.h"

#include <cstdint>

namespace scada::driver {

std::int64_t SystemClock::monotonic_ns() const noexcept {
    return scada::monotonic_ns();
}

std::int64_t SystemClock::realtime_ns() const noexcept {
    return scada::realtime_ns();
}

}  // namespace scada::driver
