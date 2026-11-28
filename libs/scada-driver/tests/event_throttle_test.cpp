// The rate limit of the identity and restart records (D-147, PR 177 S1).

#include "../src/event_throttle.h"

#include "scada/driver/channel_engine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace scada::driver::detail {
namespace {

constexpr std::int64_t PERIOD = 10;

EventRecord events(std::uint64_t count, std::string_view text) {
    EventRecord record;
    record.count = count;
    record.has_details = !text.empty();
    record.length = text.size();
    std::copy(text.begin(), text.end(), record.text.begin());
    return record;
}

std::string_view text_of(const EventRecord& record) {
    return {record.text.data(), record.length};
}

TEST(EventThrottle, TheFirstGoesAtOnceThenOnePerPeriodWithTheCount) {
    EventThrottle throttle(2, PERIOD);
    EventRecord out;
    EXPECT_FALSE(throttle.take(0, 0, false, out));  // nothing yet
    throttle.add(0, events(1, "a"));
    ASSERT_TRUE(throttle.take(0, 100, false, out));
    EXPECT_EQ(out.count, 1U);
    throttle.add(0, events(2, "b"));
    throttle.add(0, events(3, "c"));
    EXPECT_FALSE(throttle.take(0, 109, false, out));
    ASSERT_TRUE(throttle.take(0, 110, false, out));
    EXPECT_EQ(out.count, 5U);  // added, not set
    EXPECT_EQ(text_of(out), "c");
    EXPECT_FALSE(throttle.take(0, 200, false, out));  // empty again
}

TEST(EventThrottle, ASlotIsLimitedOnItsOwn) {
    EventThrottle throttle(2, PERIOD);
    EventRecord out;
    throttle.add(0, events(1, "a"));
    throttle.add(1, events(1, "b"));
    EXPECT_TRUE(throttle.take(0, 0, false, out));
    EXPECT_TRUE(throttle.take(1, 5, false, out));
    throttle.add(0, events(1, "a"));
    throttle.add(1, events(1, "b"));
    EXPECT_TRUE(throttle.take(0, 10, false, out));
    EXPECT_FALSE(throttle.take(1, 10, false, out));
    EXPECT_TRUE(throttle.take(1, 15, false, out));
}

TEST(EventThrottle, TheStopTakesWhatThePeriodHeld) {
    EventThrottle throttle(1, PERIOD);
    EventRecord out;
    throttle.add(0, events(1, "a"));
    ASSERT_TRUE(throttle.take(0, 0, false, out));
    throttle.add(0, events(4, "b"));
    EXPECT_FALSE(throttle.take(0, 1, false, out));
    ASSERT_TRUE(throttle.take(0, 1, true, out));
    EXPECT_EQ(out.count, 4U);
    EXPECT_FALSE(throttle.take(0, 2, true, out));  // nothing held
}

TEST(EventThrottle, EventsWithoutDetailsKeepTheEarlierOnes) {
    // S4: a count whose details could not be read keeps the last known.
    EventThrottle throttle(1, PERIOD);
    EventRecord out;
    throttle.add(0, events(1, "first"));
    ASSERT_TRUE(throttle.take(0, 0, false, out));
    throttle.add(0, events(2, ""));
    ASSERT_TRUE(throttle.take(0, PERIOD, false, out));
    EXPECT_EQ(out.count, 2U);
    EXPECT_TRUE(out.has_details);
    EXPECT_EQ(text_of(out), "first");
    // None ever known: none given.
    EventThrottle fresh(1, PERIOD);
    fresh.add(0, events(1, ""));
    ASSERT_TRUE(fresh.take(0, 0, false, out));
    EXPECT_FALSE(out.has_details);
}

TEST(EventThrottle, ASlotOutOfRangeAndAPeriodAtTheEndOfTimeAreSafe) {
    constexpr std::int64_t LATE = std::numeric_limits<std::int64_t>::max();
    EventThrottle throttle(1, PERIOD);
    EventRecord out;
    throttle.add(5, events(1, "a"));
    EXPECT_FALSE(throttle.take(5, 0, true, out));
    throttle.add(0, events(1, "a"));
    ASSERT_TRUE(throttle.take(0, LATE - 1, false, out));
    throttle.add(0, events(1, "a"));
    EXPECT_FALSE(throttle.take(0, LATE - 1, false, out));
    EXPECT_TRUE(throttle.take(0, LATE, false, out));
}

TEST(EventThrottle, TheDefaultPeriodIsTenSeconds) {
    EXPECT_EQ(EVENT_LOG_PERIOD_NS, 10'000'000'000);
    EventThrottle throttle(1);
    EventRecord out;
    throttle.add(0, events(1, "a"));
    ASSERT_TRUE(throttle.take(0, 0, false, out));
    throttle.add(0, events(1, "a"));
    EXPECT_FALSE(throttle.take(0, EVENT_LOG_PERIOD_NS - 1, false, out));
    EXPECT_TRUE(throttle.take(0, EVENT_LOG_PERIOD_NS, false, out));
}

}  // namespace
}  // namespace scada::driver::detail
