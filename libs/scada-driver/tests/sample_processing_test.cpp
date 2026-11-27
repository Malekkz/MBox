// Outcomes, companions, staleness, `keep_last`, bits, strings and times of
// the processing after decode (driver interface contract 5.1 to 5.8, 7.5;
// conformance tests 4, 6, 7, 8, 17 and 40, the processing half).

#include "scada/driver/sample_processing.h"
#include "sample_helpers.h"
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
#include <limits>
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
constexpr std::int64_t INT16_LOWEST = -32'768;
constexpr std::uint64_t INT16_LOWEST_PATTERN = 0x8000U;
constexpr std::int64_t MAX_AGE_S = 5;
constexpr std::uint32_t COMPANION_INDEX = 9;
// `field: [8, 11]` of a status word, and its value 5.
constexpr std::uint8_t FIELD_LOW = 8;
constexpr std::uint8_t FIELD_HIGH = 11;
constexpr std::int64_t FIELD_TEST = 5;
constexpr std::uint8_t BIT_7 = 7;
constexpr std::uint64_t UNLISTED_STATUS = 7;
constexpr std::uint8_t TOP_BIT_16 = 15;
constexpr std::uint8_t FIELD_4 = 4;
constexpr std::uint8_t FIELD_5 = 5;
constexpr std::uint8_t BEYOND_64 = 70;
constexpr std::int64_t OFFSET_MINUS_3 = -3;
// Local wall-clock times in Europe/Berlin, read as if they were UTC.
constexpr std::int64_t OCT25_0230_LOCAL = 1'792'895'400;  // repeated
constexpr std::int64_t MAR29_0230_LOCAL = 1'774'751'400;  // skipped
constexpr std::int64_t JUN1_1200_LOCAL = 1'780'315'200;

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
    explicit Point(PointRules point_rules)
        : rules_(std::move(point_rules)), state_(test::fresh_state(rules_)) {}

    bool stale(std::int64_t now_mono_ns, ProcessedSample& out) {
        return check_stale(rules_, state_, now_mono_ns, out);
    }

    ProcessedSample feed(const RawSample& sample, std::int64_t mono_ns,
                         std::span<const CompanionInput> companions = {}) {
        return test::processed(
            rules_, state_, sample, companions,
            SampleTimes{.receive_time_ns = RECEIVE_NS + mono_ns,
                        .receive_mono_ns = mono_ns});
    }

    void seed(const TagValue& stored, bool substituted) {
        seed_point_state(state_, stored, substituted);
    }

    void reset(std::int64_t mono_ns) {
        reset_point_state(state_, rules_.tag_type, mono_ns);
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

PointRules float_point_rules() {
    PointRules rules;
    rules.tag_type = TagType::FLOAT64;
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

TEST(SampleProcessing, DeviceInvalidHoldsTheLastValueThenGoesBad) {
    // DEVICE_INVALID is held like `keep_last` (decision D-131): the last
    // valid value, UNCERTAIN with STALE and the mapping's flags, BAD with
    // STALE after max_age; never GOOD.
    PointRules rules = int_rules();
    rules.max_age_ns = MAX_AGE_S * SECOND;
    Point point(rules);
    (void)point.value(SECOND_VALUE, SECOND);
    const RawSample test_mode{.outcome = Outcome::DEVICE_INVALID,
                              .mark = {.quality = Quality::UNCERTAIN,
                                       .flags = flags({QualityFlag::TEST})}};
    const ProcessedSample held = point.feed(test_mode, 2 * SECOND);
    EXPECT_EQ(held.tag.quality, Quality::UNCERTAIN);
    EXPECT_TRUE(has(held.tag.flags, QualityFlag::TEST));
    EXPECT_TRUE(has(held.tag.flags, QualityFlag::STALE));
    EXPECT_EQ(held.tag.value.i, SECOND_VALUE);
    EXPECT_TRUE(held.counts.invalid);
    const ProcessedSample unmapped =
        point.feed(RawSample{.outcome = Outcome::DEVICE_INVALID,
                             .mark = {.quality = Quality::GOOD}},
                   3 * SECOND);
    EXPECT_EQ(unmapped.tag.quality, Quality::UNCERTAIN);
    // max_age after the last valid value: BAD, by the next answer or by
    // the boundary check.
    ProcessedSample out;
    ASSERT_TRUE(point.stale((MAX_AGE_S + 1) * SECOND + 1, out));
    EXPECT_EQ(out.tag.quality, Quality::BAD);
    EXPECT_TRUE(has(out.tag.flags, QualityFlag::STALE));
    const ProcessedSample later =
        point.feed(test_mode, (MAX_AGE_S + 2) * SECOND);
    EXPECT_EQ(later.tag.quality, Quality::BAD);
    EXPECT_EQ(later.tag.value.i, SECOND_VALUE);
}

TEST(SampleProcessing, DeviceInvalidBeforeAnyReadIsBadNotAMadeUpZero) {
    PointRules rules = int_rules();
    rules.max_age_ns = MAX_AGE_S * SECOND;
    Point point(rules);
    const TagWrite write =
        point
            .feed(RawSample{.outcome = Outcome::DEVICE_INVALID,
                            .mark = {.quality = Quality::UNCERTAIN,
                                     .flags = flags({QualityFlag::TEST})}},
                  SECOND)
            .tag;
    EXPECT_EQ(write.quality, Quality::BAD);
    EXPECT_TRUE(has(write.flags, QualityFlag::NOT_INIT));
    EXPECT_TRUE(has(write.flags, QualityFlag::TEST));
}

TEST(SampleProcessing, DeviceInvalidMappedBadIsBadWithoutStale) {
    PointRules rules = int_rules();
    rules.max_age_ns = MAX_AGE_S * SECOND;
    Point point(rules);
    (void)point.value(SECOND_VALUE, SECOND);
    const TagWrite write =
        point
            .feed(RawSample{.outcome = Outcome::DEVICE_INVALID,
                            .mark = {.quality = Quality::BAD,
                                     .flags = flags({QualityFlag::BLOCKED})}},
                  2 * SECOND)
            .tag;
    EXPECT_EQ(write.quality, Quality::BAD);
    EXPECT_EQ(write.flags,
              flags({QualityFlag::BLOCKED, QualityFlag::LOCAL_TIME}));
}

TEST(SampleProcessing, IncomingFlagsAreMaskedToWhatADeviceMayGive) {
    // OWNER_DOWN is never stored and RESTORED is the library's (decision
    // D-131); STALE and NOT_INIT are the framework's.
    const std::uint16_t forbidden =
        flags({QualityFlag::OWNER_DOWN, QualityFlag::RESTORED,
               QualityFlag::STALE, QualityFlag::NOT_INIT});
    PointRules rules = int_rules();
    rules.max_age_ns = MAX_AGE_S * SECOND;
    rules.quality_rules.push_back(QualityRule{
        .self = true,
        .entries = {{.key = 1,
                     .mark = {.quality = Quality::UNCERTAIN,
                              .flags = static_cast<std::uint16_t>(
                                  forbidden | flags({QualityFlag::TEST}))}}}});
    Point point(rules);
    const TagWrite from_map = point.value(1, SECOND).tag;
    EXPECT_EQ(from_map.flags,
              flags({QualityFlag::TEST, QualityFlag::LOCAL_TIME}));
    const TagWrite no_value =
        point
            .feed(RawSample{.outcome = Outcome::NO_VALUE,
                            .mark = {.quality = Quality::BAD,
                                     .flags = forbidden}},
                  2 * SECOND)
            .tag;
    EXPECT_EQ(no_value.flags, flags({QualityFlag::LOCAL_TIME}));
    const TagWrite invalid =
        point
            .feed(RawSample{.outcome = Outcome::DEVICE_INVALID,
                            .mark = {.quality = Quality::UNCERTAIN,
                                     .flags = forbidden}},
                  3 * SECOND)
            .tag;
    EXPECT_FALSE(has(invalid.flags, QualityFlag::OWNER_DOWN));
    EXPECT_FALSE(has(invalid.flags, QualityFlag::RESTORED));
}

// Review R2 and R3 of #157: SUBSTITUTED comes from the `substitute` action
// alone. A protocol's mark (forced at the device: room only, no protocol
// carries a value with a mark yet), a configured `bad` marker and a
// `quality_from` entry cannot give it.
TEST(SampleProcessing, OnlyTheSubstituteActionGivesSubstituted) {
    const std::uint16_t substituted = flags({QualityFlag::SUBSTITUTED});
    PointRules rules = int_rules();
    rules.markers.push_back(InvalidMarker{.pattern = UINT16_ALL_ONES,
                                          .action = InvalidAction::BAD,
                                          .flags = substituted});
    rules.quality_rules.push_back(QualityRule{
        .self = true,
        .entries = {
            {.key = 1,
             .mark = {.quality = Quality::UNCERTAIN, .flags = substituted}}}});
    Point point(rules);
    EXPECT_FALSE(has(point.value(1, SECOND).tag.flags,
                     QualityFlag::SUBSTITUTED));  // quality_from
    EXPECT_FALSE(has(point.value(-1, 2 * SECOND).tag.flags,
                     QualityFlag::SUBSTITUTED));  // a `bad` marker
    for (const Outcome outcome : {Outcome::DEVICE_INVALID, Outcome::NO_VALUE}) {
        const TagWrite marked =
            point
                .feed(RawSample{.outcome = outcome,
                                .mark = {.quality = Quality::UNCERTAIN,
                                         .flags = substituted}},
                      3 * SECOND)
                .tag;
        EXPECT_FALSE(has(marked.flags, QualityFlag::SUBSTITUTED));
    }
}

// Review R1 of #157: SUBSTITUTED describes the value. It stays with a kept
// substitute through COMM_FAIL, ABSENT and NO_VALUE, and the next value
// from the device ends it.
// A BAD write that keeps the substitute KEPT_VALUE with its flag.
::testing::AssertionResult keeps_the_substitute(const TagWrite& write) {
    if (write.quality == Quality::BAD && write.value.i == KEPT_VALUE &&
        has(write.flags, QualityFlag::SUBSTITUTED)) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << "quality " << static_cast<int>(write.quality) << ", value "
           << write.value.i << ", flags " << write.flags;
}

// A point whose marker substitutes KEPT_VALUE for all ones.
PointRules substituting_rules() {
    PointRules rules = int_rules();
    rules.markers.push_back(
        InvalidMarker{.pattern = UINT16_ALL_ONES,
                      .action = InvalidAction::SUBSTITUTE,
                      .substitute = int64_value(KEPT_VALUE)});
    return rules;
}

// --- Rules that validate_point_rules refuses give BAD (#169) -------------

// One refusal of validate_point_rules: `refuse` breaks rules that are valid
// otherwise and substitute KEPT_VALUE for all ones.
struct RefusedKind {
    const char* name;
    void (*refuse)(PointRules&);
};

constexpr std::uint8_t NO_SUCH_SLOT = 5;
constexpr std::uint8_t BIT_64 = 64;
constexpr double HALF = 0.5;

constexpr std::array<RefusedKind, 17> REFUSED_KINDS{{
    {"exponent companion slot",
     [](PointRules& rules) {
         rules.exponent = ExponentRule{.companion = NO_SUCH_SLOT};
     }},
    {"time companion slot",
     [](PointRules& rules) { rules.time_companion = NO_SUCH_SLOT; }},
    {"quality rule slot",
     [](PointRules& rules) {
         rules.quality_rules = {
             QualityRule{.companion = NO_SUCH_SLOT, .entries = {}}};
     }},
    {"health rule slot",
     [](PointRules& rules) {
         rules.health = {HealthRule{.companion = NO_SUCH_SLOT, .good = {}}};
     }},
    {"bit number",
     [](PointRules& rules) { rules.bits = {BitRule{.bit = BIT_64}}; }},
    {"double point states not sorted",
     [](PointRules& rules) {
         rules.double_point =
             DoublePointRule{.states = {{.raw = 2}, {.raw = 1}}};
     }},
    {"enumeration with a scale",
     [](PointRules& rules) {
         rules.has_enum = true;
         rules.scale = ScaleRule{.integer = true};
     }},
    {"divide_by 0",
     [](PointRules& rules) {
         rules.scale = ScaleRule{.form = ScaleRule::Form::DIVIDE_BY,
                                 .integer = true,
                                 .divide_by = 0};
     }},
    {"integer tag with a binary64 scale",
     [](PointRules& rules) { rules.scale = ScaleRule{.gain = HALF}; }},
    {"integer tag on a floating raw type",
     [](PointRules& rules) { rules.raw_kind = RawKind::FLOAT; }},
    {"local device time without a zone",
     [](PointRules& rules) { rules.device_time.local = true; }},
    {"companion local time without a zone",
     [](PointRules& rules) { rules.companion_time.local = true; }},
    {"counter on a floating raw",
     [](PointRules& rules) {
         rules.tag_type = TagType::FLOAT64;
         rules.raw_kind = RawKind::FLOAT;
         rules.counter = CounterRule{.limit = UINT16_ALL_ONES};
         rules.markers.front().substitute = float64_value(-1.0);
     }},
    // CF-504 as the compile step gives it (D-115): what sample_processing_fuzz
    // found with STRING tags on, a STRING tag on a numeric raw that loses
    // its substitute's text (#169).
    {"STRING tag on a numeric raw",
     [](PointRules& rules) {
         rules.tag_type = TagType::STRING;
         rules.markers.front().substitute = string_value({});
     }},
    {"string raw on an INT64 tag",
     [](PointRules& rules) { rules.raw_kind = RawKind::STRING; }},
    {"BOOL tag on a numeric raw",
     [](PointRules& rules) {
         rules.tag_type = TagType::BOOL;
         rules.markers.front().substitute = bool_value(true);
     }},
    // What sample_processing_fuzz found with STRING tags on (#169): an
    // enumeration on a STRING tag with an unsigned raw.
    {"enumeration on a STRING tag",
     [](PointRules& rules) {
         rules.tag_type = TagType::STRING;
         rules.has_enum = true;
         rules.markers.front().substitute = string_value({});
     }},
}};

// sample_processing_fuzz with STRING tags on (#169): a STRING tag on a
// string raw, with a substitute marker, given a sample of another kind (a
// signed 8-bit raw whose pattern is the marker's). A marker never applies
// to a STRING tag: the sample is no string, so BAD with OUT_OF_RANGE, and
// never a substitute.
TEST(SampleProcessing, AMarkerNeverAppliesToAStringTag) {
    constexpr std::uint64_t MARKER = 5;
    constexpr unsigned INT8 = 8;
    PointRules rules;
    rules.tag_type = TagType::STRING;
    rules.raw_kind = RawKind::STRING;
    rules.markers.push_back(InvalidMarker{.pattern = MARKER,
                                          .action = InvalidAction::SUBSTITUTE,
                                          .substitute = string_value({})});
    ASSERT_TRUE(validate_point_rules(rules).ok());
    Point point(rules);
    const TagWrite write = point
                               .feed(RawSample{.outcome = Outcome::VALUE,
                                               .raw = raw_signed(MARKER, INT8)},
                                     SECOND)
                               .tag;
    EXPECT_EQ(write.quality, Quality::BAD);
    EXPECT_TRUE(has(write.flags, QualityFlag::OUT_OF_RANGE));
    EXPECT_FALSE(has(write.flags, QualityFlag::SUBSTITUTED));
}

// The control: the rules are valid, and all ones gives the substitute.
TEST(SampleProcessing, TheRulesOfTheRefusedKindsSubstituteWhenValid) {
    const PointRules rules = substituting_rules();
    ASSERT_TRUE(validate_point_rules(rules).ok());
    Point point(rules);
    const TagWrite write = point.value(-1, SECOND).tag;
    EXPECT_EQ(write.quality, Quality::UNCERTAIN);
    EXPECT_TRUE(has(write.flags, QualityFlag::SUBSTITUTED));
}

// A write of refused rules: BAD with OUT_OF_RANGE, never SUBSTITUTED, no
// companion's COMM_FAIL, and the driver's time (LOCAL_TIME).
::testing::AssertionResult refused_write(const TagWrite& write) {
    if (write.quality == Quality::BAD &&
        has(write.flags, QualityFlag::OUT_OF_RANGE) &&
        !has(write.flags, QualityFlag::SUBSTITUTED) &&
        !has(write.flags, QualityFlag::COMM_FAIL) &&
        has(write.flags, QualityFlag::LOCAL_TIME) &&
        !has(write.flags, QualityFlag::TIME_UNCERTAIN)) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << "quality " << static_cast<int>(write.quality) << ", flags "
           << write.flags;
}

// Whatever path a sample takes, refused rules give BAD with OUT_OF_RANGE:
// no substitute, no value read.
TEST(SampleProcessing, RefusedRulesGiveBadOnEveryPath) {
    for (const RefusedKind& kind : REFUSED_KINDS) {
        PointRules rules = substituting_rules();
        kind.refuse(rules);
        ASSERT_FALSE(validate_point_rules(rules).ok()) << kind.name;
        Point point(rules);
        // All ones would be the substitute; FIRST_VALUE a value read.
        EXPECT_TRUE(refused_write(point.value(-1, SECOND).tag)) << kind.name;
        EXPECT_TRUE(refused_write(point.value(FIRST_VALUE, 2 * SECOND).tag))
            << kind.name;
    }
}

// Refused rules take no time from a companion: a device time is not used,
// and a failed companion adds no COMM_FAIL (#169).
TEST(SampleProcessing, RefusedRulesKeepTheDriversTime) {
    PointRules rules = int_rules();
    rules.companions = {COMPANION_INDEX};
    rules.time_companion = 0;
    rules.bits = {BitRule{.bit = BIT_64}};
    ASSERT_FALSE(validate_point_rules(rules).ok());
    Point point(rules);
    const CompanionInput time{.outcome = Outcome::VALUE,
                              .raw = raw_time(SECOND)};
    const TagWrite timed = point.value(FIRST_VALUE, SECOND, {&time, 1}).tag;
    EXPECT_TRUE(refused_write(timed));
    EXPECT_EQ(timed.source_time_ns, RECEIVE_NS + SECOND);
    const CompanionInput lost{.outcome = Outcome::COMM_FAIL};
    EXPECT_TRUE(
        refused_write(point.value(FIRST_VALUE, 2 * SECOND, {&lost, 1}).tag));
}

// A young DEVICE_INVALID hold under refused rules is BAD too: the rules
// are refused before any value is held.
TEST(SampleProcessing, RefusedRulesHoldNothing) {
    PointRules rules = int_rules();
    rules.max_age_ns = MAX_AGE_S * SECOND;
    rules.time_companion = NO_SUCH_SLOT;
    ASSERT_FALSE(validate_point_rules(rules).ok());
    Point point(rules);
    (void)point.value(FIRST_VALUE, SECOND);
    const TagWrite held =
        point
            .feed(RawSample{.outcome = Outcome::DEVICE_INVALID,
                            .mark = {.quality = Quality::UNCERTAIN}},
                  2 * SECOND)
            .tag;
    EXPECT_EQ(held.quality, Quality::BAD);
    EXPECT_TRUE(has(held.flags, QualityFlag::OUT_OF_RANGE));
}

TEST(SampleProcessing, AKeptSubstituteKeepsSubstituted) {
    Point point(substituting_rules());
    const TagWrite substitute = point.value(-1, SECOND).tag;
    EXPECT_EQ(substitute.value.i, KEPT_VALUE);
    EXPECT_TRUE(has(substitute.flags, QualityFlag::SUBSTITUTED));
    std::int64_t now = SECOND;
    for (const Outcome outcome :
         {Outcome::COMM_FAIL, Outcome::ABSENT, Outcome::NO_VALUE}) {
        now += SECOND;
        EXPECT_TRUE(keeps_the_substitute(
            point.feed(RawSample{.outcome = outcome}, now).tag))
            << static_cast<int>(outcome);
    }
}

TEST(SampleProcessing, AValueReadEndsAKeptSubstitute) {
    Point point(substituting_rules());
    (void)point.value(-1, SECOND);
    (void)point.feed(RawSample{.outcome = Outcome::COMM_FAIL}, 2 * SECOND);
    const TagWrite read = point.value(FIRST_VALUE, 3 * SECOND).tag;
    EXPECT_EQ(read.quality, Quality::GOOD);
    EXPECT_FALSE(has(read.flags, QualityFlag::SUBSTITUTED));
    // And a COMM_FAIL after it keeps the read value without the flag.
    EXPECT_FALSE(
        has(point.feed(RawSample{.outcome = Outcome::COMM_FAIL}, 4 * SECOND)
                .tag.flags,
            QualityFlag::SUBSTITUTED));
}

// A refused answer keeps the substitute and its flag: it gives no value
// (review of PR 157).
TEST(SampleProcessing, ARefusedValueKeepsAKeptSubstitute) {
    Point point(substituting_rules());
    (void)point.value(-1, SECOND);
    RawValue refused = raw_signed(1, INT16);
    refused.width = 0;  // no raw value has width 0
    EXPECT_TRUE(keeps_the_substitute(
        point
            .feed(RawSample{.outcome = Outcome::VALUE, .raw = refused},
                  2 * SECOND)
            .tag));
}

// A reset point holds no substitute (review of PR 157).
TEST(SampleProcessing, AResetEndsASubstitute) {
    Point point(substituting_rules());
    (void)point.value(-1, SECOND);
    point.reset(2 * SECOND);
    EXPECT_FALSE(
        has(point.feed(RawSample{.outcome = Outcome::COMM_FAIL}, 3 * SECOND)
                .tag.flags,
            QualityFlag::SUBSTITUTED));
}

// A point that substitutes all ones and keeps the last value for 0x8000.
PointRules substituting_and_keeping_rules() {
    PointRules rules = substituting_rules();
    rules.max_age_ns = MAX_AGE_S * SECOND;
    rules.markers.push_back(InvalidMarker{.pattern = INT16_LOWEST_PATTERN,
                                          .action = InvalidAction::KEEP_LAST});
    return rules;
}

// A young hold after a substitute holds the last value read from the
// device, which is no substitute: UNCERTAIN with STALE, without
// SUBSTITUTED (review of PR 157). By `keep_last` and by DEVICE_INVALID.
TEST(SampleProcessing, AHoldAfterASubstituteHoldsTheValueReadWithoutTheFlag) {
    const auto held_value = [](const TagWrite& write) {
        return write.value.i == FIRST_VALUE &&
               write.quality == Quality::UNCERTAIN &&
               has(write.flags, QualityFlag::STALE) &&
               !has(write.flags, QualityFlag::SUBSTITUTED);
    };
    Point keep(substituting_and_keeping_rules());
    (void)keep.value(FIRST_VALUE, SECOND);
    ASSERT_TRUE(
        has(keep.value(-1, 2 * SECOND).tag.flags, QualityFlag::SUBSTITUTED));
    EXPECT_TRUE(held_value(keep.value(INT16_LOWEST, 3 * SECOND).tag));

    Point invalid(substituting_and_keeping_rules());
    (void)invalid.value(FIRST_VALUE, SECOND);
    ASSERT_TRUE(
        has(invalid.value(-1, 2 * SECOND).tag.flags, QualityFlag::SUBSTITUTED));
    EXPECT_TRUE(
        held_value(invalid
                       .feed(RawSample{.outcome = Outcome::DEVICE_INVALID,
                                       .mark = {.quality = Quality::UNCERTAIN}},
                             3 * SECOND)
                       .tag));
}

// A substitute of every type of a point with markers carries the flag.
TEST(SampleProcessing, ASubstituteOfEveryTypeIsSubstituted) {
    for (const TagValue substitute :
         {bool_value(true), int64_value(KEPT_VALUE),
          uint64_value(static_cast<std::uint64_t>(KEPT_VALUE)),
          float64_value(-1.0)}) {
        PointRules rules;
        rules.tag_type = substitute.type;
        if (substitute.type == TagType::BOOL) {
            rules.raw_kind = RawKind::BOOL;  // CF-504 (#169)
        }
        rules.markers.push_back(
            InvalidMarker{.pattern = UINT16_ALL_ONES,
                          .action = InvalidAction::SUBSTITUTE,
                          .substitute = substitute});
        Point point(rules);
        const TagWrite write =
            point
                .feed(RawSample{.outcome = Outcome::VALUE,
                                .raw = raw_unsigned(UINT16_ALL_ONES, INT16)},
                      SECOND)
                .tag;
        EXPECT_EQ(write.quality, Quality::UNCERTAIN)
            << static_cast<int>(substitute.type);
        EXPECT_TRUE(has(write.flags, QualityFlag::SUBSTITUTED))
            << static_cast<int>(substitute.type);
        EXPECT_EQ(write.value.type, substitute.type);
    }
}

// Review R1 of #157: a companion whose value was substituted does not pass
// SUBSTITUTED to the point that depends on it; the point is UNCERTAIN.
TEST(SampleProcessing, ASubstitutedCompanionDoesNotPassTheFlag) {
    PointRules rules = int_rules();
    rules.companions = {3};
    rules.quality_rules.push_back(
        QualityRule{.companion = 0, .entries = {{.key = 0, .mark = {}}}});
    Point point(rules);
    const CompanionInput substituted{
        .outcome = Outcome::VALUE,
        .raw = raw_unsigned(0, INT16),
        .mark = {.quality = Quality::UNCERTAIN,
                 .flags = flags({QualityFlag::SUBSTITUTED})}};
    const TagWrite write = point.value(1, SECOND, {&substituted, 1}).tag;
    EXPECT_EQ(write.quality, Quality::UNCERTAIN);
    EXPECT_FALSE(has(write.flags, QualityFlag::SUBSTITUTED));
}

// Review R1 of #157, across a restart: a point seeded with a stored value
// that carries SUBSTITUTED keeps the flag on the writes that keep it.
TEST(SampleProcessing, ASeededSubstituteKeepsItsFlag) {
    Point point(int_rules());
    point.seed(int64_value(KEPT_VALUE), true);
    const TagWrite kept =
        point.feed(RawSample{.outcome = Outcome::COMM_FAIL}, SECOND).tag;
    EXPECT_EQ(kept.value.i, KEPT_VALUE);
    EXPECT_TRUE(has(kept.flags, QualityFlag::SUBSTITUTED));
    Point plain(int_rules());
    plain.seed(int64_value(KEPT_VALUE), false);
    EXPECT_FALSE(has(
        plain.feed(RawSample{.outcome = Outcome::COMM_FAIL}, SECOND).tag.flags,
        QualityFlag::SUBSTITUTED));
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
    EXPECT_TRUE(sample_bit(out, 0));
    EXPECT_TRUE(sample_bit(out, 1));
    EXPECT_TRUE(sample_bit(out, 2));  // bit 7 is 0, inverted
    // A lost answer keeps the bits with the word's quality.
    const ProcessedSample lost =
        point.feed(RawSample{.outcome = Outcome::COMM_FAIL}, 2 * SECOND);
    EXPECT_TRUE(sample_bit(lost, 1));
    EXPECT_EQ(lost.tag.quality, Quality::BAD);
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
    rules.raw_kind = RawKind::STRING;
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
    rules.raw_kind = RawKind::STRING;
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
    rules.raw_kind = RawKind::STRING;
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
    rules.companion_time = rule;
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
        DeviceTimeRule{.local = true, .zone = &zone.value(), .synced = true}));
    // 2026-10-25 02:30 local is repeated: the first occurrence, 00:30 UTC.
    const CompanionInput repeated{.outcome = Outcome::VALUE,
                                  .raw = raw_time(OCT25_0230_LOCAL * SECOND)};
    const TagWrite first = point.value(1, SECOND, {&repeated, 1}).tag;
    EXPECT_EQ(first.source_time_ns, (OCT25_0230_LOCAL - 7200) * SECOND);
    EXPECT_EQ(first.flags, flags({QualityFlag::TIME_UNCERTAIN}));
    // 2026-03-29 02:30 local does not exist: moved forward to 03:30 CEST,
    // 01:30 UTC.
    const CompanionInput skipped{.outcome = Outcome::VALUE,
                                 .raw = raw_time(MAR29_0230_LOCAL * SECOND)};
    const TagWrite second = point.value(1, SECOND, {&skipped, 1}).tag;
    EXPECT_EQ(second.source_time_ns, (MAR29_0230_LOCAL - 3600) * SECOND);
    EXPECT_EQ(second.flags, flags({QualityFlag::TIME_UNCERTAIN}));
    // An ordinary hour converts without a flag.
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
    rules.device_time = DeviceTimeRule{.local = true, .zone = &zone.value()};
    Point point(rules);
    const TagWrite write =
        point
            .feed(RawSample{.outcome = Outcome::VALUE,
                            .raw = raw_time(JUN1_1200_LOCAL * SECOND)},
                  SECOND)
            .tag;
    EXPECT_EQ(write.value.i, (JUN1_1200_LOCAL - 7200) * SECOND);
}

