// Identity checks and restart indications (driver interface contract 3.6,
// 7.6; configuration schemas contract 4.8, 4.9; decision D-145).

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

TEST(Identity, EqualsAndInCompareTheTypeAndTheValue) {
    const IdentityCheck equals = check(IdentityCheck::Kind::EQUALS, {u64(12)});
    EXPECT_TRUE(identity_matches(equals, uint64_value(12)));
    EXPECT_FALSE(identity_matches(equals, uint64_value(13)));
    EXPECT_FALSE(identity_matches(equals, int64_value(12)));  // another type
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
    const IdentityCheck floats =
        check(IdentityCheck::Kind::RANGE,
              {ExpectedValue{.type = TagType::FLOAT64, .f = 1.0},
               ExpectedValue{.type = TagType::FLOAT64, .f = 2.0}});
    EXPECT_TRUE(identity_matches(floats, float64_value(1.5)));
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
}

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
    // After a connect the next sample is a new baseline, even a lower one.
    watch.reset();
    EXPECT_FALSE(watch.sample(2, uint64_value(1)));
}

TEST(RestartWatch, ABootCounterThatChangesIsARestart) {
    const std::vector<RestartRule> rules{
        RestartRule{.point = 0, .kind = RestartRule::Kind::CHANGES}};
    RestartWatch watch(rules);
    EXPECT_FALSE(watch.sample(0, string_value("boot-7")));
    EXPECT_FALSE(watch.sample(0, string_value("boot-7")));
    EXPECT_TRUE(watch.sample(0, string_value("boot-8")));
    EXPECT_FALSE(watch.sample(0, string_value("boot-8")));
    EXPECT_TRUE(watch.sample(0, uint64_value(8)));  // another type: changed
}

TEST(RestartWatch, APowerUpFlagIsARestartWhenItRises) {
    const std::vector<RestartRule> rules{
        RestartRule{.point = 1,
                    .kind = RestartRule::Kind::EQUALS,
                    .value = ExpectedValue{.type = TagType::BOOL, .b = true}}};
    RestartWatch watch(rules);
    // Found set at the first sample: that connect's, no restart.
    EXPECT_FALSE(watch.sample(1, bool_value(true)));
    EXPECT_FALSE(watch.sample(1, bool_value(true)));  // left set: no repeat
    EXPECT_FALSE(watch.sample(1, bool_value(false)));
    EXPECT_TRUE(watch.sample(1, bool_value(true)));  // the rise
    EXPECT_FALSE(watch.sample(1, bool_value(true)));
    watch.reset();
    EXPECT_FALSE(watch.sample(1, bool_value(true)));
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
