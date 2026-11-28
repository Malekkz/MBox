// The work of the health rules' patterns of stage 5 at sizes the default
// limits allow (D-120; M4 of the review of #174): tens of thousands of
// points and a thousand patterns, so an executable of its own with its own
// time limit. Invented examples only.

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

// 20,000 points named `x`, 59 `a` and a number, and 1,000 health rules
// whose pattern `*` + 40 `a` + `b` matches none of them, each after about
// 2,600 steps per name: about 5e10 steps unbounded. Only `w` + 40 `a` + `b`
// matches, a point in the health point's group: it serves, so it is not
// compared, and as the first name it keeps CF-311 (D-116, #176) cheap. The
// budget stops the patterns after about 20 rules. A rule that lists a name
// is still checked; a pattern after the budget is not.
TEST(PointLimits, HealthPatternsStopAtTheirBudget) {
    constexpr std::size_t POINTS = 20000;
    constexpr std::size_t RULES = 1000;
    constexpr std::size_t STEM = 59;     // `x` and 59 `a`
    constexpr std::size_t PATTERN = 40;  // `*`, 40 `a`, `b`
    const std::string stem = "x" + std::string(STEM, 'a');
    std::string meter = "template: meter_a\nprotocol: modbus\npoints:\n";
    for (std::size_t i = 0; i < POINTS; ++i) {
        meter +=
            "  - { name: " + stem + std::to_string(i) +
            ", address: { table: holding, register: " + std::to_string(i + 1) +
            ", type: uint16 } }\n";
    }
    meter +=
        "  - { name: w" + std::string(PATTERN, 'a') +
        "b, poll_group: fast, address: { table: holding, register: 29999, "
        "type: uint16 } }\n"
        "  - { name: h, poll_group: fast, address: { table: holding, "
        "register: 30000, type: uint16 } }\n"
        "  - { name: zz, address: { table: holding, register: 30001, type: "
        "uint16 } }\n"
        "health:\n";
    const std::string pattern = "\"*" + std::string(PATTERN, 'a') + "b\"";
    for (std::size_t i = 0; i < RULES; ++i) {
        meter +=
            "  - { point: h, good_when: 0, applies_to: [ " + pattern + " ] }\n";
    }
    meter +=
        "  - { point: h, good_when: 0, applies_to: [ zz ] }\n"
        "  - { point: h, good_when: 0, applies_to: [ \"z*\" ] }\n";

    const auto start = std::chrono::steady_clock::now();
    const Result<Validation> validation =
        validated({{"templates/meter_a.yaml", meter},
                   {"drivers/modbus/line1.yaml", one_device()}});
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    ASSERT_TRUE(validation.ok());
    Lines errors;
    for (const Diagnostic& error : validation.value().report.errors) {
        errors.push_back(format(error));
    }
    // The listed name is checked; the pattern after the budget is not.
    EXPECT_EQ(errors,
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-612: point "
                     "\"zz\": \"h\" must be in poll group \"normal\"\n"
                     "  from template templates/meter_a.yaml:21008:14 "
                     "(point zz)"}));
    // Recorded for D-120; the executable's time limit is the bound.
    RecordProperty("seconds", std::to_string(seconds));
}

}  // namespace
}  // namespace scada::conf
