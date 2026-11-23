// validate_point_rules: what the compile step refuses, so that the
// processing never meets it (decision D-131, D-133).

#include "scada/driver/point_rules.h"

#include "scada/common/status.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace scada::driver {
namespace {

using tag::TagType;

constexpr std::int64_t GAIN = 3;
constexpr std::int64_t OFFSET = -7;
constexpr std::uint8_t MISSING_SLOT = 9;
constexpr std::uint64_t DIVIDE_BY_10 = 10;
constexpr double HALF = 0.5;

PointRules valid() {
    PointRules rules;
    rules.tag_type = TagType::FLOAT64;
    rules.companions = {1, 2};
    rules.exponent = ExponentRule{.companion = 0};
    rules.quality_rules.push_back(QualityRule{
        .companion = 1,
        .has_field = true,
        .lo = 0,
        .hi = 3,
        .entries = {{.key = 0, .mark = {}}, {.key = 1, .mark = {}}}});
    rules.bits = {BitRule{.bit = 0}, BitRule{.bit = MAX_BIT_NUMBER}};
    return rules;
}

TEST(PointRules, ValidRulesPass) {
    EXPECT_TRUE(validate_point_rules(valid()).ok());
    PointRules integer;
    integer.tag_type = TagType::INT64;
    integer.scale =
        ScaleRule{.integer = true, .gain_i = GAIN, .offset_i = OFFSET};
    EXPECT_TRUE(validate_point_rules(integer).ok());
    integer.scale = ScaleRule{
        .form = ScaleRule::Form::DIVIDE_BY, .integer = true, .divide_by = 1};
    EXPECT_TRUE(validate_point_rules(integer).ok());
}

TEST(PointRules, EachFaultIsRefused) {
    const std::vector<std::function<void(PointRules&)>> faults{
        [](PointRules& rules) {
            rules.exponent = ExponentRule{.companion = 2};
        },
        [](PointRules& rules) { rules.time_companion = MISSING_SLOT; },
        [](PointRules& rules) {
            rules.quality_rules[0].companion = MISSING_SLOT;
        },
        [](PointRules& rules) {
            rules.quality_rules[0].hi = MAX_BIT_NUMBER + 1;
        },
        [](PointRules& rules) { rules.quality_rules[0].lo = 4; },
        [](PointRules& rules) { rules.quality_rules[0].entries[1].key = 0; },
        [](PointRules& rules) { rules.bits[1].bit = MAX_BIT_NUMBER + 1; },
        [](PointRules& rules) { rules.bits.resize(MAX_POINT_BITS + 1); },
        [](PointRules& rules) {
            rules.health.push_back(HealthRule{.companion = 0, .good = {2, 1}});
        },
        [](PointRules& rules) {
            rules.double_point = DoublePointRule{
                .states = {{1, DoublePoint::OPEN}, {1, DoublePoint::CLOSED}}};
        },
        [](PointRules& rules) {
            rules.scale =
                ScaleRule{.form = ScaleRule::Form::DIVIDE_BY, .divide_by = 0};
        },
        // CF-517: an integer tag needs an integer scaling and no exponent.
        [](PointRules& rules) { rules.tag_type = TagType::INT64; },
        [](PointRules& rules) {
            rules.exponent.reset();
            rules.tag_type = TagType::UINT64;
            rules.scale = ScaleRule{.form = ScaleRule::Form::DIVIDE_BY,
                                    .integer = true,
                                    .divide_by = DIVIDE_BY_10};
        },
        [](PointRules& rules) {
            rules.exponent.reset();
            rules.tag_type = TagType::INT64;
            rules.scale = ScaleRule{.gain = HALF};
        },
        // An enumeration is an INT64 tag without a scale.
        [](PointRules& rules) {
            rules.exponent.reset();
            rules.has_enum = true;
        },
        [](PointRules& rules) {
            rules.exponent.reset();
            rules.tag_type = TagType::INT64;
            rules.has_enum = true;
            rules.scale = ScaleRule{.integer = true, .offset_i = 1};
        },
        [](PointRules& rules) {
            rules.device_time = DeviceTimeRule{.local = true};
        },
        [](PointRules& rules) {
            rules.companion_time = DeviceTimeRule{.local = true};
        },
    };
    for (std::size_t index = 0; index < faults.size(); ++index) {
        PointRules rules = valid();
        faults[index](rules);
        const Status status = validate_point_rules(rules);
        EXPECT_FALSE(status.ok()) << index;
        EXPECT_EQ(status.code(), StatusCode::INVALID_ARGUMENT) << index;
    }
}

TEST(PointRules, IntegerScaling) {
    EXPECT_TRUE(is_integer_scaling(ScaleRule{.integer = true, .gain_i = 2}));
    EXPECT_TRUE(is_integer_scaling(ScaleRule{
        .form = ScaleRule::Form::DIVIDE_BY, .integer = true, .divide_by = 1}));
    EXPECT_FALSE(is_integer_scaling(ScaleRule{
        .form = ScaleRule::Form::DIVIDE_BY, .integer = true, .divide_by = 2}));
    EXPECT_FALSE(is_integer_scaling(ScaleRule{.gain = 2.0}));
}

}  // namespace
}  // namespace scada::driver
