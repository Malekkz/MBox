// Outcomes, companions, staleness, `keep_last`, bits, strings and times of
// the processing after decode (driver interface contract 5.1 to 5.8, 7.5;
// conformance tests 4, 6, 7, 8, 17 and 40, the processing half).

#include "scada/driver/sample_processing.h"
#include "scada/driver/point_rules.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/tag_value.h"
#include "scada/driver/zone_table.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::driver {
namespace {

using tag::Quality;
using tag::QualityFlag;
using tag::TagType;

constexpr std::int64_t SECOND = 1'000'000'000;
constexpr std::int64_t RECEIVE_NS = 1'800'000'000 * SECOND;

constexpr unsigned INT16 = 16;
constexpr std::uint64_t UINT16_ALL_ONES = 0xFFFFU;
// Values of the samples; any number would do.
constexpr std::int64_t FIRST_VALUE = 42;
constexpr std::int64_t SECOND_VALUE = 7;
constexpr std::int64_t THIRD_VALUE = 9;
constexpr std::int64_t STALE_VALUE = 10;
constexpr std::int64_t KEPT_VALUE = 20;
constexpr std::int64_t MAX_AGE_S = 5;
constexpr std::uint32_t COMPANION_INDEX = 9;
// `field: [8, 11]` of a status word, and its value 5.
constexpr std::uint8_t FIELD_LOW = 8;
constexpr std::uint8_t FIELD_HIGH = 11;
constexpr std::int64_t FIELD_TEST = 5;
constexpr std::uint8_t BIT_7 = 7;

std::uint16_t flags(std::initializer_list<QualityFlag> list) {
    std::uint16_t out = 0;
    for (const QualityFlag each : list) {
        out = static_cast<std::uint16_t>(out | tag::to_raw(each));
    }
    return out;
}

bool has(std::uint16_t value, QualityFlag wanted) {
    return tag::has_flag(value, wanted);
}

// One point and its state, fed samples at given monotonic times.
class Point {
public:
    explicit Point(PointRules point_rules) : rules_(std::move(point_rules)) {
        reset_point_state(state_, rules_.tag_type, 0);
    }

    bool stale(std::int64_t now_mono_ns, ProcessedSample& out) {
        return check_stale(rules_, state_, now_mono_ns, out);
    }

    ProcessedSample feed(const RawSample& sample, std::int64_t mono_ns,
                         std::span<const CompanionInput> companions = {}) {
        return process_sample(
            rules_, state_, sample, companions,
            SampleTimes{.receive_time_ns = RECEIVE_NS + mono_ns,
                        .receive_mono_ns = mono_ns});
    }

