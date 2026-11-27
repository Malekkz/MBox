// Identity checks and restart indications (driver interface contract 3.6,
// 7.6; configuration schemas contract 4.8, 4.9; decision D-147).

#include "scada/driver/identity.h"
#include "scada/driver/tag_value.h"
#include "scada/driver/text_pattern.h"
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
        check(IdentityCheck::Kind::IN, {text("MODEL-A"), text("MODEL-B")});
    EXPECT_TRUE(identity_matches(any_of, string_value("MODEL-B")));
    EXPECT_FALSE(identity_matches(any_of, string_value("MODEL-C")));
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
        check(IdentityCheck::Kind::PATTERN, {}, "MODEL-A-*X*");
    EXPECT_TRUE(identity_matches(pattern, string_value("MODEL-A-10X-R1")));
    EXPECT_FALSE(identity_matches(pattern, string_value("MODEL-E-10X")));
    EXPECT_FALSE(identity_matches(pattern, uint64_value(1)));
    // `*` matches the empty text, which a number would give: still no match
    // (review S7).
    const IdentityCheck any = check(IdentityCheck::Kind::PATTERN, {}, "*");
    EXPECT_TRUE(identity_matches(any, string_value("")));
    EXPECT_FALSE(identity_matches(any, uint64_value(1)));
    EXPECT_FALSE(identity_matches(any, bool_value(false)));
}

TEST(Identity, ACheckTheCleanedTextCouldNeverMatchIsRefused) {
    // Review S3: the device's text is cleaned before it is compared, so an
    // expected byte outside 0x20 to 0x7E can never match.
    IdentityCheck text =
        check(IdentityCheck::Kind::EQUALS,
              {ExpectedValue{.type = TagType::STRING, .text = "A\tB"}});
    EXPECT_FALSE(validate_identity_check(text, TagType::STRING).ok());
    text.values[0].text = "A B~";
    EXPECT_TRUE(validate_identity_check(text, TagType::STRING).ok());
    text.values[0].text = std::string(TAG_STRING_MAX_BYTES + 1, 'a');
    EXPECT_FALSE(validate_identity_check(text, TagType::STRING).ok());
}

TEST(Identity, AMalformedCheckIsRefused) {
    const auto u64 = [](std::uint64_t value) {
        return ExpectedValue{.type = TagType::UINT64, .u = value};
    };
    EXPECT_TRUE(validate_identity_check(
                    check(IdentityCheck::Kind::RANGE, {u64(1), u64(1)}),
                    TagType::UINT64)
                    .ok());
    EXPECT_FALSE(validate_identity_check(
                     check(IdentityCheck::Kind::RANGE, {u64(2), u64(1)}),
                     TagType::UINT64)
                     .ok());
    EXPECT_FALSE(validate_identity_check(
                     check(IdentityCheck::Kind::EQUALS, {u64(1), u64(2)}),
                     TagType::UINT64)
                     .ok());
    EXPECT_FALSE(validate_identity_check(check(IdentityCheck::Kind::IN, {}),
                                         TagType::UINT64)
                     .ok());
    // Of another type than the point, or NaN.
    EXPECT_FALSE(
        validate_identity_check(check(IdentityCheck::Kind::EQUALS, {u64(1)}),
                                TagType::INT64)
            .ok());
    EXPECT_FALSE(validate_identity_check(
                     check(IdentityCheck::Kind::EQUALS,
                           {ExpectedValue{
                               .type = TagType::FLOAT64,
                               .f = std::numeric_limits<double>::quiet_NaN()}}),
                     TagType::FLOAT64)
                     .ok());
}

TEST(Identity, APatternIsCheckedAsTheMatcherTakesIt) {
    // Ruling item 5: at most 250 bytes, no single backslash at the end, on a
    // STRING point only.
    EXPECT_TRUE(
        validate_identity_check(
            check(IdentityCheck::Kind::PATTERN, {}, "MODEL-*"), TagType::STRING)
            .ok());
    EXPECT_FALSE(
        validate_identity_check(
            check(IdentityCheck::Kind::PATTERN, {}, "MODEL-*"), TagType::UINT64)
            .ok());
    EXPECT_FALSE(
        validate_identity_check(check(IdentityCheck::Kind::PATTERN, {}, "a\\"),
                                TagType::STRING)
            .ok());
    EXPECT_TRUE(
        validate_identity_check(
            check(IdentityCheck::Kind::PATTERN, {}, "a\\\\"), TagType::STRING)
            .ok());
    EXPECT_FALSE(validate_identity_check(
                     check(IdentityCheck::Kind::PATTERN, {},
                           std::string(TEXT_PATTERN_MAX_BYTES + 1, 'a')),
                     TagType::STRING)
                     .ok());
    // Review S3: a literal the cleaned text never holds.
    EXPECT_FALSE(validate_identity_check(
                     check(IdentityCheck::Kind::PATTERN, {}, "Z\xC3\xA4hler*"),
                     TagType::STRING)
                     .ok());
    EXPECT_FALSE(
        validate_identity_check(check(IdentityCheck::Kind::PATTERN, {}, "A\tB"),
                                TagType::STRING)
            .ok());
}

TEST(Identity, ARangeWhoseEndsDoNotCompareIsRefused) {
    // Two different STRINGs are unordered (compare_values): no range.
    const auto text = [](const char* value) {
        return ExpectedValue{.type = TagType::STRING, .text = value};
    };
    EXPECT_FALSE(validate_identity_check(
                     check(IdentityCheck::Kind::RANGE, {text("A"), text("B")}),
                     TagType::STRING)
                     .ok());
    EXPECT_TRUE(validate_identity_check(
                    check(IdentityCheck::Kind::RANGE, {text("A"), text("A")}),
                    TagType::STRING)
                    .ok());
}

TEST(RestartWatch, ARuleIsCheckedForItsPoint) {
    const RestartRule decreases{.point = 0,
                                .kind = RestartRule::Kind::DECREASES};
    EXPECT_TRUE(validate_restart_rule(decreases, TagType::UINT64, false).ok());
    EXPECT_FALSE(validate_restart_rule(decreases, TagType::BOOL, false).ok());
    EXPECT_FALSE(validate_restart_rule(decreases, TagType::STRING, false).ok());
    EXPECT_FALSE(validate_restart_rule(decreases, TagType::INT64, true).ok());
    const RestartRule equals{
        .point = 0,
        .kind = RestartRule::Kind::EQUALS,
        .value = ExpectedValue{.type = TagType::FLOAT64,
                               .f = std::numeric_limits<double>::quiet_NaN()}};
    EXPECT_FALSE(validate_restart_rule(equals, TagType::FLOAT64, false).ok());
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
