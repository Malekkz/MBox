// Stage 5 on merged points (decision D-115): the cases of the review of
// #149, one test per finding.

#include "expand_cases.h"
#include "grouped_errors.h"
#include "scada/conf/diagnostic.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::conf {
namespace {

// The errors of `lines`, without the expanded points.
Lines errors(const Lines& lines) {
    Lines found;
    for (const std::string& line : lines) {
        if (line.find(": error ") != std::string::npos) {
            found.push_back(line);
        }
    }
    return found;
}

// meter_a of `points`, with `params`, and one instance with `more`.
Lines meter(std::string_view points, std::string_view more = {},
            std::string_view params = {}) {
    return errors(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\nbit_order: lsb0\n" +
                     std::string(params) + "points:\n" + std::string(points)},
                {"drivers/modbus/line1.yaml", one_device(more)}}));
}

// M1: a divisor of 1 in any written form is an integer scaling, as stage 3
// reads it, in a template and in a device of its own points.
TEST(MergedPoints, ADivisorOfOneInAnyForm) {
    EXPECT_EQ(meter("  - { name: p, value_type: int64, scale: { divide_by: "
                    "0x1 }, address: { table: input, register: 10, type: "
                    "int32 } }\n"),
              Lines{});
    EXPECT_EQ(meter("  - { name: p, scale: { divide_by: 0x1, offset: 1 }, "
                    "address: { table: input, register: 10, type: int64 } "
                    "}\n"),
              Lines{"drivers/modbus/line1.yaml:7:11: error CF-516: "
                    "\"ps1.m1.p\": raw x gain + offset can exceed int64\n"
                    "  from template templates/meter_a.yaml:5:23 (point p)"});
    EXPECT_EQ(meter("  - { name: p, value_type: int64, scale: { divide_by: "
                    "1 }, address: { table: input, register: 10, type: "
                    "int32 } }\n"),
              Lines{});
}

// M2: the scale and value-type checks hold for every protocol's point.
TEST(MergedPoints, ScalingOfAnIoPoint) {
    EXPECT_EQ(
        errors(expand(
            {{"templates/sensor_a.yaml",
              "template: sensor_a\nprotocol: io\npoints:\n"
              "  - { name: level, value_type: int64, address: { channel: "
              "in_voltage0 } }\n"},
             {"drivers/io/local.yaml",
              "channel: { name: local, backend: iio, device: adc0 }\n"
              "devices:\n  - name: ps1.s1\n    template: sensor_a\n"
              "    overrides: { level: { scale: { divide_by: 10 } } }\n"}})),
        Lines{"drivers/io/local.yaml:5:47: error CF-517: \"ps1.s1.level\": "
              "value_type int64 needs an integer scaling; use float64"});
}

// M3: a computed bit field is checked as a whole: its ends in order.
TEST(MergedPoints, AComputedBitField) {
    const std::string params = "params: { b: { type: int, default: 9 } }\n";
    EXPECT_EQ(meter("  - { name: p, address: { table: input, register: 10, "
                    "type: uint16, field: [\"b\", 3] } }\n",
                    {}, params),
              Lines{"drivers/modbus/line1.yaml:7:11: error CF-204: "
                    "address.field[0] must be between 0 and 3\n  from "
                    "template templates/meter_a.yaml:6:77 (point p)"});
    EXPECT_EQ(meter("  - { name: p, address: { table: input, register: 10, "
                    "type: uint16 }, quality_from: [ { point: self, field: "
                    "[\"b\", 3], map: { 0: good, default: bad } } ] }\n",
                    {}, params),
              Lines{"drivers/modbus/line1.yaml:7:11: error CF-204: "
                    "quality_from[0].field[0] must be between 0 and 3\n  "
                    "from template templates/meter_a.yaml:6:110 (point p)"});
}

