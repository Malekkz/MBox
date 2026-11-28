// Stage 5: the Modbus addresses of the expanded tree (configuration schemas
// contract Annex A.1 to A.5, 10.6; decision D-122; slice 7d-2 of #100).

#include "expand_cases.h"
#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/validate.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace scada::conf {
namespace {

// meter_a of Modbus with `body` after its header (points from line 3).
std::string meter(std::string_view body) {
    return "template: meter_a\nprotocol: modbus\n" + std::string(body);
}

// The errors of the tree of meter_a and the devices given (one device
// ps1.m1 of unit 1 by default).
Lines errors_of(std::string_view body, std::string_view devices = {}) {
    Lines found;
    for (const std::string& line :
         expand({{"templates/meter_a.yaml", meter(body)},
                 {"drivers/modbus/line1.yaml",
                  devices.empty() ? one_device() : channel(devices)}})) {
        if (line.find(": error ") != std::string::npos) {
            found.push_back(line);
        }
    }
    return found;
}

// The warnings of the tree of meter_a and one device with `more`, which
// has no error.
Lines warnings_of(std::string_view body, std::string_view more = {}) {
    const Result<Validation> validation =
        validated({{"templates/meter_a.yaml", meter(body)},
                   {"drivers/modbus/line1.yaml", one_device(more)}});
    EXPECT_TRUE(validation.ok());
    if (!validation.ok()) {
        return {"not validated"};
    }
    for (const Diagnostic& error : validation.value().report.errors) {
        ADD_FAILURE() << format(error);
    }
    Lines found;
    for (const Diagnostic& warning : validation.value().report.warnings) {
        found.push_back(format(warning));
    }
    return found;
}

constexpr std::string_view POINTS = "points:\n";

// --- CF-603: addresses ---------------------------------------------------

// A register outside 0 to 65,535 after address_base, and a value that runs
// past the end; the schema's own bound (65,536) is CF-204, not CF-603 too.
TEST(ModbusRules, ARegisterAfterTheBase) {
    EXPECT_EQ(
        errors_of(std::string(POINTS) +
                  "  - { name: a, address: { table: holding, register: "
                  "65536, type: uint16 } }\n"
                  "  - { name: b, address: { table: holding, register: "
                  "65535, type: uint32 } }\n"
                  "  - { name: c, address: { table: holding, register: "
                  "65535, type: uint16 } }\n"
                  "  - { name: d, address: { table: holding, register: "
                  "65534, type: uint32 } }\n"),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-602: points \"c\" "
               "and \"d\" overlap at holding 65535 of unit 1\n"
               "  from template templates/meter_a.yaml:7:53 (point d)",
               "drivers/modbus/line1.yaml:7:11: error CF-603: "
               "\"ps1.m1.a\": register 65536 is outside 0 to 65,535 after "
               "address_base 0\n"
               "  from template templates/meter_a.yaml:4:53 (point a)",
               "drivers/modbus/line1.yaml:7:11: error CF-603: "
               "\"ps1.m1.b\": registers 65535 to 65536 run past 65,535 after "
               "address_base 0\n"
               "  from template templates/meter_a.yaml:5:53 (point b)"}));
    // With base 1, register 0 is below it and 65536 is the last.
    EXPECT_EQ(errors_of("address_base: 1\n" + std::string(POINTS) +
                        "  - { name: a, address: { table: holding, register: "
                        "0, type: uint16 } }\n"
                        "  - { name: b, address: { table: holding, register: "
                        "65536, type: uint16 } }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-603: "
                     "\"ps1.m1.a\": register 0 is outside 0 to 65,535 after "
                     "address_base 1\n"
                     "  from template templates/meter_a.yaml:5:53 (point a)"}));
    // The instance's base wins over the template's.
    EXPECT_EQ(errors_of("address_base: 1\n" + std::string(POINTS) +
                            "  - { name: a, address: { table: holding, "
                            "register: 0, type: uint16 } }\n",
                        "  - { name: ps1.m1, template: meter_a, unit_id: 1, "
                        "address_base: 0 }\n"),
              Lines{});
}

// A coil or a discrete input holds one bit; a field must fit its raw type.
TEST(ModbusRules, BitsAndFieldsOfAnAddress) {
    EXPECT_EQ(
        errors_of(std::string(POINTS) +
                  "  - { name: a, address: { table: coil, register: 1, "
                  "type: uint16 } }\n"
                  "  - { name: b, address: { table: discrete, register: 1, "
                  "bit: 2 } }\n"
                  "  - { name: c, address: { table: holding, register: 1, "
                  "type: uint16, field: [8, 16] } }\n"
                  "  - { name: d, address: { table: holding, register: 2, "
                  "type: uint32, field: [8, 31] } }\n"
                  "bit_order: lsb0\n"),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-603: "
               "\"ps1.m1.a\": a coil holds one bit\n"
               "  from template templates/meter_a.yaml:4:59 (point a)",
               "drivers/modbus/line1.yaml:7:11: error CF-603: "
               "\"ps1.m1.b\": a discrete input holds one bit: bit does not "
               "apply\n"
               "  from template templates/meter_a.yaml:5:62 (point b)",
               "drivers/modbus/line1.yaml:7:11: error CF-603: "
               "\"ps1.m1.c\": field [8, 16] is wider than uint16\n"
               "  from template templates/meter_a.yaml:6:77 (point c)"}));
}

