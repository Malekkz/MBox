// The time of the health rules' patterns of stage 5 at the default budget
// (D-120; M4 of the review of #174): 20,000 points and 1,000 patterns that
// match none, about 5e10 matcher steps unbounded. A performance test, with
// release flags and its own time limit. Invented examples only.

#include "expand_cases.h"
#include "health_patterns_tree.h"
#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/validate.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>

namespace scada::conf {
namespace {

// The bound: the budget is worth about 0.75 s in release on x86-64 (D-120),
// the tree about 0.5 s without it; this leaves room for a slower runner.
constexpr double MAX_SECONDS = 10.0;

TEST(PointLimitsPerf, HealthPatternsAtTheDefaultBudget) {
    constexpr std::size_t POINTS = 20000;
    constexpr std::size_t RULES = 1000;
    const std::string meter = health_patterns_meter(POINTS, RULES);
    const auto start = std::chrono::steady_clock::now();
    const Result<Validation> validation =
        validated({{"templates/meter_a.yaml", meter},
                   {"drivers/modbus/line1.yaml", one_device()}});
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    ASSERT_TRUE(validation.ok());
    Lines errors;
    for (const Diagnostic& error : validation.value().report.errors) {
        errors.push_back(format(error));
    }
    // The listed name is checked; the pattern after the budget is not.
    EXPECT_EQ(errors, Lines{health_patterns_zz_error(POINTS, RULES)});
    RecordProperty("seconds", std::to_string(seconds));
    EXPECT_LT(seconds, MAX_SECONDS);
}

}  // namespace
}  // namespace scada::conf
