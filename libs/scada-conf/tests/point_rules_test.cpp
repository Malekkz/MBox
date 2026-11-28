// Stage 5: the keys of the points that need no protocol's address rules
// (configuration schemas contract 4.6 to 4.9 and 5.3; decision D-120;
// slice 7d-1 of #100).

#include "expand_cases.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace scada::conf {
namespace {

// meter_a with `bit_order` and the points given, one per line from line 4.
std::string meter(std::string_view points, std::string_view more = {}) {
    return "template: meter_a\nprotocol: modbus\npoints:\n" +
           std::string(points) + "bit_order: lsb0\n" + std::string(more);
}

// The errors of `lines`, the output of `expand`.
Lines only_errors(const Lines& lines) {
    Lines found;
    for (const std::string& line : lines) {
        if (line.find(": error ") != std::string::npos) {
            found.push_back(line);
        }
    }
    return found;
}

// The errors of the tree of meter_a and the devices given (one device
// ps1.m1 by default).
Lines errors_of(const std::string& meter_text, std::string_view devices = {}) {
    Lines found;
    for (const std::string& line :
         expand({{"templates/meter_a.yaml", meter_text},
                 {"drivers/modbus/line1.yaml",
                  devices.empty() ? one_device() : channel(devices)}})) {
        if (line.find(": error ") != std::string::npos) {
            found.push_back(line);
        }
    }
    return found;
}

// --- CF-619: storage of a writable point ----------------------------------

TEST(PointRules, AWritablePointNeedsStorage) {
    EXPECT_EQ(errors_of(meter(
                  "  - { name: a, access: rw, address: { table: holding, "
                  "register: 1, type: uint16 } }\n"
                  "  - { name: b, access: wo, address: { table: holding, "
                  "register: 2, type: uint16 } }\n"
                  "  - { name: c, access: rw, storage: nvm, address: { table: "
                  "holding, register: 3, type: uint16 } }\n"
                  "  - { name: d, access: wo, storage: volatile, address: { "
                  "table: holding, register: 4, type: uint16 } }\n"
                  "  - { name: e, access: ro, address: { table: holding, "
                  "register: 5, type: uint16 } }\n"
                  "  - { name: f, address: { table: holding, register: 6, "
                  "type: uint16 } }\n")),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-619: point "
                     "\"a\" is writable and needs storage: volatile or nvm\n"
                     "  from template templates/meter_a.yaml:4:24 (point a)",
                     "drivers/modbus/line1.yaml:7:11: error CF-619: point "
                     "\"b\" is writable and needs storage: volatile or nvm\n"
                     "  from template templates/meter_a.yaml:5:24 (point b)"}));
    // An override of the instance that gives `access` is at its own line.
    EXPECT_EQ(errors_of(meter("  - { name: a, address: { table: holding, "
                              "register: 1, type: uint16 } }\n"),
                        "  - name: ps1.m1\n    template: meter_a\n"
                        "    unit_id: 1\n"
                        "    overrides: { a: { access: rw } }\n"),
              (Lines{"drivers/modbus/line1.yaml:10:31: error CF-619: point "
                     "\"a\" is writable and needs storage: volatile or nvm"}));
}

// One cause at three instances is one error (D-114).
TEST(PointRules, OneCauseAtManyInstancesIsOneError) {
    EXPECT_EQ(
        errors_of(meter("  - { name: a, access: rw, address: { table: "
                        "holding, register: 1, type: uint16 } }\n"),
                  "  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n"
                  "  - { name: ps1.m2, template: meter_a, unit_id: 2 }\n"
                  "  - { name: ps1.m3, template: meter_a, unit_id: 3 }\n"),
        (Lines{"drivers/modbus/line1.yaml:7:13: error CF-619: point "
               "\"a\" is writable and needs storage: volatile or nvm "
               "(and 2 more instances: ps1.m2, ps1.m3)\n"
               "  from template templates/meter_a.yaml:4:24 (point a)"}));
}

