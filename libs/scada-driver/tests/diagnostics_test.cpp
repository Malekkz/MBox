// The windows behind `response_ms` and `requests_per_s` (driver interface
// contract 11.1, D-145).

#include "scada/driver/diagnostics.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace scada::driver {
namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): the times and counts are the test data

constexpr std::int64_t MSEC = 1'000'000;
constexpr std::int64_t SEC = 1'000 * MSEC;

TEST(ResponseWindow, IsZeroBeforeTheFirstAnswer) {
    const ResponseWindow window;
    EXPECT_DOUBLE_EQ(window.average_ms(), 0.0);
}

TEST(ResponseWindow, AveragesTheLastHundredAnswers) {
    ResponseWindow window;
    window.add(2 * MSEC);
    window.add(4 * MSEC);
    EXPECT_DOUBLE_EQ(window.average_ms(), 3.0);
    // 100 answers of 10 ms push both out.
    for (std::size_t index = 0; index < ResponseWindow::RESPONSE_SAMPLES;
         ++index) {
        window.add(10 * MSEC);
    }
    EXPECT_DOUBLE_EQ(window.average_ms(), 10.0);
    window.add(110 * MSEC);  // replaces one of them: (99 * 10 + 110) / 100
    EXPECT_DOUBLE_EQ(window.average_ms(), 11.0);
}

TEST(ResponseWindow, ANegativeTimeCountsAsZero) {
    ResponseWindow window;
    window.add(-5 * MSEC);
    window.add(4 * MSEC);
    EXPECT_DOUBLE_EQ(window.average_ms(), 2.0);
}

TEST(RateWindow, CountsTheLastTenSeconds) {
    RateWindow window;
    for (std::int64_t index = 0; index < 20; ++index) {
        window.add((5 * SEC) + (index * 100 * MSEC));  // 5.0 s to 6.9 s
    }
    EXPECT_DOUBLE_EQ(window.per_second(7 * SEC), 2.0);
    // At 15.x s the bucket of 5 s has left the window; that of 6 s has not.
    EXPECT_DOUBLE_EQ(window.per_second((15 * SEC) + MSEC), 1.0);
    EXPECT_DOUBLE_EQ(window.per_second(16 * SEC), 0.0);
}

TEST(RateWindow, AGapLongerThanTheWindowEmptiesEveryBucket) {
    RateWindow window;
    window.add(SEC);
    window.add(2 * SEC);
    window.add(1000 * SEC);
    EXPECT_DOUBLE_EQ(window.per_second(1000 * SEC), 0.1);
}

TEST(RateWindow, AClockThatGoesBackKeepsTheCount) {
    RateWindow window;
    window.add(3 * SEC);
    window.add(2 * SEC);  // counted in the newest bucket
    window.add(-SEC);     // as 0: counted too
    EXPECT_DOUBLE_EQ(window.per_second(3 * SEC), 0.3);
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

}  // namespace
}  // namespace scada::driver
