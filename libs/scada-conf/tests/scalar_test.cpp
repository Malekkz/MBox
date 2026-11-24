// The scalar forms of configuration schemas contract 1.2 and 2.2.

#include "scada/conf/scalar.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <variant>

namespace scada::conf {
namespace {

constexpr std::uint64_t UINT64_TOP = std::numeric_limits<std::uint64_t>::max();
constexpr std::int64_t INT64_TOP = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t INT64_BOTTOM = std::numeric_limits<std::int64_t>::min();

std::optional<std::int64_t> int64_of(std::string_view text) {
    const std::optional<Integer> value = parse_integer(text);
    if (!value) {
        return std::nullopt;
    }
    return as_int64(*value);
}

TEST(Scalar, IntegersOfSection12) {
    constexpr std::int64_t SIXTEEN = 16;
    constexpr std::int64_t MINUS_FIVE = -5;
    EXPECT_EQ(int64_of("0"), 0);
    EXPECT_EQ(int64_of("-0"), 0);
    EXPECT_EQ(int64_of("16"), SIXTEEN);
    EXPECT_EQ(int64_of("0x10"), SIXTEEN);
    EXPECT_EQ(int64_of("0xfF"), 255);
    EXPECT_EQ(int64_of("-5"), MINUS_FIVE);
    EXPECT_EQ(int64_of("9223372036854775807"), INT64_TOP);
    EXPECT_EQ(int64_of("-9223372036854775808"), INT64_BOTTOM);
    EXPECT_FALSE(int64_of("9223372036854775808"));
    EXPECT_FALSE(int64_of("-9223372036854775809"));
}

// 1.2: these match no rule and are refused where a number is expected.
TEST(Scalar, TextsThatAreNotIntegers) {
    for (const std::string_view text :
         {"+5", "1.", ".5", "010", "1_000", "0o17", "0b1", "-0x10", "0X10",
          "0x", "", "-", " 1", "1 ", "1e3", "x"}) {
        EXPECT_FALSE(parse_integer(text)) << text;
    }
}

TEST(Scalar, TheWholeRangeOfUint64) {
    const std::optional<Integer> top = parse_integer("18446744073709551615");
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(as_uint64(top.value_or(Integer{})), UINT64_TOP);
    EXPECT_FALSE(as_int64(top.value_or(Integer{})));
    EXPECT_EQ(
        as_uint64(parse_integer("0xFFFFFFFFFFFFFFFF").value_or(Integer{})),
        UINT64_TOP);
    EXPECT_FALSE(parse_integer("18446744073709551616"));
    EXPECT_FALSE(parse_integer("0x10000000000000000"));
    EXPECT_FALSE(as_uint64(parse_integer("-1").value_or(Integer{})));
}

// B3 of the review of #131: three results (D-107).
TEST(Scalar, IntegersOfTheFormThatAreTooLarge) {
    EXPECT_EQ(read_integer("18446744073709551615").form, IntegerForm::FITS);
    EXPECT_EQ(read_integer("18446744073709551616").form,
              IntegerForm::TOO_LARGE);
    EXPECT_EQ(read_integer("-18446744073709551616").form,
              IntegerForm::TOO_LARGE);
    EXPECT_EQ(read_integer("0xFFFFFFFFFFFFFFFF").form, IntegerForm::FITS);
    EXPECT_EQ(read_integer("0x10000000000000000").form, IntegerForm::TOO_LARGE);
    EXPECT_EQ(read_integer("0x").form, IntegerForm::NOT_INTEGER);
    EXPECT_EQ(read_integer("0xg").form, IntegerForm::NOT_INTEGER);
    EXPECT_EQ(read_integer("1.0").form, IntegerForm::NOT_INTEGER);
    EXPECT_EQ(read_integer("010").form, IntegerForm::NOT_INTEGER);
    // Too large, it is never read as a decimal.
    EXPECT_FALSE(parse_number("18446744073709551616").has_value());
    EXPECT_FALSE(parse_integer("18446744073709551616").has_value());
    EXPECT_TRUE(has_decimal_form("1e400"));
    EXPECT_FALSE(parse_decimal("1e400").has_value());
}

TEST(Scalar, Decimals) {
    constexpr double TENTH = 0.1;
    constexpr double THOUSAND = 1e3;
    constexpr double MINUS_TWO_AND_A_HALF = -2.5;
    EXPECT_EQ(parse_decimal("0.1"), TENTH);
    EXPECT_EQ(parse_decimal("1e3"), THOUSAND);
    EXPECT_EQ(parse_decimal("1E+3"), THOUSAND);
    EXPECT_EQ(parse_decimal("-2.5"), MINUS_TWO_AND_A_HALF);
    EXPECT_EQ(parse_decimal("10"), 10.0);
    EXPECT_EQ(parse_decimal("0.0"), 0.0);
    EXPECT_EQ(parse_decimal("0e5"), 0.0);
}

TEST(Scalar, DecimalsRefused) {
    // 1e-400 is not 0 but rounds to it; 1e400 is not finite (1.2).
    for (const std::string_view text :
         {"1e-400", "1e400", "1.", ".5", "+1.5", "01.5", "1.5e", "1e+", "inf",
          "nan", "1,5", "0x1p3", "1_0.5", ""}) {
        EXPECT_FALSE(parse_decimal(text)) << text;
    }
}

// 1.2: the written form is kept.
TEST(Scalar, NumbersKeepTheirWrittenForm) {
    const std::optional<Number> ten = parse_number("10");
    const std::optional<Number> ten_point_zero = parse_number("10.0");
    ASSERT_TRUE(ten.has_value() && ten_point_zero.has_value());
    EXPECT_TRUE(std::holds_alternative<Integer>(ten.value_or(Number{0.0})));
    EXPECT_TRUE(
        std::holds_alternative<double>(ten_point_zero.value_or(Number{})));
    EXPECT_FALSE(parse_number("010"));
}

TEST(Scalar, BooleansAreTrueAndFalseOnly) {
    EXPECT_EQ(parse_boolean("true"), true);
    EXPECT_EQ(parse_boolean("false"), false);
    for (const std::string_view text :
         {"True", "TRUE", "yes", "no", "on", "off", "1", "0", "y", ""}) {
        EXPECT_FALSE(parse_boolean(text)) << text;
    }
}

TEST(Scalar, DurationsOfSection22) {
    constexpr std::uint64_t HUNDRED_MS = 100;
    constexpr std::uint64_t FIVE_S = 5000;
    constexpr std::uint64_t TWO_M = 120000;
    constexpr std::uint64_t ONE_H = 3600000;
    constexpr std::uint64_t NINETY_D = 90ULL * 86400000ULL;
    constexpr std::uint64_t TEN_Y = 10ULL * 365ULL * 86400000ULL;
    EXPECT_EQ(parse_duration_ms("100ms"), HUNDRED_MS);
    EXPECT_EQ(parse_duration_ms("5s"), FIVE_S);
    EXPECT_EQ(parse_duration_ms("2m"), TWO_M);
    EXPECT_EQ(parse_duration_ms("1h"), ONE_H);
    EXPECT_EQ(parse_duration_ms("90d"), NINETY_D);
    EXPECT_EQ(parse_duration_ms("10y"), TEN_Y);
    EXPECT_EQ(parse_duration_ms("0s"), 0U);
}

TEST(Scalar, DurationsRefused) {
    // No fractions, no compound forms, no sign, one unit (2.2).
    for (const std::string_view text :
         {"1.5s", "1s500ms", "-5s", "+5s", "5", "5 s", "5S", "5sec", "s", "05s",
          "5us", "99999999999999999999ms", "584942418y"}) {
        EXPECT_FALSE(parse_duration_ms(text)) << text;
    }
}

TEST(Scalar, SizesOfSection22) {
    constexpr std::uint64_t TWO_GIB = 2ULL * 1024 * 1024 * 1024;
    constexpr std::uint64_t ONE_TIB = 1024ULL * 1024 * 1024 * 1024;
    EXPECT_EQ(parse_size_bytes("2GiB"), TWO_GIB);
    EXPECT_EQ(parse_size_bytes("1TiB"), ONE_TIB);
    EXPECT_EQ(parse_size_bytes("512B"), 512U);
    for (const std::string_view text :
         {"2GB", "2gib", "2 GiB", "1.5GiB", "GiB", "-1B", "16777216TiB"}) {
        EXPECT_FALSE(parse_size_bytes(text)) << text;
    }
}

TEST(Scalar, PercentagesOfSection22) {
    constexpr double HALF = 0.5;
    constexpr double TEN = 10.0;
    EXPECT_EQ(parse_percentage("0.5%"), HALF);
    EXPECT_EQ(parse_percentage("10%"), TEN);
    for (const std::string_view text : {"10", "%", "10 %", ".5%", "1e999%"}) {
        EXPECT_FALSE(parse_percentage(text)) << text;
    }
}

}  // namespace
}  // namespace scada::conf
