// Golden vectors of the write conversion: an engineering value and a
// point's rules give the raw value, or a refusal (driver interface contract
// 8.4, conformance test 5; configuration schemas contract 7.2).

#include "sample_helpers.h"
#include "scada/driver/point_rules.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/sample_processing.h"
#include "scada/driver/tag_value.h"
#include "scada/driver/write_conversion.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>

namespace scada::driver {
namespace {

using tag::TagType;

// The raw value of a conversion that must succeed; a refusal fails the
// test and gives a zero value.
// The raw value of a pattern that must be accepted.
RawValue pattern_value(RawKind kind, std::uint8_t width,
                       std::uint64_t pattern) {
    const std::optional<RawValue> raw = raw_from_pattern(kind, width, pattern);
    EXPECT_TRUE(raw.has_value());
    return raw.value_or(RawValue{});
}

WriteValue converted_value(const WriteRules& rules, const TagValue& value) {
    const std::optional<WriteValue> result = to_raw_value(rules, value);
    EXPECT_TRUE(result.has_value());
    return result.value_or(WriteValue{});
}

RawValue converted(const WriteRules& rules, const TagValue& value) {
    const std::optional<WriteValue> raw = to_raw_value(rules, value);
    EXPECT_TRUE(raw.has_value());
    return raw.has_value() ? raw->raw : RawValue{};
}

constexpr std::uint8_t INT16 = 16;
constexpr std::uint8_t FLOAT32_BITS = 32;
constexpr std::uint8_t FLOAT64_BITS = 64;
constexpr std::uint64_t DIVIDE_BY_10 = 10;
constexpr double GAIN_0_1 = 0.1;
constexpr std::uint64_t SIGN_BIT_32 = 0x8000'0000U;
constexpr std::uint32_t FLOAT32_NAN_PAYLOAD = 0x7FC0'1234U;
constexpr std::uint64_t SEED = 128;
constexpr int CASES = 200'000;
constexpr std::int64_t MAX_GAIN = 1000;
constexpr std::int64_t MAX_VALUE = 1'000'000'000;
constexpr std::int64_t INT16_MIN_VALUE = -32768;
constexpr std::int64_t INT16_MAX_VALUE = 32767;
constexpr std::uint64_t DIVIDE_BY_1000 = 1000;

WriteRules setpoint(TagType type, RawKind kind, std::uint8_t width,
                    const ScaleRule* scale) {
    return WriteRules{
        .tag_type = type, .raw_kind = kind, .raw_width = width, .scale = scale};
}

TEST(GoldenWrite, DecimalGainOnInt16) {
    // 12.5 kW with `gain: 0.1` on an int16 is raw 125.
    const ScaleRule scale{.gain = 0.1};
    const RawValue raw =
        converted(setpoint(TagType::FLOAT64, RawKind::SIGNED, 16, &scale),
                  float64_value(12.5));
    EXPECT_EQ(raw.number_i, 125);
    EXPECT_EQ(raw.pattern, 125U);
}

TEST(GoldenWrite, DivideByIsExact) {
    // 75.0 % with `divide_by: 10` is raw 750.
    const ScaleRule scale{.form = ScaleRule::Form::DIVIDE_BY, .divide_by = 10};
    const RawValue raw =
        converted(setpoint(TagType::FLOAT64, RawKind::UNSIGNED, 16, &scale),
                  float64_value(75.0));
    EXPECT_EQ(raw.number_u, 750U);
}

TEST(GoldenWrite, RoundsHalfAwayFromZero) {
    // CS 7.2: 12.5 kW on `divide_by: 10` is 125; -0.05 on `divide_by: 10` is
    // -0.5, which rounds to -1.
    const ScaleRule scale{.form = ScaleRule::Form::DIVIDE_BY, .divide_by = 10};
    const WriteRules rules =
        setpoint(TagType::FLOAT64, RawKind::SIGNED, 16, &scale);
    EXPECT_EQ(converted(rules, float64_value(12.5)).number_i, 125);
    EXPECT_EQ(converted(rules, float64_value(0.25)).number_i, 3);
    EXPECT_EQ(converted(rules, float64_value(-0.05)).number_i, -1);
}

TEST(GoldenWrite, ValueThatRoundsOutsideInt16IsRefused) {
    const ScaleRule scale{.gain = 0.1};
    const WriteRules rules =
        setpoint(TagType::FLOAT64, RawKind::SIGNED, 16, &scale);
    // 3276.76 / 0.1 rounds to 32768, one above the int16 maximum.
    EXPECT_FALSE(to_raw_value(rules, float64_value(3276.76)).has_value());
    EXPECT_EQ(converted(rules, float64_value(3276.7)).number_i, 32767);
    EXPECT_EQ(converted(rules, float64_value(-3276.8)).number_i, -32768);
    EXPECT_FALSE(to_raw_value(rules, float64_value(-3276.86)).has_value());
}

TEST(GoldenWrite, TwoToThe53PlusOneOnUint64IsExact) {
    // 2^53 + 1 on a UINT64 tag with gain 1 is written exactly: the integer
    // path never goes through binary64.
    const ScaleRule scale{.integer = true, .gain_i = 1};
    const WriteRules rules =
        setpoint(TagType::UINT64, RawKind::UNSIGNED, 64, &scale);
    const std::uint64_t value = (std::uint64_t{1} << 53U) + 1U;
    EXPECT_TRUE(is_integer_path(rules));
    const RawValue raw = converted(rules, uint64_value(value));
    EXPECT_EQ(raw.number_u, value);
}

TEST(GoldenWrite, IntegerPathNeedsAnExactQuotient) {
    const ScaleRule scale{.integer = true, .gain_i = 10, .offset_i = 5};
    const WriteRules rules =
        setpoint(TagType::INT64, RawKind::SIGNED, 32, &scale);
    EXPECT_EQ(converted(rules, int64_value(125)).number_i, 12);
    EXPECT_FALSE(to_raw_value(rules, int64_value(126)).has_value());
}

TEST(GoldenWrite, IntegerTagWithANonIntegerScalingIsRefused) {
    // CF-517 (decision D-133): `divide_by` and a decimal gain on an INT64
    // tag never reach the device, whatever the value.
    const ScaleRule divide{.form = ScaleRule::Form::DIVIDE_BY,
                           .integer = true,
                           .divide_by = DIVIDE_BY_1000};
    const ScaleRule decimal{.gain = 0.5};
    EXPECT_FALSE(
        to_raw_value(setpoint(TagType::INT64, RawKind::SIGNED, INT16, &divide),
                     int64_value(2))
            .has_value());
    EXPECT_FALSE(
        to_raw_value(setpoint(TagType::INT64, RawKind::SIGNED, INT16, &decimal),
                     int64_value(2))
            .has_value());
}

TEST(GoldenWrite, NegativeOffsetOnUint64AboveInt64Max) {
    // CS 7.2: a UINT64 target with a negative offset is not refused for a
    // difference above 2^63 - 1.
    const ScaleRule scale{.integer = true, .gain_i = 1, .offset_i = -1};
    const WriteRules rules =
        setpoint(TagType::UINT64, RawKind::UNSIGNED, 64, &scale);
    const std::uint64_t value = std::uint64_t{1} << 63U;
    EXPECT_EQ(converted(rules, uint64_value(value)).number_u, value + 1U);
    EXPECT_FALSE(
        to_raw_value(rules,
                     uint64_value(std::numeric_limits<std::uint64_t>::max()))
            .has_value());
}

TEST(GoldenWrite, NonFiniteIsRefused) {
    const WriteRules rules =
        setpoint(TagType::FLOAT64, RawKind::FLOAT, 32, nullptr);
    EXPECT_FALSE(
        to_raw_value(rules,
                     float64_value(std::numeric_limits<double>::quiet_NaN()))
            .has_value());
    EXPECT_FALSE(
        to_raw_value(rules,
                     float64_value(std::numeric_limits<double>::infinity()))
            .has_value());
}

TEST(GoldenWrite, Float32RoundsToNearestAndRefusesOverflow) {
    const WriteRules rules =
        setpoint(TagType::FLOAT64, RawKind::FLOAT, 32, nullptr);
    const RawValue raw = converted(rules, float64_value(0.1));
    EXPECT_EQ(raw.pattern, std::bit_cast<std::uint32_t>(0.1F));
    EXPECT_FALSE(to_raw_value(rules, float64_value(1e39)).has_value());
}

TEST(GoldenWrite, WrongTypeIsRefused) {
    const WriteRules rules =
        setpoint(TagType::FLOAT64, RawKind::SIGNED, 16, nullptr);
    EXPECT_FALSE(to_raw_value(rules, int64_value(1)).has_value());
}

TEST(GoldenWrite, BoolWithInvert) {
    WriteRules rules = setpoint(TagType::BOOL, RawKind::BOOL, 1, nullptr);
    EXPECT_EQ(converted(rules, bool_value(true)).number_u, 1U);
    rules.invert = true;
    EXPECT_EQ(converted(rules, bool_value(true)).number_u, 0U);
}

TEST(GoldenWrite, EnumerationWritesADefinedCode) {
    const std::array<std::int64_t, 3> codes{0, 1, 3};
    WriteRules rules =
        setpoint(TagType::INT64, RawKind::UNSIGNED, INT16, nullptr);
    rules.has_enum = true;
    rules.enum_codes = codes;
    EXPECT_EQ(converted(rules, int64_value(3)).number_u, 3U);
    EXPECT_FALSE(to_raw_value(rules, int64_value(2)).has_value());
}

// --- A floating raw type on an integer value type (CF-504, D-134) -------

// The tag the raw value of a write reads back as, through the processing.
TagWrite read_back(const PointRules& rules, const RawValue& raw) {
    PointState state = test::fresh_state(rules);
    return test::processed(rules, state,
                           RawSample{.outcome = Outcome::VALUE, .raw = raw}, {},
                           SampleTimes{})
        .tag;
}

TEST(GoldenWrite, FloatingRawTypeOnAnIntegerValueTypeIsRefused) {
    constexpr std::int64_t GAIN_10 = 10;
    const ScaleRule gain{.integer = true,
                         .gain_i = GAIN_10,
                         .gain = static_cast<double>(GAIN_10)};
    for (const ScaleRule* scale :
         {static_cast<const ScaleRule*>(nullptr), &gain}) {
        EXPECT_FALSE(to_raw_value(setpoint(TagType::INT64, RawKind::FLOAT,
                                           FLOAT32_BITS, scale),
                                  int64_value(40))
                         .has_value());
        EXPECT_FALSE(to_raw_value(setpoint(TagType::UINT64, RawKind::FLOAT,
                                           FLOAT64_BITS, scale),
                                  uint64_value(40))
                         .has_value());
    }
}

TEST(GoldenWrite, IntegerScalingReadsBackWhatWasWritten) {
    // `gain: 10` on an int16: a write of 40 is raw 4, which reads 40.
    constexpr std::int64_t GAIN_10 = 10;
    const ScaleRule scale{.integer = true,
                          .gain_i = GAIN_10,
                          .gain = static_cast<double>(GAIN_10)};
    const WriteValue written = converted_value(
        setpoint(TagType::INT64, RawKind::SIGNED, INT16, &scale),
        int64_value(40));
    EXPECT_EQ(written.raw.number_i, 4);
    PointRules rules;
    rules.tag_type = TagType::INT64;
    rules.raw_kind = RawKind::SIGNED;
    rules.scale = scale;
    const TagWrite read = read_back(rules, written.raw);
    EXPECT_EQ(read.quality, tag::Quality::GOOD);
    EXPECT_EQ(read.value.i, written.written.i);
}

TEST(GoldenWrite, EnumerationOnAFloatingRawTypeReadsBackItsCode) {
    // An enumeration writes its code to a float32 register; the read takes
    // the code back (CS 5.5).
    const std::array<std::int64_t, 3> codes{0, 1, 3};
    WriteRules rules =
        setpoint(TagType::INT64, RawKind::FLOAT, FLOAT32_BITS, nullptr);
    rules.has_enum = true;
    rules.enum_codes = codes;
    const WriteValue written = converted_value(rules, int64_value(3));
    EXPECT_EQ(
        std::bit_cast<std::uint32_t>(static_cast<float>(written.raw.number_f)),
        std::bit_cast<std::uint32_t>(3.0F));
    PointRules point;
    point.tag_type = TagType::INT64;
    point.raw_kind = RawKind::FLOAT;
    point.has_enum = true;
    point.enum_codes = {0, 1, 3};
    const TagWrite read = read_back(point, written.raw);
    EXPECT_EQ(read.quality, tag::Quality::GOOD);
    EXPECT_EQ(read.value.i, 3);
}

TEST(GoldenWrite, StringIsNotWritable) {
    const WriteRules rules =
        setpoint(TagType::STRING, RawKind::STRING, 0, nullptr);
    EXPECT_FALSE(to_raw_value(rules, string_value("x")).has_value());
}

// --- The value actually written (8.4: rounded to the raw step) --------

TEST(GoldenWrite, WrittenIsTheValueTheRawReadsBackAs) {
    const ScaleRule scale{.form = ScaleRule::Form::DIVIDE_BY,
                          .divide_by = DIVIDE_BY_10};
    const WriteValue result = converted_value(
        setpoint(TagType::FLOAT64, RawKind::SIGNED, INT16, &scale),
        float64_value(12.34));
    EXPECT_EQ(result.raw.number_i, 123);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(result.written.f),
              std::bit_cast<std::uint64_t>(123.0 / 10.0));
    // 12.45 on a gain of 0.1 gives 124, as 8.4 warns.
    const ScaleRule gain{.gain = GAIN_0_1};
    EXPECT_EQ(
        converted(setpoint(TagType::FLOAT64, RawKind::SIGNED, INT16, &gain),
                  float64_value(12.45))
            .number_i,
        124);
}

TEST(GoldenWrite, IntegerPathWritesTheRequestItself) {
    const ScaleRule scale{.integer = true, .gain_i = 10, .gain = 10.0};
    const WriteValue result = converted_value(
        setpoint(TagType::INT64, RawKind::SIGNED, INT16, &scale),
        int64_value(150));
    EXPECT_EQ(result.raw.number_i, 15);
    EXPECT_EQ(result.written.i, 150);
}

TEST(GoldenWrite, DivideByOneOnAnIntegerTagIsExact) {
    const ScaleRule scale{.form = ScaleRule::Form::DIVIDE_BY,
                          .integer = true,
                          .offset_i = 5,
                          .divide_by = 1,
                          .offset = 5.0};
    const WriteRules rules =
        setpoint(TagType::INT64, RawKind::SIGNED, INT16, &scale);
    EXPECT_TRUE(is_integer_path(rules));
    EXPECT_EQ(converted(rules, int64_value(12)).number_i, 7);
}

TEST(GoldenWrite, NegativeGainZeroGainAndOffsetOverflow) {
    const ScaleRule negative{.gain = -0.1};
    EXPECT_EQ(
        converted(setpoint(TagType::FLOAT64, RawKind::SIGNED, INT16, &negative),
                  float64_value(1.5))
            .number_i,
        -15);
    const ScaleRule zero{.gain = 0.0};
    EXPECT_FALSE(
        to_raw_value(setpoint(TagType::FLOAT64, RawKind::SIGNED, INT16, &zero),
                     float64_value(1.0))
            .has_value());
    // (value - offset) overflows 65 bits: refused on the integer path.
    const ScaleRule edge{.integer = true,
                         .gain_i = 1,
                         .offset_i = std::numeric_limits<std::int64_t>::min()};
    EXPECT_FALSE(
        to_raw_value(setpoint(TagType::UINT64, RawKind::UNSIGNED, 64, &edge),
                     uint64_value(std::numeric_limits<std::uint64_t>::max()))
            .has_value());
}

TEST(GoldenWrite, EdgesOfTheWidths) {
    const WriteRules unsigned_8 =
        setpoint(TagType::FLOAT64, RawKind::UNSIGNED, 8, nullptr);
    EXPECT_EQ(converted(unsigned_8, float64_value(255.0)).number_u, 255U);
    EXPECT_FALSE(to_raw_value(unsigned_8, float64_value(255.5)).has_value());
    const WriteRules signed_8 =
        setpoint(TagType::FLOAT64, RawKind::SIGNED, 8, nullptr);
    EXPECT_EQ(converted(signed_8, float64_value(-128.0)).pattern, 0x80U);
    EXPECT_FALSE(to_raw_value(signed_8, float64_value(-128.5)).has_value());
    // 2^64 is outside uint64; the largest binary64 below it fits.
    const WriteRules u64 =
        setpoint(TagType::FLOAT64, RawKind::UNSIGNED, 64, nullptr);
    EXPECT_FALSE(
        to_raw_value(u64, float64_value(18446744073709551616.0)).has_value());
    EXPECT_EQ(converted(u64, float64_value(18446744073709549568.0)).number_u,
              18446744073709549568U);
    const WriteRules u64_exact =
        setpoint(TagType::UINT64, RawKind::UNSIGNED, 64, nullptr);
    EXPECT_EQ(converted(u64_exact,
                        uint64_value(std::numeric_limits<std::uint64_t>::max()))
                  .number_u,
              std::numeric_limits<std::uint64_t>::max());
}

TEST(GoldenWrite, NegativeZeroOnAnIntegerRawIsWrittenAsPlusZero) {
    // Raw 0 reads back as +0.0; `written` is what the read gives (8.4), not
    // the sign of the request. Found by the fuzz target's read-back.
    for (const RawKind kind : {RawKind::SIGNED, RawKind::UNSIGNED}) {
        for (const double request : {-0.0, -0.3}) {
            const WriteValue result = converted_value(
                setpoint(TagType::FLOAT64, kind, INT16, nullptr),
                float64_value(request));
            EXPECT_EQ(result.raw.pattern, 0U);
            EXPECT_EQ(std::bit_cast<std::uint64_t>(result.written.f), 0U)
                << request;
        }
    }
}

TEST(GoldenWrite, Float32UnderflowAndNegativeZero) {
    const WriteRules rules =
        setpoint(TagType::FLOAT64, RawKind::FLOAT, 32, nullptr);
    EXPECT_EQ(converted(rules, float64_value(1e-50)).pattern, 0U);
    EXPECT_EQ(converted(rules, float64_value(-0.0)).pattern, SIGN_BIT_32);
    // The largest float32 plus less than half a step stays finite; half a
    // step rounds to infinity and is refused.
    EXPECT_TRUE(
        to_raw_value(rules, float64_value(0x1.fffffefffffffp127)).has_value());
    EXPECT_FALSE(
        to_raw_value(rules, float64_value(0x1.ffffffp127)).has_value());
}

TEST(GoldenWrite, WidthsTheRawTypeCannotHaveAreRefused) {
    constexpr std::array<std::uint8_t, 6> WIDTHS{0, 1, 7, 12, 65, 255};
    for (const std::uint8_t width : WIDTHS) {
        EXPECT_FALSE(to_raw_value(setpoint(TagType::FLOAT64, RawKind::SIGNED,
                                           width, nullptr),
                                  float64_value(1.0))
                         .has_value())
            << int{width};
    }
    EXPECT_FALSE(
        to_raw_value(setpoint(TagType::FLOAT64, RawKind::FLOAT, 16, nullptr),
                     float64_value(1.0))
            .has_value());
}

TEST(GoldenWrite, EnumerationWithAScaleIsRefused) {
    // An enumeration is written as its code and read as its code (decision
    // D-131); with a scale the two would not be inverse.
    const std::array<std::int64_t, 2> codes{0, 2};
    const ScaleRule offset{.integer = true, .offset_i = 10, .offset = 10.0};
    WriteRules rules =
        setpoint(TagType::INT64, RawKind::UNSIGNED, INT16, &offset);
    rules.has_enum = true;
    rules.enum_codes = codes;
    EXPECT_FALSE(to_raw_value(rules, int64_value(2)).has_value());
}

// --- Raw patterns: safe states and release values (8.4) --------------------

TEST(GoldenWrite, RawPatternsAreWrittenAsTheyStand) {
    const RawValue int32_max = pattern_value(RawKind::SIGNED, 32, 0x7FFF'FFFFU);
    EXPECT_EQ(int32_max.number_i, 2147483647);
    EXPECT_EQ(int32_max.pattern, 0x7FFF'FFFFU);
    const RawValue int16_min = pattern_value(RawKind::SIGNED, INT16, 0x8000U);
    EXPECT_EQ(int16_min.number_i, -32768);
    EXPECT_EQ(int16_min.pattern, 0x8000U);
    // A NaN with a payload, as a release value: the pattern is kept.
    const RawValue nan = pattern_value(RawKind::FLOAT, 32, FLOAT32_NAN_PAYLOAD);
    EXPECT_EQ(nan.pattern, FLOAT32_NAN_PAYLOAD);
    const RawValue unsigned_pattern =
        pattern_value(RawKind::UNSIGNED, INT16, 0xFFFFU);
    EXPECT_EQ(unsigned_pattern.number_u, 0xFFFFU);
    // Bits above the width, and widths the raw type cannot have, are refused.
    EXPECT_FALSE(
        raw_from_pattern(RawKind::SIGNED, INT16, 0x1'0000U).has_value());
    EXPECT_FALSE(raw_from_pattern(RawKind::FLOAT, INT16, 0).has_value());
    EXPECT_FALSE(raw_from_pattern(RawKind::BOOL, 1, 2).has_value());
    EXPECT_FALSE(raw_from_pattern(RawKind::STRING, INT16, 0).has_value());
}

// --- Round trip: what is written reads back as `written` --------------------

// The read of a raw value with the same scaling as the write (contract 5.1,
// step 4, binary64).
double read_scaled(const ScaleRule& scale, double raw) {
    if (scale.form == ScaleRule::Form::GAIN) {
        const double product = raw * scale.gain;
        return product + scale.offset;
    }
    const double quotient = raw / static_cast<double>(scale.divide_by);
    return quotient + scale.offset;
}

TEST(GoldenWrite, RoundTripEveryInt16OnSeveralScalings) {
    const std::array<ScaleRule, 5> scalings{
        ScaleRule{.form = ScaleRule::Form::DIVIDE_BY,
                  .divide_by = DIVIDE_BY_10},
        ScaleRule{.form = ScaleRule::Form::DIVIDE_BY,
                  .divide_by = DIVIDE_BY_1000,
                  .offset = -40.0},
        ScaleRule{.gain = GAIN_0_1}, ScaleRule{.gain = 0.01, .offset = 5.0},
        ScaleRule{.gain = -0.1}};
    for (const ScaleRule& scale : scalings) {
        const WriteRules rules =
            setpoint(TagType::FLOAT64, RawKind::SIGNED, INT16, &scale);
        for (std::int64_t raw = INT16_MIN_VALUE; raw <= INT16_MAX_VALUE;
             ++raw) {
            // Read, then write what was read: the same raw value, and
            // `written` equals the value read.
            const double value = read_scaled(scale, static_cast<double>(raw));
            const WriteValue result =
                converted_value(rules, float64_value(value));
            ASSERT_EQ(result.raw.number_i, raw);
            ASSERT_EQ(std::bit_cast<std::uint64_t>(result.written.f),
                      std::bit_cast<std::uint64_t>(value));
        }
    }
}

TEST(GoldenWrite, RoundTripTheIntegerPath) {
    // Deterministic pseudo-random cases: written value equals the request,
    // and the read of the raw value gives the request back.
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp): a fixed seed keeps the test deterministic (CLAUDE.md, Tests)
    std::mt19937_64 random(SEED);
    std::uniform_int_distribution<std::int64_t> gains(-MAX_GAIN, MAX_GAIN);
    std::uniform_int_distribution<std::int64_t> values(-MAX_VALUE, MAX_VALUE);
    for (int index = 0; index < CASES; ++index) {
        const std::int64_t gain = gains(random);
        if (gain == 0) {
            continue;
        }
        const std::int64_t offset = values(random);
        const ScaleRule scale{
            .integer = true, .gain_i = gain, .offset_i = offset};
        const WriteRules rules =
            setpoint(TagType::INT64, RawKind::SIGNED, 64, &scale);
        const std::int64_t raw = values(random);
        const std::int64_t value = (raw * gain) + offset;
        const std::optional<WriteValue> result =
            to_raw_value(rules, int64_value(value));
        ASSERT_TRUE(result.has_value()) << gain << " " << offset << " " << raw;
        const WriteValue written = result.value_or(WriteValue{});
        ASSERT_EQ(written.raw.number_i, raw);
        ASSERT_EQ(written.written.i, value);
    }
}

}  // namespace
}  // namespace scada::driver
