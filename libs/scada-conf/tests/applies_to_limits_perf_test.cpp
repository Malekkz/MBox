// The time of `applies_to` patterns for CF-311 at stage 5 at the default
// budget (D-116; #176): 20,000 points and 1,000 patterns that match none,
// about 5e10 matcher steps unbounded, which took 51.4 s in release before
// the budget counted steps. A performance test, with release flags and its
// own time limit. Invented examples only.

#include "applies_to_tree.h"
#include "expand_cases.h"
#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/validate.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace scada::conf {
namespace {

// The bound, with room for a slower runner (D-116 gives the measured time).
constexpr double MAX_SECONDS = 10.0;

TEST(AppliesToLimitsPerf, PatternsAtTheDefaultBudget) {
    constexpr std::size_t POINTS = 20000;
    constexpr std::size_t RULES = 1000;
    const std::string meter = applies_to_meter(POINTS, RULES);
    const auto start = std::chrono::steady_clock::now();
    const Result<Validation> validation =
        validated({{"templates/meter_a.yaml", meter},
                   {"drivers/modbus/line1.yaml", one_device()}});
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    ASSERT_TRUE(validation.ok());
    bool named = false;
    for (const Diagnostic& error : validation.value().report.errors) {
        named = named || format(error).find("\"nope\"") != std::string::npos;
    }
    // The listed name is checked whatever the budget, and the budget that
    // runs out is CF-113 (the review of #179).
    EXPECT_TRUE(named);
    bool refused = false;
    for (const Diagnostic& error : validation.value().report.errors) {
        refused = refused || format(error).find("CF-113") != std::string::npos;
    }
    EXPECT_TRUE(refused);
    RecordProperty("seconds", std::to_string(seconds));
    EXPECT_LT(seconds, MAX_SECONDS);
}

// The errors of `meter` with one device, and the seconds it took.
std::pair<Lines, double> timed(const std::string& meter) {
    const auto start = std::chrono::steady_clock::now();
    const Result<Validation> validation =
        validated({{"templates/meter_a.yaml", meter},
                   {"drivers/modbus/line1.yaml", one_device()}});
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    EXPECT_TRUE(validation.ok());
    Lines errors;
    if (validation.ok()) {
        for (const Diagnostic& error : validation.value().report.errors) {
            errors.push_back(format(error));
        }
    }
    return {errors, seconds};
}

bool has(const Lines& lines, std::string_view text) {
    return std::ranges::any_of(lines, [text](const std::string& line) {
        return line.find(text) != std::string::npos;
    });
}

// Patterns that each match only the last name, one step on each other: the
// budget runs out because every visit counts, and CF-113 refuses the tree
// (the review of #179, items 1 and 2).
TEST(AppliesToLimitsPerf, LatePatternsAreCountedToo) {
    constexpr std::size_t POINTS = 20000;
    const auto [errors, seconds] = timed(late_patterns_meter(POINTS));
    EXPECT_TRUE(has(errors, "CF-113"));
    RecordProperty("seconds", std::to_string(seconds));
    EXPECT_LT(seconds, MAX_SECONDS);
}

// The reviewer's tree: 40 patterns that match only the last of 20,000
// points, then `nomatch*`. CF-113, not a tree accepted unchecked.
TEST(AppliesToLimitsPerf, TheReviewersTree) {
    constexpr std::size_t POINTS = 20000;
    constexpr std::size_t RULES = 40;
    const auto [errors, seconds] = timed(last_match_meter(POINTS, RULES));
    EXPECT_TRUE(has(errors, "CF-113"));
    RecordProperty("seconds", std::to_string(seconds));
    EXPECT_LT(seconds, MAX_SECONDS);
}

}  // namespace
}  // namespace scada::conf