// M4: the shapes of a double point and the registers of a write-only point.
TEST(MergedPoints, ShapesAndWriteOnlyRegisters) {
    EXPECT_EQ(meter("  - { name: p, kind: double_point, address: { on: { "
                    "table: holding, register: 60, bit: 4 } } }\n"),
              Lines{"drivers/modbus/line1.yaml:7:11: error CF-202: the "
                    "address of \"ps1.m1.p\" needs off\n  from template "
                    "templates/meter_a.yaml:5:45 (point p)"});
    EXPECT_EQ(meter("  - { name: p, kind: double_point, address: { fault: { "
                    "table: holding, register: 60, bit: 6 } } }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-202: the "
                     "address of \"ps1.m1.p\" needs on\n  from template "
                     "templates/meter_a.yaml:5:45 (point p)",
                     "drivers/modbus/line1.yaml:7:11: error CF-202: the "
                     "address of \"ps1.m1.p\" needs off\n  from template "
                     "templates/meter_a.yaml:5:45 (point p)"}));
    EXPECT_EQ(meter("  - { name: p, access: wo, storage: volatile, address: "
                    "{ write: { on: { value: 1 }, off: { value: 0 } } } }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-202: write.on "
                     "of \"ps1.m1.p\" needs table\n  from template "
                     "templates/meter_a.yaml:5:71 (point p)",
                     "drivers/modbus/line1.yaml:7:11: error CF-202: write.on "
                     "of \"ps1.m1.p\" needs register\n  from template "
                     "templates/meter_a.yaml:5:71 (point p)",
                     "drivers/modbus/line1.yaml:7:11: error CF-202: write.off "
                     "of \"ps1.m1.p\" needs table\n  from template "
                     "templates/meter_a.yaml:5:90 (point p)",
                     "drivers/modbus/line1.yaml:7:11: error CF-202: write.off "
                     "of \"ps1.m1.p\" needs register\n  from template "
                     "templates/meter_a.yaml:5:90 (point p)"}));
}

// M5: a point whose computed value is wrong gets no further check.
TEST(MergedPoints, AWrongComputedValueStopsThePoint) {
    EXPECT_EQ(meter("  - { name: p, value_type: int64, address: { table: "
                    "input, register: 10, type: uint16, field: [0, \"k\"] } "
                    "}\n",
                    {}, "params: { k: { type: int, default: 64 } }\n"),
              Lines{"drivers/modbus/line1.yaml:7:11: error CF-204: "
                    "address.field[1] must be between 0 and 63\n  from "
                    "template templates/meter_a.yaml:6:99 (point p)"});
}

// M6: one cause met at many instances is one error; an instance's own
// default that is wrong at every point is one error.
TEST(MergedPoints, OneCauseOneError) {
    std::string devices;
    for (int unit = 1; unit <= 3; ++unit) {
        devices += "  - { name: ps1.m" + std::to_string(unit) +
                   ", template: meter_a, unit_id: " + std::to_string(unit) +
                   " }\n";
    }
    EXPECT_EQ(errors(expand({{"templates/meter_a.yaml",
                              "template: meter_a\nprotocol: modbus\npoints:\n"
                              "  - { name: a, value_type: int64, address: { "
                              "table: input, register: 1, type: float32 } }\n"
                              "  - { name: b, value_type: int64, address: { "
                              "table: input, register: 3, type: float32 } }\n"},
                             {"drivers/modbus/line1.yaml", channel(devices)}})),
              (Lines{"drivers/modbus/line1.yaml:7:13: error CF-504: "
                     "\"ps1.m1.a\": value_type int64 cannot hold float32 (and "
                     "2 more instances: ps1.m2, ps1.m3)\n  from template "
                     "templates/meter_a.yaml:4:28 (point a)",
                     "drivers/modbus/line1.yaml:7:13: error CF-504: "
                     "\"ps1.m1.b\": value_type int64 cannot hold float32 (and "
                     "2 more instances: ps1.m2, ps1.m3)\n  from template "
                     "templates/meter_a.yaml:5:28 (point b)"}));
    EXPECT_EQ(meter("  - { name: a, address: { table: input, register: 1, "
                    "type: uint16 } }\n"
                    "  - { name: b, address: { table: input, register: 2, "
                    "type: uint16 } }\n",
                    "    params: { k: 99 }\n"
                    "    defaults: { scale: { divide_by: \"k - 99\" } }\n",
                    "params: { k: { type: int, default: 1 } }\n"),
              Lines{"drivers/modbus/line1.yaml:11:37: error CF-204: "
                    "scale.divide_by must be between 1 and any"});
}

