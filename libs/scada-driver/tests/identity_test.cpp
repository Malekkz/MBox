// Identity checks and restart indications (driver interface contract 3.6,
// 7.6; configuration schemas contract 4.8, 4.9; decision D-147).

#include "scada/driver/identity.h"
#include "scada/driver/tag_value.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace scada::driver {
namespace {

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers): the values are the test data

using tag::TagType;

ExpectedValue u64(std::uint64_t value) {
    return ExpectedValue{.type = TagType::UINT64, .u = value};
}
ExpectedValue f64(double value) {
    return ExpectedValue{.type = TagType::FLOAT64, .f = value};
}
ExpectedValue text(std::string value) {
    return ExpectedValue{.type = TagType::STRING, .text = std::move(value)};
}

IdentityCheck check(IdentityCheck::Kind kind, std::vector<ExpectedValue> values,
                    std::string pattern = {}) {
    return IdentityCheck{.point = 0,
                         .kind = kind,
                         .values = std::move(values),
                         .pattern = std::move(pattern)};
}

// --- The one comparison -----------------------------------------------------

TEST(CompareValues, SignedAndUnsignedCompareByTheirValues) {
    EXPECT_EQ(compare_values(int64_value(-1), uint64_value(0)),
              ValueOrder::LESS);  // not 2^64 - 1
    EXPECT_EQ(compare_values(uint64_value(0), int64_value(-1)),
              ValueOrder::GREATER);
    EXPECT_EQ(compare_values(int64_value(7), uint64_value(7)),
              ValueOrder::EQUAL);
    EXPECT_EQ(
        compare_values(uint64_value(std::numeric_limits<std::uint64_t>::max()),
                       int64_value(std::numeric_limits<std::int64_t>::max())),
        ValueOrder::GREATER);
}

TEST(CompareValues, AnIntegerAndADoubleCompareExactly) {
    // 2^53 + 1 has no double: it is above 2^53 as a double, not equal.
    constexpr std::int64_t ABOVE = (std::int64_t{1} << 53) + 1;
    EXPECT_EQ(compare_values(int64_value(ABOVE),
                             float64_value(static_cast<double>(ABOVE))),
              ValueOrder::GREATER);
    EXPECT_EQ(compare_values(int64_value(3), float64_value(3.0)),
              ValueOrder::EQUAL);
    EXPECT_EQ(compare_values(int64_value(3), float64_value(3.5)),
              ValueOrder::LESS);
    EXPECT_EQ(compare_values(int64_value(-3), float64_value(-3.5)),
              ValueOrder::GREATER);
    EXPECT_EQ(compare_values(float64_value(2.5), uint64_value(2)),
              ValueOrder::GREATER);
    EXPECT_EQ(compare_values(uint64_value(1), float64_value(-0.5)),
              ValueOrder::GREATER);
    EXPECT_EQ(
        compare_values(int64_value(std::numeric_limits<std::int64_t>::max()),
                       float64_value(9'223'372'036'854'775'808.0)),
        ValueOrder::LESS);  // 2^63
    EXPECT_EQ(
        compare_values(uint64_value(std::numeric_limits<std::uint64_t>::max()),
                       float64_value(18'446'744'073'709'551'616.0)),
        ValueOrder::LESS);  // 2^64
    EXPECT_EQ(
        compare_values(int64_value(1),
                       float64_value(std::numeric_limits<double>::quiet_NaN())),
        ValueOrder::UNORDERED);
    // The edges (review S7): INT64_MIN is -2^63 exactly, and 0 is -0.0.
    EXPECT_EQ(
        compare_values(int64_value(std::numeric_limits<std::int64_t>::min()),
                       float64_value(-9'223'372'036'854'775'808.0)),
        ValueOrder::EQUAL);
    EXPECT_EQ(compare_values(uint64_value(0), float64_value(-0.0)),
              ValueOrder::EQUAL);
    EXPECT_EQ(compare_values(float64_value(-0.0), uint64_value(0)),
              ValueOrder::EQUAL);
}

TEST(CompareValues, OtherTypesCompareOnlyWithTheirOwn) {
    EXPECT_EQ(compare_values(bool_value(false), bool_value(true)),
              ValueOrder::LESS);
    EXPECT_EQ(compare_values(bool_value(true), int64_value(1)),
              ValueOrder::UNORDERED);
    EXPECT_EQ(compare_values(string_value("a"), string_value("a")),
              ValueOrder::EQUAL);
    EXPECT_EQ(compare_values(string_value("a"), string_value("b")),
              ValueOrder::UNORDERED);  // a STRING has no order
    EXPECT_EQ(compare_values(string_value("1"), int64_value(1)),
              ValueOrder::UNORDERED);
}