    ProcessedSample value(std::int64_t raw, std::int64_t mono_ns,
                          std::span<const CompanionInput> companions = {}) {
        return feed(
            RawSample{.outcome = Outcome::VALUE, .raw = raw_signed(raw, INT16)},
            mono_ns, companions);
    }

private:
    PointRules rules_;
    PointState state_;
};

PointRules int_rules() {
    PointRules rules;
    rules.tag_type = TagType::INT64;
    return rules;
}

// --- Conformance test 4: each outcome gives its quality and flags --------

TEST(SampleProcessing, ValueIsGoodWithLocalTime) {
    Point point(int_rules());
    const TagWrite write = point.value(FIRST_VALUE, SECOND).tag;
    EXPECT_EQ(write.value.i, FIRST_VALUE);
    EXPECT_EQ(write.quality, Quality::GOOD);
    EXPECT_EQ(write.flags, flags({QualityFlag::LOCAL_TIME}));
    EXPECT_EQ(write.source_time_ns, RECEIVE_NS + SECOND);
}

TEST(SampleProcessing, NoValueIsBadWithTheMarkersFlagsAndKeepsTheValue) {
    Point point(int_rules());
    (void)point.value(FIRST_VALUE, SECOND);
    const ProcessedSample out =
        point.feed(RawSample{.outcome = Outcome::NO_VALUE,
                             .mark = {.quality = Quality::BAD,
                                      .flags = flags({QualityFlag::BLOCKED})}},
                   2 * SECOND);
    EXPECT_EQ(out.tag.value.i, FIRST_VALUE);
    EXPECT_EQ(out.tag.quality, Quality::BAD);
    EXPECT_EQ(out.tag.flags,
              flags({QualityFlag::BLOCKED, QualityFlag::LOCAL_TIME}));
    EXPECT_TRUE(out.counts.sentinel);
}

TEST(SampleProcessing, AbsentIsBadNotInitIfNeverReadAndCounted) {
    Point point(int_rules());
    const ProcessedSample first =
        point.feed(RawSample{.outcome = Outcome::ABSENT}, SECOND);
    EXPECT_EQ(first.tag.quality, Quality::BAD);
    EXPECT_EQ(first.tag.flags,
              flags({QualityFlag::NOT_INIT, QualityFlag::LOCAL_TIME}));
    EXPECT_TRUE(first.counts.became_absent);
    const ProcessedSample again =
        point.feed(RawSample{.outcome = Outcome::ABSENT}, 2 * SECOND);
    EXPECT_FALSE(again.counts.became_absent);
    // A lost answer says nothing about presence.
    const ProcessedSample lost =
        point.feed(RawSample{.outcome = Outcome::COMM_FAIL}, 3 * SECOND);
    EXPECT_FALSE(lost.counts.left_absent);
    EXPECT_TRUE(point.value(1, 4 * SECOND).counts.left_absent);
}

TEST(SampleProcessing, DeviceInvalidTakesTheMappingAndIsNeverGood) {
    Point point(int_rules());
    (void)point.value(SECOND_VALUE, SECOND);
    const ProcessedSample test_mode =
        point.feed(RawSample{.outcome = Outcome::DEVICE_INVALID,
                             .mark = {.quality = Quality::UNCERTAIN,
                                      .flags = flags({QualityFlag::TEST})}},
                   2 * SECOND);
    EXPECT_EQ(test_mode.tag.quality, Quality::UNCERTAIN);
    EXPECT_TRUE(has(test_mode.tag.flags, QualityFlag::TEST));
    EXPECT_EQ(test_mode.tag.value.i, SECOND_VALUE);
    EXPECT_TRUE(test_mode.counts.invalid);
    const ProcessedSample unmapped =
        point.feed(RawSample{.outcome = Outcome::DEVICE_INVALID,
                             .mark = {.quality = Quality::GOOD}},
                   3 * SECOND);
    EXPECT_EQ(unmapped.tag.quality, Quality::UNCERTAIN);
}

TEST(SampleProcessing, CommFailIsBadCommFailAndKeepsTheValue) {
    Point point(int_rules());
    (void)point.value(THIRD_VALUE, SECOND);
    const TagWrite write =
        point.feed(RawSample{.outcome = Outcome::COMM_FAIL}, 2 * SECOND).tag;
    EXPECT_EQ(write.value.i, THIRD_VALUE);
    EXPECT_EQ(write.quality, Quality::BAD);
    EXPECT_EQ(write.flags,
              flags({QualityFlag::COMM_FAIL, QualityFlag::LOCAL_TIME}));
}

TEST(SampleProcessing, CommFailBeforeAnyValueKeepsNotInit) {
    Point point(int_rules());
    const TagWrite write =
        point.feed(RawSample{.outcome = Outcome::COMM_FAIL}, SECOND).tag;
    EXPECT_EQ(write.value.i, 0);
    EXPECT_EQ(write.flags, flags({QualityFlag::COMM_FAIL, QualityFlag::NOT_INIT,
                                  QualityFlag::LOCAL_TIME}));
}

// --- Conformance test 6: several samples of one point in one delivery ----

TEST(SampleProcessing, SeveralSamplesAreProcessedOneByOneInOrder) {
    Point point(int_rules());
    std::vector<std::int64_t> written;
    for (const std::int64_t raw : {1, 2, 2, 3}) {
        written.push_back(point.value(raw, SECOND).tag.value.i);
    }
    EXPECT_EQ(written, (std::vector<std::int64_t>{1, 2, 2, 3}));
}

// --- Conformance test 8: a dependent point with a failed companion -------

TEST(SampleProcessing, QualityRuleOnACompanionField) {
    // `quality_from: [{ point: status, field: [8, 11], map: { 0: good, 5:
    // [bad, TEST] }, default: bad }]`.
    PointRules rules = int_rules();
    rules.companions = {3};
    rules.quality_rules.push_back(QualityRule{
        .companion = 0,
        .has_field = true,
        .lo = FIELD_LOW,
        .hi = FIELD_HIGH,
        .entries = {{.key = 0, .mark = {}},
                    {.key = FIELD_TEST,
                     .mark = {.quality = Quality::BAD,
                              .flags = flags({QualityFlag::TEST})}}}});
    Point point(rules);
    const CompanionInput normal{.outcome = Outcome::VALUE,
                                .raw = raw_unsigned(0x00FF, 16)};
    EXPECT_EQ(point.value(1, SECOND, {&normal, 1}).tag.quality, Quality::GOOD);
    const CompanionInput test{.outcome = Outcome::VALUE,
                              .raw = raw_unsigned(0x0500, 16)};
    const TagWrite tested = point.value(1, SECOND, {&test, 1}).tag;
    EXPECT_EQ(tested.quality, Quality::BAD);
    EXPECT_TRUE(has(tested.flags, QualityFlag::TEST));
    const CompanionInput other{.outcome = Outcome::VALUE,
                               .raw = raw_unsigned(0x0300, 16)};
    EXPECT_EQ(point.value(1, SECOND, {&other, 1}).tag.quality, Quality::BAD);
}

TEST(SampleProcessing, FailedCompanionMakesTheDependentBad) {
    PointRules rules = int_rules();
    rules.companions = {3};
    rules.quality_rules.push_back(
        QualityRule{.companion = 0, .entries = {{.key = 0, .mark = {}}}});
    Point point(rules);
    const CompanionInput lost{.outcome = Outcome::COMM_FAIL};
    const TagWrite write = point.value(1, SECOND, {&lost, 1}).tag;
    EXPECT_EQ(write.quality, Quality::BAD);
    EXPECT_TRUE(has(write.flags, QualityFlag::COMM_FAIL));
    // A missing slot counts as a lost companion.
    EXPECT_EQ(point.value(1, SECOND).tag.quality, Quality::BAD);
}

TEST(SampleProcessing, HealthRuleWithCommFail) {
    PointRules rules = int_rules();
    rules.companions = {4};
    rules.health.push_back(
        HealthRule{.companion = 0, .good = {1}, .comm_fail = true});
    Point point(rules);
    const CompanionInput online{.outcome = Outcome::VALUE,
                                .raw = raw_unsigned(1, 16)};
    EXPECT_EQ(point.value(5, SECOND, {&online, 1}).tag.quality, Quality::GOOD);
    // Cached values behind a gateway that lost its device: COMM_FAIL.
    const CompanionInput offline{.outcome = Outcome::VALUE,
                                 .raw = raw_unsigned(0, 16)};
    const TagWrite cached = point.value(5, SECOND, {&offline, 1}).tag;
    EXPECT_EQ(cached.quality, Quality::BAD);
    EXPECT_TRUE(has(cached.flags, QualityFlag::COMM_FAIL));
    // A stale health point counts as not good.
    const CompanionInput stale{
        .outcome = Outcome::VALUE, .raw = raw_unsigned(1, 16), .stale = true};
    EXPECT_EQ(point.value(5, SECOND, {&stale, 1}).tag.quality, Quality::BAD);
}

// --- Conformance test 17: STALE after max_age, cleared by a good read ----

TEST(SampleProcessing, StaleAfterMaxAgeOnceAndClearedByTheNextGoodRead) {
    PointRules rules = int_rules();
    rules.max_age_ns = 3 * SECOND;
    Point point(rules);
    (void)point.value(STALE_VALUE, SECOND);
    ProcessedSample out;
    EXPECT_FALSE(point.stale(4 * SECOND, out));
    ASSERT_TRUE(point.stale(4 * SECOND + 1, out));
    EXPECT_EQ(out.tag.value.i, STALE_VALUE);
    EXPECT_EQ(out.tag.quality, Quality::GOOD);
    EXPECT_TRUE(has(out.tag.flags, QualityFlag::STALE));
    // Once.
    EXPECT_FALSE(point.stale(10 * SECOND, out));
    // A lost answer keeps STALE; a good read clears it.
    EXPECT_TRUE(
        has(point.feed(RawSample{.outcome = Outcome::COMM_FAIL}, 11 * SECOND)
                .tag.flags,
            QualityFlag::STALE));
    EXPECT_FALSE(
        has(point.value(11, 12 * SECOND).tag.flags, QualityFlag::STALE));
}

TEST(SampleProcessing, NoStaleWithoutMaxAge) {
    Point point(int_rules());
    (void)point.value(STALE_VALUE, SECOND);
    ProcessedSample out;
    EXPECT_FALSE(point.stale(1000 * SECOND, out));
}

// --- Conformance test 40: `on_invalid: keep_last` -------------------------

TEST(SampleProcessing, KeepLastIsUncertainStaleThenBadAfterMaxAge) {
    PointRules rules = int_rules();
    rules.max_age_ns = MAX_AGE_S * SECOND;
    rules.markers.push_back(InvalidMarker{.pattern = UINT16_ALL_ONES,
                                          .action = InvalidAction::KEEP_LAST});
    Point point(rules);
    (void)point.value(KEPT_VALUE, SECOND);
    const TagWrite kept = point.value(-1, 2 * SECOND).tag;  // 0xFFFF in int16
    EXPECT_EQ(kept.value.i, KEPT_VALUE);
    EXPECT_EQ(kept.quality, Quality::UNCERTAIN);
    EXPECT_TRUE(has(kept.flags, QualityFlag::STALE));
    EXPECT_FALSE(has(kept.flags, QualityFlag::SUBSTITUTED));
    // The marker keeps coming; within max_age of the valid value it stays.
    EXPECT_EQ(point.value(-1, 6 * SECOND).tag.quality, Quality::UNCERTAIN);
    // Past max_age since the last valid value: BAD with STALE, by the next
    // marker or by the boundary check, whichever comes first.
    ProcessedSample out;
    ASSERT_TRUE(point.stale(6 * SECOND + 1, out));
    EXPECT_EQ(out.tag.quality, Quality::BAD);
    EXPECT_TRUE(has(out.tag.flags, QualityFlag::STALE));
    EXPECT_EQ(out.tag.value.i, KEPT_VALUE);
    const TagWrite later = point.value(-1, 7 * SECOND).tag;
    EXPECT_EQ(later.quality, Quality::BAD);
    // A valid value ends it.
    const TagWrite valid = point.value(21, 8 * SECOND).tag;
    EXPECT_EQ(valid.quality, Quality::GOOD);
    EXPECT_FALSE(has(valid.flags, QualityFlag::STALE));
}

TEST(SampleProcessing, KeepLastWithoutAValidValueIsBad) {
    PointRules rules = int_rules();
    rules.max_age_ns = MAX_AGE_S * SECOND;
    rules.markers.push_back(InvalidMarker{.pattern = UINT16_ALL_ONES,
                                          .action = InvalidAction::KEEP_LAST});
    Point point(rules);
    const TagWrite write = point.value(-1, SECOND).tag;
    EXPECT_EQ(write.quality, Quality::BAD);
    EXPECT_TRUE(has(write.flags, QualityFlag::NOT_INIT));
}

// --- Bits ------------------------------------------------------------------

TEST(SampleProcessing, NamedBitsFromOneReadWithInvert) {
    PointRules rules = int_rules();
    rules.has_tag = false;
    rules.bits = {BitRule{.bit = 0}, BitRule{.bit = 3},
                  BitRule{.bit = BIT_7, .invert = true}};
    Point point(rules);
    const ProcessedSample out =
        point.feed(RawSample{.outcome = Outcome::VALUE,
                             .raw = raw_unsigned(0b0000'1001, 16)},
                   SECOND);
    EXPECT_FALSE(out.write_tag);
    ASSERT_EQ(out.bit_count, 3U);
    EXPECT_TRUE(out.bits.at(0).value.b);
    EXPECT_TRUE(out.bits.at(1).value.b);
    EXPECT_TRUE(out.bits.at(2).value.b);  // bit 7 is 0, inverted
    EXPECT_EQ(out.bits.at(0).value.type, TagType::BOOL);
    // A lost answer keeps the bits with the word's quality.
    const ProcessedSample lost =
        point.feed(RawSample{.outcome = Outcome::COMM_FAIL}, 2 * SECOND);
    EXPECT_TRUE(lost.bits.at(1).value.b);
    EXPECT_EQ(lost.bits.at(1).quality, Quality::BAD);
}

TEST(SampleProcessing, BoolPointWithInvert) {
    PointRules rules;
    rules.tag_type = TagType::BOOL;
    rules.invert = true;
    Point point(rules);
    EXPECT_FALSE(
        point
            .feed(RawSample{.outcome = Outcome::VALUE, .raw = raw_bool(true)},
                  SECOND)
            .tag.value.b);
}

// --- Strings ---------------------------------------------------------------

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

TEST(SampleProcessing, StringIsKeptAndWrittenAgainOnALostAnswer) {
    PointRules rules;
    rules.tag_type = TagType::STRING;
    Point point(rules);
    std::string text = "unit 7";
    const ProcessedSample out = point.feed(
        RawSample{.outcome = Outcome::VALUE, .raw = raw_string(bytes_of(text))},
        SECOND);
    EXPECT_EQ(out.tag.value.text, "unit 7");
    text = "garbage";  // the frame is gone after the delivery
    const ProcessedSample lost =
        point.feed(RawSample{.outcome = Outcome::COMM_FAIL}, 2 * SECOND);
    EXPECT_EQ(lost.tag.value.text, "unit 7");
}

TEST(SampleProcessing, LongStringIsCutAtACharacterBoundaryWithOverflow) {
    PointRules rules;
    rules.tag_type = TagType::STRING;
    Point point(rules);
    // 249 ASCII bytes, then a 2-byte character that would end at byte 251.
    const std::string text = std::string(249, 'a') + "\xC3\xA9" + "bc";
    const TagWrite write =
        point
            .feed(RawSample{.outcome = Outcome::VALUE,
                            .raw = raw_string(bytes_of(text))},
                  SECOND)
            .tag;
    EXPECT_EQ(write.value.text.size(), 249U);
    EXPECT_TRUE(has(write.flags, QualityFlag::OVERFLOW));
    EXPECT_EQ(write.quality, Quality::GOOD);
}

TEST(SampleProcessing, InvalidUtf8IsBad) {
    PointRules rules;
    rules.tag_type = TagType::STRING;
    Point point(rules);
    const std::string text = "ab\xC0\x80";  // an overlong NUL
    const TagWrite write =
        point
            .feed(RawSample{.outcome = Outcome::VALUE,
                            .raw = raw_string(bytes_of(text))},
                  SECOND)
            .tag;
    EXPECT_EQ(write.quality, Quality::BAD);
    EXPECT_TRUE(has(write.flags, QualityFlag::OUT_OF_RANGE));
}

// --- Conformance test 7: time of a sample ----------------------------------

PointRules timed_rules(DeviceTimeRule rule) {
    PointRules rules = int_rules();
    rules.companions = {COMPANION_INDEX};
    rules.time_companion = 0;
    rules.companion_time = std::move(rule);
    return rules;
}

TEST(SampleProcessing, DeviceTimeInUtcSyncedHasNoFlag) {
    Point point(timed_rules(DeviceTimeRule{.synced = true}));
    const std::int64_t device = 1'700'000'000 * SECOND;
    const CompanionInput time{.outcome = Outcome::VALUE,
                              .raw = raw_time(device)};
    const TagWrite write = point.value(1, SECOND, {&time, 1}).tag;
    EXPECT_EQ(write.source_time_ns, device);
    EXPECT_EQ(write.flags, 0);
}

TEST(SampleProcessing, DeviceTimeWithTrustUnknownIsTimeUncertain) {
    Point point(timed_rules(DeviceTimeRule{}));
    const CompanionInput time{.outcome = Outcome::VALUE,
                              .raw = raw_time(1'700'000'000 * SECOND)};
    EXPECT_EQ(point.value(1, SECOND, {&time, 1}).tag.flags,
              flags({QualityFlag::TIME_UNCERTAIN}));
}

TEST(SampleProcessing, DeviceTimeOlderThanTheLastIsWrittenAsReceived) {
    Point point(timed_rules(DeviceTimeRule{.synced = true}));
    const CompanionInput later{.outcome = Outcome::VALUE,
                               .raw = raw_time(2000 * SECOND)};
    const CompanionInput earlier{.outcome = Outcome::VALUE,
                                 .raw = raw_time(1000 * SECOND)};
    (void)point.value(1, SECOND, {&later, 1});
    EXPECT_EQ(point.value(1, 2 * SECOND, {&earlier, 1}).tag.source_time_ns,
              1000 * SECOND);
}

TEST(SampleProcessing, LocalDeviceTimeInTheRepeatedAndSkippedHour) {
    auto zone = ZoneTable::build("Europe/Berlin");
    ASSERT_TRUE(zone.ok());
    Point point(timed_rules(
        DeviceTimeRule{.local = true, .zone = zone.value(), .synced = true}));
    // 2026-10-25 02:30 local is repeated: the first occurrence, 00:30 UTC.
    constexpr std::int64_t OCT25_0230_LOCAL = 1'792'895'400;
    const CompanionInput repeated{.outcome = Outcome::VALUE,
                                  .raw = raw_time(OCT25_0230_LOCAL * SECOND)};
    const TagWrite first = point.value(1, SECOND, {&repeated, 1}).tag;
    EXPECT_EQ(first.source_time_ns, (OCT25_0230_LOCAL - 7200) * SECOND);
    EXPECT_EQ(first.flags, flags({QualityFlag::TIME_UNCERTAIN}));
    // 2026-03-29 02:30 local does not exist: moved forward to 03:30 CEST,
    // 01:30 UTC.
    constexpr std::int64_t MAR29_0230_LOCAL = 1'774'751'400;
    const CompanionInput skipped{.outcome = Outcome::VALUE,
                                 .raw = raw_time(MAR29_0230_LOCAL * SECOND)};
    const TagWrite second = point.value(1, SECOND, {&skipped, 1}).tag;
    EXPECT_EQ(second.source_time_ns, (MAR29_0230_LOCAL - 3600) * SECOND);
    EXPECT_EQ(second.flags, flags({QualityFlag::TIME_UNCERTAIN}));
    // An ordinary hour converts without a flag.
    constexpr std::int64_t JUN1_1200_LOCAL = 1'780'315'200;
    const CompanionInput summer{.outcome = Outcome::VALUE,
                                .raw = raw_time(JUN1_1200_LOCAL * SECOND)};
    const TagWrite third = point.value(1, SECOND, {&summer, 1}).tag;
    EXPECT_EQ(third.source_time_ns, (JUN1_1200_LOCAL - 7200) * SECOND);
    EXPECT_EQ(third.flags, 0);
}

TEST(SampleProcessing, FailedTimeCompanionMakesTheValueBad) {
    Point point(timed_rules(DeviceTimeRule{.synced = true}));
    const CompanionInput lost{.outcome = Outcome::COMM_FAIL};
    const TagWrite write = point.value(1, SECOND, {&lost, 1}).tag;
    EXPECT_EQ(write.quality, Quality::BAD);
    EXPECT_TRUE(has(write.flags, QualityFlag::COMM_FAIL));
}

TEST(SampleProcessing, TimePointValueIsUtcNanoseconds) {
    auto zone = ZoneTable::build("Europe/Berlin");
    ASSERT_TRUE(zone.ok());
    PointRules rules = int_rules();
    rules.device_time = DeviceTimeRule{.local = true, .zone = zone.value()};
    Point point(rules);
    constexpr std::int64_t JUN1_1200_LOCAL = 1'780'315'200;
    const TagWrite write =
        point
            .feed(RawSample{.outcome = Outcome::VALUE,
                            .raw = raw_time(JUN1_1200_LOCAL * SECOND)},
                  SECOND)
            .tag;
    EXPECT_EQ(write.value.i, (JUN1_1200_LOCAL - 7200) * SECOND);
}

}  // namespace
}  // namespace scada::driver
