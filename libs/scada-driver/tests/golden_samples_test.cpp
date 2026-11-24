// Golden vectors of the processing after decode: a raw value and a point's
// rules give value bits, quality and flags, bit-identical on both
// architectures (driver interface contract 5.1, conformance test 5).
//
// The raw values are given as the pattern a protocol assembles (after byte
// order, word order and parts); the bytes-to-pattern half of the vectors
// belongs to each protocol and its simulator (#105, #106).

#include "sample_helpers.h"
#include "scada/driver/point_rules.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/sample_processing.h"
#include "scada/driver/tag_value.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <vector>

namespace scada::driver {
namespace {

using tag::Quality;
using tag::QualityFlag;
using tag::TagType;

constexpr std::int64_t RECEIVE_NS = 1'800'000'000'000'000'000;
constexpr std::int64_t MONO_NS = 5'000'000'000;

// Raw widths, patterns and the numbers of the vectors.
constexpr unsigned INT16 = 16;
constexpr std::uint64_t INT16_MAX_PATTERN = 0x7FFFU;
constexpr std::uint64_t UINT16_ALL_ONES = 0xFFFFU;
constexpr std::uint64_t SIGN_BIT_32 = 0x8000'0000U;  // -0.0f, INT32_MIN
constexpr std::uint64_t FLOAT32_INFINITY = 0x7F80'0000U;
constexpr std::uint32_t FLOAT32_NAN_PAYLOAD = 0x7FC0'1234U;
constexpr std::uint32_t COMPANION_INDEX = 7;
constexpr std::int64_t GAIN_3 = 3;
constexpr std::int64_t OFFSET_MINUS_7 = -7;
constexpr double GAIN_0_1 = 0.1;
constexpr double OFFSET_0_2 = 0.2;
constexpr std::uint64_t DIVIDE_BY_10 = 10;
constexpr double HUGE_GAIN = 1e300;
constexpr double RAW_RANGE_MAX = 4000.0;
constexpr std::int64_t DEVICE_OPEN = 10;
constexpr std::int64_t DEVICE_CLOSED = 20;
constexpr std::int64_t DEVICE_TRAVELLING = 30;
constexpr std::uint64_t UINT16_LIMIT = 65535;
constexpr std::uint64_t MOD10K_LIMIT = 9999;
constexpr std::uint64_t COUNTER_START = 1000;

std::uint16_t flags(std::initializer_list<QualityFlag> list) {
    std::uint16_t out = 0;
    for (const QualityFlag each : list) {
        out = static_cast<std::uint16_t>(out | tag::to_raw(each));
    }
    return out;
}

constexpr std::uint16_t LOCAL = tag::to_raw(QualityFlag::LOCAL_TIME);

// Processes one VALUE sample of a fresh point and returns the tag write.
TagWrite run(const PointRules& rules, const RawValue& raw,
             std::span<const CompanionInput> companions = {}) {
    PointState state = test::fresh_state(rules);
    const ProcessedSample out = test::processed(
        rules, state, RawSample{.outcome = Outcome::VALUE, .raw = raw},
        companions,
        SampleTimes{.receive_time_ns = RECEIVE_NS, .receive_mono_ns = MONO_NS});
    return out.tag;
}

PointRules float_rules() {
    PointRules rules;
    rules.tag_type = TagType::FLOAT64;
    return rules;
}

std::uint64_t bits_of(double value) {
    return std::bit_cast<std::uint64_t>(value);
}

// --- Step 1: invalid markers on the pattern -----------------------------

TEST(GoldenSamples, MarkerMatchesThePatternOfTheRawWidth) {
    PointRules rules;
    rules.tag_type = TagType::INT64;
    rules.markers.push_back(
        InvalidMarker{.pattern = INT16_MAX_PATTERN,
                      .flags = flags({QualityFlag::OUT_OF_RANGE})});
    const TagWrite write = run(rules, raw_signed(0x7FFF, 16));
    EXPECT_EQ(write.quality, Quality::BAD);
    EXPECT_EQ(write.flags,
              flags({QualityFlag::OUT_OF_RANGE, QualityFlag::NOT_INIT,
                     QualityFlag::LOCAL_TIME}));
    EXPECT_EQ(write.value.i, 0);  // never had a value
}

TEST(GoldenSamples, NegativeMarkerIsTwosComplementOfTheWidth) {
    // `-2147483648` on an int32 is the pattern 0x80000000 (CS 2.3).
    PointRules rules;
    rules.tag_type = TagType::INT64;
    rules.markers.push_back(InvalidMarker{.pattern = SIGN_BIT_32});
    EXPECT_EQ(
        run(rules, raw_signed(std::numeric_limits<std::int32_t>::min(), 32))
            .quality,
        Quality::BAD);
    EXPECT_EQ(run(rules, raw_signed(-2147483647, 32)).quality, Quality::GOOD);
}

TEST(GoldenSamples, Float32MarkerAfterLittleWordOrder) {
    // A float32 sent as two registers in little word order: the protocol
    // assembles the pattern 0x7F800000 (+infinity), the marker's pattern.
    PointRules rules = float_rules();
    rules.markers.push_back(InvalidMarker{.pattern = FLOAT32_INFINITY});
    const TagWrite write =
        run(rules, raw_float32(std::numeric_limits<float>::infinity()));
    EXPECT_EQ(write.quality, Quality::BAD);
    EXPECT_EQ(write.flags,
              flags({QualityFlag::NOT_INIT, QualityFlag::LOCAL_TIME}));
}

TEST(GoldenSamples, NegativeZeroMarkerIsNotPositiveZero) {
    PointRules rules = float_rules();
    rules.markers.push_back(InvalidMarker{.pattern = SIGN_BIT_32});
    EXPECT_EQ(run(rules, raw_float32(-0.0F)).quality, Quality::BAD);
    EXPECT_EQ(run(rules, raw_float32(0.0F)).quality, Quality::GOOD);
}

TEST(GoldenSamples, NanMarkerMatchesAnyNan) {
    PointRules rules = float_rules();
    rules.markers.push_back(InvalidMarker{.any_nan = true});
    RawValue raw = raw_float32(std::numeric_limits<float>::quiet_NaN());
    raw.pattern = FLOAT32_NAN_PAYLOAD;  // a NaN with a payload
    raw.number_f =
        static_cast<double>(std::bit_cast<float>(FLOAT32_NAN_PAYLOAD));
    EXPECT_EQ(run(rules, raw).quality, Quality::BAD);
}

// --- Step 2: non-finite without a marker ---------------------------------

TEST(GoldenSamples, NonFiniteWithoutMarkerIsBadOutOfRange) {
    const PointRules rules = float_rules();
    for (const float value : {std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::quiet_NaN()}) {
        const TagWrite write = run(rules, raw_float32(value));
        EXPECT_EQ(write.quality, Quality::BAD);
        EXPECT_EQ(write.flags,
                  flags({QualityFlag::OUT_OF_RANGE, QualityFlag::NOT_INIT,
                         QualityFlag::LOCAL_TIME}));
    }
}

// --- Step 3: exponent ------------------------------------------------------

TEST(GoldenSamples, NegativeExponentDividesByAnExactPower) {
    PointRules rules = float_rules();
    rules.companions = {COMPANION_INDEX};
    rules.exponent = ExponentRule{.companion = 0};
    const CompanionInput exponent{.outcome = Outcome::VALUE,
                                  .raw = raw_signed(-3, 16)};
    // 12345 / 1000, not 12345 * 0.001.
    const TagWrite write = run(rules, raw_signed(12345, 16), {&exponent, 1});
    EXPECT_EQ(write.quality, Quality::GOOD);
    EXPECT_EQ(bits_of(write.value.f), bits_of(12345.0 / 1000.0));
    EXPECT_EQ(bits_of(write.value.f), 0x4028B0A3D70A3D71U);
}

TEST(GoldenSamples, ExponentWithDivideDirection) {
    PointRules rules = float_rules();
    rules.companions = {COMPANION_INDEX};
    rules.exponent = ExponentRule{.companion = 0, .divide = true};
    const CompanionInput exponent{.outcome = Outcome::VALUE,
                                  .raw = raw_signed(2, 16)};
    EXPECT_EQ(bits_of(run(rules, raw_signed(150, 16), {&exponent, 1}).value.f),
              bits_of(1.5));
}

TEST(GoldenSamples, ExponentOutOfRangeIsBad) {
    PointRules rules = float_rules();
    rules.companions = {COMPANION_INDEX};
    rules.exponent = ExponentRule{.companion = 0};
    const CompanionInput exponent{.outcome = Outcome::VALUE,
                                  .raw = raw_signed(21, 16)};
    const TagWrite write = run(rules, raw_signed(1, 16), {&exponent, 1});
    EXPECT_EQ(write.quality, Quality::BAD);
    EXPECT_NE(write.flags & tag::to_raw(QualityFlag::OUT_OF_RANGE), 0);
}

TEST(GoldenSamples, ExponentCompanionFailedIsBadWithItsFlags) {
    PointRules rules = float_rules();
    rules.companions = {COMPANION_INDEX};
    rules.exponent = ExponentRule{.companion = 0};
    const CompanionInput exponent{.outcome = Outcome::COMM_FAIL};
    const TagWrite write = run(rules, raw_signed(1, 16), {&exponent, 1});
    EXPECT_EQ(write.quality, Quality::BAD);
    EXPECT_NE(write.flags & tag::to_raw(QualityFlag::COMM_FAIL), 0);
}

// --- Step 4: scale ---------------------------------------------------------

TEST(GoldenSamples, IntegerScaleIsExact) {
    PointRules rules;
    rules.tag_type = TagType::INT64;
    rules.scale = ScaleRule{
        .integer = true, .gain_i = GAIN_3, .offset_i = OFFSET_MINUS_7};
    EXPECT_EQ(run(rules, raw_signed(-1000, 16)).value.i, -3007);
}

TEST(GoldenSamples, IntegerScaleAboveTwoToThe53IsExact) {
    PointRules rules;
    rules.tag_type = TagType::UINT64;
    rules.scale = ScaleRule{.integer = true, .gain_i = 1, .offset_i = 1};
    const std::uint64_t raw = (std::uint64_t{1} << 53U) + 1U;
    EXPECT_EQ(run(rules, raw_unsigned(raw, 64)).value.u, raw + 1U);
}

TEST(GoldenSamples, IntegerScaleOverflowIsBadOverflow) {
    // An offset at the edge: raw x gain fits, adding the offset does not.
    PointRules rules;
    rules.tag_type = TagType::INT64;
    rules.scale =
        ScaleRule{.integer = true,
                  .gain_i = 1,
                  .offset_i = std::numeric_limits<std::int64_t>::max()};
    const TagWrite write = run(rules, raw_signed(1, 16));
    EXPECT_EQ(write.quality, Quality::BAD);
    EXPECT_NE(write.flags & tag::to_raw(QualityFlag::OVERFLOW), 0);
}

TEST(GoldenSamples, DecimalGainIsTwoOperations) {
    // raw x 0.1 + 0.2, rounded after each step: 0.30000000000000004 for raw
    // 1 (a fused multiply-add would give 0.3).
    PointRules rules = float_rules();
    rules.scale = ScaleRule{.gain = GAIN_0_1, .offset = OFFSET_0_2};
    EXPECT_EQ(bits_of(run(rules, raw_signed(1, 16)).value.f),
              0x3FD3333333333334U);
    EXPECT_EQ(bits_of(run(rules, raw_signed(1234, 16)).value.f),
              bits_of((1234.0 * 0.1) + 0.2));
}

TEST(GoldenSamples, DivideByIsTheExactDecimalForm) {
    PointRules rules = float_rules();
    rules.scale = ScaleRule{.form = ScaleRule::Form::DIVIDE_BY,
                            .divide_by = DIVIDE_BY_10};
    EXPECT_EQ(bits_of(run(rules, raw_signed(125, 16)).value.f), bits_of(12.5));
    EXPECT_EQ(bits_of(run(rules, raw_signed(-1, 16)).value.f), bits_of(-0.1));
}

TEST(GoldenSamples, IntegerTagWithANonIntegerScalingIsBad) {
    // CF-517 refuses these (decision D-133); the framework never computes
    // a value for them.
    PointRules divide;
    divide.tag_type = TagType::INT64;
    divide.scale = ScaleRule{.form = ScaleRule::Form::DIVIDE_BY,
                             .integer = true,
                             .divide_by = DIVIDE_BY_10};
    EXPECT_EQ(run(divide, raw_signed(DIVIDE_BY_10, INT16)).quality,
              Quality::BAD);
    PointRules exponent;
    exponent.tag_type = TagType::INT64;
    exponent.companions = {COMPANION_INDEX};
    exponent.exponent = ExponentRule{.companion = 0};
    const CompanionInput power{.outcome = Outcome::VALUE,
                               .raw = raw_signed(1, INT16)};
    EXPECT_EQ(run(exponent, raw_signed(1, INT16), {&power, 1}).quality,
              Quality::BAD);
}

TEST(GoldenSamples, ScaleThatOverflowsBinary64IsBadOverflow) {
    PointRules rules = float_rules();
    rules.scale = ScaleRule{.gain = HUGE_GAIN};
    const TagWrite write =
        run(rules, raw_float64(std::numeric_limits<double>::max()));
    EXPECT_EQ(write.quality, Quality::BAD);
    EXPECT_NE(write.flags & tag::to_raw(QualityFlag::OVERFLOW), 0);
}

// --- raw_range -------------------------------------------------------------

TEST(GoldenSamples, RawRangeGivesUncertainOutOfRange) {
    PointRules rules;
    rules.tag_type = TagType::INT64;
    rules.raw_range = RawRange{.min = 0.0, .max = RAW_RANGE_MAX};
    const TagWrite inside = run(rules, raw_unsigned(4000, 16));
    EXPECT_EQ(inside.quality, Quality::GOOD);
    const TagWrite outside = run(rules, raw_unsigned(4001, 16));
    EXPECT_EQ(outside.quality, Quality::UNCERTAIN);
    EXPECT_EQ(outside.flags,
              flags({QualityFlag::OUT_OF_RANGE, QualityFlag::LOCAL_TIME}));
    EXPECT_EQ(outside.value.i, 4001);
}

// --- on_invalid actions ----------------------------------------------------

TEST(GoldenSamples, SubstituteIsUncertainWithoutSubstitutedUntilTagDatabase11) {
    PointRules rules = float_rules();
    rules.markers.push_back(InvalidMarker{.pattern = UINT16_ALL_ONES,
                                          .action = InvalidAction::SUBSTITUTE,
                                          .substitute = float64_value(-1.0)});
    const TagWrite write = run(rules, raw_unsigned(0xFFFF, 16));
    EXPECT_EQ(write.quality, Quality::UNCERTAIN);
    EXPECT_EQ(bits_of(write.value.f), bits_of(-1.0));
    EXPECT_EQ(write.flags, LOCAL);
}

// --- Enumerations ----------------------------------------------------------

TEST(GoldenSamples, EnumerationUnknownCodeIsUncertainOutOfRange) {
    PointRules rules;
    rules.tag_type = TagType::INT64;
    rules.has_enum = true;
    rules.enum_codes = {-1, 0, 1, 3};
    EXPECT_EQ(run(rules, raw_signed(3, 16)).quality, Quality::GOOD);
    const TagWrite unknown = run(rules, raw_signed(2, 16));
    EXPECT_EQ(unknown.quality, Quality::UNCERTAIN);
    EXPECT_EQ(unknown.value.i, 2);
    EXPECT_NE(unknown.flags & tag::to_raw(QualityFlag::OUT_OF_RANGE), 0);
}

TEST(GoldenSamples, EnumerationFromFloatRoundsWithinATolerance) {
    PointRules rules;
    rules.tag_type = TagType::INT64;
    rules.has_enum = true;
    rules.enum_codes = {0, 1, 3};
    EXPECT_EQ(run(rules, raw_float32(2.9995F)).value.i, 3);
    EXPECT_EQ(run(rules, raw_float32(2.9F)).quality, Quality::BAD);
}

// --- Double points ---------------------------------------------------------

PointRules two_bit_rules() {
    PointRules rules;
    rules.tag_type = TagType::INT64;
    rules.double_point =
        DoublePointRule{.states = {{0, DoublePoint::INTERMEDIATE},
                                   {1, DoublePoint::OPEN},
                                   {2, DoublePoint::CLOSED},
                                   {3, DoublePoint::BAD_STATE}}};
    return rules;
}

TEST(GoldenSamples, DoublePointCodesZeroToThree) {
    const PointRules rules = two_bit_rules();
    for (std::uint64_t code = 0; code <= 2; ++code) {
        const TagWrite write = run(rules, raw_unsigned(code, 2));
        EXPECT_EQ(write.value.i, static_cast<std::int64_t>(code));
        EXPECT_EQ(write.quality, Quality::GOOD) << code;
    }
    const TagWrite faulty = run(rules, raw_unsigned(3, 2));
    EXPECT_EQ(faulty.value.i, 3);
    EXPECT_EQ(faulty.quality, Quality::BAD);
    EXPECT_EQ(faulty.flags, LOCAL);
}

TEST(GoldenSamples, DoublePointStatesMap) {
    // An integer shape: the device codes 10 open, 20 closed, 30 travelling.
    PointRules rules;
    rules.tag_type = TagType::INT64;
    rules.double_point = DoublePointRule{
        .states = {{DEVICE_OPEN, DoublePoint::OPEN},
                   {DEVICE_CLOSED, DoublePoint::CLOSED},
                   {DEVICE_TRAVELLING, DoublePoint::INTERMEDIATE}}};
    EXPECT_EQ(run(rules, raw_unsigned(20, 16)).value.i, 2);
    EXPECT_EQ(run(rules, raw_unsigned(30, 16)).value.i, 0);
    const TagWrite unmapped = run(rules, raw_unsigned(11, 16));
    EXPECT_EQ(unmapped.value.i, 3);
    EXPECT_EQ(unmapped.quality, Quality::BAD);
}

// --- Counters --------------------------------------------------------------

class CounterRun {
public:
    explicit CounterRun(CounterRule rule) {
        rules_.tag_type = TagType::UINT64;
        rules_.counter = rule;
        reset_point_state(state_, rules_.tag_type, 0);
    }