// --- Identity checks --------------------------------------------------------

TEST(Identity, EqualsAndInCompareTheValues) {
    const IdentityCheck equals = check(IdentityCheck::Kind::EQUALS, {u64(12)});
    EXPECT_TRUE(identity_matches(equals, uint64_value(12)));
    EXPECT_TRUE(identity_matches(equals, int64_value(12)));  // its value
    EXPECT_FALSE(identity_matches(equals, uint64_value(13)));
    const IdentityCheck any_of =
        check(IdentityCheck::Kind::IN, {text("SUN2000"), text("SUN3000")});
    EXPECT_TRUE(identity_matches(any_of, string_value("SUN3000")));
    EXPECT_FALSE(identity_matches(any_of, string_value("SUN4000")));
    EXPECT_FALSE(identity_matches(check(IdentityCheck::Kind::EQUALS, {}),
                                  uint64_value(0)));
}

TEST(Identity, ARangeIncludesBothEnds) {
    const IdentityCheck range =
        check(IdentityCheck::Kind::RANGE, {u64(10), u64(20)});
    EXPECT_TRUE(identity_matches(range, uint64_value(10)));
    EXPECT_TRUE(identity_matches(range, uint64_value(20)));
    EXPECT_FALSE(identity_matches(range, uint64_value(9)));
    EXPECT_FALSE(identity_matches(range, uint64_value(21)));
    EXPECT_FALSE(identity_matches(range, int64_value(-1)));
    const IdentityCheck floats =
        check(IdentityCheck::Kind::RANGE, {f64(1.0), f64(2.0)});
    EXPECT_TRUE(identity_matches(floats, float64_value(1.5)));
    EXPECT_TRUE(identity_matches(floats, int64_value(2)));
    EXPECT_FALSE(identity_matches(
        floats, float64_value(std::numeric_limits<double>::quiet_NaN())));
    EXPECT_FALSE(identity_matches(
        check(IdentityCheck::Kind::RANGE, {text("a"), text("z")}),
        string_value("m")));  // a STRING has no order
}

TEST(Identity, APatternMatchesAStringOnly) {
    const IdentityCheck pattern =
        check(IdentityCheck::Kind::PATTERN, {}, "SUN2000-*KTL*");
    EXPECT_TRUE(identity_matches(pattern, string_value("SUN2000-10KTL-M1")));
    EXPECT_FALSE(identity_matches(pattern, string_value("SUN5000-10KTL")));
    EXPECT_FALSE(identity_matches(pattern, uint64_value(1)));
    // `*` matches the empty text, which a number would give: still no match
    // (review S7).
    const IdentityCheck any = check(IdentityCheck::Kind::PATTERN, {}, "*");
    EXPECT_TRUE(identity_matches(any, string_value("")));
    EXPECT_FALSE(identity_matches(any, uint64_value(1)));
    EXPECT_FALSE(identity_matches(any, bool_value(false)));
}

// --- Restart rules ----------------------------------------------------------

TEST(RestartWatch, ACounterGoingBackIsARestart) {
    const std::vector<RestartRule> rules{
        RestartRule{.point = 2, .kind = RestartRule::Kind::DECREASES}};
    RestartWatch watch(rules);
    EXPECT_FALSE(watch.sample(2, uint64_value(100)));  // the baseline
    EXPECT_FALSE(watch.sample(2, uint64_value(100)));
    EXPECT_FALSE(watch.sample(2, uint64_value(160)));
    EXPECT_FALSE(watch.sample(1, uint64_value(0)));  // another point
    EXPECT_TRUE(watch.sample(2, uint64_value(3)));
    EXPECT_FALSE(watch.sample(2, uint64_value(5)));
}