// --- CF-614: a write-only point is not read --------------------------------

TEST(PointRules, AWriteOnlyPointIsNotRead) {
    EXPECT_EQ(
        errors_of(meter(
            "  - { name: a, access: wo, storage: volatile, poll_group: fast, "
            "address: { table: holding, register: 1, type: uint16 } }\n"
            "  - { name: b, access: wo, storage: volatile, logging: { "
            "policy: p }, address: { table: holding, register: 2, type: "
            "uint16 } }\n"
            "  - { name: c, access: rw, storage: volatile, poll_group: fast, "
            "address: { table: holding, register: 3, type: uint16 } }\n")),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-614: point \"a\" is "
               "write-only and cannot be read\n"
               "  from template templates/meter_a.yaml:4:59 (point a)",
               "drivers/modbus/line1.yaml:7:11: error CF-614: point \"b\" is "
               "write-only and cannot be read\n"
               "  from template templates/meter_a.yaml:5:56 (point b)",
               "templates/meter_a.yaml:5:66: error CF-310: logging policy "
               "\"p\" does not exist"}));
}

// --- CF-640: keep_last needs max_age -----------------------------------

TEST(PointRules, KeepLastNeedsMaxAge) {
    EXPECT_EQ(
        errors_of(meter(
            "  - { name: a, on_invalid: keep_last, address: { table: "
            "holding, register: 1, type: uint16 } }\n"
            "  - { name: b, invalid_values: [ { raw: 0, action: keep_last } "
            "], address: { table: holding, register: 2, type: uint16 } }\n"
            "  - { name: c, on_invalid: keep_last, max_age: 10s, address: { "
            "table: holding, register: 3, type: uint16 } }\n"
            "  - { name: d, on_invalid: bad, address: { table: holding, "
            "register: 4, type: uint16 } }\n"
            "  - { name: e, invalid_values: [ { raw: 0, action: bad } ], "
            "address: { table: holding, register: 5, type: uint16 } }\n")),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-640: point \"a\": "
               "keep_last needs max_age\n"
               "  from template templates/meter_a.yaml:4:28 (point a)",
               "drivers/modbus/line1.yaml:7:11: error CF-640: point \"b\": "
               "keep_last needs max_age\n"
               "  from template templates/meter_a.yaml:5:52 (point b)"}));
}

// --- CF-611: max_age and update_period ---------------------------------

// The given `max_age`, or 3 periods of the point's group: `normal` (1 s) by
// default, `slow` (10 s), or a group of the channel.
TEST(PointRules, MaxAgeBelowTheUpdatePeriod) {
    EXPECT_EQ(
        errors_of(meter(
            "  - { name: a, update_period: 5s, max_age: 2s, address: { "
            "table: holding, register: 1, type: uint16 } }\n"
            "  - { name: b, update_period: 5s, address: { table: holding, "
            "register: 2, type: uint16 } }\n"
            "  - { name: c, update_period: 5s, poll_group: slow, address: { "
            "table: holding, register: 3, type: uint16 } }\n"
            "  - { name: d, update_period: 5s, max_age: 5s, address: { "
            "table: holding, register: 4, type: uint16 } }\n"
            "  - { name: e, update_period: 3s, address: { table: holding, "
            "register: 5, type: uint16 } }\n")),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-611: point \"a\": "
               "max_age is shorter than the device's update period\n"
               "  from template templates/meter_a.yaml:4:44 (point a)",
               "drivers/modbus/line1.yaml:7:11: error CF-611: point \"b\": "
               "max_age is shorter than the device's update period\n"
               "  from template templates/meter_a.yaml:5:31 (point b)"}));
    // The channel's own groups replace the defaults: `normal` is 2 s here.
    const std::string groups =
        "channel:\n  name: line1\n  type: rtu\n"
        "  device: /dev/serial/by-path/platform-serial1\n  baud: 9600\n"
        "  poll_groups: { normal: { period: 2s } }\n"
        "devices:\n  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n";
    EXPECT_EQ(expand({{"templates/meter_a.yaml",
                       meter("  - { name: b, update_period: 5s, address: { "
                             "table: holding, register: 2, type: uint16 } "
                             "}\n"
                             "  - { name: c, update_period: 7s, address: { "
                             "table: holding, register: 3, type: uint16 } "
                             "}\n")},
                      {"drivers/modbus/line1.yaml", groups}}),
              (Lines{"drivers/modbus/line1.yaml:8:13: error CF-611: point "
                     "\"c\": max_age is shorter than the device's update "
                     "period\n"
                     "  from template templates/meter_a.yaml:5:31 (point c)"}));
}

