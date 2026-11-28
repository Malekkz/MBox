// The time of the Modbus address checks of stage 5 (D-122) on a large
// tree: 40,000 points of 128 registers each in one device, on distinct
// registers of 80 units, so that every register claim goes through the
// sweeps of CF-602 and CF-601. A performance test, with release flags and
// its own time limit. Invented examples only.

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

// The bound, with room for a slower runner (D-122 gives the measured time).
constexpr double MAX_SECONDS = 10.0;

// The bound of ManyWritablePoints: about 3 times the time measured with
// the fix (D-122), well under the 24.9 s the review measured without it.
constexpr double MAX_WRITABLE_SECONDS = 15.0;

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

// CF-615 reads each point's own writes (M2 of the review of #180): a
// template of 32,768 `rw` holding registers and 32,768 `rw` coils with a
// `functions` list, at three instances. Before the fix every point scanned
// all the device's writes.
TEST(ModbusRulesPerf, ManyWritablePoints) {
    constexpr std::size_t EACH = 32768;
    // `access` and `storage` come from `defaults`, and the lines are short,
    // so that the template, about 4.3 MB, stays well under the file limit
    // of 8 MiB (contract 1.3; an item of the delta check of #180).
    std::string meter =
        "template: meter_a\nprotocol: modbus\n"
        "functions: [1, 3, 5, 6, 15, 16]\n"
        "defaults: {access: rw, storage: volatile}\npoints:\n";
    for (std::size_t i = 0; i < EACH; ++i) {
        meter += "- {name: h" + std::to_string(i) +
                 ", address: {table: holding, register: " + std::to_string(i) +
                 ", type: uint16}}\n";
        meter += "- {name: c" + std::to_string(i) +
                 ", address: {table: coil, register: " + std::to_string(i) +
                 "}}\n";
    }
    EXPECT_LT(meter.size(), std::size_t{5} << 20U);  // 3 MiB below the limit
    Files files = {
        {"templates/meter_a.yaml", meter},
        {"drivers/modbus/line1.yaml",
         channel("  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n"
                 "  - { name: ps1.m2, template: meter_a, unit_id: 2 }\n"
                 "  - { name: ps1.m3, template: meter_a, unit_id: 3 }\n")}};
    // 3 x 65,536 point tags and their signals pass the default 50,000.
    files.emplace("system.yaml",
                  std::string(SYSTEM) + "limits: { max_tags: 1000000 }\n");
    const auto start = std::chrono::steady_clock::now();
    const Result<Validation> validation = validated(files);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    ASSERT_TRUE(validation.ok());
    EXPECT_EQ(validation.value().report.error_count, 0U);
    for (const Diagnostic& error : validation.value().report.errors) {
        ADD_FAILURE() << format(error);
    }
    RecordProperty("seconds", std::to_string(seconds));
    EXPECT_LT(seconds, MAX_WRITABLE_SECONDS);
}

}  // namespace
}  // namespace scada::conf