TEST(RestartWatch, ACountersWrapIsARestartToo) {
    // The ruling's change (review M6): every decrease fires, a wrap of a
    // counter with `at_limit: wrap` included; a false restart once per wrap
    // period costs only a re-check.
    const std::vector<RestartRule> rules{
        RestartRule{.point = 0, .kind = RestartRule::Kind::DECREASES}};
    RestartWatch watch(rules);
    EXPECT_FALSE(watch.sample(0, uint64_value(65'530)));
    EXPECT_TRUE(watch.sample(0, uint64_value(4)));  // a wrap
    EXPECT_FALSE(watch.sample(0, uint64_value(9)));
}

TEST(RestartWatch, AnUnorderedValueNeitherFiresNorMovesTheBaseline) {
    // Review S5: 100, NaN, 5 fires `decreases` against 100.
    const std::vector<RestartRule> rules{
        RestartRule{.point = 0, .kind = RestartRule::Kind::DECREASES},
        RestartRule{.point = 1, .kind = RestartRule::Kind::CHANGES}};
    RestartWatch watch(rules);
    EXPECT_FALSE(watch.sample(0, float64_value(100.0)));
    EXPECT_FALSE(watch.sample(
        0, float64_value(std::numeric_limits<double>::quiet_NaN())));
    EXPECT_TRUE(watch.sample(0, float64_value(5.0)));
    // A STRING longer than the tag database holds is no sample either.
    EXPECT_FALSE(watch.sample(1, string_value("v1")));
    const std::string long_text(TAG_STRING_MAX_BYTES + 1, 'x');
    EXPECT_FALSE(watch.sample(1, string_value(long_text)));
    EXPECT_FALSE(watch.sample(1, string_value("v1")));
}

TEST(RestartWatch, TheBaselineLastsUntilAReset) {
    // No reset at a lost connection: a device that restarted while the
    // connection was down is seen by its first sample after (D-147).
    const std::vector<RestartRule> rules{
        RestartRule{.point = 0, .kind = RestartRule::Kind::DECREASES}};
    RestartWatch watch(rules);
    EXPECT_FALSE(watch.sample(0, uint64_value(5000)));
    EXPECT_TRUE(watch.sample(0, uint64_value(12)));
    // An attach or a reload: a new baseline, even a lower one.
    watch.reset();
    EXPECT_FALSE(watch.sample(0, uint64_value(1)));
    EXPECT_FALSE(watch.sample(0, uint64_value(2)));
}

TEST(RestartWatch, ABootCounterThatChangesIsARestart) {
    const std::vector<RestartRule> rules{
        RestartRule{.point = 0, .kind = RestartRule::Kind::CHANGES}};
    RestartWatch watch(rules);
    EXPECT_FALSE(watch.sample(0, string_value("boot-7")));
    EXPECT_FALSE(watch.sample(0, string_value("boot-7")));
    EXPECT_TRUE(watch.sample(0, string_value("boot-8")));
    EXPECT_FALSE(watch.sample(0, string_value("boot-8")));
    EXPECT_TRUE(watch.sample(0, uint64_value(8)));  // another type
}

TEST(RestartWatch, APowerUpFlagIsARestartWhenItRises) {
    const std::vector<RestartRule> rules{
        RestartRule{.point = 1,
                    .kind = RestartRule::Kind::EQUALS,
                    .value = ExpectedValue{.type = TagType::BOOL, .b = true}}};
    RestartWatch watch(rules);
    // Found set at the first sample: the baseline, no restart.
    EXPECT_FALSE(watch.sample(1, bool_value(true)));
    EXPECT_FALSE(watch.sample(1, bool_value(true)));  // left set
    EXPECT_FALSE(watch.sample(1, bool_value(false)));
    EXPECT_TRUE(watch.sample(1, bool_value(true)));  // the rise
    EXPECT_FALSE(watch.sample(1, bool_value(true)));
    // Samples that were not valid are not given to the watch, so the edge
    // is taken across them.
    EXPECT_FALSE(watch.sample(1, bool_value(false)));
    EXPECT_TRUE(watch.sample(1, bool_value(true)));
}

TEST(RestartWatch, AnEqualsRuleComparesAcrossNumberTypes) {
    const std::vector<RestartRule> rules{RestartRule{
        .point = 0, .kind = RestartRule::Kind::EQUALS, .value = u64(0)}};
    RestartWatch watch(rules);
    EXPECT_FALSE(watch.sample(0, int64_value(3)));
    EXPECT_TRUE(watch.sample(0, int64_value(0)));  // INT64 0 is 0
}

TEST(RestartWatch, AnyRuleOfThePointDecides) {
    const std::vector<RestartRule> rules{
        RestartRule{.point = 0, .kind = RestartRule::Kind::DECREASES},
        RestartRule{
            .point = 0, .kind = RestartRule::Kind::EQUALS, .value = u64(0)}};
    RestartWatch watch(rules);
    EXPECT_FALSE(watch.sample(0, uint64_value(5)));
    EXPECT_TRUE(watch.sample(0, uint64_value(0)));  // both hold
    EXPECT_FALSE(watch.sample(0, uint64_value(1)));
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

}  // namespace
}  // namespace scada::driver
