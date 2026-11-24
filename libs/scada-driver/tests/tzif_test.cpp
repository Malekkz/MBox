// The zone file reader (RFC 9636) and its POSIX TZ strings: what is read,
// and every malformed form refused (decision D-130).

#include "../src/tzif.h"

#include "tzif_builder.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scada::driver::detail {
namespace {

constexpr std::int32_t HOUR_S = 3600;
constexpr std::int64_t DAY_S = 86'400;
constexpr std::int64_t FIRST_AT = 100;
constexpr std::int64_t SECOND_AT = 200;

TEST(TzString, StandardOnly) {
    const auto parsed = parse_tz_string("MST7");
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.value().std_offset_s, -7 * HOUR_S);
    EXPECT_FALSE(parsed.value().has_dst);
    const auto quoted = parse_tz_string("<+0530>-5:30");
    ASSERT_TRUE(quoted.ok());
    EXPECT_EQ(quoted.value().std_offset_s, (5 * HOUR_S) + (HOUR_S / 2));
}

TEST(TzString, DaylightWithMonthWeekDayRules) {
    const auto parsed = parse_tz_string("CET-1CEST,M3.5.0,M10.5.0/3");
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.value().std_offset_s, HOUR_S);
    EXPECT_EQ(parsed.value().dst_offset_s,
              2 * HOUR_S);  // default: one hour more
    EXPECT_EQ(parsed.value().start.month, 3);
    EXPECT_EQ(parsed.value().start.week, 5);
    EXPECT_EQ(parsed.value().start.time_s, 2 * HOUR_S);  // default 02:00
    EXPECT_EQ(parsed.value().end.time_s, 3 * HOUR_S);
}

TEST(TzString, JulianZeroBasedAndExtendedTimes) {
    const auto parsed = parse_tz_string("<-03>3<-02>,M3.5.0/-2,M10.5.0/-1");
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.value().start.time_s, -2 * HOUR_S);
    const auto julian = parse_tz_string("EST5EDT,J60/2,300/167");
    ASSERT_TRUE(julian.ok());
    EXPECT_EQ(julian.value().start.form, TzRuleDate::Form::JULIAN_NO_LEAP);
    EXPECT_EQ(julian.value().end.form, TzRuleDate::Form::ZERO_BASED);
    EXPECT_EQ(julian.value().end.time_s, 167 * HOUR_S);
}

TEST(TzString, MalformedFormsAreRefused) {
    for (const char* text :
         {"", "M", "AB1", "<AB>1", "CET", "CET-25", "CET-1CEST",
          "CET-1CEST,M3.5.0", "CET-1CEST,M13.5.0,M10.5.0",
          "CET-1CEST,M3.6.0,M10.5.0", "CET-1CEST,M3.5.7,M10.5.0",
          "CET-1CEST,J0,J365", "CET-1CEST,366,0", "CET-1CEST,M3.5.0/168,0",
          "CET-1:60", "CET-1CEST,M3.5.0,M10.5.0x", "<AB>1"}) {
        EXPECT_FALSE(parse_tz_string(text).ok()) << text;
    }
    EXPECT_FALSE(
        parse_tz_string(std::string(TZ_STRING_MAX_BYTES + 1, 'A')).ok());
}