// A read range outside the table, or upside down; an identity check's own
// address.
TEST(ModbusRules, RangesAndIdentityAddresses) {
    EXPECT_EQ(
        errors_of("read:\n  ranges:\n"
                  "    - { kind: no_read, table: holding, first: 65536, "
                  "last: 65536 }\n"
                  "    - { kind: atomic, table: holding, first: 9, last: 3 "
                  "}\n"
                  "identify:\n"
                  "  - { address: { table: holding, register: 65536, type: "
                  "uint16 }, equals: 1 }\n"
                  "points:\n"
                  "  - { name: a, address: { table: holding, register: 1, "
                  "type: uint16 } }\n"),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-603: range no_read "
               "holding 65536 to 65536 of device \"ps1.m1\": register 65536 "
               "is outside 0 to 65,535 after address_base 0\n"
               "  from template templates/meter_a.yaml:5:47 (read)",
               "drivers/modbus/line1.yaml:7:11: error CF-603: range atomic "
               "holding 9 to 3 of device \"ps1.m1\": first is above last\n"
               "  from template templates/meter_a.yaml:6:46 (read)",
               "drivers/modbus/line1.yaml:7:11: error CF-603: identity check "
               "1 of \"ps1.m1\": register 65536 is outside 0 to 65,535 after "
               "address_base 0\n"
               "  from template templates/meter_a.yaml:8:44 (identify)"}));
}

// --- CF-602: points of one device ----------------------------------------

TEST(ModbusRules, PointsReadingOneRegister) {
    EXPECT_EQ(
        errors_of(std::string(POINTS) +
                  "  - { name: a, address: { table: holding, register: 10, "
                  "type: uint32 } }\n"
                  "  - { name: b, address: { table: holding, register: 11, "
                  "type: uint16 } }\n"
                  // Bits and fields share a register with its value (A.3).
                  "  - { name: c, address: { table: holding, register: 10, "
                  "bit: 3 } }\n"
                  "  - { name: d, address: { table: holding, register: 11, "
                  "field: [0, 3] } }\n"
                  // Another table, another unit.
                  "  - { name: e, address: { table: input, register: 10, "
                  "type: uint16 } }\n"
                  "  - { name: f, address: { table: holding, register: 10, "
                  "type: uint16, unit_id: 2 } }\n"
                  // Parts that meet a value.
                  "  - { name: g, address: { table: holding, parts: [ { "
                  "register: 21 }, { register: 30 } ], type: uint32 } }\n"
                  "  - { name: h, address: { table: holding, register: 29, "
                  "type: uint32 } }\n"
                  // A double point's coil.
                  "  - { name: i, kind: double_point, address: { on: { "
                  "table: coil, register: 5 }, off: { table: coil, register: "
                  "6 } } }\n"
                  "  - { name: j, address: { table: coil, register: 6 } }\n"
                  "bit_order: lsb0\n"),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-602: points \"i\" "
               "and \"j\" overlap at coil 6 of unit 1\n"
               "  from template templates/meter_a.yaml:13:50 (point j)",
               "drivers/modbus/line1.yaml:7:11: error CF-602: points \"a\" "
               "and \"b\" overlap at holding 11 of unit 1\n"
               "  from template templates/meter_a.yaml:5:53 (point b)",
               // At h, written after g, though g's second part is met
               // after h, which starts before it.
               "drivers/modbus/line1.yaml:7:11: error CF-602: points \"g\" "
               "and \"h\" overlap at holding 30 of unit 1\n"
               "  from template templates/meter_a.yaml:11:53 (point h)"}));
}

// Write-only points share a command register when the bits each writes are
// disjoint (A.5's example); they do not meet a point that reads.
TEST(ModbusRules, WriteOnlyPointsOnACommandRegister) {
    const std::string command =
        "  - { name: breaker, kind: double_point, access: wo, storage: "
        "volatile, address: { table: holding, register: 40, write: { values: "
        "{ 1: 0x0001, 2: 0x0002 } } } }\n"
        "  - { name: reset, access: wo, storage: volatile, address: { table: "
        "holding, register: 40, write: { mask: 0x0010 } } }\n"
        "  - { name: status, address: { table: holding, register: 40, type: "
        "uint16 } }\n";
    EXPECT_EQ(errors_of(std::string(POINTS) + command), Lines{});
    EXPECT_EQ(
        errors_of(std::string(POINTS) + command +
                  "  - { name: other, access: wo, storage: volatile, "
                  "address: { table: holding, register: 40, write: { mask: "
                  "0x0002 } } }\n"
                  "  - { name: plain, access: wo, storage: volatile, "
                  "address: { table: holding, register: 41, type: uint16 } }\n"
                  "  - { name: low, access: wo, storage: volatile, address: { "
                  "table: holding, register: 41, write: { value: 1 } } }\n"
                  "  - { name: none, access: wo, storage: volatile, address: { "
                  "table: holding, register: 41, write: { value: 0 } } }\n"),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-602: points "
               "\"breaker\" and \"other\" overlap at holding 40 of unit 1\n"
               "  from template templates/meter_a.yaml:7:88 (point other)",
               "drivers/modbus/line1.yaml:7:11: error CF-602: points "
               "\"plain\" and \"low\" overlap at holding 41 of unit 1\n"
               "  from template templates/meter_a.yaml:9:86 (point low)"}));
}