// --- CF-612: companions and health points ------------------------------

TEST(PointRules, CompanionsAreReadInThePollGroup) {
    EXPECT_EQ(
        errors_of(meter(
            "  - { name: x, poll_group: slow, address: { table: holding, "
            "register: 1, type: int16 } }\n"
            "  - { name: y, address: { table: holding, register: 2, type: "
            "int16 } }\n"
            "  - { name: w, access: wo, storage: volatile, address: { table: "
            "holding, register: 3, type: uint16 } }\n"
            "  - { name: a, exponent_from: { point: x, direction: multiply }, "
            "address: { table: holding, register: 4, type: int16 } }\n"
            "  - { name: b, exponent_from: { point: y, direction: multiply }, "
            "address: { table: holding, register: 5, type: int16 } }\n"
            "  - { name: c, quality_from: [ { point: self, field: [0, 1], "
            "map: { 0: good, default: bad } }, { point: w, field: [0, 1], "
            "map: { 0: good, default: bad } } ], address: { table: holding, "
            "register: 6, type: uint16 } }\n"
            "  - { name: d, source_time_from: x, address: { table: holding, "
            "register: 7, type: uint16 } }\n"
            "  - { name: e, poll_group: slow, source_time_from: x, address: "
            "{ table: holding, register: 8, type: uint16 } }\n")),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-612: point \"a\": "
               "\"x\" must be in poll group \"normal\"\n"
               "  from template templates/meter_a.yaml:7:40 (point a)",
               "drivers/modbus/line1.yaml:7:11: error CF-612: point \"c\": "
               "\"w\" must be in poll group \"normal\"\n"
               "  from template templates/meter_a.yaml:9:105 (point c)",
               "drivers/modbus/line1.yaml:7:11: error CF-612: point \"d\": "
               "\"x\" must be in poll group \"normal\"\n"
               "  from template templates/meter_a.yaml:10:34 (point d)"}));
}

