// The budget of `applies_to` patterns for CF-311 at stage 5 (D-116; #176):
// a pattern the budget runs out in, and those after it, are not checked; a
// listed name is. A small budget here, so that it runs quickly under the
// sanitizers; `applies_to_limits_perf_test.cpp` times the tree at the
// default budget. An executable of its own with its own time limit.
// Invented examples only.

#include "applies_to_tree.h"
#include "expand_cases.h"
#include "scada/conf/expand.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace scada::conf {
namespace {

constexpr std::size_t POINTS = 2000;
constexpr std::size_t RULES = 100;
// About 3.5e6 units a pattern of the tree: room for 14 patterns.
constexpr std::uint64_t SMALL_BUDGET = 50'000'000;
constexpr std::ptrdiff_t CHECKED = 14;

std::ptrdiff_t count_of(const Lines& lines, const std::string& text) {
    return std::ranges::count_if(lines, [&text](const std::string& line) {
        return line.find(text) != std::string::npos;
    });
}

TEST(AppliesToLimits, PatternsStopAtTheirBudget) {
    ExpandLimits limits;
    limits.applies_to_pattern_work = SMALL_BUDGET;
    const Expanded expanded = expand_limited(
        {{"templates/meter_a.yaml", applies_to_meter(POINTS, RULES)},
         {"drivers/modbus/line1.yaml", one_device()}},
        limits);
    // The patterns the budget covered, CF-113 once at the first it did not
    // (the review of #179), and the listed name after it; not `q*`.
    EXPECT_EQ(count_of(expanded.lines, "CF-311"), CHECKED);
    EXPECT_EQ(count_of(expanded.lines,
                       "CF-113: the pattern matching of applies_to names "
                       "exceeds its limit of 50000000 units"),
              1);
    EXPECT_EQ(count_of(expanded.lines, "CF-310: point \"nope\""), 1);
    EXPECT_EQ(count_of(expanded.lines, "\"q*\""), 0);
}

// With room for every pattern, the last one is checked too.
TEST(AppliesToLimits, PatternsWithinTheirBudget) {
    constexpr std::size_t FEW = 5;
    ExpandLimits limits;
    limits.applies_to_pattern_work = SMALL_BUDGET;
    const Expanded expanded = expand_limited(
        {{"templates/meter_a.yaml", applies_to_meter(POINTS, FEW)},
         {"drivers/modbus/line1.yaml", one_device()}},
        limits);
    EXPECT_EQ(count_of(expanded.lines, "CF-311"),
              static_cast<std::ptrdiff_t>(FEW) + 1);
    EXPECT_EQ(count_of(expanded.lines, "\"q*\""), 1);
    EXPECT_EQ(count_of(expanded.lines, "CF-113"), 0);
}

// The tree of the review of #179: patterns that match only the last name.
// A budget that runs out refuses the tree, which then cannot pass with
// `nomatch*` unchecked.
TEST(AppliesToLimits, ABudgetThatRunsOutRefusesTheTree) {
    constexpr std::size_t EXPENSIVE = 40;
    ExpandLimits limits;
    limits.applies_to_pattern_work = SMALL_BUDGET;
    const Expanded expanded = expand_limited(
        {{"templates/meter_a.yaml", last_match_meter(POINTS, EXPENSIVE)},
         {"drivers/modbus/line1.yaml", one_device()}},
        limits);
    EXPECT_EQ(count_of(expanded.lines, "CF-113"), 1);
    EXPECT_EQ(count_of(expanded.lines, "\"nomatch*\""), 0);
    EXPECT_GT(expanded.error_count, 0U);
}

// Patterns that each match only the last name, one step on each other: the
// budget runs out only because every visit counts (the review of #179,
// item 2).
TEST(AppliesToLimits, EveryVisitCounts) {
    constexpr std::size_t FEW_POINTS = 2000;
    ExpandLimits limits;
    limits.applies_to_pattern_work = SMALL_BUDGET;
    const Expanded expanded = expand_limited(
        {{"templates/meter_a.yaml", late_patterns_meter(FEW_POINTS)},
         {"drivers/modbus/line1.yaml", one_device()}},
        limits);
    EXPECT_EQ(count_of(expanded.lines, "CF-113"), 1);
}

}  // namespace
}  // namespace scada::conf