// `write.on` and `write.off` write their own registers, and a value of
// several registers splits its pattern by the word order.
TEST(ModbusRules, WriteTargetsAndWordOrder) {
    EXPECT_EQ(
        errors_of(
            std::string(POINTS) +
            "  - { name: a, kind: double_point, access: wo, storage: "
            "volatile, address: { write: { on: { table: holding, "
            "register: 50, value: 1 }, off: { table: holding, register: "
            "51, value: 1 } } } }\n"
            "  - { name: b, access: wo, storage: volatile, address: { "
            "table: holding, register: 51, write: { mask: 0x0001 } } }\n"
            // 0x00010000 is the first register's bit 0 (big word order).
            "  - { name: c, access: wo, storage: volatile, address: { "
            "table: holding, register: 60, type: uint32, write: { mask: "
            "0x00010000 } } }\n"
            "  - { name: d, access: wo, storage: volatile, address: { "
            "table: holding, register: 61, write: { mask: 0x0001 } } }\n"
            "  - { name: e, access: wo, storage: volatile, address: { "
            "table: holding, register: 60, write: { mask: 0x0001 } } }\n"),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-602: points \"a\" "
               "and \"b\" overlap at holding 51 of unit 1\n"
               "  from template templates/meter_a.yaml:5:84 (point b)",
               "drivers/modbus/line1.yaml:7:11: error CF-602: points \"c\" "
               "and \"e\" overlap at holding 60 of unit 1\n"
               "  from template templates/meter_a.yaml:8:84 (point e)"}));
}

// A little word order puts a value's low word first; `write.quantity`
// writes every bit of the registers past the value's own (A.5).
TEST(ModbusRules, LittleWordOrderAndQuantity) {
    EXPECT_EQ(
        errors_of(
            std::string(POINTS) +
            // Bit 0 of the value is in register 70, not 71.
            "  - { name: a, access: wo, storage: volatile, address: { "
            "table: holding, register: 70, type: uint32, word_order: "
            "little, write: { mask: 0x00000001 } } }\n"
            "  - { name: b, access: wo, storage: volatile, address: { "
            "table: holding, register: 71, write: { mask: 0x0001 } } }\n"
            "  - { name: c, access: wo, storage: volatile, address: { "
            "table: holding, register: 70, write: { mask: 0x0001 } } }\n"
            // The second register of the quantity is written whole.
            "  - { name: d, access: wo, storage: volatile, address: { "
            "table: holding, register: 80, write: { mask: 0x0001, "
            "quantity: 2 } } }\n"
            "  - { name: e, access: wo, storage: volatile, address: { "
            "table: holding, register: 81, write: { mask: 0x0001 } } }\n"),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-602: points \"a\" "
               "and \"c\" overlap at holding 70 of unit 1\n"
               "  from template templates/meter_a.yaml:6:84 (point c)",
               "drivers/modbus/line1.yaml:7:11: error CF-602: points \"d\" "
               "and \"e\" overlap at holding 81 of unit 1\n"
               "  from template templates/meter_a.yaml:8:84 (point e)"}));
}

// One cause at three instances is one error (D-114).
TEST(ModbusRules, OneCauseAtManyInstancesIsOneError) {
    EXPECT_EQ(
        errors_of(std::string(POINTS) +
                      "  - { name: a, address: { table: holding, "
                      "register: 1, type: uint16 } }\n"
                      "  - { name: b, address: { table: holding, "
                      "register: 1, type: uint16 } }\n",
                  "  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n"
                  "  - { name: ps1.m2, template: meter_a, unit_id: 2 }\n"
                  "  - { name: ps1.m3, template: meter_a, unit_id: 3 }\n"),
        (Lines{"drivers/modbus/line1.yaml:7:13: error CF-602: points "
               "\"a\" and \"b\" overlap at holding 1 of unit 1 (and 2 "
               "more instances: ps1.m2, ps1.m3)\n"
               "  from template templates/meter_a.yaml:5:53 (point b)"}));
}

// --- CF-601: devices of one channel --------------------------------------

