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

#include <chrono>
#include <cstddef>
#include <string>

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
    // The listed name is checked whatever the budget.
    EXPECT_TRUE(named);
    RecordProperty("seconds", std::to_string(seconds));
    EXPECT_LT(seconds, MAX_SECONDS);
}

// Many patterns each matched at once: the visits alone, all counted.
TEST(AppliesToLimitsPerf, CheapPatternsAreCountedToo) {
    constexpr std::size_t POINTS = 20000;
    const std::string meter = cheap_patterns_meter(POINTS);
    const auto start = std::chrono::steady_clock::now();
    const Result<Validation> validation =
        validated({{"templates/meter_a.yaml", meter},
                   {"drivers/modbus/line1.yaml", one_device()}});
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    ASSERT_TRUE(validation.ok());
    EXPECT_EQ(validation.value().report.error_count, 0U);
    RecordProperty("seconds", std::to_string(seconds));
    EXPECT_LT(seconds, MAX_SECONDS);
}

}  // namespace
}  // namespace scada::conf