// M7: the read ranges' ends and a health rule's bit field are evaluated
// at stage 4 and checked here.
TEST(MergedPoints, ReadRangesAndHealthFields) {
    const std::string points =
        "  - { name: p, address: { table: holding, register: 1, type: uint16 "
        "} }\n";
    EXPECT_EQ(meter(points, {},
                    "read: { ranges: [ { kind: no_read, table: holding, "
                    "first: \"nosuchname + 1\", last: 10 } ] }\n"),
              Lines{"drivers/modbus/line1.yaml:7:11: error CF-410: expression "
                    "\"nosuchname + 1\": \"nosuchname\" is not a parameter and "
                    "no variable bound to this point\n  from template "
                    "templates/meter_a.yaml:4:60 (read)"});
    EXPECT_EQ(meter(points, {},
                    "params: { b: { type: int, default: 1 } }\n"
                    "read: { ranges: [ { kind: no_read, table: holding, "
                    "first: \"b + 70000\", last: 10 } ] }\n"
                    "health: [ { point: p, good_when: { field: [\"b * 70\", "
                    "3], equals: 1 }, applies_to: all } ]\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-204: "
                     "read.ranges[0].first must be between 0 and 65536\n  "
                     "from template templates/meter_a.yaml:5:59 (read)",
                     "drivers/modbus/line1.yaml:7:11: error CF-204: "
                     "health[0].good_when.field[0] must be between 0 and "
                     "63\n  from template templates/meter_a.yaml:6:44 "
                     "(health)"}));
}

// S2: a device's own key has no point in its note.
TEST(MergedPoints, ADevicesOwnKeyNamesNoPoint) {
    EXPECT_EQ(meter("  - { name: p, address: { table: holding, register: 1, "
                    "type: uint16 } }\n",
                    {}, "device_time: { zone: local }\n"),
              Lines{"drivers/modbus/line1.yaml:7:11: error CF-202: device_time "
                    "of \"ps1.m1\" needs tz\n  from template "
                    "templates/meter_a.yaml:4:14"});
}

// A point of `keys` (inside its braces after the name) in meter_a.
Lines point(std::string_view keys, std::string_view params = {}) {
    return meter("  - { name: p, " + std::string(keys) + " }\n", {}, params);
}