// A health point is in the group of every read point it applies to: one
// error per rule.
TEST(PointRules, HealthPointsAreReadInThePollGroup) {
    const std::string points =
        "  - { name: h, poll_group: slow, address: { table: holding, "
        "register: 1, type: uint16 } }\n"
        "  - { name: a, address: { table: holding, register: 2, type: uint16 "
        "} }\n"
        "  - { name: b, address: { table: holding, register: 3, type: uint16 "
        "} }\n"
        "  - { name: s1, poll_group: slow, address: { table: holding, "
        "register: 4, type: uint16 } }\n";
    EXPECT_EQ(errors_of(meter(points,
                              "health:\n  - { point: h, good_when: 0, "
                              "applies_to: all }\n")),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-612: point "
                     "\"a\": \"h\" must be in poll group \"normal\"\n"
                     "  from template templates/meter_a.yaml:10:14 (point "
                     "a)"}));
    // `all` from a health point of the first read point's group: the first
    // point of another group; and a health point that is not read.
    EXPECT_EQ(errors_of(meter(points + "  - { name: w, access: wo, storage: "
                                       "volatile, address: { table: holding, "
                                       "register: 5, type: uint16 } }\n",
                              "health:\n  - { point: a, good_when: 0, "
                              "applies_to: all }\n"
                              "  - { point: w, good_when: 0, applies_to: [ "
                              "w, \"s*\" ] }\n")),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-612: point "
                     "\"h\": \"a\" must be in poll group \"slow\"\n"
                     "  from template templates/meter_a.yaml:11:14 (point "
                     "h)",
                     "drivers/modbus/line1.yaml:7:11: error CF-612: point "
                     "\"s1\": \"w\" must be in poll group \"slow\"\n"
                     "  from template templates/meter_a.yaml:12:14 (point "
                     "s1)"}));
    // A pattern that names only points of its group.
    EXPECT_EQ(errors_of(meter(points,
                              "health:\n  - { point: h, good_when: 0, "
                              "applies_to: [ \"s*\" ] }\n")),
              Lines{});
    EXPECT_EQ(errors_of(meter(points,
                              "health:\n  - { point: h, good_when: 0, "
                              "applies_to: [ s1, b ] }\n")),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-612: point "
                     "\"b\": \"h\" must be in poll group \"normal\"\n"
                     "  from template templates/meter_a.yaml:10:14 (point "
                     "b)"}));
}

// --- CF-613: states of a double point ----------------------------------

TEST(PointRules, ADoublePointOfAFieldOrAnIntegerNeedsStates) {
    EXPECT_EQ(
        errors_of(meter(
            "  - { name: a, kind: double_point, address: { table: holding, "
            "register: 1, field: [0, 1] } }\n"
            "  - { name: b, kind: double_point, address: { table: holding, "
            "register: 2, type: uint16 } }\n"
            "  - { name: c, kind: double_point, states: { 0: 0, 1: 1, 2: 2, "
            "3: 3 }, address: { table: holding, register: 3, type: uint16 } "
            "}\n"
            "  - { name: d, kind: double_point, address: { on: { table: "
            "holding, register: 4, bit: 4 }, off: { table: holding, register: "
            "4, bit: 5 } } }\n"
            "  - { name: e, kind: double_point, address: { on: { table: "
            "coil, register: 5 }, off: { table: coil, register: 6 } } }\n"
            // A written double point writes its codes by its encoding.
            "  - { name: f, kind: double_point, access: wo, storage: "
            "volatile, address: { table: holding, register: 7, type: uint16 "
            "} }\n")),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-613: point \"a\": "
               "states must be given\n"
               "  from template templates/meter_a.yaml:4:22 (point a)",
               "drivers/modbus/line1.yaml:7:11: error CF-613: point \"b\": "
               "states must be given\n"
               "  from template templates/meter_a.yaml:5:22 (point b)"}));
}

// --- CF-616: identity checks -------------------------------------------

TEST(PointRules, AnIdentityCheckReadsItsPoint) {
    EXPECT_EQ(
        errors_of(meter(
            "  - { name: model, address: { table: holding, register: 1, type: "
            "uint16 } }\n"
            "  - { name: w, access: wo, storage: volatile, address: { table: "
            "holding, register: 2, type: uint16 } }\n",
            "identify:\n  - { point: model, equals: 7 }\n"
            "  - { point: w, equals: 7 }\n")),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-616: identity check "
               "2 of \"ps1.m1\": point \"w\" is not read\n"
               "  from template templates/meter_a.yaml:9:14 (identify)"}));
}

// --- CF-618: bit_order -------------------------------------------------