    TagWrite next(std::uint64_t raw) {
        return test::processed(rules_, state_,
                               RawSample{.outcome = Outcome::VALUE,
                                         .raw = raw_unsigned(raw, INT16)},
                               {}, SampleTimes{})
            .tag;
    }

    ProcessedSample next_sample(std::uint64_t raw) {
        return test::processed(rules_, state_,
                               RawSample{.outcome = Outcome::VALUE,
                                         .raw = raw_unsigned(raw, INT16)},
                               {}, SampleTimes{});
    }

private:
    PointRules rules_;
    PointState state_;
};

TEST(GoldenSamples, CounterWrapIsWrittenWithoutFlag) {
    CounterRun counter(CounterRule{.limit = UINT16_LIMIT});
    EXPECT_EQ(counter.next(65000).quality, Quality::GOOD);
    const TagWrite wrapped = counter.next(10);
    EXPECT_EQ(wrapped.value.u, 10U);
    EXPECT_EQ(wrapped.quality, Quality::GOOD);
    EXPECT_EQ(wrapped.flags, LOCAL);
}

TEST(GoldenSamples, CounterSaturateSetsOverflowAtTheLimit) {
    CounterRun counter(CounterRule{.limit = MOD10K_LIMIT, .saturate = true});
    EXPECT_EQ(counter.next(9998).flags, LOCAL);
    const TagWrite at_limit = counter.next(9999);
    EXPECT_EQ(at_limit.value.u, 9999U);
    EXPECT_NE(at_limit.flags & tag::to_raw(QualityFlag::OVERFLOW), 0);
}

TEST(GoldenSamples, CounterDecreaseAccept) {
    CounterRun counter(CounterRule{.limit = UINT16_LIMIT});
    (void)counter.next(COUNTER_START);
    const TagWrite lower = counter.next(900);
    EXPECT_EQ(lower.value.u, 900U);
    EXPECT_EQ(lower.quality, Quality::GOOD);
}

TEST(GoldenSamples, CounterDecreaseHoldKeepsThePreviousUntilPassed) {
    CounterRun counter(CounterRule{
        .limit = UINT16_LIMIT, .on_decrease = CounterRule::OnDecrease::HOLD});
    (void)counter.next(COUNTER_START);
    const TagWrite held = counter.next(900);
    EXPECT_EQ(held.value.u, 1000U);
    EXPECT_EQ(held.quality, Quality::UNCERTAIN);
    EXPECT_EQ(counter.next(999).value.u, 1000U);
    const TagWrite passed = counter.next(1001);
    EXPECT_EQ(passed.value.u, 1001U);
    EXPECT_EQ(passed.quality, Quality::GOOD);
}

TEST(GoldenSamples, CounterDecreaseResetWritesAndCounts) {
    CounterRun counter(CounterRule{
        .limit = UINT16_LIMIT, .on_decrease = CounterRule::OnDecrease::RESET});
    (void)counter.next(COUNTER_START);
    const ProcessedSample out = counter.next_sample(5);
    EXPECT_EQ(out.tag.value.u, 5U);
    EXPECT_TRUE(out.counts.counter_reset);
}

}  // namespace
}  // namespace scada::driver