// S1: CF-504 where 4.7 decides that a value type cannot hold the raw type,
// and the pairs it holds.
TEST(MergedPoints, WhatAValueTypeCanHold) {
    const std::vector<std::pair<std::string_view, std::string_view>> rows = {
        {"value_type: int64, address: { table: input, register: 1, type: "
         "string, length: 4 }",
         "drivers/modbus/line1.yaml:7:11: error CF-504: \"ps1.m1.p\": "
         "value_type int64 cannot hold string\n  from template "
         "templates/meter_a.yaml:5:28 (point p)"},
        {"enum: { 0: a, 1: b }, address: { table: input, register: 1, type: "
         "bytes, length: 4 }",
         "drivers/modbus/line1.yaml:7:11: error CF-504: \"ps1.m1.p\": "
         "value_type int64 (derived from the enumeration) cannot hold bytes\n  "
         "from template templates/meter_a.yaml:5:47 (point p)"},
        {"value_type: bool, address: { table: input, register: 1, type: uint16 "
         "}",
         "drivers/modbus/line1.yaml:7:11: error CF-504: \"ps1.m1.p\": "
         "value_type bool cannot hold uint16\n  from template "
         "templates/meter_a.yaml:5:28 (point p)"},
        {"value_type: string, address: { table: input, register: 1, type: "
         "uint16 }",
         "drivers/modbus/line1.yaml:7:11: error CF-504: \"ps1.m1.p\": "
         "value_type string cannot hold uint16\n  from template "
         "templates/meter_a.yaml:5:28 (point p)"},
        {"value_type: float64, address: { table: input, register: 1, type: "
         "string, length: 4 }",
         "drivers/modbus/line1.yaml:7:11: error CF-504: \"ps1.m1.p\": "
         "value_type float64 cannot hold string\n  from template "
         "templates/meter_a.yaml:5:28 (point p)"},
        {"value_type: string, address: { table: discrete, register: 1 }",
         "drivers/modbus/line1.yaml:7:11: error CF-504: \"ps1.m1.p\": "
         "value_type string cannot hold bool\n  from template "
         "templates/meter_a.yaml:5:28 (point p)"},
        {"value_type: string, address: { table: coil, register: 1 }",
         "drivers/modbus/line1.yaml:7:11: error CF-504: \"ps1.m1.p\": "
         "value_type string cannot hold bool\n  from template "
         "templates/meter_a.yaml:5:28 (point p)"},
        {"value_type: bool, address: { table: holding, register: 1, bit: 3, "
         "type: uint16 }",
         ""},
        {"value_type: float64, address: { table: input, register: 1, type: "
         "int16 }",
         ""},
        {"value_type: string, address: { table: input, register: 1, type: "
         "string, length: 4 }",
         ""},
        {"kind: counter, counter: { limit: 100, at_limit: wrap }, address: { "
         "table: input, register: 1, type: float32 }",
         "drivers/modbus/line1.yaml:7:11: error CF-504: \"ps1.m1.p\": a "
         "counter cannot hold float32\n  from template "
         "templates/meter_a.yaml:5:40 (point p)"},
    };
    for (const auto& [keys, error] : rows) {
        EXPECT_EQ(point(keys),
                  error.empty() ? Lines{} : Lines{std::string(error)})
            << keys;
    }
}

// S5: the cache of checked values keys on the path and on the label; two
// instances may give one key different values.
TEST(MergedPoints, TheCacheOfCheckedValues) {
    const std::string params = "params: { k: { type: int, default: 16 } }\n";
    EXPECT_EQ(point("address: { table: holding, register: \"k\", bit: \"k\", "
                    "type: uint16 }",
                    params),
              Lines{"drivers/modbus/line1.yaml:7:11: error CF-204: "
                    "address.bit must be between 0 and 15\n  from template "
                    "templates/meter_a.yaml:6:63 (point p)"});
    EXPECT_EQ(point("address: { table: input, register: 1, type: int64, "
                    "field: [\"k * 4\", \"k * 4\"] }",
                    params),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-204: "
                     "address.field[0] must be between 0 and 63\n  from "
                     "template templates/meter_a.yaml:6:75 (point p)",
                     "drivers/modbus/line1.yaml:7:11: error CF-204: "
                     "address.field[1] must be between 0 and 63\n  from "
                     "template templates/meter_a.yaml:6:84 (point p)"}));
    EXPECT_EQ(errors(expand(
                  {{"templates/meter_a.yaml",
                    "template: meter_a\nprotocol: modbus\n" + params +
                        "points:\n  - { name: p, address: { table: holding, "
                        "register: \"k\", type: uint16 } }\n"},
                   {"drivers/modbus/line1.yaml",
                    channel("  - { name: ps1.m1, template: meter_a, unit_id: "
                            "1 }\n  - { name: ps1.m2, template: meter_a, "
                            "unit_id: 2, params: { k: 70000 } }\n")}})),
              Lines{"drivers/modbus/line1.yaml:8:13: error CF-204: "
                    "address.register must be between 0 and 65536\n  from "
                    "template templates/meter_a.yaml:5:53 (point p)"});
}

