// The time of the inputs the task rules read (D-125) on a large tree: 20,000
// virtual tags of an rt_required task, each summing one pattern over 20,000
// other virtual tags. What a tag reaches through virtual tags is found once
// per tag and once per pattern, over the graph of CF-525; it was copied into
// every reader before, 400,000,000 names here, which the tsan run of #189
// found. A performance test, with release flags and its own time limit.
// Invented examples only.

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

// The bound: about 10 times the time measured with the perf build (D-125),
// far below the time without the fix.
constexpr double MAX_SECONDS = 3.0;

TEST(TaskRulesPerf, ManyReadersOfAPatternOfVirtualTags) {
    constexpr std::size_t TAGS = 20000;
    std::string tags = "tags:\n";
    for (std::size_t i = 0; i < TAGS; ++i) {
        tags += "  - { name: p.i" + std::to_string(i) +
                ".v, type: virtual, task: slow, expr: \"1\" }\n";
    }
    for (std::size_t i = 0; i < TAGS; ++i) {
        tags += "  - { name: q.i" + std::to_string(i) +
                ", type: virtual, task: fast, expr: \"sum(\\\"p.*.v\\\")\" }\n";
    }
    const auto start = std::chrono::steady_clock::now();
    const Result<Validation> validation =
        validated({{"tags/site.yaml", tags},
                   {"control/tasks.yaml",
                    "tasks:\n"
                    "  fast: { period: 10ms, rt_required: true }\n"
                    "  normal: { period: 100ms }\n"
                    "  slow: { period: 1s }\n"},
                   {"system.yaml",
                    std::string(SYSTEM) + "limits: { max_tags: 100000 }\n"}});
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    ASSERT_TRUE(validation.ok());
    EXPECT_EQ(validation.value().report.error_count, 0U);
    RecordProperty("seconds", std::to_string(seconds));
    EXPECT_LT(seconds, MAX_SECONDS);
}

// M2 of the review of #189: a chain of 20,000 virtual tags of an
// rt_required task, each reading the one before, the first a Modbus point.
// Every tag reaches the point (20,000 CF-904); found once per tag, in the
// order of the graph's components. The walk per reader was quadratic.
TEST(TaskRulesPerf, AChainOfVirtualTags) {
    constexpr std::size_t TAGS = 20000;
    std::string tags =
        "tags:\n  - { name: c.i0, type: virtual, task: fast, "
        "expr: \"ps1.m1.flow + 1\" }\n";
    for (std::size_t i = 1; i < TAGS; ++i) {
        tags += "  - { name: c.i" + std::to_string(i) +
                ", type: virtual, task: fast, expr: \"c.i" +
                std::to_string(i - 1) + " + 1\" }\n";
    }
    const auto start = std::chrono::steady_clock::now();
    const Result<Validation> validation = validated(
        {{"tags/site.yaml", tags},
         {"templates/meter_a.yaml",
          "template: meter_a\nprotocol: modbus\npoints:\n"
          "  - { name: flow, address: { table: holding, register: 1, type: "
          "uint16 } }\n"},
         {"drivers/modbus/line1.yaml", one_device()},
         {"control/tasks.yaml",
          "tasks:\n"
          "  fast: { period: 10ms, rt_required: true }\n"
          "  normal: { period: 100ms }\n"
          "  slow: { period: 1s }\n"},
         {"system.yaml",
          std::string(SYSTEM) + "limits: { max_tags: 100000 }\n"}});
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    ASSERT_TRUE(validation.ok());
    EXPECT_EQ(validation.value().report.error_count, TAGS);
    RecordProperty("seconds", std::to_string(seconds));
    EXPECT_LT(seconds, MAX_SECONDS);
}

}  // namespace
}  // namespace scada::conf