TEST(TzRule, DatesOfTheRules) {
    // Last Sunday of March 2026 is the 29th, at 02:00 local standard
    // (+1 h): 01:00 UTC.
    const TzRuleDate last_sunday_march{.form = TzRuleDate::Form::MONTH_WEEK_DAY,
                                       .day = 0,
                                       .month = 3,
                                       .week = 5,
                                       .time_s = 2 * HOUR_S};
    EXPECT_EQ(rule_instant_s(last_sunday_march, 2026, HOUR_S),
              (days_from_civil(2026, 3, 29) * DAY_S) + HOUR_S);
    // `J60` is 1 March in every year; `59` is 1 March only in a leap year.
    const TzRuleDate j60{.form = TzRuleDate::Form::JULIAN_NO_LEAP, .day = 60};
    EXPECT_EQ(rule_instant_s(j60, 2024, 0),
              days_from_civil(2024, 3, 1) * DAY_S);
    EXPECT_EQ(rule_instant_s(j60, 2026, 0),
              days_from_civil(2026, 3, 1) * DAY_S);
    const TzRuleDate z59{.form = TzRuleDate::Form::ZERO_BASED, .day = 59};
    EXPECT_EQ(rule_instant_s(z59, 2024, 0),
              days_from_civil(2024, 2, 29) * DAY_S);
    EXPECT_EQ(days_from_civil(1970, 1, 1), 0);
    EXPECT_EQ(days_from_civil(2000, 3, 1), 11'017);
}

TEST(Tzif, ReadsTransitionsAndFooter) {
    test::TzifSpec spec;
    spec.transitions = {FIRST_AT, SECOND_AT};
    spec.type_of = {1, 0};
    spec.offsets = {0, HOUR_S};
    spec.footer = "UTC0";
    const auto zone = parse_tzif(test::build_tzif(spec));
    ASSERT_TRUE(zone.ok());
    EXPECT_EQ(zone.value().transitions_s,
              (std::vector<std::int64_t>{FIRST_AT, SECOND_AT}));
    EXPECT_EQ(zone.value().offsets_s, (std::vector<std::int32_t>{HOUR_S, 0}));
    EXPECT_EQ(zone.value().initial_offset_s, 0);
    ASSERT_TRUE(zone.value().footer.has_value());
}

TEST(Tzif, MalformedFilesAreRefused) {
    const test::TzifSpec good{.transitions = {FIRST_AT},
                              .type_of = {0},
                              .offsets = {HOUR_S},
                              .footer = "CET-1"};
    ASSERT_TRUE(parse_tzif(test::build_tzif(good)).ok());

    std::vector<std::vector<std::byte>> broken;
    test::TzifSpec version1 = good;
    version1.version = '\0';
    broken.push_back(test::build_tzif(version1));
    test::TzifSpec leaps = good;
    leaps.leap_count = 1;
    broken.push_back(test::build_tzif(leaps));
    test::TzifSpec unsorted = good;
    unsorted.transitions = {SECOND_AT, FIRST_AT};
    unsorted.type_of = {0, 0};
    broken.push_back(test::build_tzif(unsorted));
    test::TzifSpec bad_type = good;
    bad_type.type_of = {1};
    broken.push_back(test::build_tzif(bad_type));
    test::TzifSpec far_offset = good;
    far_offset.offsets = {TZIF_MAX_OFFSET_S + 1};
    broken.push_back(test::build_tzif(far_offset));
    test::TzifSpec no_types = good;
    no_types.offsets = {};
    no_types.type_of = {};
    no_types.transitions = {};
    broken.push_back(test::build_tzif(no_types));
    test::TzifSpec bad_footer = good;
    bad_footer.footer = "CET-1CEST";
    broken.push_back(test::build_tzif(bad_footer));
    std::vector<std::byte> magic = test::build_tzif(good);
    magic[0] = std::byte{'X'};
    broken.push_back(magic);
    std::vector<std::byte> no_newline = test::build_tzif(good);
    no_newline.pop_back();
    broken.push_back(no_newline);
    std::vector<std::byte> extra_line = test::build_tzif(good);
    extra_line.insert(extra_line.end() - 1, std::byte{'\n'});
    broken.push_back(extra_line);
    const std::vector<std::byte> whole = test::build_tzif(good);
    for (std::size_t length = 0; length < whole.size(); ++length) {
        broken.emplace_back(
            whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(length));
    }
    broken.emplace_back(TZIF_MAX_BYTES + 1, std::byte{0});
    for (std::size_t index = 0; index < broken.size(); ++index) {
        EXPECT_FALSE(parse_tzif(broken[index]).ok()) << index;
    }
}

TEST(Tzif, CountsAboveTheLimitsAreRefused) {
    test::TzifSpec many;
    many.offsets = {0};
    for (std::uint32_t index = 0; index <= TZIF_MAX_TRANSITIONS; ++index) {
        many.transitions.push_back(index);
        many.type_of.push_back(0);
    }
    EXPECT_FALSE(parse_tzif(test::build_tzif(many)).ok());
}

}  // namespace
}  // namespace scada::driver::detail
