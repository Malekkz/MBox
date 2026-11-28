// The time of the Modbus address checks of stage 5 (D-122) on a large
// tree: 40,000 points of 128 registers each in one device, on distinct
// registers of 80 units, so that every register claim goes through the
// sweeps of CF-602 and CF-601. A performance test, with release flags and
// its own time limit. Invented examples only.

#include "expand_cases.h"
#include "scada/common/result.h"
#include "scada/conf/validate.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>

namespace scada::conf {
namespace {

// The bound, with room for a slower runner (D-122 gives the measured time).
constexpr double MAX_SECONDS = 10.0;

TEST(ModbusRulesPerf, ManyLongValues) {
    constexpr std::size_t POINTS = 40000;
    constexpr std::size_t PER_UNIT = 500;   // 500 x 128 registers per unit
    constexpr std::size_t REGISTERS = 128;  // a string of 256 bytes
    std::string meter =
        "template: meter_a\nprotocol: modbus\nbit_order: lsb0\npoints:\n";
    for (std::size_t i = 0; i < POINTS; ++i) {
        meter += "  - { name: s" + std::to_string(i) +
                 ", address: { table: holding, register: " +
                 std::to_string((i % PER_UNIT) * REGISTERS) +
                 ", type: string, length: 256, unit_id: " +
                 std::to_string(i / PER_UNIT + 1) + " } }\n";
    }
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
