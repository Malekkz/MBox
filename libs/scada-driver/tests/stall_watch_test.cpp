// The channel watchdog's arithmetic (driver interface contract 2.6,
// decision D-142, the delta check of PR 150) on a fake clock: when a stuck
// worker is found, and exactly how much of a late check is credited.

#include "../src/stall_watch.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>

namespace scada::driver::detail {
namespace {

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers): the times are the test data

constexpr std::int64_t MSEC = 1'000'000;
constexpr std::int64_t PERIOD = 100 * MSEC;
constexpr std::int64_t LIMIT = 1000 * MSEC;

// One worker that never moves, with LIMIT.
std::array<Watched, 1> stuck() {
    return {Watched{.progress = 0, .limit_ns = LIMIT, .returned = false}};
}

// Checks at every period from `from_ns` (exclusive) to `to_ns`
// (inclusive); the time of the first that finds the stuck worker, if any.
std::optional<std::int64_t> first_stall(StallWatch& watch, std::int64_t from_ns,
                                        std::int64_t to_ns) {
    const std::array<Watched, 1> workers = stuck();
    for (std::int64_t now = from_ns + PERIOD; now <= to_ns; now += PERIOD) {
        if (watch.check(now, workers).has_value()) {
            return now;
        }
    }
    return std::nullopt;
}

TEST(StallWatch, AStuckWorkerIsFoundAtItsLimit) {
    StallWatch watch(PERIOD);
    watch.start(1, 0);
    EXPECT_EQ(first_stall(watch, 0, 3000 * MSEC), 1000 * MSEC);
}

TEST(StallWatch, APauseIsCreditedWithTheTimeLostLessOnePeriod) {
    // Checks at 100 to 300 ms, then none until 800 ms: a gap of 500 ms,
    // 400 ms of it lost. The stuck worker is found at 1,400 ms, not at
    // 1,000 (no credit), 1,500 (the whole gap) or 1,800 (credited twice).
    StallWatch watch(PERIOD);
    watch.start(1, 0);
    EXPECT_EQ(first_stall(watch, 0, 300 * MSEC), std::nullopt);
    EXPECT_FALSE(watch.check(800 * MSEC, stuck()).has_value());
    EXPECT_EQ(watch.last_pause_ns(), 500 * MSEC);
    EXPECT_EQ(first_stall(watch, 800 * MSEC, 3000 * MSEC), 1400 * MSEC);
}

TEST(StallWatch, AGapOfTwoPeriodsIsNoPause) {
    // A gap of exactly two periods (200 ms) is not a pause: nothing is
    // credited, and the worker is found at its limit.
    StallWatch watch(PERIOD);
    watch.start(1, 0);
    EXPECT_FALSE(watch.check(100 * MSEC, stuck()).has_value());
    EXPECT_FALSE(watch.check(300 * MSEC, stuck()).has_value());
    EXPECT_EQ(watch.last_pause_ns(), 0);
    EXPECT_EQ(first_stall(watch, 300 * MSEC, 3000 * MSEC), 1000 * MSEC);
}

TEST(StallWatch, APauseBeforeTheFirstCheckIsCredited) {
    // The delta's start-time hole: the watch starts with the workers (at
    // 10 s here, not at 0, so that a watch clock left at 0 is seen), so a
    // first check 1,500 ms later is a pause, credited 1,400 ms: neither a
    // stall at once nor a credit of the whole time since 0.
    StallWatch watch(PERIOD);
    watch.start(1, 10'000 * MSEC);
    EXPECT_FALSE(watch.check(11'500 * MSEC, stuck()).has_value());
    EXPECT_EQ(watch.last_pause_ns(), 1500 * MSEC);
    EXPECT_EQ(first_stall(watch, 11'500 * MSEC, 15'000 * MSEC), 12'400 * MSEC);
}

TEST(StallWatch, ShortPausesNeverHideAStuckWorker) {
    // Checks at 100, 200 and 500 ms of every 500 ms: each 300 ms gap is
    // credited 200 ms, so the worker's time grows by 300 ms every 500 ms
    // and its limit is reached at 1,600 ms (credit kept: 600 ms).
    StallWatch watch(PERIOD);
    watch.start(1, 0);
    std::optional<std::int64_t> found;
    for (std::int64_t base = 0; !found.has_value() && base < 10'000 * MSEC;
         base += 500 * MSEC) {
        for (const std::int64_t offset : {100 * MSEC, 200 * MSEC, 500 * MSEC}) {
            if (watch.check(base + offset, stuck()).has_value()) {
                found = base + offset;
                break;
            }
        }
    }
    EXPECT_EQ(found, 1600 * MSEC);
}

TEST(StallWatch, AMovingWorkerAndOneThatReturnedAreNeverStalled) {
    StallWatch watch(PERIOD);
    watch.start(2, 0);
    std::array<Watched, 2> workers{
        Watched{.progress = 0, .limit_ns = LIMIT, .returned = false},
        Watched{.progress = 0, .limit_ns = LIMIT, .returned = true}};
    for (std::int64_t now = PERIOD; now <= 5000 * MSEC; now += PERIOD) {
        ++workers[0].progress;
        EXPECT_FALSE(watch.check(now, workers).has_value()) << now;
    }
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

}  // namespace
}  // namespace scada::driver::detail