// --- Step 6 for every outcome and marker action (CS 4.7, 4.9) ---------

// A meter behind a gateway: a health rule with COMM_FAIL on a connection
// status companion (slot 0), a status word with `quality_from` (slot 1).
PointRules gateway_rules(InvalidAction action) {
    PointRules rules;
    rules.tag_type = TagType::FLOAT64;
    rules.max_age_ns = MAX_AGE_S * SECOND;
    rules.companions = {COMPANION_INDEX, COMPANION_INDEX + 1};
    rules.health.push_back(
        HealthRule{.companion = 0, .good = {1}, .comm_fail = true});
    rules.markers.push_back(InvalidMarker{
        .any_nan = true, .action = action, .substitute = float64_value(0.0)});
    return rules;
}

const RawSample NAN_SAMPLE{
    .outcome = Outcome::VALUE,
    .raw = raw_float32(std::numeric_limits<float>::quiet_NaN())};
const RawSample POWER_SAMPLE{.outcome = Outcome::VALUE,
                             .raw = raw_float32(1500.5F)};

std::array<CompanionInput, 2> link(std::uint64_t status) {
    return {CompanionInput{.outcome = Outcome::VALUE,
                           .raw = raw_unsigned(status, INT16)},
            CompanionInput{.outcome = Outcome::VALUE,
                           .raw = raw_unsigned(0, INT16)}};
}

