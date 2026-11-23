// Golden vectors of the write conversion: an engineering value and a
// point's rules give the raw value, or a refusal (driver interface contract
// 8.4, conformance test 5; configuration schemas contract 7.2).

#include "scada/driver/point_rules.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/tag_value.h"
#include "scada/driver/write_conversion.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>

namespace scada::driver {
namespace {

using tag::TagType;

// The raw value of a conversion that must succeed; a refusal fails the
// test and gives a zero value.
RawValue converted(const WriteRules& rules, const TagValue& value) {
    const std::optional<RawValue> raw = to_raw_value(rules, value);
    EXPECT_TRUE(raw.has_value());
    return raw.value_or(RawValue{});
}

constexpr std::uint8_t INT16 = 16;
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

TEST(GoldenWrite, StringIsNotWritable) {
    const WriteRules rules =
        setpoint(TagType::STRING, RawKind::STRING, 0, nullptr);
    EXPECT_FALSE(to_raw_value(rules, string_value("x")).has_value());
}

}  // namespace
}  // namespace scada::driver