TEST(ModbusRules, DevicesOfOneChannel) {
    const std::string body = std::string(POINTS) +
                             "  - { name: a, address: { table: holding, "
                             "register: 1, type: uint16 } }\n";
    // Slices of one gateway unit with disjoint registers are allowed.
    EXPECT_EQ(errors_of(body,
                        "  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n"
                        "  - name: ps1.m2\n    unit_id: 1\n    points:\n"
                        "      - { name: b, address: { table: holding, "
                        "register: 2, type: uint16 } }\n"),
              Lines{});
    EXPECT_EQ(
        errors_of(body,
                  "  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n"
                  "  - name: ps1.m2\n    unit_id: 2\n    points:\n"
                  "      - { name: b, address: { table: holding, "
                  "register: 1, type: uint16, unit_id: 1 } }\n"
                  // Base 1 puts ps1.m3's register 1 at address 0.
                  "  - { name: ps1.m3, template: meter_a, unit_id: 1, "
                  "address_base: 1 }\n"
                  "  - { name: ps1.m4, template: meter_a, unit_id: 4 }\n"),
        (Lines{"drivers/modbus/line1.yaml:8:11: error CF-601: devices "
               "\"ps1.m1\" and \"ps1.m2\" overlap at holding 1 of unit "
               "1"}));
    EXPECT_EQ(errors_of(body,
                        "  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n"
                        "  - { name: ps1.m2, template: meter_a, unit_id: 2 }\n"
                        "  - name: ps1.m3\n    unit_id: 3\n"
                        "    address_base: 1\n    points:\n"
                        "      - { name: b, address: { table: holding, "
                        "register: 2, type: uint16, unit_id: 2 } }\n"),
              (Lines{"drivers/modbus/line1.yaml:9:11: error CF-601: devices "
                     "\"ps1.m2\" and \"ps1.m3\" overlap at holding 2 of unit "
                     "2"}));
}

// A pair of devices is one error, at the device given later, also when
// its base puts its registers before the other's.
TEST(ModbusRules, APairOfDevicesIsOneError) {
    EXPECT_EQ(errors_of(std::string(POINTS) +
                            "  - { name: a, address: { table: holding, "
                            "register: 10, type: uint32 } }\n"
                            "  - { name: b, address: { table: holding, "
                            "register: 21, type: uint16 } }\n",
                        "  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n"
                        "  - { name: ps1.m2, template: meter_a, unit_id: 1, "
                        "address_base: 1 }\n"),
              (Lines{"drivers/modbus/line1.yaml:8:13: error CF-601: devices "
                     "\"ps1.m1\" and \"ps1.m2\" overlap at holding 11 of "
                     "unit 1"}));
}

// Two devices that write disjoint bits of one command register still share
// its address (A.3): the bits rule of A.5 is within one device.
TEST(ModbusRules, DevicesShareARegisterWhateverItsBits) {
    EXPECT_EQ(errors_of(std::string(POINTS) +
                            "  - { name: a, access: wo, storage: volatile, "
                            "address: { table: holding, register: 1, write: { "
                            "mask: 0x0001 } } }\n",
                        "  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n"
                        "  - name: ps1.m2\n    unit_id: 1\n    points:\n"
                        "      - { name: b, access: wo, storage: volatile, "
                        "address: { table: holding, register: 1, write: { "
                        "mask: 0x0002 } } }\n"),
              (Lines{"drivers/modbus/line1.yaml:8:11: error CF-601: devices "
                     "\"ps1.m1\" and \"ps1.m2\" overlap at holding 1 of unit "
                     "1"}));
}

// --- CF-610: read ranges -------------------------------------------------

TEST(ModbusRules, SideEffectRanges) {
    const std::string points =
        "points:\n"
        "  - { name: a, poll_group: fast, address: { table: holding, "
        "register: 10, type: uint16 } }\n"
        "  - { name: b, address: { table: holding, register: 12, type: "
        "uint16 } }\n"
        "  - { name: c, poll_group: fast, address: { table: holding, "
        "register: 20, type: uint32 } }\n"
        "  - { name: d, access: wo, storage: volatile, address: { table: "
        "holding, register: 30, type: uint16 } }\n";
    EXPECT_EQ(
        errors_of("read:\n  ranges:\n"
                  "    - { kind: side_effect, table: holding, first: 10, "
                  "last: 12 }\n"
                  "    - { kind: exclusive, table: holding, first: 21, last: "
                  "21 }\n"
                  "    - { kind: side_effect, table: holding, first: 30, "
                  "last: 30 }\n"
                  "    - { kind: no_read, table: holding, first: 40, last: "
                  "45 }\n"
                  "    - { kind: side_effect, table: holding, first: 45, "
                  "last: 46 }\n"
                  "    - { kind: atomic, table: holding, first: 50, last: 55 "
                  "}\n"
                  "    - { kind: no_read, table: holding, first: 55, last: 56 "
                  "}\n" +
                  points),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-610: range "
               "side_effect holding 45 to 46 of device \"ps1.m1\": it "
               "overlaps range no_read holding 40 to 45\n"
               "  from template templates/meter_a.yaml:9:15 (read)",
               // An atomic read must not touch a no_read range (M3 of the
               // review of #180).
               "drivers/modbus/line1.yaml:7:11: error CF-610: range no_read "
               "holding 55 to 56 of device \"ps1.m1\": it overlaps range "
               "atomic holding 50 to 55\n"
               "  from template templates/meter_a.yaml:11:15 (read)",
               "drivers/modbus/line1.yaml:7:11: error CF-610: range "
               "side_effect holding 10 to 12 of device \"ps1.m1\": points "
               "\"b\" and \"a\" are in different poll groups\n"
               "  from template templates/meter_a.yaml:5:15 (read)",
               "drivers/modbus/line1.yaml:7:11: error CF-610: range "
               "side_effect holding 30 to 30 of device \"ps1.m1\": point "
               "\"d\" writes it\n"
               "  from template templates/meter_a.yaml:7:15 (read)"}));
    // An identity check that reads a side effect, by address or by point.
    EXPECT_EQ(
        errors_of("read:\n  ranges:\n"
                  "    - { kind: side_effect, table: holding, first: 10, "
                  "last: 10 }\n"
                  "    - { kind: exclusive, table: holding, first: 20, last: "
                  "20 }\n"
                  "identify:\n"
                  "  - { address: { table: holding, register: 10, type: "
                  "uint16 }, equals: 1 }\n"
                  "  - { point: c, equals: 1 }\n" +
                  points),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-610: range "
               "side_effect holding 10 to 10 of device \"ps1.m1\": identity "
               "check 1 reads it\n"
               "  from template templates/meter_a.yaml:5:15 (read)",
               "drivers/modbus/line1.yaml:7:11: error CF-610: range exclusive "
               "holding 20 to 20 of device \"ps1.m1\": identity check 2 reads "
               "it\n"
               "  from template templates/meter_a.yaml:6:15 (read)"}));
}

