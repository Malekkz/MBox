// The rate limit of log records (driver interface contract 11.3; D-148).

#include "../src/log_limit.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace scada::driver::detail {
namespace {

constexpr std::int64_t PERIOD = 10;
constexpr std::size_t KINDS = 3;
constexpr std::size_t KEYS = 4;

std::vector<LogSummary> held(LogLimit& limit) {
    std::vector<LogSummary> out;
    LogSummary summary;
    while (limit.next_held(summary)) {
        out.push_back(summary);
    }
    return out;
}

std::vector<LogSummary> due(LogLimit& limit, std::int64_t now) {
    std::vector<LogSummary> out;
    LogSummary summary;
    while (limit.next_due(now, summary)) {
        out.push_back(summary);
    }
    return out;
}

TEST(LogLimit, TheFirstGoesAtOnceAndTheRestAreSummedAfterThePeriod) {
    LogLimit limit(KINDS, KEYS, PERIOD);
    EXPECT_TRUE(limit.admit(1, 2, 100));
    EXPECT_FALSE(limit.admit(1, 2, 101));
    EXPECT_FALSE(limit.admit(1, 2, 109));
    EXPECT_TRUE(due(limit, 109).empty());
    EXPECT_EQ(due(limit, 110),
              (std::vector<LogSummary>{{.kind = 1, .key = 2, .count = 2}}));
    EXPECT_TRUE(due(limit, 111).empty());
}

TEST(LogLimit, ASummaryStartsTheNextPeriod) {
    LogLimit limit(KINDS, KEYS, PERIOD);
    EXPECT_TRUE(limit.admit(0, 0, 0));
    EXPECT_FALSE(limit.admit(0, 0, 5));
    EXPECT_EQ(due(limit, 12).size(), 1U);  // the summary at 12
    // Within the period the summary started: held.
    EXPECT_FALSE(limit.admit(0, 0, 21));
    EXPECT_TRUE(due(limit, 21).empty());
    EXPECT_EQ(due(limit, 22),
              (std::vector<LogSummary>{{.kind = 0, .key = 0, .count = 1}}));
}

TEST(LogLimit, AQuietPeriodLetsTheNextRecordGoAtOnce) {
    LogLimit limit(KINDS, KEYS, PERIOD);
    EXPECT_TRUE(limit.admit(0, 1, 0));
    EXPECT_TRUE(due(limit, 10).empty());
    EXPECT_TRUE(limit.admit(0, 1, 10));
    EXPECT_FALSE(limit.admit(0, 1, 19));
}

TEST(LogLimit, AHeldCountIsTakenBeforeALateRecordGoes) {
    // A record after the period, before the summary was taken: it joins the
    // summary, so that the summary comes first and nothing is out of order.
    LogLimit limit(KINDS, KEYS, PERIOD);
    EXPECT_TRUE(limit.admit(2, 3, 0));
    EXPECT_FALSE(limit.admit(2, 3, 1));
    EXPECT_FALSE(limit.admit(2, 3, 30));
    EXPECT_EQ(due(limit, 30),
              (std::vector<LogSummary>{{.kind = 2, .key = 3, .count = 2}}));
}

TEST(LogLimit, KindsAndDevicesAreLimitedApart) {
    LogLimit limit(KINDS, KEYS, PERIOD);
    EXPECT_TRUE(limit.admit(0, 0, 0));
    EXPECT_TRUE(limit.admit(1, 0, 0));
    EXPECT_TRUE(limit.admit(0, 1, 0));
    // The entry of no key.
    EXPECT_TRUE(limit.admit(0, KEYS, 0));
    EXPECT_FALSE(limit.admit(0, KEYS, 1));
    EXPECT_FALSE(limit.admit(1, 0, 1));
    EXPECT_EQ(due(limit, 10),
              (std::vector<LogSummary>{{.kind = 0, .key = KEYS, .count = 1},
                                       {.kind = 1, .key = 0, .count = 1}}));
}

TEST(LogLimit, EachSummaryIsDueAtItsOwnPeriodsEnd) {
    LogLimit limit(KINDS, KEYS, PERIOD);
    EXPECT_TRUE(limit.admit(0, 0, 0));
    EXPECT_FALSE(limit.admit(0, 0, 1));
    EXPECT_TRUE(limit.admit(1, 1, 5));
    EXPECT_FALSE(limit.admit(1, 1, 6));
    EXPECT_EQ(due(limit, 10).size(), 1U);
    EXPECT_TRUE(due(limit, 14).empty());
    EXPECT_EQ(due(limit, 15),
              (std::vector<LogSummary>{{.kind = 1, .key = 1, .count = 1}}));
}

TEST(LogLimit, NextHeldTakesEveryCountAtTheStop) {
    LogLimit limit(KINDS, KEYS, PERIOD);
    EXPECT_TRUE(limit.admit(0, 0, 0));
    EXPECT_FALSE(limit.admit(0, 0, 1));
    EXPECT_TRUE(limit.admit(2, 4, 0));
    EXPECT_FALSE(limit.admit(2, 4, 2));
    EXPECT_FALSE(limit.admit(2, 4, 3));
    EXPECT_EQ(held(limit),
              (std::vector<LogSummary>{{.kind = 0, .key = 0, .count = 1},
                                       {.kind = 2, .key = 4, .count = 2}}));
    EXPECT_TRUE(due(limit, 100).empty());
}

TEST(LogLimit, AnIndexOutOfRangeIsAlwaysLogged) {
    LogLimit limit(KINDS, KEYS, PERIOD);
    EXPECT_TRUE(limit.admit(KINDS, 0, 0));
    EXPECT_TRUE(limit.admit(KINDS, 0, 0));
    EXPECT_TRUE(limit.admit(0, KEYS + 1, 0));
    EXPECT_TRUE(limit.admit(0, KEYS + 1, 0));
}

TEST(LogLimit, APeriodNearTheEndOfTimeDoesNotOverflow) {
    constexpr std::int64_t LATE = std::numeric_limits<std::int64_t>::max() - 1;
    LogLimit limit(KINDS, KEYS, PERIOD);
    EXPECT_TRUE(limit.admit(0, 0, LATE));
    EXPECT_FALSE(limit.admit(0, 0, LATE));
    EXPECT_TRUE(due(limit, LATE).empty());
}

TEST(LogLimit, TheDefaultPeriodIsTenSeconds) {
    LogLimit limit(1, 0);
    EXPECT_TRUE(limit.admit(0, 0, 0));
    EXPECT_FALSE(limit.admit(0, 0, 1));
    EXPECT_TRUE(due(limit, LOG_LIMIT_PERIOD_NS - 1).empty());
    EXPECT_EQ(due(limit, LOG_LIMIT_PERIOD_NS).size(), 1U);
    EXPECT_EQ(LOG_LIMIT_PERIOD_NS, 10'000'000'000);
}

}  // namespace
}  // namespace scada::driver::detail
