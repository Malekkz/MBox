// The budget of the health rules' patterns of stage 5 (D-120; M4 of the
// review of #174): a pattern the budget runs out in, and those after it,
// are not checked; a listed name is. A small budget here, so that it runs
// quickly under the sanitizers; `point_limits_perf_test.cpp` times the tree
// at the default budget. An executable of its own with its own time limit.
// Invented examples only.

#include "expand_cases.h"
#include "health_patterns_tree.h"
#include "scada/conf/expand.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace scada::conf {
namespace {

constexpr std::size_t POINTS = 2000;
constexpr std::size_t RULES = 100;

// About 5.3e6 units a pattern of the tree: room for 10 patterns.
constexpr std::uint64_t SMALL_BUDGET = 50'000'000;

TEST(PointLimits, HealthPatternsStopAtTheirBudget) {
    ExpandLimits limits;
    limits.health_pattern_work = SMALL_BUDGET;
    const Expanded expanded = expand_limited(
        {{"templates/meter_a.yaml", health_patterns_meter(POINTS, RULES)},
         {"drivers/modbus/line1.yaml", one_device()}},
        limits);
    // The listed name is checked; the pattern after the budget is not.
    EXPECT_EQ(expanded.lines, Lines{health_patterns_zz_error(POINTS, RULES)});
}

// With room for every pattern, the last one is checked too.
TEST(PointLimits, HealthPatternsWithinTheirBudget) {
    constexpr std::size_t FEW = 5;
    ExpandLimits limits;
    limits.health_pattern_work = SMALL_BUDGET;
    const Expanded expanded = expand_limited(
        {{"templates/meter_a.yaml", health_patterns_meter(POINTS, FEW)},
         {"drivers/modbus/line1.yaml", one_device()}},
        limits);
    ASSERT_EQ(expanded.lines.size(), 2U);
    EXPECT_EQ(expanded.lines.front(), health_patterns_zz_error(POINTS, FEW));
    EXPECT_NE(expanded.lines.back().find("point \"zz\""), std::string::npos);
}

}  // namespace
}  // namespace scada::conf