// S5: the edges of CF-516: the sign of gain and offset, the int64 ends,
// the overflow of the product, a field of 64 bits (the widest a bit field
// can be: bits 0 to 63), the top of mod10k.
TEST(MergedPoints, TheEdgesOfAnIntegerScaling) {
    const std::vector<std::pair<std::string_view, std::string_view>> rows = {
        {"scale: { gain: -4294967296 }, address: { table: input, register: 1, "
         "type: int32 }",
         "drivers/modbus/line1.yaml:7:11: error CF-516: \"ps1.m1.p\": raw x "
         "gain + offset can exceed int64\n  from template "
         "templates/meter_a.yaml:5:23 (point p)"},
        {"scale: { offset: -1 }, address: { table: input, register: 1, type: "
         "int64 }",
         "drivers/modbus/line1.yaml:7:11: error CF-516: \"ps1.m1.p\": raw x "
         "gain + offset can exceed int64\n  from template "
         "templates/meter_a.yaml:5:23 (point p)"},
        {"value_type: uint64, scale: { gain: -1 }, address: { table: input, "
         "register: 1, type: uint16 }",
         "drivers/modbus/line1.yaml:7:11: error CF-516: \"ps1.m1.p\": raw x "
         "gain + offset can exceed uint64\n  from template "
         "templates/meter_a.yaml:5:43 (point p)"},
        {"scale: { gain: 2 }, address: { table: input, register: 1, type: "
         "int64, field: [0, 63] }",
         "drivers/modbus/line1.yaml:7:11: error CF-516: \"ps1.m1.p\": raw x "
         "gain + offset can exceed int64\n  from template "
         "templates/meter_a.yaml:5:23 (point p)"},
        {"scale: { gain: 1000 }, address: { table: input, register: 1, type: "
         "mod10k, count: 4 }",
         "drivers/modbus/line1.yaml:7:11: error CF-516: \"ps1.m1.p\": raw x "
         "gain + offset can exceed int64\n  from template "
         "templates/meter_a.yaml:5:23 (point p)"},
        {"scale: { gain: 100 }, address: { table: input, register: 1, type: "
         "mod10k, count: 4 }",
         ""},
        {"scale: { gain: 4294967296 }, address: { table: input, register: 1, "
         "type: int32, field: [0, 30] }",
         ""},
    };
    for (const auto& [keys, error] : rows) {
        EXPECT_EQ(point(keys),
                  error.empty() ? Lines{} : Lines{std::string(error)})
            << keys;
    }
}

// S5: each key that a merged map must have (CF-202). The two keys of an
// epoch are in `MergedPoints.IncompleteMergedMaps`.
TEST(MergedPoints, EveryKeyAMergedMapNeeds) {
    const std::vector<std::pair<std::string_view, std::string_view>> rows = {
        {"exponent_from: { point: q }, address: { table: input, register: 1, "
         "type: int16 }",
         "drivers/modbus/line1.yaml:7:11: error CF-202: exponent_from of "
         "\"ps1.m1.p\" needs direction\n  from template "
         "templates/meter_a.yaml:5:31 (point p)"},
        {"address: { table: input, register: 1, type: uint16 }, bits: { 0: { "
         "invert: true } }",
         "drivers/modbus/line1.yaml:7:11: error CF-202: bit 0 of \"ps1.m1.p\" "
         "needs name\n  from template templates/meter_a.yaml:5:81 (point p)"},
        {"on_invalid: { replace: true }, address: { table: input, register: 1, "
         "type: uint16 }",
         "drivers/modbus/line1.yaml:7:11: error CF-202: on_invalid of "
         "\"ps1.m1.p\" needs substitute\n  from template "
         "templates/meter_a.yaml:5:28 (point p)"},
        {"address: { table: input, register: 1, type: string }",
         "drivers/modbus/line1.yaml:7:11: error CF-202: the address of "
         "\"ps1.m1.p\" needs length\n  from template "
         "templates/meter_a.yaml:5:25 (point p)"},
        {"address: { table: input, register: 1, type: mod10k }",
         "drivers/modbus/line1.yaml:7:11: error CF-202: the address of "
         "\"ps1.m1.p\" needs count\n  from template "
         "templates/meter_a.yaml:5:25 (point p)"},
        {"address: { table: input, register: 1, type: datetime }",
         "drivers/modbus/line1.yaml:7:11: error CF-202: the address of "
         "\"ps1.m1.p\" needs fields\n  from template "
         "templates/meter_a.yaml:5:25 (point p)"},
        {"address: { table: input, type: uint16 }",
         "drivers/modbus/line1.yaml:7:11: error CF-202: the address of "
         "\"ps1.m1.p\" needs register\n  from template "
         "templates/meter_a.yaml:5:25 (point p)"},
        {"address: { register: 1, type: uint16 }",
         "drivers/modbus/line1.yaml:7:11: error CF-202: the address of "
         "\"ps1.m1.p\" needs table\n  from template "
         "templates/meter_a.yaml:5:25 (point p)"},
    };
    for (const auto& [keys, error] : rows) {
        EXPECT_EQ(point(keys),
                  error.empty() ? Lines{} : Lines{std::string(error)})
            << keys;
    }
}

