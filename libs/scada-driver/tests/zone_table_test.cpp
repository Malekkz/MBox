// Local time to UTC (driver interface contract 5.8; configuration schemas
// contract 4.6, `device_time`).

#include "scada/driver/zone_table.h"

#include "scada/common/status.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace scada::driver {
namespace {

constexpr std::int64_t SECOND = 1'000'000'000;
constexpr std::int64_t HOUR = 3600 * SECOND;

// Seconds since 1970 of local wall-clock times, read as if they were UTC.
constexpr std::int64_t OCT25_0130 = 1'792'891'800;  // 2026-10-25 01:30
constexpr std::int64_t OCT25_0230 = 1'792'895'400;  // 02:30, repeated
constexpr std::int64_t OCT25_0330 = 1'792'899'000;  // 03:30
constexpr std::int64_t MAR29_0130 = 1'774'747'800;  // 2026-03-29 01:30
constexpr std::int64_t MAR29_0230 = 1'774'751'400;  // 02:30, skipped
constexpr std::int64_t MAR29_0330 = 1'774'755'000;  // 03:30

TEST(ZoneTable, DefaultIsUtc) {
    const ZoneTable utc;
    const UtcTime time = utc.to_utc(OCT25_0230 * SECOND);
    EXPECT_EQ(time.utc_ns, OCT25_0230 * SECOND);
    EXPECT_FALSE(time.uncertain);
}

TEST(ZoneTable, UnknownZoneIsNotFound) {
    const auto table = ZoneTable::build("Nowhere/Invented");
    ASSERT_FALSE(table.ok());
    EXPECT_EQ(table.status().code(), StatusCode::NOT_FOUND);
}

TEST(ZoneTable, AroundTheEndOfSummerTime) {
    const auto table = ZoneTable::build("Europe/Berlin");
    ASSERT_TRUE(table.ok());
    const ZoneTable& zone = table.value();
    // Before: CEST, +2 h.
    EXPECT_EQ(zone.to_utc(OCT25_0130 * SECOND).utc_ns,
              OCT25_0130 * SECOND - 2 * HOUR);
    EXPECT_FALSE(zone.to_utc(OCT25_0130 * SECOND).uncertain);
    // The repeated hour: the first occurrence, still +2 h, and uncertain.
    const UtcTime repeated = zone.to_utc(OCT25_0230 * SECOND);
    EXPECT_EQ(repeated.utc_ns, OCT25_0230 * SECOND - 2 * HOUR);
    EXPECT_TRUE(repeated.uncertain);
    // After: CET, +1 h.
    EXPECT_EQ(zone.to_utc(OCT25_0330 * SECOND).utc_ns,
              OCT25_0330 * SECOND - HOUR);
    EXPECT_FALSE(zone.to_utc(OCT25_0330 * SECOND).uncertain);
}

TEST(ZoneTable, AroundTheStartOfSummerTime) {
    const auto table = ZoneTable::build("Europe/Berlin");
    ASSERT_TRUE(table.ok());
    const ZoneTable& zone = table.value();
    EXPECT_EQ(zone.to_utc(MAR29_0130 * SECOND).utc_ns,
              MAR29_0130 * SECOND - HOUR);
    // The skipped hour moves forward by the change: 02:30 reads as 03:30
    // CEST, which is 01:30 UTC.
    const UtcTime skipped = zone.to_utc(MAR29_0230 * SECOND);
    EXPECT_EQ(skipped.utc_ns, MAR29_0230 * SECOND - HOUR);
    EXPECT_TRUE(skipped.uncertain);
    EXPECT_EQ(zone.to_utc(MAR29_0330 * SECOND).utc_ns,
              MAR29_0330 * SECOND - 2 * HOUR);
    EXPECT_FALSE(zone.to_utc(MAR29_0330 * SECOND).uncertain);
}

TEST(ZoneTable, BoundsOfTheTable) {
    const auto table = ZoneTable::build("Europe/Berlin");
    ASSERT_TRUE(table.ok());
    // Before 1970 and after 2100: the nearest end's offset, uncertain.
    EXPECT_TRUE(table.value().to_utc(-HOUR).uncertain);
    constexpr std::int64_t YEAR_2200 = 7'258'118'400;
    EXPECT_TRUE(table.value().to_utc(YEAR_2200 * SECOND).uncertain);
}

TEST(ZoneTable, ZoneWithoutChangesIsCertain) {
    const auto table = ZoneTable::build("Etc/GMT-3");  // +3 h, no DST
    ASSERT_TRUE(table.ok());
    const UtcTime time = table.value().to_utc(OCT25_0230 * SECOND);
    EXPECT_EQ(time.utc_ns, OCT25_0230 * SECOND - 3 * HOUR);
    EXPECT_FALSE(time.uncertain);
}

}  // namespace
}  // namespace scada::driver