// --- CF-615: functions ---------------------------------------------------

TEST(ModbusRules, FunctionsTheDeviceSupports) {
    const std::string points =
        "points:\n"
        "  - { name: a, address: { table: coil, register: 1 } }\n"
        "  - { name: b, address: { table: input, register: 1, type: uint16 } "
        "}\n"
        "  - { name: c, access: rw, storage: volatile, address: { table: "
        "holding, register: 1, type: uint16 } }\n"
        "  - { name: d, access: wo, storage: volatile, address: { table: "
        "holding, register: 2, type: uint32 } }\n";
    EXPECT_EQ(
        errors_of("functions: [3, 6]\n" + points),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-615: point \"a\" "
               "needs function 1, which device \"ps1.m1\" does not support\n"
               "  from template templates/meter_a.yaml:5:50 (point a)",
               "drivers/modbus/line1.yaml:7:11: error CF-615: point \"b\" "
               "needs function 4, which device \"ps1.m1\" does not support\n"
               "  from template templates/meter_a.yaml:6:51 (point b)",
               "drivers/modbus/line1.yaml:7:11: error CF-615: point \"d\" "
               "needs function 16, which device \"ps1.m1\" does not support\n"
               "  from template templates/meter_a.yaml:8:84 (point d)"}));
    // 16 for 6, 3 for 4 where the input table is the holding table, and
    // repeated 5 for 15.
    EXPECT_EQ(errors_of("functions: [1, 3, 16]\n"
                        "read: { input_as_holding: true }\n" +
                        points),
              Lines{});
    EXPECT_EQ(errors_of("functions: [1, 3, 5, 6, 16]\npoints:\n"
                        "  - { name: e, access: wo, storage: volatile, "
                        "address: { table: coil, register: 3, write: { "
                        "function: 15 } } }\n"),
              Lines{});
    EXPECT_EQ(
        errors_of("functions: [1, 3, 6, 16]\npoints:\n"
                  "  - { name: e, access: wo, storage: volatile, address: { "
                  "table: coil, register: 3, write: { function: 15 } } }\n"),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-615: point \"e\" "
               "needs function 15, which device \"ps1.m1\" does not support\n"
               "  from template templates/meter_a.yaml:5:81 (point e)"}));
}

// --- CW-607: idle time-outs ----------------------------------------------

TEST(ModbusRules, AnIdleTimeoutShorterThanThePolls) {
    const std::string points =
        "points:\n"
        "  - { name: a, poll_group: slow, address: { table: holding, "
        "register: 1, type: uint16 } }\n";
    // The default backoff_max is 300 s.
    EXPECT_EQ(warnings_of("idle_timeout: 60s\n" + points),
              (Lines{"drivers/modbus/line1.yaml:7:11: warning CW-607: device "
                     "\"ps1.m1\": the connection may be closed between polls; "
                     "set keepalive\n"
                     "  from template templates/meter_a.yaml:3:15"}));
    EXPECT_EQ(warnings_of("idle_timeout: 60s\nkeepalive: 30s\n" + points),
              Lines{});
    EXPECT_EQ(
        warnings_of("idle_timeout: 60s\n" + points, "    backoff_max: 30s\n"),
        Lines{});
    // The slow group's 10 s, past a 5 s time-out.
    EXPECT_EQ(
        warnings_of("idle_timeout: 5s\n" + points, "    backoff_max: 2s\n"),
        (Lines{"drivers/modbus/line1.yaml:7:11: warning CW-607: device "
               "\"ps1.m1\": the connection may be closed between polls; "
               "set keepalive\n"
               "  from template templates/meter_a.yaml:3:15"}));
}

// --- Review of #180 ------------------------------------------------------

