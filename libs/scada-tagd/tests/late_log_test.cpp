// Unit tests for the rate limit of the `heartbeat late` record (issue #61).

#include "late_log.h"

#include "scada/tag/generation_host.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using scada::tag::LateHeartbeats;
using scada::tagd::LATE_LOG_INTERVAL_NS;
using scada::tagd::LateLog;
using scada::tagd::LateRecord;

constexpr std::int64_t INTERVAL_NS = 1'000;
constexpr std::int64_t START_NS = 5'000;
// Figures derived from the total, so that each record shows whose it is.
constexpr std::int64_t GAP_PER_BEAT_NS = 10;
constexpr std::int64_t TIME_PER_BEAT_NS = 100;

LateHeartbeats late(const std::string& process, std::uint32_t added,
                    std::uint32_t total, bool baseline = false) {
    return LateHeartbeats{
        .process = process,
        .added = added,
        .total = total,
        .longest_gap_ns = total * GAP_PER_BEAT_NS,
        .last_late_mono_ns = total * TIME_PER_BEAT_NS,
        .baseline = baseline,
    };
}

TEST(LateLog, TheIntervalIsTenSeconds) {
    EXPECT_EQ(LATE_LOG_INTERVAL_NS, 10'000'000'000);
}

TEST(LateLog, TheFirstGrowthIsLoggedAtOnce) {
    LateLog log(INTERVAL_NS);
    const std::vector<LateRecord> out =
        log.admit(std::vector{late("io", 2, 2)}, START_NS);
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].process, "io");
    EXPECT_EQ(out[0].added, 2U);
    EXPECT_EQ(out[0].total, 2U);
    EXPECT_EQ(out[0].longest_gap_ns, 20);
    EXPECT_EQ(out[0].last_late_mono_ns, 200);
    EXPECT_EQ(out[0].checks, 1U);
    EXPECT_FALSE(out[0].baseline);
}

// Issue #61: under persistent overload a record per process and check could
// reach journald's rate limit. Growth within the interval is held and
// summed; no count is lost.
TEST(LateLog, GrowthWithinTheIntervalIsHeldAndSummed) {
    LateLog log(INTERVAL_NS);
    ASSERT_EQ(log.admit(std::vector{late("io", 1, 1)}, START_NS).size(), 1U);
    EXPECT_TRUE(log.admit(std::vector{late("io", 2, 3)}, START_NS + 1).empty());
    EXPECT_TRUE(log.admit({}, START_NS + 2).empty());
    EXPECT_TRUE(
        log.admit(std::vector{late("io", 4, 7)}, START_NS + INTERVAL_NS - 1)
            .empty());

    // The interval has ended: one record with the sum and the latest
    // figures, also at a check that found nothing new.
    const std::vector<LateRecord> out = log.admit({}, START_NS + INTERVAL_NS);
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].added, 6U);
    EXPECT_EQ(out[0].total, 7U);
    EXPECT_EQ(out[0].longest_gap_ns, 70);
    EXPECT_EQ(out[0].last_late_mono_ns, 700);
    EXPECT_EQ(out[0].checks, 2U);

    // A new interval starts with that record.
    EXPECT_TRUE(
        log.admit(std::vector{late("io", 1, 8)}, START_NS + INTERVAL_NS + 1)
            .empty());
    EXPECT_EQ(log.admit({}, START_NS + (2 * INTERVAL_NS)).size(), 1U);
}

TEST(LateLog, GrowthAfterAQuietIntervalIsLoggedAtOnce) {
    LateLog log(INTERVAL_NS);
    ASSERT_EQ(log.admit(std::vector{late("io", 1, 1)}, START_NS).size(), 1U);
    const std::vector<LateRecord> out =
        log.admit(std::vector{late("io", 1, 2)}, START_NS + (3 * INTERVAL_NS));
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].added, 1U);
}

TEST(LateLog, EachProcessHasItsOwnInterval) {
    LateLog log(INTERVAL_NS);
    ASSERT_EQ(log.admit(std::vector{late("io", 1, 1)}, START_NS).size(), 1U);
    const std::vector<LateRecord> out = log.admit(
        std::vector{late("io", 1, 2), late("control", 1, 1)}, START_NS + 1);
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].process, "control");
}

// A baseline is the first look at a region, at most once per life of the
// process, so it is not held. Growth held before it belongs to the region
// looked at before, and comes first.
TEST(LateLog, ABaselineIsLoggedAtOnceAfterWhatWasHeld) {
    LateLog log(INTERVAL_NS);
    ASSERT_EQ(log.admit(std::vector{late("io", 1, 1)}, START_NS).size(), 1U);
    EXPECT_TRUE(log.admit(std::vector{late("io", 2, 3)}, START_NS + 1).empty());
    const std::vector<LateRecord> out =
        log.admit(std::vector{late("io", 5, 5, true)}, START_NS + 2);
    ASSERT_EQ(out.size(), 2U);
    EXPECT_FALSE(out[0].baseline);
    EXPECT_EQ(out[0].added, 2U);
    EXPECT_TRUE(out[1].baseline);
    EXPECT_EQ(out[1].added, 5U);
    // The baseline did not end the interval of the earlier record.
    EXPECT_TRUE(log.admit(std::vector{late("io", 1, 6)}, START_NS + 3).empty());
}

TEST(LateLog, TheSumDoesNotWrapAt32Bits) {
    constexpr std::uint32_t MAX = std::numeric_limits<std::uint32_t>::max();
    LateLog log(INTERVAL_NS);
    ASSERT_EQ(log.admit(std::vector{late("io", 1, 1)}, START_NS).size(), 1U);
    EXPECT_TRUE(
        log.admit(std::vector{late("io", MAX, 0)}, START_NS + 1).empty());
    EXPECT_TRUE(
        log.admit(std::vector{late("io", MAX, 0)}, START_NS + 2).empty());
    const std::vector<LateRecord> out = log.admit({}, START_NS + INTERVAL_NS);
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].added, 2ULL * MAX);
}

TEST(LateLog, DrainReturnsWhatIsHeld) {
    LateLog log(INTERVAL_NS);
    ASSERT_EQ(log.admit(std::vector{late("io", 1, 1)}, START_NS).size(), 1U);
    EXPECT_TRUE(log.admit(std::vector{late("io", 3, 4)}, START_NS + 1).empty());
    const std::vector<LateRecord> out = log.drain();
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].added, 3U);
    EXPECT_TRUE(log.drain().empty());
}

TEST(LateLog, AnIntervalAtTheEndOfTheClockDoesNotOverflow) {
    constexpr std::int64_t MAX = std::numeric_limits<std::int64_t>::max();
    LateLog log(MAX);
    ASSERT_EQ(log.admit(std::vector{late("io", 1, 1)}, START_NS).size(), 1U);
    EXPECT_TRUE(log.admit(std::vector{late("io", 1, 2)}, MAX - 1).empty());
    EXPECT_EQ(log.admit({}, MAX).size(), 1U);
}

TEST(LateLog, AZeroIntervalLogsEveryCheck) {
    LateLog log(0);
    EXPECT_EQ(log.admit(std::vector{late("io", 1, 1)}, START_NS).size(), 1U);
    EXPECT_EQ(log.admit(std::vector{late("io", 1, 2)}, START_NS).size(), 1U);
}

}  // namespace