void expect_bad_comm_fail(const TagWrite& write) {
    EXPECT_EQ(write.quality, Quality::BAD);
    EXPECT_TRUE(has(write.flags, QualityFlag::COMM_FAIL));
}

TEST(SampleProcessing, HealthRuleAppliesToAValue) {
    Point point(gateway_rules(InvalidAction::SUBSTITUTE));
    const auto offline = link(0);
    expect_bad_comm_fail(point.feed(POWER_SAMPLE, SECOND, offline).tag);
    const auto online = link(1);
    EXPECT_EQ(point.feed(NAN_SAMPLE, 2 * SECOND, online).tag.quality,
              Quality::UNCERTAIN);
}

TEST(SampleProcessing, HealthRuleAppliesToASubstitute) {
    Point point(gateway_rules(InvalidAction::SUBSTITUTE));
    const auto offline = link(0);
    expect_bad_comm_fail(point.feed(NAN_SAMPLE, SECOND, offline).tag);
    std::array<CompanionInput, 2> lost = link(1);
    lost[0] = CompanionInput{.outcome = Outcome::COMM_FAIL};
    expect_bad_comm_fail(point.feed(NAN_SAMPLE, 2 * SECOND, lost).tag);
}

TEST(SampleProcessing, HealthRuleAppliesToKeepLastAndBad) {
    Point keep(gateway_rules(InvalidAction::KEEP_LAST));
    const auto online = link(1);
    const auto offline = link(0);
    (void)keep.feed(POWER_SAMPLE, SECOND, online);
    expect_bad_comm_fail(keep.feed(NAN_SAMPLE, 2 * SECOND, offline).tag);
    Point bad_marker(gateway_rules(InvalidAction::BAD));
    expect_bad_comm_fail(bad_marker.feed(NAN_SAMPLE, SECOND, offline).tag);
}