// Once per device, at the template's first point that uses a bit; not for a
// template that gives `bit_order`.
TEST(PointRules, BitsNeedBitOrder) {
    const std::string points =
        "template: meter_a\nprotocol: modbus\npoints:\n"
        "  - { name: a, address: { table: holding, register: 1, type: uint16 "
        "} }\n"
        "  - { name: b, address: { table: holding, register: 2, bit: 3 } }\n"
        "  - { name: c, address: { table: holding, register: 3, field: [0, "
        "3] } }\n";
    EXPECT_EQ(errors_of(points),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-618: template "
                     "\"meter_a\" uses bits and needs bit_order\n"
                     "  from template templates/meter_a.yaml:5:61 (point b)"}));
    EXPECT_EQ(errors_of(points + "bit_order: msb0\n"), Lines{});
    // A device without a template: no level gives `bit_order`, and the
    // device is named (M3 of the review of #174).
    EXPECT_EQ(errors_of(points + "bit_order: msb0\n",
                        "  - name: ps1.m1\n    unit_id: 1\n"
                        "    points:\n"
                        "      - { name: b, address: { table: holding, "
                        "register: 2, bit: 3 } }\n"),
              (Lines{"drivers/modbus/line1.yaml:10:65: error CF-618: device "
                     "\"ps1.m1\" uses bits and needs bit_order"}));
    // `bits` and a quality rule's field count too.
    EXPECT_EQ(errors_of("template: meter_a\nprotocol: modbus\npoints:\n"
                        "  - { name: s, address: { table: holding, register: "
                        "1, type: uint16 }, bits: { 0: { name: hot } } }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-618: template "
                     "\"meter_a\" uses bits and needs bit_order\n"
                     "  from template templates/meter_a.yaml:4:78 (point s)"}));
    EXPECT_EQ(errors_of("template: meter_a\nprotocol: modbus\npoints:\n"
                        "  - { name: s, quality_from: [ { point: self, bit: "
                        "1, map: { 0: good, default: bad } } ], address: { "
                        "table: holding, register: 1, type: uint16 } }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-618: template "
                     "\"meter_a\" uses bits and needs bit_order\n"
                     "  from template templates/meter_a.yaml:4:52 (point s)"}));
}

// --- The review of #174 ------------------------------------------------

// M3: every use of a bit in the expanded device needs `bit_order`: an
// instance's own bit point, a double point's `on` or `fault` part, a health
// rule's `good_when` and an identity check's address.
TEST(PointRules, EveryUseOfABitNeedsBitOrder) {
    const std::string plain =
        "template: meter_a\nprotocol: modbus\npoints:\n"
        "  - { name: a, address: { table: holding, register: 1, type: uint16 "
        "} }\n";
    EXPECT_EQ(errors_of(plain,
                        "  - name: ps1.m1\n    template: meter_a\n"
                        "    unit_id: 1\n    points:\n"
                        "      - { name: b, address: { table: holding, "
                        "register: 2, bit: 3 } }\n"),
              (Lines{"drivers/modbus/line1.yaml:11:65: error CF-618: template "
                     "\"meter_a\" uses bits and needs bit_order"}));
    EXPECT_EQ(errors_of(plain +
                        "  - { name: d, kind: double_point, address: { on: { "
                        "table: holding, register: 5, bit: 1 }, off: { table: "
                        "coil, register: 6 } } }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-618: template "
                     "\"meter_a\" uses bits and needs bit_order\n"
                     "  from template templates/meter_a.yaml:5:87 (point d)"}));
    EXPECT_EQ(
        errors_of(plain +
                  "  - { name: d, kind: double_point, address: { on: { "
                  "table: coil, register: 5 }, off: { table: coil, "
                  "register: 6 }, fault: { table: holding, register: 7, "
                  "bit: 2 } } }\n"),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-618: template "
               "\"meter_a\" uses bits and needs bit_order\n"
               "  from template templates/meter_a.yaml:5:159 (point d)"}));
    EXPECT_EQ(errors_of(plain + "health:\n  - { point: a, good_when: { field: "
                                "[0, 1], equals: 0 }, applies_to: all }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-618: template "
                     "\"meter_a\" uses bits and needs bit_order\n"
                     "  from template templates/meter_a.yaml:6:37 (health)"}));
    EXPECT_EQ(
        errors_of(plain + "identify:\n  - { address: { table: holding, "
                          "register: 9, bit: 4 }, equals: 1 }\n"),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-618: template "
               "\"meter_a\" uses bits and needs bit_order\n"
               "  from template templates/meter_a.yaml:6:52 (identify)"}));
}