// A byte is a part of its register (A.2): the two bytes of one register,
// and a bit or a field of either half, are read together; writes of one
// half meet (M1 of the review of #180).
TEST(ModbusRules, TheBytesOfOneRegister) {
    EXPECT_EQ(
        errors_of(std::string(POINTS) +
                  "  - { name: a, address: { table: holding, register: 1, "
                  "type: uint8, byte: high } }\n"
                  "  - { name: b, address: { table: holding, register: 1, "
                  "type: uint8, byte: low } }\n"
                  "  - { name: c, address: { table: holding, register: 1, "
                  "bit: 9 } }\n"
                  "  - { name: d, address: { table: holding, register: 1, "
                  "type: uint16, field: [0, 3] } }\n"
                  "  - { name: e, access: wo, storage: volatile, address: { "
                  "table: holding, register: 2, type: uint8, byte: high } }\n"
                  "  - { name: f, access: wo, storage: volatile, address: { "
                  "table: holding, register: 2, type: uint8, byte: low } }\n"
                  "  - { name: g, access: wo, storage: volatile, address: { "
                  "table: holding, register: 2, bit: 12 } }\n"
                  "  - { name: h, access: wo, storage: volatile, address: { "
                  "table: holding, register: 2, write: { mask: 0x0008 } } }\n"
                  "bit_order: lsb0\n"),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-602: points \"e\" a"
               "nd \"g\" overlap at holding 2 of unit 1\n  from template tem"
               "plates/meter_a.yaml:10:84 (point g)",
               "drivers/modbus/line1.yaml:7:11: error CF-602: points \"f\" a"
               "nd \"h\" overlap at holding 2 of unit 1\n  from template tem"
               "plates/meter_a.yaml:11:84 (point h)"}));
    // Under `msb0` a byte keeps its half: bit 2 is in the high byte, bit
    // 10 in the low one (an item of the delta check of #180).
    EXPECT_EQ(
        errors_of(std::string(POINTS) +
                  "  - { name: a, access: rw, storage: volatile, address: { "
                  "table: holding, register: 5, type: uint8, byte: high } }\n"
                  "  - { name: b, access: rw, storage: volatile, address: { "
                  "table: holding, register: 5, bit: 2 } }\n"
                  "  - { name: c, access: rw, storage: volatile, address: { "
                  "table: holding, register: 6, type: uint8, byte: high } }\n"
                  "  - { name: d, access: rw, storage: volatile, address: { "
                  "table: holding, register: 6, bit: 10 } }\n"
                  "bit_order: msb0\n"),
        Lines{"drivers/modbus/line1.yaml:7:11: error CF-602: points \"a\" "
              "and \"b\" overlap at holding 5 of unit 1\n"
              "  from template templates/meter_a.yaml:5:84 (point b)"});
}

// A range's own `unit_id` is the unit of its registers: points of unit 2
// in two poll groups meet a unit-2 side-effect range, and the same
// registers of the device's unit 1 do not (an item of the delta check of
// #180).
TEST(ModbusRules, ARangeOfItsOwnUnit) {
    EXPECT_EQ(
        errors_of("read:\n  ranges:\n"
                  "    - { kind: side_effect, table: holding, first: 60, "
                  "last: 61, unit_id: 2 }\n"
                  "points:\n"
                  "  - { name: a, poll_group: fast, address: { table: holding, "
                  "register: 60, type: uint16, unit_id: 2 } }\n"
                  "  - { name: b, address: { table: holding, register: 61, "
                  "type: uint16, unit_id: 2 } }\n"
                  "  - { name: c, poll_group: fast, address: { table: holding, "
                  "register: 60, type: uint16 } }\n"
                  "  - { name: d, address: { table: holding, register: 61, "
                  "type: uint16 } }\n"),
        Lines{"drivers/modbus/line1.yaml:7:11: error CF-610: range "
              "side_effect holding 60 to 61 of device \"ps1.m1\": points "
              "\"b\" and \"a\" are in different poll groups\n"
              "  from template templates/meter_a.yaml:5:15 (read)"});
    EXPECT_EQ(
        errors_of("read:\n  ranges:\n"
                  "    - { kind: side_effect, table: holding, first: 60, "
                  "last: 61, unit_id: 2 }\n"
                  "points:\n"
                  "  - { name: c, poll_group: fast, address: { table: holding, "
                  "register: 60, type: uint16 } }\n"
                  "  - { name: d, address: { table: holding, register: 61, "
                  "type: uint16 } }\n"),
        Lines{});
}

// A `write.quantity` that runs past 65,535 is CF-603, at the quantity
// (S1 of the review of #180).
TEST(ModbusRules, AWriteQuantityPastTheEnd) {
    EXPECT_EQ(
        errors_of(std::string(POINTS) +
                  "  - { name: a, access: wo, storage: volatile, address: { "
                  "table: holding, register: 65530, write: { mask: 0x0001, "
                  "quantity: 10 } } }\n"
                  "  - { name: b, access: wo, storage: volatile, address: { "
                  "table: holding, register: 65520, write: { mask: 0x0001, "
                  "quantity: 16 } } }\n"),
        Lines{"drivers/modbus/line1.yaml:7:11: error CF-603: \"ps1.m1.a\": "
              "write quantity 10 from register 65530 runs past 65,535 after"
              " address_base 0\n  from template templates/meter_a.yaml:4:12"
              "4 (point a)"});
}