TEST(SampleProcessing, HealthRuleAppliesToDeviceInvalidAndNoValue) {
    Point point(gateway_rules(InvalidAction::SUBSTITUTE));
    const auto online = link(1);
    const auto offline = link(0);
    (void)point.feed(POWER_SAMPLE, SECOND, online);
    expect_bad_comm_fail(
        point
            .feed(RawSample{.outcome = Outcome::DEVICE_INVALID,
                            .mark = {.quality = Quality::UNCERTAIN}},
                  2 * SECOND, offline)
            .tag);
    expect_bad_comm_fail(
        point.feed(RawSample{.outcome = Outcome::NO_VALUE}, 3 * SECOND, offline)
            .tag);
}

TEST(SampleProcessing, QualityFromAppliesToKeepLast) {
    // A status word that says "fault" while the raw value is the marker:
    // BAD, not the held value UNCERTAIN.
    PointRules rules = gateway_rules(InvalidAction::KEEP_LAST);
    rules.health.clear();
    rules.quality_rules.push_back(
        QualityRule{.companion = 1, .entries = {{.key = 0, .mark = {}}}});
    Point point(rules);
    std::array<CompanionInput, 2> status = link(1);
    (void)point.feed(POWER_SAMPLE, SECOND, status);
    status[1].raw =
        raw_unsigned(UNLISTED_STATUS, INT16);  // not in the map: `default` BAD
    EXPECT_EQ(point.feed(NAN_SAMPLE, 2 * SECOND, status).tag.quality,
              Quality::BAD);
}

