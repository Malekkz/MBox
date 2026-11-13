// Unit tests for the clock helpers.

#include "scada/common/clock.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

namespace {

template <typename Clock>
std::int64_t chrono_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

TEST(Clock, RealtimeAgreesWithTheSystemClock) {
    // The standard library's system_clock is CLOCK_REALTIME; a reading of
    // ours taken between two of its readings must lie between them.
    const std::int64_t before = chrono_ns<std::chrono::system_clock>();
    const std::int64_t now = scada::realtime_ns();
    const std::int64_t after = chrono_ns<std::chrono::system_clock>();
    EXPECT_LE(before, now);
    EXPECT_LE(now, after);
}

TEST(Clock, RealtimeIsInNanoseconds) {
    // 2020-01-01T00:00:00Z, to catch a wrong unit.
    constexpr std::int64_t YEAR_2020_NS =
        1'577'836'800 * scada::NANOSECONDS_PER_SECOND;
    EXPECT_GT(scada::realtime_ns(), YEAR_2020_NS);
}

TEST(Clock, MonotonicAgreesWithTheSteadyClock) {
    // Likewise steady_clock is CLOCK_MONOTONIC.
    const std::int64_t before = chrono_ns<std::chrono::steady_clock>();
    const std::int64_t now = scada::monotonic_ns();
    const std::int64_t after = chrono_ns<std::chrono::steady_clock>();
    EXPECT_LE(before, now);
    EXPECT_LE(now, after);
}

TEST(Clock, MonotonicNeverGoesBackwards) {
    constexpr int READINGS = 100'000;
    bool ordered = true;
    std::int64_t previous = scada::monotonic_ns();
    for (int i = 0; i < READINGS; ++i) {
        const std::int64_t now = scada::monotonic_ns();
        ordered = ordered && now >= previous;
        previous = now;
    }
    EXPECT_TRUE(ordered);
}

}  // namespace