// Ranges of any two different kinds must not overlap (A.4; the lead's
// ruling M3 of the review of #180); two of one kind may.
TEST(ModbusRules, RangesOfEachPairOfKinds) {
    EXPECT_EQ(
        errors_of(
            "read:\n  ranges:\n"
            "    - { kind: side_effect, table: holding, first: 100, last: 105 "
            "}\n"
            "    - { kind: exclusive, table: holding, first: 105, last: 109 }\n"
            "    - { kind: side_effect, table: holding, first: 200, last: 205 "
            "}\n"
            "    - { kind: atomic, table: holding, first: 205, last: 209 }\n"
            "    - { kind: side_effect, table: holding, first: 300, last: 305 "
            "}\n"
            "    - { kind: no_read, table: holding, first: 305, last: 309 }\n"
            "    - { kind: exclusive, table: holding, first: 400, last: 405 }\n"
            "    - { kind: atomic, table: holding, first: 405, last: 409 }\n"
            "    - { kind: exclusive, table: holding, first: 500, last: 505 }\n"
            "    - { kind: no_read, table: holding, first: 505, last: 509 }\n"
            "    - { kind: atomic, table: holding, first: 600, last: 605 }\n"
            "    - { kind: no_read, table: holding, first: 605, last: 609 }\n"
            "    - { kind: atomic, table: holding, first: 900, last: 905 }\n"
            "    - { kind: atomic, table: holding, first: 903, last: 909 }\n"
            "points:\n"
            "  - { name: a, address: { table: holding, register: "
            "1, type: uint16 } }\n"),
        // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): long messages
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-610: range exclusive "
               "holding 105 to 109 of device \"ps1.m1\": it overlaps "
               "range side_effect holding 100 to 105\n"
               "  from template templates/meter_a.yaml:6:15 (read)",
               "drivers/modbus/line1.yaml:7:11: error CF-610: range atomic "
               "holding 205 to 209 of device \"ps1.m1\": it overlaps "
               "range side_effect holding 200 to 205\n"
               "  from template templates/meter_a.yaml:8:15 (read)",
               "drivers/modbus/line1.yaml:7:11: error CF-610: range no_read "
               "holding 305 to 309 of device \"ps1.m1\": it overlaps "
               "range side_effect holding 300 to 305\n"
               "  from template templates/meter_a.yaml:10:15 (read)",
               "drivers/modbus/line1.yaml:7:11: error CF-610: range atomic "
               "holding 405 to 409 of device \"ps1.m1\": it overlaps "
               "range exclusive holding 400 to 405\n"
               "  from template templates/meter_a.yaml:12:15 (read)",
               "drivers/modbus/line1.yaml:7:11: error CF-610: range no_read "
               "holding 505 to 509 of device \"ps1.m1\": it overlaps "
               "range exclusive holding 500 to 505\n"
               "  from template templates/meter_a.yaml:14:15 (read)",
               "drivers/modbus/line1.yaml:7:11: error CF-610: range no_read "
               "holding 605 to 609 of device \"ps1.m1\": it overlaps "
               "range atomic holding 600 to 605\n"
               "  from template templates/meter_a.yaml:16:15 (read)"}));
}

// The mutants of S2 of the review of #180, one test each.

// A range's registers are after `address_base`, as the points' are.
TEST(ModbusRules, ARangeAfterTheBase) {
    EXPECT_EQ(
        errors_of("address_base: 1\n"
                  "read:\n  ranges:\n"
                  "    - { kind: side_effect, table: holding, first: "
                  "100, last: 101 }\n"
                  "points:\n"
                  "  - { name: a, poll_group: fast, address: { table: "
                  "holding, register: 100, type: uint16 } }\n"
                  "  - { name: b, address: { table: holding, register: "
                  "101, type: uint16 } }\n"),
        Lines{"drivers/modbus/line1.yaml:7:11: error CF-610: range side_eff"
              "ect holding 100 to 101 of device \"ps1.m1\": points \"b\" an"
              "d \"a\" are in different poll groups\n  from template templa"
              "tes/meter_a.yaml:6:15 (read)"});
}

// An `rw` point writes: it meets a `wo` point's coil, and writes a
// side-effect range.
TEST(ModbusRules, AReadWritePointWrites) {
    EXPECT_EQ(
        errors_of("read:\n  ranges:\n"
                  "    - { kind: side_effect, table: holding, first: "
                  "30, last: 30 }\n"
                  "points:\n"
                  "  - { name: a, access: rw, storage: volatile, "
                  "address: { table: coil, register: 5 } }\n"
                  "  - { name: b, access: wo, storage: volatile, "
                  "address: { table: coil, register: 5 } }\n"
                  "  - { name: c, access: rw, storage: volatile, "
                  "address: { table: holding, register: 30, type: "
                  "uint16 } }\n"),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-602: points \"a\" a"
               "nd \"b\" overlap at coil 5 of unit 1\n  from template templa"
               "tes/meter_a.yaml:8:81 (point b)",
               "drivers/modbus/line1.yaml:7:11: error CF-610: range side_eff"
               "ect holding 30 to 30 of device \"ps1.m1\": point \"c\" write"
               "s it\n  from template templates/meter_a.yaml:5:15 (read)"}));
}