// The kept limit of the grouping: the diagnostic at the limit is counted,
// not kept, and a repeated text is dropped (S5 of the review of #149).
TEST(MergedPoints, TheKeptLimitOfTheGrouping) {
    const auto diagnostic = [](std::uint16_t code, std::string message) {
        Diagnostic made;
        made.code = code;
        made.location = Location{.file = "f.yaml", .line = 1, .column = 1};
        made.message = std::move(message);
        return made;
    };
    Diagnostics diagnostics;
    GroupedErrors grouped(2);
    grouped.add(diagnostic(204, "a"), {}, "d1", diagnostics);
    grouped.add(diagnostic(204, "a"), {}, "d1", diagnostics);
    grouped.add(diagnostic(204, "b"), {}, "d1", diagnostics);
    grouped.add(diagnostic(204, "c"), {}, "d1", diagnostics);
    grouped.flush(diagnostics);
    Report report;
    diagnostics.move_into(report);
    ASSERT_EQ(report.errors.size(), 2U);
    EXPECT_EQ(report.errors[0].message, "a");
    EXPECT_EQ(report.errors[1].message, "b");
    EXPECT_EQ(report.error_count, 3U);
}

// S5: the range of a bit field (one bit, a few, all 64), a product past 64
// bits, a point refused once, and the derived type of a double point.
TEST(MergedPoints, BitFieldRangesAndOneErrorAPoint) {
    const std::vector<std::pair<std::string_view, std::string_view>> rows = {
        // A 64-bit field of uint64 is uint64's range: never a shift by 64.
        {"scale: { offset: 1 }, address: { table: input, register: 1, type: "
         "uint64, field: [0, 63] }",
         "drivers/modbus/line1.yaml:7:11: error CF-516: \"ps1.m1.p\": "
         "raw x gain + offset can exceed uint64\n  from template "
         "templates/meter_a.yaml:5:23 (point p)"},
        // The product overflows 64 bits.
        {"scale: { gain: 2 }, address: { table: input, register: 1, type: "
         "uint64 }",
         "drivers/modbus/line1.yaml:7:11: error CF-516: \"ps1.m1.p\": "
         "raw x gain + offset can exceed uint64\n  from template "
         "templates/meter_a.yaml:5:23 (point p)"},
        // A field's range, not its raw type's: 15 x 2^59 fits int64.
        {"scale: { gain: 576460752303423488 }, address: { table: input, "
         "register: 1, type: uint16, field: [0, 3] }",
         ""},
        {"scale: { gain: 576460752303423488 }, address: { table: input, "
         "register: 1, type: uint16 }",
         "drivers/modbus/line1.yaml:7:11: error CF-516: \"ps1.m1.p\": "
         "raw x gain + offset can exceed int64\n  from template "
         "templates/meter_a.yaml:5:23 (point p)"},
        // A field of one bit: 0 to 1.
        {"scale: { offset: 9223372036854775807 }, address: { table: input, "
         "register: 1, type: uint16, field: [5, 5] }",
         "drivers/modbus/line1.yaml:7:11: error CF-516: \"ps1.m1.p\": "
         "raw x gain + offset can exceed int64\n  from template "
         "templates/meter_a.yaml:5:23 (point p)"},
        {"scale: { offset: 9223372036854775806 }, address: { table: input, "
         "register: 1, type: uint16, field: [5, 5] }",
         ""},
        // One error for a point refused: no CF-517 after its CF-504.
        {"kind: counter, enum: { 0: a, 1: b }, scale: { gain: 0.5 }, "
         "address: { table: input, register: 1, type: float32 }",
         "drivers/modbus/line1.yaml:7:11: error CF-504: \"ps1.m1.p\": "
         "a counter cannot hold float32\n  from template "
         "templates/meter_a.yaml:5:22 (point p)"},
        {"kind: double_point, states: { 0: 0, 1: 1, 2: 2, 3: 3 }, address: { "
         "table: input, register: 1, type: string, length: 4 }",
         "drivers/modbus/line1.yaml:7:11: error CF-504: \"ps1.m1.p\": "
         "value_type int64 (derived from kind double_point) cannot hold "
         "string\n  from template templates/meter_a.yaml:5:81 (point p)"},
    };
    for (const auto& [keys, error] : rows) {
        EXPECT_EQ(point(keys),
                  error.empty() ? Lines{} : Lines{std::string(error)})
            << keys;
    }
}

