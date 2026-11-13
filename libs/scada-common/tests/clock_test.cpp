#include "check.h"
#include "scada/common/clock.h"

#include <chrono>
#include <cstdint>

namespace {

template <typename Clock>
std::int64_t chrono_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

}  // namespace

int main() {
    scada::test::Checker checker;

    {
        // The standard library's system_clock is CLOCK_REALTIME; a reading
        // of ours taken between two of its readings must lie between them.
        const std::int64_t before = chrono_ns<std::chrono::system_clock>();
        const std::int64_t now = scada::realtime_ns();
        const std::int64_t after = chrono_ns<std::chrono::system_clock>();
        checker.expect(before <= now && now <= after,
                       "realtime_ns agrees with the system clock");

        // 2020-01-01T00:00:00Z, to catch a wrong unit.
        constexpr std::int64_t YEAR_2020_NS =
            1'577'836'800 * scada::NANOSECONDS_PER_SECOND;
        checker.expect(now > YEAR_2020_NS, "realtime_ns is in nanoseconds");
    }
    {
        // Likewise steady_clock is CLOCK_MONOTONIC.
        const std::int64_t before = chrono_ns<std::chrono::steady_clock>();
        const std::int64_t now = scada::monotonic_ns();
        const std::int64_t after = chrono_ns<std::chrono::steady_clock>();
        checker.expect(before <= now && now <= after,
                       "monotonic_ns agrees with the steady clock");
    }
    {
        bool ordered = true;
        std::int64_t previous = scada::monotonic_ns();
        for (int i = 0; i < 100'000; ++i) {
            const std::int64_t now = scada::monotonic_ns();
            ordered = ordered && now >= previous;
            previous = now;
        }
        checker.expect(ordered, "monotonic_ns never goes backwards");
    }

    return checker.exit_code();
}