// --- Companions carry their own quality (decision D-131) -------------------

TEST(SampleProcessing, ACompanionWhoseOwnMarkerMatchedFailsTheDependent) {
    // A time companion whose 0 is its "no time" marker: the companion's own
    // processing made it BAD, so the value is not stamped 1970 and GOOD.
    PointRules rules = int_rules();
    rules.companions = {COMPANION_INDEX};
    rules.time_companion = 0;
    rules.companion_time = DeviceTimeRule{.synced = true};
    Point point(rules);
    const CompanionInput no_time{.outcome = Outcome::VALUE,
                                 .raw = raw_time(0),
                                 .mark = {.quality = Quality::BAD}};
    const TagWrite write = point.value(1, SECOND, {&no_time, 1}).tag;
    EXPECT_EQ(write.quality, Quality::BAD);
    EXPECT_NE(write.source_time_ns, 0);
}

TEST(SampleProcessing, AnUncertainCompanionMakesTheDependentUncertain) {
    PointRules rules = float_point_rules();
    rules.companions = {COMPANION_INDEX};
    rules.exponent = ExponentRule{.companion = 0};
    Point point(rules);
    const CompanionInput exponent{
        .outcome = Outcome::VALUE,
        .raw = raw_signed(1, INT16),
        .mark = {.quality = Quality::UNCERTAIN,
                 .flags = flags({QualityFlag::TEST})}};
    const TagWrite write = point.value(2, SECOND, {&exponent, 1}).tag;
    EXPECT_EQ(write.quality, Quality::UNCERTAIN);
    EXPECT_TRUE(has(write.flags, QualityFlag::TEST));
    EXPECT_EQ(write.value.f, 20.0);
}

TEST(SampleProcessing, CompanionFlagsReachTheDependent) {
    PointRules rules = int_rules();
    rules.companions = {COMPANION_INDEX};
    rules.quality_rules.push_back(
        QualityRule{.companion = 0, .entries = {{.key = 0, .mark = {}}}});
    Point point(rules);
    const CompanionInput blocked{
        .outcome = Outcome::VALUE,
        .raw = raw_unsigned(0, INT16),
        .mark = {.quality = Quality::BAD,
                 .flags = flags({QualityFlag::BLOCKED})}};
    const TagWrite write = point.value(1, SECOND, {&blocked, 1}).tag;
    EXPECT_EQ(write.quality, Quality::BAD);
    EXPECT_TRUE(has(write.flags, QualityFlag::BLOCKED));
    const CompanionInput absent{.outcome = Outcome::ABSENT};
    EXPECT_EQ(point.value(1, SECOND, {&absent, 1}).tag.quality, Quality::BAD);
}

TEST(SampleProcessing, TimeCompanionOfTheWrongKindIsBad) {
    PointRules rules = int_rules();
    rules.companions = {COMPANION_INDEX};
    rules.time_companion = 0;
    Point point(rules);
    const CompanionInput number{.outcome = Outcome::VALUE,
                                .raw = raw_signed(1, INT16)};
    EXPECT_EQ(point.value(1, SECOND, {&number, 1}).tag.quality, Quality::BAD);
}

// --- Quality rules ---------------------------------------------------------

TEST(SampleProcessing, QualityRuleOnTheOwnWordWithAndWithoutDefault) {
    PointRules rules = int_rules();
    rules.quality_rules.push_back(
        QualityRule{.self = true,
                    .has_field = true,
                    .lo = TOP_BIT_16,
                    .hi = TOP_BIT_16,
                    .entries = {{.key = 0, .mark = {}}},
                    .otherwise = {.quality = Quality::UNCERTAIN}});
    Point with_default(rules);
    EXPECT_EQ(with_default.value(1, SECOND).tag.quality, Quality::GOOD);
    EXPECT_EQ(with_default.value(-1, SECOND).tag.quality, Quality::UNCERTAIN);
    rules.quality_rules[0].otherwise =
        QualityRule{}.otherwise;  // no default: BAD
    Point without_default(rules);
    EXPECT_EQ(without_default.value(-1, SECOND).tag.quality, Quality::BAD);
}