// A point refused by CF-504 gets no CF-517 too, when merging brought the
// scaling (S5 of the review of #149).
TEST(MergedPoints, OneErrorForARefusedPoint) {
    EXPECT_EQ(meter("  - { name: p, value_type: int64, address: { table: "
                    "input, register: 1, type: float32 } }\n",
                    "    overrides: { p: { scale: { gain: 0.5 } } }\n"),
              Lines{"drivers/modbus/line1.yaml:7:11: error CF-504: "
                    "\"ps1.m1.p\": value_type int64 cannot hold float32\n"
                    "  from template templates/meter_a.yaml:5:28 (point p)"});
}

// The keys of an io address by its channel's backend (B.2, B.3).
TEST(MergedPoints, TheKeysOfAnIoBackend) {
    const auto io = [](std::string_view backend, std::string_view address) {
        return errors(expand(
            {{"templates/sensor_a.yaml",
              "template: sensor_a\nprotocol: io\npoints:\n"
              "  - { name: level, address: " +
                  std::string(address) + " }\n"},
             {"drivers/io/local.yaml",
              "channel: { name: local, backend: " + std::string(backend) +
                  ", device: adc0 }\n"
                  "devices:\n  - { name: ps1.s1, template: sensor_a }\n"}}));
    };
    EXPECT_EQ(io("iio", "{ channel: in_voltage0 }"), Lines{});
    EXPECT_EQ(io("iio", "{ filter: 1s }"),
              Lines{"drivers/io/local.yaml:3:13: error CF-202: the address of "
                    "\"ps1.s1.level\" needs channel\n  from template "
                    "templates/sensor_a.yaml:4:29 (point level)"});
    EXPECT_EQ(io("simulator", "{ profile: { type: ramp } }"), Lines{});
    EXPECT_EQ(io("simulator", "{ filter: 1s }"),
              Lines{"drivers/io/local.yaml:3:13: error CF-202: the address of "
                    "\"ps1.s1.level\" needs profile\n  from template "
                    "templates/sensor_a.yaml:4:29 (point level)"});
    EXPECT_EQ(io("simulator", "{ profile: { period: 10 } }"),
              Lines{"drivers/io/local.yaml:3:13: error CF-202: the profile of "
                    "\"ps1.s1.level\" needs type\n  from template "
                    "templates/sensor_a.yaml:4:40 (point level)"});
}

// S5: an enumeration a template names may be in enums.yaml.
TEST(MergedPoints, AnEnumerationOfEnumsYaml) {
    EXPECT_EQ(errors(expand({{"templates/meter_a.yaml",
                              "template: meter_a\nprotocol: modbus\npoints:\n"
                              "  - { name: p, enum: states, address: { table: "
                              "input, register: 1, type: uint16 } }\n"},
                             {"enums.yaml",
                              "enums: { states: { 0: off, 1: on "
                              "} }\n"},
                             {"drivers/modbus/line1.yaml", one_device()}})),
              Lines{});
}

}  // namespace
}  // namespace scada::conf