// With `msb0`, bit 0 is the register's most significant bit.
TEST(ModbusRules, MostSignificantBitFirst) {
    EXPECT_EQ(
        errors_of(std::string(POINTS) +
                  "  - { name: a, access: wo, storage: volatile, address: { "
                  "table: holding, register: 3, bit: 0 } }\n"
                  "  - { name: b, access: wo, storage: volatile, address: { "
                  "table: holding, register: 3, write: { mask: 0x8000 } } }\n"
                  "  - { name: c, access: wo, storage: volatile, address: { "
                  "table: holding, register: 4, bit: 0 } }\n"
                  "  - { name: d, access: wo, storage: volatile, address: { "
                  "table: holding, register: 4, write: { mask: 0x0001 } } }\n"
                  "bit_order: msb0\n"),
        Lines{"drivers/modbus/line1.yaml:7:11: error CF-602: points \"a\" a"
              "nd \"b\" overlap at holding 3 of unit 1\n  from template tem"
              "plates/meter_a.yaml:5:84 (point b)"});
}

// The registers of a string of odd length, an epoch, a datetime and a
// mod10k value.
TEST(ModbusRules, TheRegistersOfEachType) {
    EXPECT_EQ(
        errors_of(std::string(POINTS) +
                  "  - { name: s, address: { table: holding, register: 10, "
                  "type: string, length: 5 } }\n"
                  "  - { name: t, address: { table: holding, register: 12, "
                  "type: uint16 } }\n"
                  "  - { name: e, address: { table: holding, register: 20, "
                  "type: epoch, width: 64, resolution: 1s } }\n"
                  "  - { name: f, address: { table: holding, register: 23, "
                  "type: uint16 } }\n"
                  "  - { name: d, address: { table: holding, register: 30, "
                  "type: datetime, fields: [year, month, day, hour] } }\n"
                  "  - { name: g, address: { table: holding, register: 33, "
                  "type: uint16 } }\n"
                  "  - { name: m, address: { table: holding, register: 40, "
                  "type: mod10k, count: 3 } }\n"
                  "  - { name: h, address: { table: holding, register: 42, "
                  "type: uint16 } }\n"),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-602: points \"s\" a"
               "nd \"t\" overlap at holding 12 of unit 1\n  from template te"
               "mplates/meter_a.yaml:5:53 (point t)",
               "drivers/modbus/line1.yaml:7:11: error CF-602: points \"e\" a"
               "nd \"f\" overlap at holding 23 of unit 1\n  from template te"
               "mplates/meter_a.yaml:7:53 (point f)",
               "drivers/modbus/line1.yaml:7:11: error CF-602: points \"d\" a"
               "nd \"g\" overlap at holding 33 of unit 1\n  from template te"
               "mplates/meter_a.yaml:9:53 (point g)",
               "drivers/modbus/line1.yaml:7:11: error CF-602: points \"m\" a"
               "nd \"h\" overlap at holding 42 of unit 1\n  from template te"
               "mplates/meter_a.yaml:11:53 (point h)"}));
}

// A `values` map writes the union of its patterns, and a negative pattern
// is its two's complement.
TEST(ModbusRules, ValuesAndNegativePatterns) {
    EXPECT_EQ(
        errors_of(
            std::string(POINTS) +
            "  - { name: a, kind: double_point, access: wo, storage: "
            "volatile, address: { table: holding, register: 50, write: "
            "{ values: { 1: 0x0004, 2: 0x0001 } } } }\n"
            "  - { name: b, access: wo, storage: volatile, address: { "
            "table: holding, register: 50, write: { mask: 0x0004 } } }\n"
            "  - { name: c, access: wo, storage: volatile, address: { "
            "table: holding, register: 51, write: { value: -1 } } }\n"
            "  - { name: d, access: wo, storage: volatile, address: { "
            "table: holding, register: 51, write: { mask: 0x8000 } } }\n"),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-602: points \"a\" a"
               "nd \"b\" overlap at holding 50 of unit 1\n  from template te"
               "mplates/meter_a.yaml:5:84 (point b)",
               "drivers/modbus/line1.yaml:7:11: error CF-602: points \"c\" a"
               "nd \"d\" overlap at holding 51 of unit 1\n  from template te"
               "mplates/meter_a.yaml:7:84 (point d)"}));
}

// CW-607 needs a period longer than the time-out, not equal to it, and an
// explicit `keepalive: off` is no keep-alive.
TEST(ModbusRules, AnIdleTimeoutAtItsBound) {
    const std::string points =
        "points:\n"
        "  - { name: a, poll_group: slow, address: { table: holding, "
        "register: 1, type: uint16 } }\n";
    EXPECT_EQ(warnings_of("idle_timeout: 300s\n" + points), Lines{});
    EXPECT_EQ(warnings_of("idle_timeout: 60s\nkeepalive: off\n" + points),
              Lines{"drivers/modbus/line1.yaml:7:11: warning CW-607: device "
                    "\"ps1.m1\": the connection may be closed between polls; "
                    "set keepalive\n"
                    "  from template templates/meter_a.yaml:3:15"});
}

// A device given twice (CF-303) is checked once: it does not meet itself.
TEST(ModbusRules, ADeviceGivenTwice) {
    EXPECT_EQ(
        errors_of(std::string(POINTS) +
                      "  - { name: a, address: { table: holding, "
                      "register: 1, type: uint16 } }\n",
                  "  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n"
                  "  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n"),
        Lines{"drivers/modbus/line1.yaml:8:13: error CF-303: device \"ps1.m"
              "1\" is defined twice; first at drivers/modbus/line1.yaml:7"});
}

}  // namespace
}  // namespace scada::conf
