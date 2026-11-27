// Local time to UTC from compiled zone files (driver interface contract
// 5.8; configuration schemas contract 4.6, `device_time`; decision D-130).
// The zones of the system's database are checked against the C library in
// zone_table_all_zones_test.cpp; these are the reader's own cases.

#include "scada/driver/zone_table.h"

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "tzif_builder.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace scada::driver {
namespace {

constexpr std::int64_t SECOND = 1'000'000'000;
constexpr std::int64_t HOUR_S = 3600;
constexpr std::int64_t HOUR = HOUR_S * SECOND;
constexpr std::int64_t HALF_HOUR = HOUR / 2;
constexpr std::int32_t PLUS_ONE_S = 3600;
constexpr std::int32_t PLUS_5_30_S = (5 * PLUS_ONE_S) + (PLUS_ONE_S / 2);
constexpr std::int32_t PLUS_6_S = 6 * PLUS_ONE_S;

// Seconds since 1970 of local wall-clock times, read as if they were UTC.
constexpr std::int64_t OCT25_0130 = 1'792'891'800;  // 2026-10-25 01:30
constexpr std::int64_t OCT25_0230 = 1'792'895'400;  // 02:30, repeated
constexpr std::int64_t OCT25_0330 = 1'792'899'000;  // 03:30
constexpr std::int64_t MAR29_0130 = 1'774'747'800;  // 2026-03-29 01:30
constexpr std::int64_t MAR29_0230 = 1'774'751'400;  // 02:30, skipped
constexpr std::int64_t MAR29_0330 = 1'774'755'000;  // 03:30
constexpr std::int64_t JUL01_1200 = 1'782'907'200;  // 2026-07-01 12:00
constexpr std::int64_t JAN15_1200 = 1'768'478'400;  // 2026-01-15 12:00

ZoneTable zone(const char* name) {
    Result<ZoneTable> table = ZoneTable::build(name);
    EXPECT_TRUE(table.ok()) << name;
    return table.ok() ? table.value() : ZoneTable{};
}

TEST(ZoneTable, DefaultIsUtc) {
    const ZoneTable utc;
    const UtcTime time = utc.to_utc(OCT25_0230 * SECOND);
    EXPECT_EQ(time.utc_ns, OCT25_0230 * SECOND);
    EXPECT_FALSE(time.uncertain);
}

void expect_refused_name(const char* name) {
    EXPECT_FALSE(is_zone_name(name)) << name;
    const auto table = ZoneTable::build(name);
    EXPECT_EQ(table.status().code(), StatusCode::INVALID_ARGUMENT) << name;
}

TEST(ZoneTable, NamesAreCheckedBeforeAPathIsBuilt) {
    for (const char* name : {"Europe/Berlin", "America/Argentina/Buenos_Aires",
                             "Etc/GMT-3", "America/Port-au-Prince"}) {
        EXPECT_TRUE(is_zone_name(name)) << name;
    }
    for (const char* name :
         {"", "/etc/passwd", "../x", "Europe/../Berlin", "Europe//Berlin",
          "Europe/", ".hidden", "a b", "Europe/Berlin\n"}) {
        expect_refused_name(name);
    }
    expect_refused_name(std::string(ZONE_NAME_MAX_BYTES + 1, 'a').c_str());
}

TEST(ZoneTable, UnknownZoneIsNotFound) {
    const auto table = ZoneTable::build("Nowhere/Invented");
    ASSERT_FALSE(table.ok());
    EXPECT_EQ(table.status().code(), StatusCode::NOT_FOUND);
}

TEST(ZoneTable, AroundTheEndOfSummerTime) {
    const ZoneTable berlin = zone("Europe/Berlin");
    EXPECT_EQ(berlin.to_utc(OCT25_0130 * SECOND).utc_ns,
              OCT25_0130 * SECOND - 2 * HOUR);
    EXPECT_FALSE(berlin.to_utc(OCT25_0130 * SECOND).uncertain);
    // The repeated hour: the first occurrence, still +2 h, and uncertain.
    const UtcTime repeated = berlin.to_utc(OCT25_0230 * SECOND);
    EXPECT_EQ(repeated.utc_ns, OCT25_0230 * SECOND - 2 * HOUR);
    EXPECT_TRUE(repeated.uncertain);
    EXPECT_EQ(berlin.to_utc(OCT25_0330 * SECOND).utc_ns,
              OCT25_0330 * SECOND - HOUR);
    EXPECT_FALSE(berlin.to_utc(OCT25_0330 * SECOND).uncertain);
}

TEST(ZoneTable, AroundTheStartOfSummerTime) {
    const ZoneTable berlin = zone("Europe/Berlin");
    EXPECT_EQ(berlin.to_utc(MAR29_0130 * SECOND).utc_ns,
              MAR29_0130 * SECOND - HOUR);
    // The skipped hour moves forward by the change: 02:30 reads as 03:30
    // CEST, which is 01:30 UTC.
    const UtcTime skipped = berlin.to_utc(MAR29_0230 * SECOND);
    EXPECT_EQ(skipped.utc_ns, MAR29_0230 * SECOND - HOUR);
    EXPECT_TRUE(skipped.uncertain);
    EXPECT_EQ(berlin.to_utc(MAR29_0330 * SECOND).utc_ns,
              MAR29_0330 * SECOND - 2 * HOUR);
}

TEST(ZoneTable, WestOfUtcAfterAChangeOfStandardOffset) {
    // The case the C++ library got wrong (decision D-130): 12:00 local on
    // 2026-07-01 is 19:00 UTC, by the zone file.
    const ZoneTable vancouver = zone("America/Vancouver");
    const UtcTime noon = vancouver.to_utc(JUL01_1200 * SECOND);
    EXPECT_EQ(noon.utc_ns, (JUL01_1200 + 7 * HOUR_S) * SECOND);
    EXPECT_FALSE(noon.uncertain);
}

TEST(ZoneTable, SouthernHemisphereAndHalfHourChange) {
    // Lord Howe Island: +10:30 in its winter, +11 in its summer (January).
    const ZoneTable lord_howe = zone("Australia/Lord_Howe");
    constexpr std::int64_t PLUS_11_S = 11 * HOUR_S;
    constexpr std::int64_t PLUS_10_30_S = (10 * HOUR_S) + (HOUR_S / 2);
    EXPECT_EQ(lord_howe.to_utc(JAN15_1200 * SECOND).utc_ns,
              (JAN15_1200 - PLUS_11_S) * SECOND);
    EXPECT_EQ(lord_howe.to_utc(JUL01_1200 * SECOND).utc_ns,
              (JUL01_1200 - PLUS_10_30_S) * SECOND);
    // Sydney: +11 in January, +10 in July.
    const ZoneTable sydney = zone("Australia/Sydney");
    EXPECT_EQ(sydney.to_utc(JAN15_1200 * SECOND).utc_ns,
              (JAN15_1200 - PLUS_11_S) * SECOND);
    EXPECT_EQ(sydney.to_utc(JUL01_1200 * SECOND).utc_ns,
              (JUL01_1200 - 10 * HOUR_S) * SECOND);
}

TEST(ZoneTable, IrishSummerAndWinter) {
    // Dublin is UTC+1 in summer and UTC in winter, whichever way the
    // database writes its rule.
    const ZoneTable dublin = zone("Europe/Dublin");
    EXPECT_EQ(dublin.to_utc(JUL01_1200 * SECOND).utc_ns,
              (JUL01_1200 - HOUR_S) * SECOND);
    EXPECT_EQ(dublin.to_utc(JAN15_1200 * SECOND).utc_ns, JAN15_1200 * SECOND);
}

TEST(ZoneTable, BoundsOfTheTableSaturate) {
    const ZoneTable berlin = zone("Europe/Berlin");
    const ZoneTable new_york = zone("America/New_York");
    constexpr std::int64_t MIN = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t MAX = std::numeric_limits<std::int64_t>::max();
    // Before 1970 and after 2100: the nearest end's offset, uncertain, and
    // never past the limits of int64.
    const UtcTime early = berlin.to_utc(MIN);
    EXPECT_TRUE(early.uncertain);
    EXPECT_EQ(early.utc_ns, MIN);
    const UtcTime late = new_york.to_utc(MAX);
    EXPECT_TRUE(late.uncertain);
    EXPECT_EQ(late.utc_ns, MAX);
    EXPECT_TRUE(berlin.to_utc(-HOUR).uncertain);
    constexpr std::int64_t YEAR_2200_S = 7'258'118'400;
    EXPECT_TRUE(berlin.to_utc(YEAR_2200_S * SECOND).uncertain);
}

TEST(ZoneTable, ZoneWithoutChangesIsCertain) {
    const ZoneTable gmt_minus_3 = zone("Etc/GMT-3");  // +3 h, no DST
    const UtcTime time = gmt_minus_3.to_utc(OCT25_0230 * SECOND);
    EXPECT_EQ(time.utc_ns, OCT25_0230 * SECOND - 3 * HOUR);
    EXPECT_FALSE(time.uncertain);
}

TEST(ZoneTable, FooterRuleAloneGivesEveryYear) {
    // A file with no transitions: the footer's rule decides from 1970 on.
    test::TzifSpec spec;
    spec.offsets = {PLUS_ONE_S};
    spec.footer = "CET-1CEST,M3.5.0,M10.5.0/3";
    const auto table = ZoneTable::from_tzif(test::build_tzif(spec));
    ASSERT_TRUE(table.ok());
    EXPECT_TRUE(table.value().to_utc(OCT25_0230 * SECOND).uncertain);
    EXPECT_EQ(table.value().to_utc(JUL01_1200 * SECOND).utc_ns,
              (JUL01_1200 - 2 * HOUR_S) * SECOND);
    constexpr std::size_t CHANGES_PER_YEAR = 2;
    constexpr std::size_t YEARS =
        ZONE_TABLE_LAST_YEAR - ZONE_TABLE_FIRST_YEAR + 1;
    EXPECT_EQ(table.value().interval_count(), (CHANGES_PER_YEAR * YEARS) + 1);
}

TEST(ZoneTable, HalfHourOffsetSkippedHalfHour) {
    // An invented zone at +5:30 that moves to +6 on 2026-03-29 at 02:00
    // local: 02:00 to 02:30 local is skipped.
    constexpr std::int64_t LOCAL_0200_S = 1'774'749'600;
    constexpr std::int64_t CHANGE_S =
        LOCAL_0200_S - (5 * HOUR_S) - (HOUR_S / 2);
    test::TzifSpec spec;
    spec.transitions = {CHANGE_S};
    spec.type_of = {1};
    spec.offsets = {PLUS_5_30_S, PLUS_6_S};
    spec.footer = "<+06>-6";
    const auto table = ZoneTable::from_tzif(test::build_tzif(spec));
    ASSERT_TRUE(table.ok());
    constexpr std::int64_t LOCAL_0215_S = LOCAL_0200_S + (HOUR_S / 4);
    const UtcTime skipped = table.value().to_utc(LOCAL_0215_S * SECOND);
    EXPECT_TRUE(skipped.uncertain);
    EXPECT_EQ(skipped.utc_ns,
              (LOCAL_0215_S * SECOND) - ((5 * HOUR) + HALF_HOUR));
}

constexpr std::int32_t MINUS_5_S = -5 * PLUS_ONE_S;
constexpr std::int32_t PLUS_10_S = 10 * PLUS_ONE_S;
constexpr std::int64_t DAYLIGHT_WEST_S = 4 * HOUR_S;  // EDT, UTC-4
constexpr std::int64_t DAYLIGHT_EAST_S = 11 * HOUR_S;
constexpr std::int64_t JUL01_1988_1200 = 583'761'600;
constexpr std::int64_t JAN01_2027_0030 = 1'798'763'400;
constexpr std::int64_t DEC31_2026_2330 = 1'798'759'800;

ZoneTable footer_only(std::int32_t standard_s, const char* footer) {
    test::TzifSpec spec;
    spec.offsets = {standard_s};
    spec.footer = footer;
    Result<ZoneTable> table = ZoneTable::from_tzif(test::build_tzif(spec));
    EXPECT_TRUE(table.ok()) << footer;
    return table.ok() ? table.value() : ZoneTable{};
}

TEST(ZoneTable, DaylightTimeAllYearIsNeverStandardTime) {
    // RFC 9636 3.3.1: `EST5EDT,0/0,J365/25` is daylight time all year. The
    // end of each year meets the start of the next at one instant; ordered
    // the other way round it gave whole years of standard time, certain.
    const ZoneTable table = footer_only(MINUS_5_S, "EST5EDT,0/0,J365/25");
    for (const std::int64_t local_s :
         {JUL01_1988_1200, JUL01_1200, JAN01_2027_0030, DEC31_2026_2330,
          JAN15_1200}) {
        const UtcTime time = table.to_utc(local_s * SECOND);
        EXPECT_EQ(time.utc_ns, (local_s + DAYLIGHT_WEST_S) * SECOND) << local_s;
        EXPECT_FALSE(time.uncertain) << local_s;
    }
    EXPECT_EQ(table.interval_count(), 1U);
}

TEST(ZoneTable, DaylightTimeAllYearEastOfUtcReachesTheEndOfTheTable) {
    // East of UTC the end of 2100 falls inside the table; it meets the start
    // of 2101, so the last hours of the table are daylight time too.
    const ZoneTable table = footer_only(PLUS_10_S, "<+10>-10<+11>,0/0,J365/25");
    EXPECT_EQ(table.to_utc(JAN01_2027_0030 * SECOND).utc_ns,
              (JAN01_2027_0030 - DAYLIGHT_EAST_S) * SECOND);
    EXPECT_EQ(table.interval_count(), 1U);
}

TEST(ZoneTable, WithoutAFooterTimesAfterTheLastTransitionAreUncertain) {
    // RFC 9636 3.3: an empty footer gives no rule for later times.
    constexpr std::int64_t CHANGE_S = 1'700'000'000;  // 2023-11-14
    test::TzifSpec spec;
    spec.transitions = {CHANGE_S};
    spec.type_of = {1};
    spec.offsets = {PLUS_ONE_S, PLUS_6_S};
    spec.footer = "";
    const auto table = ZoneTable::from_tzif(test::build_tzif(spec));
    ASSERT_TRUE(table.ok());
    constexpr std::int64_t JUL01_2020_1200 = 1'593'604'800;
    const UtcTime before = table.value().to_utc(JUL01_2020_1200 * SECOND);
    EXPECT_FALSE(before.uncertain);
    EXPECT_EQ(before.utc_ns, (JUL01_2020_1200 - PLUS_ONE_S) * SECOND);
    const UtcTime after = table.value().to_utc(JUL01_1200 * SECOND);
    EXPECT_TRUE(after.uncertain);
    EXPECT_EQ(after.utc_ns, (JUL01_1200 - PLUS_6_S) * SECOND);
    // With no transition either, nothing is certain.
    test::TzifSpec bare;
    bare.offsets = {PLUS_ONE_S};
    bare.footer = "";
    const auto none = ZoneTable::from_tzif(test::build_tzif(bare));
    ASSERT_TRUE(none.ok());
    EXPECT_TRUE(none.value().to_utc(JUL01_1200 * SECOND).uncertain);
}

// Issue #158, found by tzif_fuzz: changes of offset closer together than
// the span of the offsets (+15:04:16 and +18:08:23, 38,656 s and 128 s
// apart, then the footer's +3 h). A local time of the first interval also
// lies in the interval before it and in the footer's; the interval that
// holds it was outside the search window of two, and the conversion gave
// the footer's later instant as certain. Every interval that may hold a
// local time is searched now: the earliest instant, marked uncertain.
TEST(ZoneTable, ChangesCloserThanTheirOffsetsAreAllSearched) {
    constexpr std::int64_t FIRST_S = 15'599'616;  // 1970-06-30
    constexpr std::int32_t EAST_15_S = 54'256;
    constexpr std::int32_t EAST_18_S = 65'303;
    constexpr std::int64_t SECOND_S = 15'638'272;
    constexpr std::int64_t THIRD_S = 15'638'400;
    test::TzifSpec spec;
    spec.transitions = {FIRST_S, SECOND_S, THIRD_S};
    spec.type_of = {1, 0, 1};
    spec.offsets = {EAST_18_S, EAST_15_S};
    spec.footer = "CET-3";
    const auto table = ZoneTable::from_tzif(test::build_tzif(spec));
    ASSERT_TRUE(table.ok());
    for (const std::int64_t instant :
         {FIRST_S, FIRST_S + 1, FIRST_S + ((SECOND_S - FIRST_S) / 2)}) {
        const std::int64_t local = instant + EAST_15_S;
        const UtcTime back = table.value().to_utc(local * SECOND);
        // Repeated: held by the first interval and by the footer's, and
        // near the first change also by the interval before it, which
        // gives the earliest instant.
        EXPECT_TRUE(back.uncertain) << instant;
        EXPECT_LE(back.utc_ns, instant * SECOND) << instant;
        const std::int64_t before = local - EAST_18_S;
        EXPECT_EQ(back.utc_ns, (before < FIRST_S ? before : instant) * SECOND)
            << instant;
    }
}

// Issue #158, the input of PR 159's fuzz job: changes 2 min and then 1 s
// apart (+2 h, +1 h, +2 h) and a footer without daylight time at -3 h.
// The footer's change falls on the last transition's instant and replaces
// it, so that no interval is empty: the +1 h interval, then -3 h.
TEST(ZoneTable, AFooterChangeAtTheLastTransitionReplacesIt) {
    constexpr std::int64_t FIRST_S = 1'700'000'000;
    constexpr std::int64_t SECOND_S = FIRST_S + 120;
    constexpr std::int64_t THIRD_S = SECOND_S + 1;
    constexpr std::int32_t PLUS_1_S = 3'600;
    constexpr std::int32_t PLUS_2_S = 7'200;
    constexpr std::int64_t MINUS_3_S = -10'800;
    test::TzifSpec spec;
    spec.transitions = {FIRST_S, SECOND_S, THIRD_S};
    spec.type_of = {0, 1, 0};
    spec.offsets = {PLUS_2_S, PLUS_1_S};
    spec.footer = "CET3";
    const auto table = ZoneTable::from_tzif(test::build_tzif(spec));
    ASSERT_TRUE(table.ok());
    // From 1970 at +2 h, +1 h from the second change, -3 h from the third.
    EXPECT_EQ(table.value().interval_count(), 3U);
    // A local time of the +2 h interval: it also lies in the -3 h one, a
    // repeated time, given its first occurrence.
    const UtcTime early =
        table.value().to_utc((FIRST_S + 60 + PLUS_2_S) * SECOND);
    EXPECT_EQ(early.utc_ns, (FIRST_S + 60) * SECOND);
    EXPECT_TRUE(early.uncertain);
    // A local time only the -3 h interval holds.
    const std::int64_t late_s = THIRD_S + 86'400;
    const UtcTime late = table.value().to_utc((late_s + MINUS_3_S) * SECOND);
    EXPECT_EQ(late.utc_ns, late_s * SECOND);
    EXPECT_FALSE(late.uncertain);
}

// A file whose changes are so close that a local time may lie in more than
// sixteen intervals is refused, so that a conversion stays bounded (#158).
TEST(ZoneTable, ChangesTooCloseForTheSearchAreRefused) {
    constexpr std::int64_t START_S = 1'700'000'000;
    constexpr std::int64_t APART_S = 60;
    constexpr std::size_t CHANGES = 40;
    test::TzifSpec spec;
    for (std::size_t index = 0; index < CHANGES; ++index) {
        spec.transitions.push_back(
            START_S + (static_cast<std::int64_t>(index) * APART_S));
        spec.type_of.push_back(static_cast<std::uint8_t>(index % 2));
    }
    spec.offsets = {PLUS_6_S, -PLUS_6_S};
    spec.footer = "";
    EXPECT_FALSE(ZoneTable::from_tzif(test::build_tzif(spec)).ok());
    // The same changes a day apart are taken.
    constexpr std::int64_t DAY_S = 86'400;
    for (std::size_t index = 0; index < CHANGES; ++index) {
        spec.transitions.at(index) =
            START_S + (static_cast<std::int64_t>(index) * DAY_S);
    }
    EXPECT_TRUE(ZoneTable::from_tzif(test::build_tzif(spec)).ok());
}

}  // namespace
}  // namespace scada::driver
