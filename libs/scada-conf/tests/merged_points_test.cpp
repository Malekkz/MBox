// Stage 5 on merged points (decision D-115): the cases of the review of
// #149, one test per finding.

#include "expand_cases.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

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

}  // namespace
}  // namespace scada::conf