// M1, M2: a `wo` point's `logging: none` and `alarms: []` say nothing, and
// keys only a `defaults` map gives are not the point's; an override is.
TEST(PointRules, WriteOnlyKeysThatAreNotThePoints) {
    const std::string write_only =
        "  - { name: w, access: wo, storage: volatile, logging: none, alarms: "
        "[], address: { table: holding, register: 1, type: uint16 } }\n";
    EXPECT_EQ(errors_of(meter(write_only)), Lines{});
    EXPECT_EQ(errors_of(meter(write_only,
                              "defaults: { poll_group: slow, logging: { "
                              "mode: on_change, raw_retention: 1d } }\n")),
              Lines{});
    EXPECT_EQ(errors_of(meter(write_only),
                        "  - name: ps1.m1\n    template: meter_a\n"
                        "    unit_id: 1\n"
                        "    defaults: { poll_group: slow }\n"),
              Lines{});
    EXPECT_EQ(errors_of(meter(write_only),
                        "  - name: ps1.m1\n    template: meter_a\n"
                        "    unit_id: 1\n"
                        "    overrides: { w: { poll_group: slow } }\n"),
              (Lines{"drivers/modbus/line1.yaml:10:35: error CF-614: point "
                     "\"w\" is write-only and cannot be read"}));
}

// S1 and S2: the rules of read values do not apply to a `wo` point: its
// `keep_last` from `defaults`, its `max_age` and its companions.
TEST(PointRules, ReadRulesSkipWriteOnlyPoints) {
    EXPECT_EQ(
        errors_of(meter(
            "  - { name: x, poll_group: slow, address: { table: holding, "
            "register: 1, type: int16 } }\n"
            "  - { name: w, access: wo, storage: volatile, update_period: 5s, "
            "max_age: 1s, exponent_from: { point: x, direction: multiply }, "
            "address: { table: holding, register: 2, type: int16 } }\n",
            "defaults: { on_invalid: keep_last, max_age: 10s }\n")),
        Lines{});
    // A `wo` companion in the reading point's group is still not read.
    EXPECT_EQ(
        errors_of(meter(
            "  - { name: w, access: wo, storage: volatile, address: { table: "
            "holding, register: 1, type: int16 } }\n"
            "  - { name: a, exponent_from: { point: w, direction: multiply }, "
            "address: { table: holding, register: 2, type: int16 } }\n")),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-612: point \"a\": "
               "\"w\" must be in poll group \"normal\"\n"
               "  from template templates/meter_a.yaml:5:40 (point a)"}));
}

// S2: health rules. A `wo` point a rule names is not read and not checked;
// a `wo` health point in the group serves none; the first by name of the
// listed names and of the points of `all` is named.
TEST(PointRules, HealthRulesAndWriteOnlyPoints) {
    const std::string points =
        "  - { name: z, address: { table: holding, register: 1, type: uint16 "
        "} }\n"
        "  - { name: b, address: { table: holding, register: 2, type: uint16 "
        "} }\n"
        "  - { name: w, access: wo, storage: volatile, address: { table: "
        "holding, register: 3, type: uint16 } }\n"
        "  - { name: h, poll_group: fast, address: { table: holding, "
        "register: 4, type: uint16 } }\n";
    EXPECT_EQ(errors_of(meter(points,
                              "health:\n  - { point: h, good_when: 0, "
                              "applies_to: [ w ] }\n")),
              Lines{});
    EXPECT_EQ(
        errors_of(meter(points,
                        "health:\n  - { point: h, good_when: 0, "
                        "applies_to: [ z, b ] }\n")),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-612: point "
               "\"b\": \"h\" must be in poll group \"normal\"\n"
               "  from template templates/meter_a.yaml:10:14 (point b)"}));
    EXPECT_EQ(
        errors_of(meter(points,
                        "health:\n  - { point: h, good_when: 0, "
                        "applies_to: all }\n")),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-612: point "
               "\"b\": \"h\" must be in poll group \"normal\"\n"
               "  from template templates/meter_a.yaml:10:14 (point b)"}));
    // A `wo` health point in the points' group does not serve them.
    EXPECT_EQ(
        errors_of(meter(points,
                        "health:\n  - { point: w, good_when: 0, "
                        "applies_to: [ b ] }\n")),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-612: point "
               "\"b\": \"w\" must be in poll group \"normal\"\n"
               "  from template templates/meter_a.yaml:10:14 (point b)"}));
}