TEST(SampleProcessing, SixtyFourBitFieldWithTheTopBitSetMatchesNoKey) {
    PointRules rules;
    rules.tag_type = TagType::UINT64;
    rules.quality_rules.push_back(
        QualityRule{.self = true,
                    .has_field = true,
                    .lo = 0,
                    .hi = MAX_BIT_NUMBER,
                    .entries = {{.key = 0, .mark = {}}},
                    .otherwise = {.quality = Quality::UNCERTAIN}});
    Point point(rules);
    const TagWrite write =
        point
            .feed(RawSample{.outcome = Outcome::VALUE,
                            .raw = raw_unsigned(std::uint64_t{1} << 63U, 64)},
                  SECOND)
            .tag;
    EXPECT_EQ(write.quality, Quality::UNCERTAIN);
}

TEST(SampleProcessing, HealthRuleWithAField) {
    PointRules rules = int_rules();
    rules.companions = {COMPANION_INDEX};
    rules.health.push_back(HealthRule{.companion = 0,
                                      .has_field = true,
                                      .lo = FIELD_4,
                                      .hi = FIELD_5,
                                      .good = {2}});
    Point point(rules);
    const CompanionInput good{.outcome = Outcome::VALUE,
                              .raw = raw_unsigned(0b10'0000, INT16)};
    const CompanionInput fault{.outcome = Outcome::VALUE,
                               .raw = raw_unsigned(0b01'0000, INT16)};
    EXPECT_EQ(point.value(1, SECOND, {&good, 1}).tag.quality, Quality::GOOD);
    const TagWrite bad_write = point.value(1, SECOND, {&fault, 1}).tag;
    EXPECT_EQ(bad_write.quality, Quality::BAD);
    EXPECT_FALSE(has(bad_write.flags, QualityFlag::COMM_FAIL));
}

// --- Rules that validate_point_rules refuses give BAD, never UB --------------

TEST(SampleProcessing, OutOfRangeRuleFieldsGiveBad) {
    PointRules bits = int_rules();
    bits.bits = {BitRule{.bit = MAX_BIT_NUMBER + 1}};
    Point bit_point(bits);
    EXPECT_EQ(bit_point.value(1, SECOND).tag.quality, Quality::BAD);
    PointRules field = int_rules();
    field.quality_rules.push_back(
        QualityRule{.self = true,
                    .has_field = true,
                    .lo = FIELD_LOW,
                    .hi = BEYOND_64,
                    .entries = {},
                    .otherwise = {.quality = Quality::UNCERTAIN}});
    Point field_point(field);
    // Refused rules are BAD, not the map's `otherwise` (#169).
    EXPECT_EQ(field_point.value(1, SECOND).tag.quality, Quality::BAD);
    PointRules slot = int_rules();
    slot.exponent = ExponentRule{.companion = 3};
    slot.tag_type = TagType::FLOAT64;
    Point slot_point(slot);
    EXPECT_EQ(slot_point.value(1, SECOND).tag.quality, Quality::BAD);
}

TEST(SampleProcessing, RawValueWithAWidthOfZeroIsBad) {
    // A protocol that left the width 0 would make every marker miss.
    PointRules rules = int_rules();
    rules.markers.push_back(InvalidMarker{.pattern = UINT16_ALL_ONES});
    Point point(rules);
    RawValue raw = raw_unsigned(UINT16_ALL_ONES, INT16);
    raw.width = 0;
    EXPECT_EQ(
        point.feed(RawSample{.outcome = Outcome::VALUE, .raw = raw}, SECOND)
            .tag.quality,
        Quality::BAD);
}

// --- Integer scaling with divide_by 1 (decision D-133) -----------------------

TEST(SampleProcessing, DivideByOneOnAnIntegerTagIsExact) {
    PointRules rules = int_rules();
    rules.scale = ScaleRule{.form = ScaleRule::Form::DIVIDE_BY,
                            .integer = true,
                            .offset_i = OFFSET_MINUS_3,
                            .divide_by = 1,
                            .offset = static_cast<double>(OFFSET_MINUS_3)};
    Point point(rules);
    const TagWrite write = point.value(FIRST_VALUE, SECOND).tag;
    EXPECT_EQ(write.quality, Quality::GOOD);
    EXPECT_EQ(write.value.i, FIRST_VALUE + OFFSET_MINUS_3);
}

// A floating raw value on an integer tag: BAD, and refused by the rules.
void expect_floating_raw_refused(const PointRules& rules) {
    Point point(rules);
    const TagWrite write = point
                               .feed(RawSample{.outcome = Outcome::VALUE,
                                               .raw = raw_float32(4.0F)},
                                     SECOND)
                               .tag;
    EXPECT_EQ(write.quality, Quality::BAD);
    EXPECT_TRUE(has(write.flags, QualityFlag::OUT_OF_RANGE));
    EXPECT_TRUE(has(write.flags, QualityFlag::NOT_INIT));
    EXPECT_FALSE(validate_point_rules(rules).ok());
}

TEST(SampleProcessing, FloatingRawOnAnIntegerTagIsBadScaledOrNot) {
    // CF-504 (decision D-134): an INT64 or UINT64 value is never read from
    // a floating raw type; with `gain: 10` the raw 4.0 would otherwise be 4
    // where an int16 raw 4 gives 40.
    constexpr std::int64_t GAIN_10 = 10;
    for (const TagType type : {TagType::INT64, TagType::UINT64}) {
        PointRules rules;
        rules.tag_type = type;
        rules.raw_kind = RawKind::FLOAT;
        expect_floating_raw_refused(rules);
        rules.scale = ScaleRule{.form = ScaleRule::Form::GAIN,
                                .integer = true,
                                .gain_i = GAIN_10,
                                .gain = static_cast<double>(GAIN_10)};
        expect_floating_raw_refused(rules);
    }
}

TEST(SampleProcessing, AnUncertainCompanionDegradesAQualityRule) {
    // `quality_from` on a companion read UNCERTAIN: the rule's GOOD result
    // is at most UNCERTAIN, with the companion's flags (decision D-131).
    PointRules rules = int_rules();
    rules.companions = {COMPANION_INDEX};
    rules.quality_rules.push_back(
        QualityRule{.companion = 0, .entries = {{.key = 0, .mark = {}}}});
    Point point(rules);
    const CompanionInput uncertain{
        .outcome = Outcome::VALUE,
        .raw = raw_unsigned(0, INT16),
        .mark = {.quality = Quality::UNCERTAIN,
                 .flags = flags({QualityFlag::TEST})}};
    const TagWrite write = point.value(2, SECOND, {&uncertain, 1}).tag;
    EXPECT_EQ(write.quality, Quality::UNCERTAIN);
    EXPECT_TRUE(has(write.flags, QualityFlag::TEST));
    EXPECT_EQ(write.value.i, 2);
}

TEST(SampleProcessing, AnUncertainCompanionDegradesAHealthRule) {
    PointRules rules = int_rules();
    rules.companions = {COMPANION_INDEX};
    rules.health.push_back(
        HealthRule{.companion = 0, .good = {1}, .comm_fail = true});
    Point point(rules);
    const CompanionInput uncertain{
        .outcome = Outcome::VALUE,
        .raw = raw_unsigned(1, INT16),
        .mark = {.quality = Quality::UNCERTAIN,
                 .flags = flags({QualityFlag::BLOCKED})}};
    const TagWrite write = point.value(2, SECOND, {&uncertain, 1}).tag;
    EXPECT_EQ(write.quality, Quality::UNCERTAIN);
    EXPECT_TRUE(has(write.flags, QualityFlag::BLOCKED));
    // A health point that is UNCERTAIN and not good is still BAD.
    const CompanionInput down{
        .outcome = Outcome::VALUE,
        .raw = raw_unsigned(0, INT16),
        .mark = {.quality = Quality::UNCERTAIN, .flags = 0}};
    const TagWrite lost = point.value(2, SECOND, {&down, 1}).tag;
    EXPECT_EQ(lost.quality, Quality::BAD);
    EXPECT_TRUE(has(lost.flags, QualityFlag::COMM_FAIL));
}

// --- Bits --------------------------------------------------------------------

TEST(SampleProcessing, BitsKeepTheirValuesWhenTheWordMatchedAMarker) {
    PointRules rules = int_rules();
    rules.bits = {BitRule{.bit = 0}};
    rules.markers.push_back(InvalidMarker{.pattern = UINT16_ALL_ONES});
    Point point(rules);
    const ProcessedSample before =
        point.feed(RawSample{.outcome = Outcome::COMM_FAIL}, SECOND);
    EXPECT_FALSE(sample_bit(before, 0));
    EXPECT_TRUE(has(before.tag.flags, QualityFlag::NOT_INIT));
    EXPECT_TRUE(sample_bit(point.value(1, 2 * SECOND), 0));
    const ProcessedSample marker = point.value(-1, 3 * SECOND);  // 0xFFFF
    EXPECT_TRUE(sample_bit(marker, 0));  // kept from the last valid word
    EXPECT_EQ(marker.tag.quality, Quality::BAD);
}

TEST(SampleProcessing, StaleCheckCoversTheBits) {
    PointRules rules = int_rules();
    rules.max_age_ns = MAX_AGE_S * SECOND;
    rules.bits = {BitRule{.bit = 1}};
    Point point(rules);
    (void)point.value(2, SECOND);
    ProcessedSample out;
    ASSERT_TRUE(point.stale((MAX_AGE_S + 2) * SECOND, out));
    EXPECT_EQ(out.bit_count, 1U);
    EXPECT_TRUE(sample_bit(out, 0));
    EXPECT_TRUE(has(out.tag.flags, QualityFlag::STALE));
}

// --- Counters
// ------------------------------------------------------------------

PointRules counter_rules(CounterRule rule) {
    PointRules rules;
    rules.tag_type = TagType::UINT64;
    rules.counter = rule;
    return rules;
}

TagWrite counter_value(Point& point, std::uint64_t raw, unsigned width) {
    return point
        .feed(RawSample{.outcome = Outcome::VALUE,
                        .raw = raw_unsigned(raw, width)},
              SECOND)
        .tag;
}

TEST(SampleProcessing, ThirtyTwoBitCounterSteppingBackAcrossHalfIsAWrap) {
    // The contract's wrap rule (5.4): a decrease from at least limit/2 to
    // below it is a wrap, written GOOD with no flag, even with `hold`.
    constexpr std::uint64_t LIMIT = 0xFFFF'FFFFU;
    constexpr std::uint64_t HIGH = 3'000'000'000U;
    constexpr std::uint64_t LOW = 2'000'000'000U;
    constexpr unsigned INT32 = 32;
    Point point(counter_rules(CounterRule{
        .limit = LIMIT, .on_decrease = CounterRule::OnDecrease::HOLD}));
    (void)counter_value(point, HIGH, INT32);
    const TagWrite after = counter_value(point, LOW, INT32);
    EXPECT_EQ(after.value.u, LOW);
    EXPECT_EQ(after.quality, Quality::GOOD);
    EXPECT_EQ(after.flags, flags({QualityFlag::LOCAL_TIME}));
}

TEST(SampleProcessing, CounterWrapAtExactlyHalfTheLimit) {
    constexpr std::uint64_t LIMIT = UINT16_ALL_ONES;
    Point point(counter_rules(CounterRule{
        .limit = LIMIT, .on_decrease = CounterRule::OnDecrease::HOLD}));
    (void)counter_value(point, LIMIT / 2, INT16);  // 32767: at least half
    EXPECT_EQ(counter_value(point, 10, INT16).quality, Quality::GOOD);  // wrap
    (void)counter_value(point, LIMIT / 2 - 1, INT16);  // below half
    EXPECT_EQ(counter_value(point, 10, INT16).quality,
              Quality::UNCERTAIN);  // hold
}

TEST(SampleProcessing, CounterSaturateThenADecreaseFollowsOnDecrease) {
    constexpr std::uint64_t LIMIT = 9999;
    Point point(counter_rules(
        CounterRule{.limit = LIMIT,
                    .saturate = true,
                    .on_decrease = CounterRule::OnDecrease::HOLD}));
    EXPECT_TRUE(
        has(counter_value(point, LIMIT, INT16).flags, QualityFlag::OVERFLOW));
    const TagWrite down =
        counter_value(point, 3, INT16);  // no wrap under saturate
    EXPECT_EQ(down.value.u, LIMIT);
    EXPECT_EQ(down.quality, Quality::UNCERTAIN);
}

TEST(SampleProcessing, CounterHoldKeepsItsOwnValueAcrossASubstituteAndALoss) {
    PointRules rules = counter_rules(
        CounterRule{.limit = UINT16_ALL_ONES,
                    .on_decrease = CounterRule::OnDecrease::HOLD});
    rules.markers.push_back(InvalidMarker{.pattern = UINT16_ALL_ONES,
                                          .action = InvalidAction::SUBSTITUTE,
                                          .substitute = uint64_value(0)});
    Point point(rules);
    constexpr std::uint64_t FIRST = 1000;
    (void)counter_value(point, FIRST, INT16);
    EXPECT_EQ(counter_value(point, UINT16_ALL_ONES, INT16).value.u, 0U);
    (void)point.feed(RawSample{.outcome = Outcome::COMM_FAIL}, SECOND);
    const TagWrite held = counter_value(point, 900, INT16);
    EXPECT_EQ(held.value.u, FIRST);
    EXPECT_EQ(held.quality, Quality::UNCERTAIN);
}

TEST(SampleProcessing, CounterOnANegativeOrFloatingRawIsBad) {
    PointRules rules = counter_rules(CounterRule{.limit = UINT16_ALL_ONES});
    rules.tag_type = TagType::INT64;
    Point point(rules);
    EXPECT_EQ(point.value(-1, SECOND).tag.quality, Quality::BAD);
    PointRules floating = counter_rules(CounterRule{.limit = UINT16_ALL_ONES});
    floating.tag_type = TagType::FLOAT64;
    Point float_point(floating);
    EXPECT_EQ(float_point
                  .feed(RawSample{.outcome = Outcome::VALUE,
                                  .raw = raw_float32(1.0F)},
                        SECOND)
                  .tag.quality,
              Quality::BAD);
}

// --- Double points
// -------------------------------------------------------------

TEST(SampleProcessing, DoublePointFromAFloatRawUsesTheTolerance) {
    PointRules rules = int_rules();
    rules.double_point = DoublePointRule{
        .states = {{1, DoublePoint::OPEN}, {2, DoublePoint::CLOSED}}};
    Point point(rules);
    const TagWrite near_one = point
                                  .feed(RawSample{.outcome = Outcome::VALUE,
                                                  .raw = raw_float32(1.0004F)},
                                        SECOND)
                                  .tag;
    EXPECT_EQ(near_one.value.i, 1);
    EXPECT_EQ(near_one.quality, Quality::GOOD);
    // 3 (faulty) then a valid code: GOOD again.
    rules.double_point->states.push_back({3, DoublePoint::BAD_STATE});
    Point again(rules);
    EXPECT_EQ(again.value(3, SECOND).tag.quality, Quality::BAD);
    EXPECT_EQ(again.value(2, 2 * SECOND).tag.quality, Quality::GOOD);
}

TEST(SampleProcessing, DoublePointWithAQualityRule) {
    PointRules rules = int_rules();
    rules.double_point = DoublePointRule{.states = {{1, DoublePoint::OPEN}}};
    rules.companions = {COMPANION_INDEX};
    rules.quality_rules.push_back(
        QualityRule{.companion = 0, .entries = {{.key = 0, .mark = {}}}});
    Point point(rules);
    const CompanionInput fault{.outcome = Outcome::VALUE,
                               .raw = raw_unsigned(1, INT16)};
    const TagWrite write = point.value(1, SECOND, {&fault, 1}).tag;
    EXPECT_EQ(write.value.i, 1);
    EXPECT_EQ(write.quality, Quality::BAD);
}

// --- Substitute, strings, exponent edges
// -----------------------------------------

TEST(SampleProcessing, SubstituteBeforeAnyReadThenStale) {
    PointRules rules = int_rules();
    rules.max_age_ns = MAX_AGE_S * SECOND;
    rules.markers.push_back(
        InvalidMarker{.pattern = UINT16_ALL_ONES,
                      .action = InvalidAction::SUBSTITUTE,
                      .substitute = int64_value(KEPT_VALUE)});
    Point point(rules);
    const TagWrite first = point.value(-1, SECOND).tag;
    EXPECT_EQ(first.value.i, KEPT_VALUE);
    EXPECT_EQ(first.quality, Quality::UNCERTAIN);
    EXPECT_TRUE(has(first.flags, QualityFlag::SUBSTITUTED));
    EXPECT_FALSE(has(first.flags, QualityFlag::NOT_INIT));  // decision D-131
    ProcessedSample out;
    ASSERT_TRUE(point.stale((MAX_AGE_S + 1) * SECOND, out));  // never confirmed
    EXPECT_TRUE(has(out.tag.flags, QualityFlag::STALE));
    // STALE is added to the kept substitute; SUBSTITUTED stays with it.
    EXPECT_TRUE(has(out.tag.flags, QualityFlag::SUBSTITUTED));
}

TEST(SampleProcessing, StringEdges) {
    PointRules rules;
    rules.tag_type = TagType::STRING;
    rules.raw_kind = RawKind::STRING;
    Point point(rules);
    const std::string exact(TAG_STRING_MAX_BYTES, 'x');
    const TagWrite whole =
        point
            .feed(RawSample{.outcome = Outcome::VALUE,
                            .raw = raw_string(bytes_of(exact))},
                  SECOND)
            .tag;
    EXPECT_EQ(whole.value.text.size(), TAG_STRING_MAX_BYTES);
    EXPECT_FALSE(has(whole.flags, QualityFlag::OVERFLOW));
    // A 4-byte character that would end at byte 251.
    const std::string four = std::string(247, 'a') + "\xF0\x9F\x98\x80";
    const TagWrite cut = point
                             .feed(RawSample{.outcome = Outcome::VALUE,
                                             .raw = raw_string(bytes_of(four))},
                                   SECOND)
                             .tag;
    EXPECT_EQ(cut.value.text.size(), 247U);
    EXPECT_TRUE(has(cut.flags, QualityFlag::OVERFLOW));
    // A surrogate (U+D800 encoded) is not UTF-8.
    const std::string surrogate = "\xED\xA0\x80";
    EXPECT_EQ(point
                  .feed(RawSample{.outcome = Outcome::VALUE,
                                  .raw = raw_string(bytes_of(surrogate))},
                        SECOND)
                  .tag.quality,
              Quality::BAD);
}

TEST(SampleProcessing, ExponentAtItsLimits) {
    PointRules rules = float_point_rules();
    rules.companions = {COMPANION_INDEX};
    rules.exponent = ExponentRule{.companion = 0};
    Point point(rules);
    for (const std::int64_t exponent :
         {EXPONENT_MIN, std::int64_t{0}, EXPONENT_MAX}) {
        const CompanionInput power{.outcome = Outcome::VALUE,
                                   .raw = raw_signed(exponent, INT16)};
        const TagWrite write = point.value(3, SECOND, {&power, 1}).tag;
        EXPECT_EQ(write.quality, Quality::GOOD) << exponent;
    }
    const CompanionInput below{.outcome = Outcome::VALUE,
                               .raw = raw_signed(EXPONENT_MIN - 1, INT16)};
    EXPECT_EQ(point.value(3, SECOND, {&below, 1}).tag.quality, Quality::BAD);
}

// --- A time-valued point (CS 4.6; the lead's ruling)
// ----------------------------

TEST(SampleProcessing, TimeValueWithTrustUnknownIsTimeUncertain) {
    PointRules rules = int_rules();
    rules.device_time = DeviceTimeRule{};  // UTC, trust unknown
    Point point(rules);
    const TagWrite write =
        point
            .feed(RawSample{.outcome = Outcome::VALUE, .raw = raw_time(SECOND)},
                  SECOND)
            .tag;
    EXPECT_EQ(write.value.i, SECOND);
    EXPECT_TRUE(has(write.flags, QualityFlag::TIME_UNCERTAIN));
    rules.device_time.synced = true;
    Point synced(rules);
    EXPECT_FALSE(has(
        synced
            .feed(RawSample{.outcome = Outcome::VALUE, .raw = raw_time(SECOND)},
                  SECOND)
            .tag.flags,
        QualityFlag::TIME_UNCERTAIN));
}

TEST(SampleProcessing, TimeValueInTheRepeatedAndSkippedHour) {
    auto zone = ZoneTable::build("Europe/Berlin");
    ASSERT_TRUE(zone.ok());
    PointRules rules = int_rules();
    rules.device_time =
        DeviceTimeRule{.local = true, .zone = &zone.value(), .synced = true};
    Point point(rules);
    for (const std::int64_t local_s : {OCT25_0230_LOCAL, MAR29_0230_LOCAL}) {
        const TagWrite write =
            point
                .feed(RawSample{.outcome = Outcome::VALUE,
                                .raw = raw_time(local_s * SECOND)},
                      SECOND)
                .tag;
        EXPECT_TRUE(has(write.flags, QualityFlag::TIME_UNCERTAIN)) << local_s;
    }
    EXPECT_FALSE(
        has(point
                .feed(RawSample{.outcome = Outcome::VALUE,
                                .raw = raw_time(JUN1_1200_LOCAL * SECOND)},
                      SECOND)
                .tag.flags,
            QualityFlag::TIME_UNCERTAIN));
}

// Refused rules (#169): never a certain time, and never a value at all.
TEST(SampleProcessing, LocalTimeWithoutAZoneIsBad) {
    PointRules rules = int_rules();
    rules.device_time = DeviceTimeRule{.local = true, .synced = true};
    Point point(rules);
    const TagWrite write =
        point
            .feed(RawSample{.outcome = Outcome::VALUE, .raw = raw_time(SECOND)},
                  SECOND)
            .tag;
    EXPECT_EQ(write.quality, Quality::BAD);
    EXPECT_TRUE(has(write.flags, QualityFlag::OUT_OF_RANGE));
}

TEST(SampleProcessing, TimeRawOnAFloatTagIsBad) {
    Point point(float_point_rules());
    EXPECT_EQ(
        point
            .feed(RawSample{.outcome = Outcome::VALUE, .raw = raw_time(SECOND)},
                  SECOND)
            .tag.quality,
        Quality::BAD);
}

}  // namespace
}  // namespace scada::driver