// S2: a `parts` double point needs `states`; the shapes of two bits do not,
// and outside Modbus there is no such rule.
TEST(PointRules, StatesOfMoreShapes) {
    EXPECT_EQ(
        errors_of(meter(
            "  - { name: p, kind: double_point, address: { table: holding, "
            "parts: [ { register: 2 }, { register: 1 } ], type: uint32 } }\n")),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-613: point \"p\": "
               "states must be given\n"
               "  from template templates/meter_a.yaml:4:22 (point p)"}));
    // `on` and `off` make it the two-bit shape, whose `states` has a
    // default (A.2), whatever else the address gives.
    EXPECT_EQ(
        errors_of(meter(
            "  - { name: d, kind: double_point, address: { table: holding, "
            "register: 7, type: uint16, on: { table: holding, register: 8, "
            "bit: 1 }, off: { table: holding, register: 8, bit: 2 } } }\n")),
        Lines{});
    EXPECT_EQ(only_errors(expand(
                  {{"templates/sensor_a.yaml",
                    "template: sensor_a\nprotocol: io\npoints:\n"
                    "  - { name: pos, kind: double_point, address: { line: 1, "
                    "direction: input } }\n"},
                   {"drivers/io/local.yaml",
                    "channel: { name: local, backend: gpio, chip: gpiochip0 }\n"
                    "devices:\n  - { name: ps1.s1, template: sensor_a }\n"}})),
              Lines{});
}

// S2: a device given twice (CF-303) is checked once; a channel's groups
// replace the defaults, so `slow` is not one of them.
TEST(PointRules, DevicesTwiceAndTheChannelsGroups) {
    const std::string writable = meter(
        "  - { name: c, access: rw, address: { table: holding, "
        "register: 1, type: uint16 } }\n");
    const Lines twice =
        errors_of(writable,
                  "  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n"
                  "  - { name: ps1.m1, template: meter_a, unit_id: 2 }\n");
    EXPECT_EQ(std::ranges::count_if(twice,
                                    [](const std::string& line) {
                                        return line.find("CF-619") !=
                                               std::string::npos;
                                    }),
              1);
    for (const std::string& line : twice) {
        EXPECT_EQ(line.find("more instance"), std::string::npos) << line;
    }
    const std::string groups =
        "channel:\n  name: line1\n  type: rtu\n"
        "  device: /dev/serial/by-path/platform-serial1\n  baud: 9600\n"
        "  poll_groups: { normal: { period: 2s } }\n"
        "devices:\n  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n";
    for (const std::string& line :
         expand({{"templates/meter_a.yaml",
                  meter("  - { name: s, poll_group: slow, update_period: 40s, "
                        "address: { table: holding, register: 1, type: "
                        "uint16 } }\n")},
                 {"drivers/modbus/line1.yaml", groups}})) {
        EXPECT_EQ(line.find("CF-611"), std::string::npos) << line;
    }
}

}  // namespace
}  // namespace scada::conf
