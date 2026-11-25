// Stage 4, the expansion of templates and instances (contract 4.4, 4.5;
// decision D-114): each step of the merge order, with a case where another
// order would give another result, and each CF-4xx code, with its place in
// the instance and the template line it came from.

#include "expanded_text.h"
#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/validate.h"
#include "scada/test/temp_directory.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scada::conf {
namespace {

namespace fs = std::filesystem;

using Lines = std::vector<std::string>;
using Files = std::map<std::string, std::string>;

constexpr std::string_view SYSTEM =
    "unit: { id: site-0042, name: Test unit }\n"
    "time: { timezone: Etc/UTC, sync: { mode: none } }\n";

// Validates and expands the tree of `files` with a system.yaml: its errors
// in the form of contract 1.4 (with their notes), or its expanded points,
// with each device's keys first when `with_devices`.
Lines expand(const Files& files, bool with_devices = false) {
    std::optional<test::TempDirectory> temp =
        test::TempDirectory::create("scada-conf-expand-");
    EXPECT_TRUE(temp.has_value());
    if (!temp) {
        return {"no temporary directory"};
    }
    Files all = files;
    all.try_emplace("system.yaml", SYSTEM);
    for (const auto& [path, content] : all) {
        const fs::path full = temp->path() / path;
        fs::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::binary);
        out << content;
        EXPECT_TRUE(out.good());
    }
    const Result<Validation> validation = validate_and_expand(temp->path());
    EXPECT_TRUE(validation.ok());
    if (!validation.ok()) {
        return {"not validated"};
    }
    const Validation& result = validation.value();
    if (!result.report.errors.empty()) {
        Lines lines;
        for (const Diagnostic& error : result.report.errors) {
            lines.push_back(format(error));
        }
        return lines;
    }
    if (!result.expanded) {
        return {"no expanded tree"};
    }
    return points_of(*result.expanded, with_devices);
}

// A Modbus RTU channel holding the devices given.
std::string channel(std::string_view devices) {
    return "channel:\n"
           "  name: line1\n"
           "  type: rtu\n"
           "  device: /dev/serial/by-path/platform-serial1\n"
           "  baud: 9600\n"
           "devices:\n" +
           std::string(devices);
}

// One device `ps1.m1` of template `meter_a`, with more keys after it.
std::string one_device(std::string_view more = {}) {
    return channel(
        "  - name: ps1.m1\n"
        "    template: meter_a\n"
        "    unit_id: 1\n" +
        std::string(more));
}

TEST(Expand, AnInstanceGetsTheTemplatesPoints) {
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\n"
                 "points:\n"
                 "  - name: flow\n"
                 "    unit: m3/h\n"
                 "    address: { table: holding, register: 100, type: "
                 "float32 }\n"
                 "  - name: total\n"
                 "    address: { table: holding, register: 102, type: "
                 "uint32 }\n"},
                {"drivers/modbus/line1.yaml",
                 channel("  - { name: ps1.fm1, template: meter_a, unit_id: "
                         "11 }\n"
                         "  - { name: ps1.fm2, template: meter_a, unit_id: "
                         "12 }\n")}}),
        (Lines{"ps1.fm1.flow: {unit: m3/h, address: {table: holding, register: "
               "100, type: float32}}",
               "ps1.fm1.total: {address: {table: holding, register: 102, "
               "type: uint32}}",
               "ps1.fm2.flow: {unit: m3/h, address: {table: holding, register: "
               "100, type: float32}}",
               "ps1.fm2.total: {address: {table: holding, register: 102, "
               "type: uint32}}"}));
}

// Step 3: levels from the root down, a child's keys winning; a base
// template gives the signal, the protocol template its address.
TEST(Expand, LevelsMergeFromTheRootDown) {
    EXPECT_EQ(
        expand({{"templates/base_a.yaml",
                 "template: base_a\n"
                 "points:\n"
                 "  - { name: flow, unit: l/s, description: Flow }\n"
                 "  - { name: temp, unit: degC }\n"},
                {"templates/mid_a.yaml",
                 "template: mid_a\nextends: base_a\nprotocol: modbus\n"
                 "points:\n"
                 "  - { name: flow, unit: m3/h, address: { table: holding, "
                 "register: 10, type: uint16 } }\n"
                 "  - { name: temp, address: { table: input, register: 1, "
                 "type: int16 } }\n"},
                {"templates/meter_a.yaml",
                 "template: meter_a\nextends: mid_a\nprotocol: modbus\n"
                 "points:\n"
                 "  - { name: flow, address: { register: 20 } }\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        (Lines{"ps1.m1.flow: {unit: m3/h, description: Flow, address: {table: "
               "holding, register: 20, type: uint16}}",
               "ps1.m1.temp: {unit: degC, address: {table: input, register: 1, "
               "type: int16}}"}));
}

// Step 2: parameters of every type, defaults and the instance's values.
TEST(Expand, ParametersOfEveryType) {
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\n"
                 "params:\n"
                 "  base: { type: int, min: 0, max: 1000, default: 100 }\n"
                 "  factor: { type: number, default: 0.5 }\n"
                 "  has_temp: { type: bool, default: false }\n"
                 "  model: { type: string, default: a }\n"
                 "  variant: { type: enum, values: [x, y], default: x }\n"
                 "  options: { type: set, values: [meter, battery] }\n"
                 "points:\n"
                 "  - { name: flow, scale: { gain: \"factor * 2\" }, address: "
                 "{ table: holding, register: \"base + 1\", type: uint16 } }\n"
                 "  - { name: temp, when: has_temp, address: { table: "
                 "holding, register: \"base + 2\", type: int16 } }\n"
                 "  - { name: y_only, when: \"variant = 'y' AND model = "
                 "'b'\", address: { table: holding, register: 9, type: "
                 "uint16 } }\n"
                 "  - { name: meter, when: \"'meter' IN options\", address: "
                 "{ table: holding, register: 7, type: uint16 } }\n"},
                {"drivers/modbus/line1.yaml",
                 one_device("    params: { base: 200, has_temp: true, model: "
                            "b, variant: y, options: [meter] }\n")}}),
        (Lines{"ps1.m1.flow: {scale: {gain: 1.0}, address: {table: holding, "
               "register: 201, type: uint16}}",
               "ps1.m1.temp: {address: {table: holding, register: 202, type: "
               "int16}}",
               "ps1.m1.y_only: {address: {table: holding, register: 9, type: "
               "uint16}}",
               "ps1.m1.meter: {address: {table: holding, register: 7, type: "
               "uint16}}"}));
}

TEST(Expand, ParameterErrors) {
    const std::string templates =
        "template: meter_a\nprotocol: modbus\n"
        "params:\n"
        "  inputs: { type: int, min: 1, max: 32 }\n"
        "  mode: { type: enum, values: [a, b], default: a }\n"
        "  options: { type: set, values: [meter], default: [] }\n";
    EXPECT_EQ(expand({{"templates/meter_a.yaml", templates},
                      {"drivers/modbus/line1.yaml", one_device()}}),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-411: parameter "
                     "\"inputs\" needs a value\n"
                     "  from template templates/meter_a.yaml:4:3 (parameter "
                     "inputs)"}));
    EXPECT_EQ(expand({{"templates/meter_a.yaml", templates},
                      {"drivers/modbus/line1.yaml",
                       one_device("    params: { inputs: 40, mode: c, options: "
                                  "[grid], extra: 1 }\n")}}),
              (Lines{"drivers/modbus/line1.yaml:10:23: error CF-412: parameter "
                     "\"inputs\": 40 is above its max 32",
                     "drivers/modbus/line1.yaml:10:33: error CF-412: parameter "
                     "\"mode\": \"c\" is not one of its values",
                     "drivers/modbus/line1.yaml:10:45: error CF-412: parameter "
                     "\"options\": \"grid\" is not one of its values",
                     "drivers/modbus/line1.yaml:10:53: error CF-412: parameter "
                     "\"extra\": the template has no parameter of this name"}));
}

// A child changes a default, never a type (CF-415).
TEST(Expand, AChildChangesADefaultNotAType) {
    EXPECT_EQ(
        expand({{"templates/base_a.yaml",
                 "template: base_a\n"
                 "params: { inputs: { type: int, default: 4 } }\n"},
                {"templates/meter_a.yaml",
                 "template: meter_a\nextends: base_a\nprotocol: modbus\n"
                 "params: { inputs: { default: 6 } }\n"
                 "points:\n"
                 "  - { name: count, address: { table: holding, register: "
                 "inputs, type: uint16 } }\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        Lines{"ps1.m1.count: {address: {table: holding, register: 6, type: "
              "uint16}}"});
    EXPECT_EQ(expand({{"templates/base_a.yaml",
                       "template: base_a\n"
                       "params: { inputs: { type: int, default: 4 } }\n"},
                      {"templates/meter_a.yaml",
                       "template: meter_a\nextends: base_a\nprotocol: modbus\n"
                       "params: { inputs: { type: number, default: 6 } }\n"},
                      {"drivers/modbus/line1.yaml", one_device()}}),
              Lines{"templates/meter_a.yaml:4:27: error CF-415: parameter "
                    "\"inputs\" cannot change its type"});
}

// Step 3.2: `repeat` with one and two variables, an irregular list, an
// empty range, and an address computed from the variable.
TEST(Expand, RepeatBlocks) {
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\n"
                 "params: { inputs: { type: int, default: 2 } }\n"
                 "repeat:\n"
                 "  - for: { n: \"1..inputs\" }\n"
                 "    points:\n"
                 "      - { name: \"in{n}\", address: { table: holding, "
                 "register: \"if(n <= 1, 100, 200 + n)\", type: uint16 } }\n"
                 "  - for: { a: \"1..2\", b: [1, \"3..3\"] }\n"
                 "    when: \"b <> 1 OR a = 1\"\n"
                 "    points:\n"
                 "      - { name: \"c{a}.p{b}\", address: { table: holding, "
                 "register: \"10 * a + b\", type: uint16 } }\n"
                 "  - for: { n: \"25..inputs\" }\n"
                 "    points:\n"
                 "      - { name: \"never{n}\", address: { table: holding, "
                 "register: n, type: uint16 } }\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): long lines
        (Lines{"ps1.m1.in1: {address: {table: holding, register: 100, type: "
               "uint16}}",
               "ps1.m1.in2: {address: {table: holding, register: 202, type: "
               "uint16}}",
               "ps1.m1.c1.p1: {address: {table: holding, register: 11, type: "
               "uint16}}",
               "ps1.m1.c1.p3: {address: {table: holding, register: 13, type: "
               "uint16}}",
               "ps1.m1.c2.p3: {address: {table: holding, register: 23, type: "
               "uint16}}"}));
}

// A point a child's `repeat` makes merges into the one a parent's made;
// a variable bound to two values is CF-422.
TEST(Expand, JoinedBindings) {
    const std::string base =
        "template: base_a\n"
        "repeat:\n"
        "  - for: { n: \"1..2\" }\n"
        "    points: [ { name: \"pv{n}\", unit: V } ]\n";
    EXPECT_EQ(
        expand({{"templates/base_a.yaml", base},
                {"templates/meter_a.yaml",
                 "template: meter_a\nextends: base_a\nprotocol: modbus\n"
                 "repeat:\n"
                 "  - for: { n: \"1..2\" }\n"
                 "    points:\n"
                 "      - { name: \"pv{n}\", address: { table: holding, "
                 "register: \"n * 2\", type: uint16 } }\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        (Lines{"ps1.m1.pv1: {unit: V, address: {table: holding, register: 2, "
               "type: uint16}}",
               "ps1.m1.pv2: {unit: V, address: {table: holding, register: 4, "
               "type: uint16}}"}));
    EXPECT_EQ(
        expand({{"templates/base_a.yaml", base},
                {"templates/meter_a.yaml",
                 "template: meter_a\nextends: base_a\nprotocol: modbus\n"
                 "repeat:\n"
                 "  - for: { n: \"2..2\", k: \"1..1\" }\n"
                 "    points:\n"
                 "      - { name: \"pv{k}\", address: { table: holding, "
                 "register: n, type: uint16 } }\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-422: point \"pv1\": "
               "variable \"n\" is 1 in \"base_a\" and 2 in \"meter_a\"\n"
               "  from template templates/meter_a.yaml:7:17 (point pv1)"}));
}

// Step 3.4 and step 4: `disable` and `only` of a template and of an
// instance; an entry that matches nothing is CF-413.
TEST(Expand, DisableAndOnly) {
    const std::string base =
        "template: base_a\n"
        "points:\n"
        "  - { name: inv1.power, unit: kW }\n"
        "  - { name: inv12.power, unit: kW }\n"
        "  - { name: inv1.dc.power, unit: kW }\n"
        "  - { name: status, unit: \"1\" }\n";
    const std::string meter =
        "template: meter_a\nextends: base_a\nprotocol: modbus\n"
        "disable: [\"inv1.dc.*\"]\n"
        "defaults: { address: { table: holding, register: 1, type: uint16 } "
        "}\n";
    EXPECT_EQ(
        expand({{"templates/base_a.yaml", base},
                {"templates/meter_a.yaml", meter},
                {"drivers/modbus/line1.yaml",
                 one_device("    only: [\"inv*.power\"]\n")}}),
        (Lines{"ps1.m1.inv1.power: {address: {table: holding, register: 1, "
               "type: uint16}, unit: kW}",
               "ps1.m1.inv12.power: {address: {table: holding, register: 1, "
               "type: uint16}, unit: kW}"}));
    EXPECT_EQ(
        expand({{"templates/base_a.yaml", base},
                {"templates/meter_a.yaml", meter},
                {"drivers/modbus/line1.yaml",
                 one_device("    disable: [status, \"inv1.dc.power\"]\n")}}),
        (Lines{"drivers/modbus/line1.yaml:10:23: error CF-413: point "
               "\"inv1.dc.power\" does not exist in template \"meter_a\""}));
}

// Step 5: the template's defaults under the instance's under the point's
// own keys; `invalid_values` of defaults by raw type.
TEST(Expand, DefaultsUnderEveryPoint) {
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\n"
                 "defaults:\n"
                 "  poll_group: slow\n"
                 "  max_age: 30s\n"
                 "  address: { word_order: little }\n"
                 "  invalid_values: { uint16: [0xFFFF], float32: [nan] }\n"
                 "points:\n"
                 "  - { name: a, address: { table: holding, register: 1, "
                 "type: uint16 } }\n"
                 "  - { name: b, max_age: 5s, address: { table: holding, "
                 "register: 2, type: float32, word_order: big } }\n"
                 "  - { name: c, address: { table: holding, register: 4, "
                 "type: int32 } }\n"},
                {"drivers/modbus/line1.yaml",
                 one_device("    defaults: { poll_group: fast }\n")}}),
        (Lines{"ps1.m1.a: {poll_group: fast, max_age: 30s, address: "
               "{word_order: little, table: holding, register: 1, type: "
               "uint16}, invalid_values: [0xFFFF]}",
               "ps1.m1.b: {poll_group: fast, max_age: 5s, address: "
               "{word_order: big, table: holding, register: 2, type: "
               "float32}, invalid_values: [nan]}",
               "ps1.m1.c: {poll_group: fast, max_age: 30s, address: "
               "{word_order: little, table: holding, register: 4, type: "
               "int32}}"}));
    // A point with no written `type` counts by the protocol's default raw
    // type: uint16 for a register, bool for a coil (Annex A.2).
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\n"
                 "defaults: { invalid_values: { uint16: [0xFFFF], bool: [] } "
                 "}\n"
                 "points:\n"
                 "  - { name: a, address: { table: holding, register: 1 } }\n"
                 "  - { name: b, address: { table: coil, register: 2 } }\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        (Lines{"ps1.m1.a: {invalid_values: [0xFFFF], address: {table: "
               "holding, register: 1}}",
               "ps1.m1.b: {invalid_values: [], address: {table: coil, "
               "register: 2}}"}));
}

// Step 6: overrides merge maps, replace lists whole, change form within an
// exclusive group, and `replace: true` replaces a map.
TEST(Expand, Overrides) {
    EXPECT_EQ(
        expand(
            {{"templates/meter_a.yaml",
              "template: meter_a\nprotocol: modbus\n"
              "points:\n"
              "  - name: flow\n"
              "    scale: { gain: 2, offset: 1 }\n"
              "    alarms: [ { type: high, limit: 4, priority: high }, { "
              "type: low, limit: 1, priority: low } ]\n"
              "    address: { table: holding, register: 1, type: uint16, "
              "word_order: little }\n"
              "  - name: level\n"
              "    address: { table: holding, register: 3, type: uint16 }\n"},
             {"drivers/modbus/line1.yaml",
              one_device("    overrides:\n"
                         "      flow:\n"
                         "        scale: { divide_by: 10 }\n"
                         "        alarms: [ { type: high, limit: 9, "
                         "priority: high } ]\n"
                         "      level:\n"
                         "        address: { replace: true, table: input, "
                         "register: 7, type: int16 }\n")}}),
        (Lines{"ps1.m1.flow: {scale: {offset: 1, divide_by: 10}, alarms: "
               "[{type: high, limit: 9, priority: high}], address: {table: "
               "holding, register: 1, type: uint16, word_order: little}}",
               "ps1.m1.level: {address: {table: input, register: 7, type: "
               "int16}}"}));
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\n"
                 "points:\n"
                 "  - { name: flow, address: { table: holding, register: 1, "
                 "type: uint16 } }\n"},
                {"drivers/modbus/line1.yaml",
                 one_device("    overrides: { flwo: { unit: l } }\n")}}),
        (Lines{"drivers/modbus/line1.yaml:10:18: error CF-413: point \"flwo\" "
               "does not exist in template \"meter_a\""}));
}

// An override of a point a `repeat` made is evaluated with that point's
// variables (4.5, step 7).
TEST(Expand, AnOverrideUsesTheVariablesOfItsPoint) {
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\n"
                 "repeat:\n"
                 "  - for: { n: \"1..2\" }\n"
                 "    points:\n"
                 "      - { name: \"pv{n}\", address: { table: holding, "
                 "register: n, type: uint16 } }\n"},
                {"drivers/modbus/line1.yaml",
                 one_device("    overrides: { pv2: { address: { register: "
                            "\"500 + n\" } } }\n")}}),
        (Lines{"ps1.m1.pv1: {address: {table: holding, register: 1, type: "
               "uint16}}",
               "ps1.m1.pv2: {address: {table: holding, register: 502, type: "
               "uint16}}"}));
}

// An instance's own points come after step 6; a name that exists is
// CF-416, as is a name given twice in one level or a bit named like a
// point.
TEST(Expand, InstancePointsAndNamesGivenTwice) {
    const std::string meter =
        "template: meter_a\nprotocol: modbus\n"
        "points:\n"
        "  - { name: flow, address: { table: holding, register: 1, type: "
        "uint16 } }\n";
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml", meter},
                {"drivers/modbus/line1.yaml",
                 one_device("    points:\n"
                            "      - { name: extra, address: { table: "
                            "holding, register: 9, type: uint16 } }\n")}}),
        (Lines{"ps1.m1.flow: {address: {table: holding, register: 1, type: "
               "uint16}}",
               "ps1.m1.extra: {address: {table: holding, register: 9, type: "
               "uint16}}"}));
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml", meter},
                {"drivers/modbus/line1.yaml",
                 one_device("    points:\n"
                            "      - { name: flow, address: { table: "
                            "holding, register: 9, type: uint16 } }\n")}}),
        (Lines{"drivers/modbus/line1.yaml:11:17: error CF-416: point \"flow\" "
               "is defined twice"}));
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\nbit_order: lsb0\n"
                 "points:\n"
                 "  - { name: flow, address: { table: holding, register: 1, "
                 "type: uint16 } }\n"
                 "  - { name: flow, address: { table: holding, register: 2, "
                 "type: uint16 } }\n"
                 "  - { name: word, tag: false, address: { table: holding, "
                 "register: 3, type: uint16 }, bits: { 0: flow } }\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        (Lines{"templates/meter_a.yaml:6:13: error CF-416: point \"flow\" "
               "is defined twice"}));
}

// An instance without a template lists its points; `params`, `disable`
// and `only` are refused (4.5), by stage 3, before expansion.
TEST(Expand, AnInstanceWithoutATemplate) {
    EXPECT_EQ(
        expand({{"drivers/modbus/line1.yaml",
                 channel("  - name: ps1.m1\n"
                         "    unit_id: 1\n"
                         "    points:\n"
                         "      - { name: flow, address: { table: holding, "
                         "register: 1, type: uint16 } }\n")}}),
        Lines{"ps1.m1.flow: {address: {table: holding, register: 1, type: "
              "uint16}}"});
    EXPECT_EQ(
        expand(
            {{"drivers/modbus/line1.yaml", channel("  - name: ps1.m1\n"
                                                   "    unit_id: 1\n"
                                                   "    params: { a: 1 }\n"
                                                   "    disable: [flow]\n")}}),
        (Lines{"drivers/modbus/line1.yaml:9:5: error CF-201: unknown key "
               "\"params\" in devices[0] (no template)",
               "drivers/modbus/line1.yaml:10:5: error CF-201: unknown key "
               "\"disable\" in devices[0] (no template)"}));
}

// 4.3: a cycle, more than 4 parents, and templates of the wrong kind.
TEST(Expand, TheChain) {
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nextends: meter_b\nprotocol: modbus\n"},
                {"templates/meter_b.yaml",
                 "template: meter_b\nextends: meter_a\nprotocol: modbus\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        // Once at each template of the cycle, at its own `extends` (O-6).
        (Lines{"templates/meter_a.yaml:2:10: error CF-401: template "
               "\"meter_a\": extends forms a cycle through \"meter_b\"",
               "templates/meter_b.yaml:2:10: error CF-401: template "
               "\"meter_b\": extends forms a cycle through \"meter_a\""}));
    Files deep{{"drivers/modbus/line1.yaml", one_device()},
               {"templates/meter_a.yaml",
                "template: meter_a\nextends: t1\nprotocol: modbus\n"}};
    // meter_a and five parents t1 to t5: one more than the limit.
    constexpr int PARENTS = 5;
    for (int level = 1; level <= PARENTS; ++level) {
        const std::string name = "t" + std::to_string(level);
        deep.emplace("templates/" + name + ".yaml",
                     "template: " + name + "\nprotocol: modbus\n" +
                         (level < PARENTS
                              ? "extends: t" + std::to_string(level + 1) + "\n"
                              : std::string()));
    }
    EXPECT_EQ(expand(deep),
              (Lines{"templates/meter_a.yaml:2:10: error CF-401: template "
                     "\"meter_a\": extends is deeper than 4 parents"}));
    EXPECT_EQ(
        expand(
            {{"templates/base_a.yaml", "template: base_a\nextends: meter_b\n"},
             {"templates/meter_b.yaml",
              "template: meter_b\nprotocol: modbus\n"},
             {"templates/meter_a.yaml",
              "template: meter_a\nextends: base_a\nprotocol: modbus\n"},
             {"drivers/modbus/line1.yaml", one_device()}}),
        (Lines{"templates/base_a.yaml:2:10: error CF-421: template "
               "\"base_a\": a base template cannot extend a protocol "
               "template"}));
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml", "template: meter_a\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        (Lines{"drivers/modbus/line1.yaml:8:15: error CF-421: template "
               "\"meter_a\": a base template has no protocol; a device uses "
               "a template of its channel's"}));
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml", "template: meter_a\nprotocol: io\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        (Lines{"drivers/modbus/line1.yaml:8:15: error CF-420: template "
               "\"meter_a\" is for \"io\", the channel is \"modbus\""}));
}

// 8: tag names of exactly 127 bytes are valid; 128 is CF-402 at the
// instance, naming the template line; the same for a bit (CF-403).
TEST(Expand, TagNameLengths) {
    // `ps1.m1.` is 7 bytes: a point name of 120 makes a tag of 127.
    constexpr std::size_t FITS = 120;
    const std::string fits(FITS, 'p');
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\nbit_order: lsb0\n"
                 "points:\n"
                 "  - { name: " +
                     fits +
                     ", address: { table: holding, register: 1, type: "
                     "uint16 } }\n"
                     "  - { name: w, tag: false, address: { table: "
                     "holding, register: 2, type: uint16 }, bits: { 0: " +
                     fits + "q } }\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-403: generated tag "
               "\"ps1.m1." +
               fits.substr(0, 57) +
               "\"... is 128 bytes, the limit is 127\n"
               "  from template templates/meter_a.yaml:6:95 (point w)"}));
}

// 65,536 points are valid; one more is CF-414, found while the points are
// made, so a `repeat` over many points never builds more than the limit.
TEST(Expand, ThePointLimit) {
    const auto with = [](std::string_view range) {
        return expand(
            {{"templates/meter_a.yaml",
              "template: meter_a\nprotocol: modbus\n"
              "repeat:\n"
              "  - for: { n: \"" +
                  std::string(range) +
                  "\" }\n"
                  "    points:\n"
                  "      - { name: \"a{n}\", address: { table: holding, "
                  "register: 1, type: uint16 } }\n"
                  "      - { name: \"b{n}\", address: { table: holding, "
                  "register: 2, type: uint16 } }\n"},
             {"drivers/modbus/line1.yaml", one_device()},
             {"system.yaml", std::string(SYSTEM) + "limits: { max_tags: "
                                                   "1000000 }\n"}});
    };
    EXPECT_EQ(with("1..32768").size(), 65536U);
    // CF-414 counts the points that exist after step 4, of the instance
    // (C2 of the review of #146).
    EXPECT_EQ(with("1..32769"),
              Lines{"drivers/modbus/line1.yaml:7:11: error CF-414: template "
                    "\"meter_a\" expands to 65538 points, the limit is "
                    "65536"});
}

// C2 of the review of #146: the limits of 1.3 hold for the expanded tree,
// on running totals; passing one stops the expansion.
TEST(Expand, TheLimitsOfTheExpandedTree) {
    const std::string meter =
        "template: meter_a\nprotocol: modbus\n"
        "repeat:\n"
        "  - for: { n: \"1..4\" }\n"
        "    points:\n"
        "      - { name: \"a{n}\", address: { table: holding, register: 1, "
        "type: uint16 } }\n";
    // `limits.max_tags` (CF-1001): 3 devices of 4 tags pass 10 at the third.
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml", meter},
                {"system.yaml",
                 std::string(SYSTEM) + "limits: { max_tags: 10 }\n"},
                {"drivers/modbus/line1.yaml",
                 channel("  - { name: ps1.m1, template: meter_a, unit_id: 1 "
                         "}\n"
                         "  - { name: ps1.m2, template: meter_a, unit_id: 2 "
                         "}\n"
                         "  - { name: ps1.m3, template: meter_a, unit_id: 3 "
                         "}\n"
                         "  - { name: ps1.m4, template: meter_a, unit_id: 4 "
                         "}\n")}}),
        (Lines{"drivers/modbus/line1.yaml:9:13: error CF-1001: 12 tags, the "
               "limit is 10"}));
    // The nodes an instance makes count before step 4: 40,000 points of
    // more than 100 nodes each pass 4,000,000.
    std::string list = "[";
    constexpr int VALUES = 100;
    for (int value = 0; value < VALUES; ++value) {
        list += (value == 0 ? "" : ", ") + std::to_string(value);
    }
    list += "]";
    EXPECT_EQ(expand({{"templates/meter_a.yaml",
                       "template: meter_a\nprotocol: modbus\n"
                       "repeat:\n"
                       "  - for: { n: \"1..40000\" }\n"
                       "    points:\n"
                       "      - { name: \"a{n}\", invalid_values: " +
                           list +
                           ", address: { table: holding, register: 1, type: "
                           "uint16 } }\n"},
                      {"drivers/modbus/line1.yaml", one_device()}}),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-113: the "
                     "expanded tree exceeds its limit of 4000000 nodes\n  from "
                     "template templates/meter_a.yaml:6:17"}));
    // Every `repeat` value evaluated counts toward the work of the tree:
    // 62 blocks of 65,536 values pass 4,000,000.
    std::string blocks = "template: meter_a\nprotocol: modbus\nrepeat:\n";
    constexpr int BLOCKS = 62;
    for (int block = 0; block < BLOCKS; ++block) {
        blocks +=
            "  - { for: { n: \"1..65536\" }, when: \"false\", points: "
            "[] }\n";
    }
    EXPECT_EQ(expand({{"templates/meter_a.yaml", blocks},
                      {"drivers/modbus/line1.yaml", one_device()}}),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-113: the repeat "
                     "values of the tree exceed their limit of 4000000\n  from "
                     "template templates/meter_a.yaml:65:5"}));
}

// T6 of the review of #146: what a swap of two steps, or of two loops,
// would change, and the edges of each limit and pattern.
namespace {

// A template `meter_a` of Modbus with `body` after its header, and one
// instance with `more`.
Lines one_template(std::string_view body, std::string_view more = {}) {
    return expand(
        {{"templates/meter_a.yaml",
          "template: meter_a\nprotocol: modbus\n" + std::string(body)},
         {"drivers/modbus/line1.yaml", one_device(more)}});
}

constexpr std::string_view ADDRESS =
    "address: { table: holding, register: 1, type: uint16 }";

}  // namespace

// Two `for` variables: the first outermost (4.4).
TEST(Expand, TheFirstVariableIsOutermost) {
    EXPECT_EQ(one_template("repeat:\n"
                           "  - for: { a: \"1..2\", b: \"1..2\" }\n"
                           "    points: [ { name: \"p{a}_{b}\", " +
                           std::string(ADDRESS) + " } ]\n"),
              (Lines{"ps1.m1.p1_1: {address: {table: holding, register: 1, "
                     "type: uint16}}",
                     "ps1.m1.p1_2: {address: {table: holding, register: 1, "
                     "type: uint16}}",
                     "ps1.m1.p2_1: {address: {table: holding, register: 1, "
                     "type: uint16}}",
                     "ps1.m1.p2_2: {address: {table: holding, register: 1, "
                     "type: uint16}}"}));
}

// A level's `disable` applies after its own points are joined (3.4), and
// an override needs a point that exists after step 4 (CF-413).
TEST(Expand, TheOrderOfDisableAndOfOverrides) {
    EXPECT_EQ(one_template("disable: [q]\npoints:\n  - { name: p, " +
                           std::string(ADDRESS) + " }\n  - { name: q, " +
                           std::string(ADDRESS) + " }\n"),
              (Lines{"ps1.m1.p: {address: {table: holding, register: 1, type: "
                     "uint16}}"}));
    EXPECT_EQ(one_template("points:\n  - { name: p, when: \"false\", " +
                               std::string(ADDRESS) + " }\n  - { name: q, " +
                               std::string(ADDRESS) + " }\n",
                           "    overrides: { p: { unit: V } }\n"),
              (Lines{"drivers/modbus/line1.yaml:10:18: error CF-413: point "
                     "\"p\" does not exist in template \"meter_a\""}));
}

// 2.5: `?` is one character of a segment, never a dot; `*` stays within a
// segment.
TEST(Expand, PatternsStayWithinASegment) {
    const std::string points = "points:\n  - { name: a.b, " +
                               std::string(ADDRESS) + " }\n  - { name: ab, " +
                               std::string(ADDRESS) + " }\n";
    EXPECT_EQ(one_template(points, "    disable: [\"a?b\"]\n"),
              (Lines{"drivers/modbus/line1.yaml:10:15: error CF-413: point "
                     "\"a?b\" does not exist in template \"meter_a\""}));
    EXPECT_EQ(one_template(points, "    disable: [\"a*\"]\n"),
              (Lines{"ps1.m1.a.b: {address: {table: holding, register: 1, "
                     "type: uint16}}"}));
}

// The edges of the `repeat` limits (CF-113, 4.4): 65,536 values pass and
// 65,537 do not; so for the pairs of two variables; a `for` value that is
// no range or INT is CF-523.
TEST(Expand, TheEdgesOfRepeat) {
    const auto block = [](std::string_view range) {
        return "repeat:\n  - for: " + std::string(range) + "\n    points: []\n";
    };
    EXPECT_EQ(one_template(block("{ n: \"1..65536\" }")), Lines{});
    EXPECT_EQ(one_template(block("{ n: \"1..65537\" }")),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-113: the range "
                     "of \"n\" exceeds its limit of 65536\n  from template "
                     "templates/meter_a.yaml:4:15"}));
    EXPECT_EQ(one_template(block("{ a: \"1..256\", b: \"1..256\" }")), Lines{});
    EXPECT_EQ(one_template(block("{ a: \"1..256\", b: \"1..257\" }")),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-113: the values "
                     "of a repeat block exceed its limit of 65536\n  from "
                     "template templates/meter_a.yaml:4:10"}));
    EXPECT_EQ(one_template("params: { c: { type: bool, default: true } }\n" +
                           block("{ n: \"c\" }")),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-523: "
                     "expression: the value must be a range or an INT, not "
                     "BOOL\n  from template templates/meter_a.yaml:5:15"}));
}

// The edges of a parameter: its `min` itself passes; a quoted "true" is no
// bool (1.2); a bit name given twice is CF-416; one cause is reported once.
TEST(Expand, TheEdgesOfParametersAndBits) {
    const std::string params =
        "params: { k: { type: int, default: 1, min: 2 }, f: { type: bool, "
        "default: false } }\n"
        "points:\n  - { name: p, " +
        std::string(ADDRESS) + " }\n";
    EXPECT_EQ(one_template(params, "    params: { k: 2 }\n"),
              (Lines{"ps1.m1.p: {address: {table: holding, register: 1, type: "
                     "uint16}}"}));
    EXPECT_EQ(one_template(params, "    params: { k: 2, f: \"true\" }\n"),
              (Lines{"drivers/modbus/line1.yaml:10:24: error CF-412: parameter "
                     "\"f\": must be true or false"}));
    EXPECT_EQ(
        one_template("bit_order: lsb0\npoints:\n  - { name: w, tag: "
                     "false, " +
                     std::string(ADDRESS) + ", bits: { 0: run, 1: run } }\n"),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-416: point \"run\" is "
               "defined twice\n  from template templates/meter_a.yaml:5:103 "
               "(point w)"}));
    // A default that fails under two points of one instance is one cause
    // at one instance: one error, with no count of instances.
    EXPECT_EQ(one_template("params: { k: { type: int, default: 0 } }\n"
                           "defaults: { scale: { gain: \"1 DIV k\" } }\n"
                           "points:\n  - { name: p, " +
                           std::string(ADDRESS) + " }\n  - { name: q, " +
                           std::string(ADDRESS) + " }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-410: expression "
                     "\"1 DIV k\": division by zero\n  from template "
                     "templates/meter_a.yaml:4:31 (point p; k = 0)"}));
}

// `expanded_diagnostic` (expand.h) places a diagnostic of a later stage as
// the expander does: at the instance, with the template's line as a note.
TEST(Expand, AnExpandedDiagnostic) {
    std::optional<test::TempDirectory> temp =
        test::TempDirectory::create("scada-conf-expand-");
    ASSERT_TRUE(temp.has_value());
    if (!temp) {
        return;
    }
    Files all{{"templates/meter_a.yaml",
               "template: meter_a\nprotocol: modbus\npoints:\n  - { name: p, " +
                   std::string(ADDRESS) + " }\n"},
              {"drivers/modbus/line1.yaml", one_device()},
              {"system.yaml", std::string(SYSTEM)}};
    for (const auto& [path, content] : all) {
        const fs::path full = temp->path() / path;
        fs::create_directories(full.parent_path());
        std::ofstream(full, std::ios::binary) << content;
    }
    const Result<Validation> validation = validate_and_expand(temp->path());
    ASSERT_TRUE(validation.ok());
    if (!validation.ok() || !validation.value().expanded) {
        FAIL() << "no expanded tree";
        return;
    }
    const ExpandedTree& tree = *validation.value().expanded;
    // The node of the point's `register` value, written in the template.
    std::optional<NodeId> from_template;
    std::optional<NodeId> from_instance;
    for (NodeId node = 0; node < tree.document.size(); ++node) {
        const Location place = tree.document.location(node);
        if (tree.document.kind(node) == NodeKind::SCALAR &&
            tree.document.text(node) == "1" &&
            place.file == "templates/meter_a.yaml") {
            from_template = node;
        }
        if (tree.document.kind(node) == NodeKind::SCALAR &&
            tree.document.text(node) == "ps1.m1") {
            from_instance = node;
        }
    }
    ASSERT_TRUE(from_template && from_instance);
    if (!from_template || !from_instance) {
        return;
    }
    EXPECT_EQ(format(expanded_diagnostic(tree, 1, *from_template, "a message",
                                         "point p")),
              "drivers/modbus/line1.yaml:7:11: error CF-1: a message\n  from "
              "template templates/meter_a.yaml:4:53 (point p)");
    EXPECT_EQ(format(expanded_diagnostic(tree, 1, *from_instance, "a message")),
              "drivers/modbus/line1.yaml:7:11: error CF-1: a message");
}

TEST(Expand, APointOfAProtocolTemplateNeedsAnAddress) {
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\n"
                 "points: [ { name: flow, unit: l } ]\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-417: point \"flow\" "
               "has no address\n"
               "  from template templates/meter_a.yaml:3:19 (point flow)"}));
}

TEST(Expand, AnExpressionThatFailsIsCf410) {
    EXPECT_EQ(expand({{"templates/meter_a.yaml",
                       "template: meter_a\nprotocol: modbus\n"
                       "params: { k: { type: int, default: 0 } }\n"
                       "points:\n"
                       "  - { name: flow, address: { table: holding, register: "
                       "\"100 DIV k\", type: uint16 } }\n"},
                      {"drivers/modbus/line1.yaml", one_device()}}),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-410: expression "
                     "\"100 DIV k\": division by zero\n"
                     "  from template templates/meter_a.yaml:5:61 (point flow; "
                     "k = 0)"}));
}

// A parameter that no level of the chain gives a type is CF-202.
TEST(Expand, AParameterNeedsAType) {
    EXPECT_EQ(expand({{"templates/meter_a.yaml",
                       "template: meter_a\nprotocol: modbus\n"
                       "params: { inputs: { default: 4 } }\n"},
                      {"drivers/modbus/line1.yaml", one_device()}}),
              Lines{"templates/meter_a.yaml:3:11: error CF-202: parameter "
                    "\"inputs\" needs type"});
}

// B5 and B6 of the review of #144: at the instance, over its whole chain,
// a configuration expression that fails is CF-410, a type included, in the
// form of 2.4.
TEST(Expand, NamesAndTypesAtTheInstance) {
    EXPECT_EQ(expand({{"templates/meter_a.yaml",
                       "template: meter_a\nprotocol: modbus\n"
                       "params: { inputs: { type: int, default: 2 } }\n"
                       "points:\n"
                       "  - { name: p, when: \"inputs + 1\", address: { table: "
                       "holding, register: 1, type: uint16 } }\n"},
                      {"drivers/modbus/line1.yaml", one_device()}}),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-410: expression "
                     "\"inputs + 1\": the value must be BOOL, not INT\n"
                     "  from template templates/meter_a.yaml:5:22"}));
}

// Every expression of a point's keys is a number (2.4), at the instance
// as stage 5 types a template's (D-113): an override or an instance's own
// point that gives a BOOL is CF-410, not a register `true`.
TEST(Expand, ThePointsExpressionsAreNumbers) {
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\n"
                 "params: { base: { type: int, default: 4 } }\n"
                 "points:\n"
                 "  - { name: p, address: { table: holding, register: 1, "
                 "type: uint16 } }\n"},
                {"drivers/modbus/line1.yaml",
                 one_device("    overrides: { p: { address: { register: "
                            "\"base > 2\" } } }\n"
                            "    points:\n"
                            "      - { name: q, address: { table: holding, "
                            "register: \"1 > 2\", type: uint16 } }\n")}}),
        (Lines{"drivers/modbus/line1.yaml:10:50: error CF-410: expression "
               "\"base > 2\": the value must be an INT, not BOOL",
               "drivers/modbus/line1.yaml:12:60: error CF-410: expression "
               "\"1 > 2\": the value must be an INT, not BOOL"}));
}

// The identity checks (4.8) are evaluated with the parameters, the
// template's and the instance's appended to them, and typed as numbers.
TEST(Expand, TheIdentityChecksAreEvaluated) {
    const std::string meter =
        "template: meter_a\nprotocol: modbus\n"
        "params: { base: { type: int, default: 4 } }\n"
        "identify:\n"
        "  - { address: { table: holding, register: \"base + 1\", type: "
        "uint16 }, equals: 7 }\n"
        "points:\n"
        "  - { name: p, address: { table: holding, register: 1, type: "
        "uint16 } }\n";
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml", meter},
                {"drivers/modbus/line1.yaml",
                 one_device("    params: { base: 10 }\n"
                            "    identify:\n"
                            "      - { address: { table: holding, register: "
                            "\"base * 2\", type: uint16 }, range: [1, "
                            "\"base\"] }\n")}},
               true),
        (Lines{"ps1.m1: {identify: [{address: {table: holding, register: 11, "
               "type: uint16}, equals: 7}, {address: {table: holding, "
               "register: 20, type: uint16}, range: [1, 10]}], unit_id: 1}",
               "ps1.m1.p: {address: {table: holding, register: 1, type: "
               "uint16}}"}));
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml", meter},
                {"drivers/modbus/line1.yaml",
                 one_device("    identify:\n"
                            "      - { address: { table: holding, register: "
                            "\"1 > 2\", type: uint16 }, equals: 7 }\n")}}),
        Lines{"drivers/modbus/line1.yaml:11:51: error CF-410: expression "
              "\"1 > 2\": the value must be an INT, not BOOL"});
}

// --- The review of #146 ------------------------------------------------

// B1: a range over the whole INT span is refused from its bounds; its
// count does not wrap to 0.
TEST(Expand, ARangeOverTheWholeIntSpan) {
    EXPECT_EQ(expand({{"templates/meter_a.yaml",
                       "template: meter_a\nprotocol: modbus\n"
                       "repeat:\n"
                       "  - for: { n: \"-9223372036854775807 - "
                       "1..9223372036854775807\" }\n"
                       "    points:\n"
                       "      - { name: \"p{n}\", address: { table: holding, "
                       "register: 1, type: uint16 } }\n"},
                      {"drivers/modbus/line1.yaml", one_device()}}),
              Lines{"drivers/modbus/line1.yaml:7:11: error CF-113: the range "
                    "of \"n\" exceeds its limit of 65536\n"
                    "  from template templates/meter_a.yaml:4:15"});
}

// B2: without a template steps 1 to 3 are empty, and steps 4 to 8 apply to
// the instance's points: its `when`, defaults (with `invalid_values` by raw
// type) and overrides.
TEST(Expand, ADeviceWithoutTemplate) {
    EXPECT_EQ(
        expand({{"drivers/modbus/line1.yaml",
                 channel("  - name: d1\n"
                         "    unit_id: 1\n"
                         "    defaults: { poll_group: fast, address: { table: "
                         "holding, type: uint16 }, invalid_values: { uint16: "
                         "[65535] } }\n"
                         "    overrides: { p: { unit: V } }\n"
                         "    points:\n"
                         "      - { name: p, address: { register: 1 } }\n"
                         "      - { name: q, when: \"false\", address: { "
                         "register: 2 } }\n")}}),
        Lines{"d1.p: {poll_group: fast, address: {table: holding, type: "
              "uint16, register: 1}, invalid_values: [65535], unit: V}"});
}

// B3: an instance's own point whose `when` is false does not exist, with a
// template too; its `when` has the parameters.
TEST(Expand, TheWhenOfAnInstancesPoint) {
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\n"
                 "params: { o: { type: bool, default: false } }\n"
                 "points:\n"
                 "  - { name: p, address: { table: holding, register: 1, "
                 "type: uint16 } }\n"},
                {"drivers/modbus/line1.yaml",
                 one_device("    points:\n"
                            "      - { name: y, when: \"false\", address: { "
                            "table: holding, register: 2, type: uint16 } }\n"
                            "      - { name: z, when: o, address: { table: "
                            "holding, register: 3, type: uint16 } }\n"
                            "      - { name: w, when: \"NOT o\", address: { "
                            "table: holding, register: 4, type: uint16 } "
                            "}\n")}}),
        (Lines{"ps1.m1.p: {address: {table: holding, register: 1, type: "
               "uint16}}",
               "ps1.m1.w: {address: {table: holding, register: 4, type: "
               "uint16}}"}));
}

// B4 (the ruling O-1): a block's `when` is carried on each point it makes
// and decided at step 4, so a child that refines a point of an absent
// option does not bring it back, and its `disable` of it always matches.
TEST(Expand, ABlocksWhenIsDecidedAtStepFour) {
    const std::string core =
        "template: inv_core\nprotocol: modbus\n"
        "params: { has_dc: { type: bool, default: false } }\n"
        "repeat:\n"
        "  - for: { n: \"1..2\" }\n"
        "    when: has_dc\n"
        "    points:\n"
        "      - { name: \"pv{n}.voltage\", unit: V, address: { table: "
        "holding, register: \"100 + n\", type: uint16 } }\n";
    const std::string refines =
        "template: inv_b\nextends: inv_core\nprotocol: modbus\n"
        "points: [ { name: pv1.voltage, address: { register: 300 } } ]\n";
    const auto device = [](std::string_view more) {
        return channel(
            "  - name: ps1.i1\n    template: inv_b\n    unit_id: 1\n" +
            std::string(more));
    };
    EXPECT_EQ(expand({{"templates/inv_core.yaml", core},
                      {"templates/inv_b.yaml", refines},
                      {"drivers/modbus/line1.yaml", device("")}}),
              Lines{});
    EXPECT_EQ(expand({{"templates/inv_core.yaml", core},
                      {"templates/inv_b.yaml", refines},
                      {"drivers/modbus/line1.yaml",
                       device("    params: { has_dc: true }\n")}}),
              (Lines{"ps1.i1.pv1.voltage: {unit: V, address: {table: holding, "
                     "register: 300, type: uint16}}",
                     "ps1.i1.pv2.voltage: {unit: V, address: {table: holding, "
                     "register: 102, type: uint16}}"}));
    EXPECT_EQ(expand({{"templates/inv_core.yaml", core},
                      {"templates/inv_b.yaml",
                       "template: inv_b\nextends: inv_core\nprotocol: "
                       "modbus\ndisable: [\"pv1.voltage\"]\n"},
                      {"drivers/modbus/line1.yaml", device("")}}),
              Lines{});
}

// B5: `invalid_values` of `defaults` by raw type is picked after the
// overrides, which may change the raw type.
TEST(Expand, InvalidValuesByTheRawTypeAfterTheOverrides) {
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\n"
                 "defaults: { invalid_values: { uint16: [65535], int16: "
                 "[32768], float32: [nan] } }\n"
                 "points:\n"
                 "  - { name: p, address: { table: holding, register: 1, "
                 "type: uint16 } }\n"
                 "  - { name: q, address: { table: holding, register: 2, "
                 "type: uint16 } }\n"},
                {"drivers/modbus/line1.yaml",
                 one_device("    overrides:\n"
                            "      p: { address: { type: int16 } }\n"
                            "      q: { address: { type: float32 } }\n")}}),
        (Lines{"ps1.m1.p: {invalid_values: [32768], address: {table: holding, "
               "register: 1, type: int16}}",
               "ps1.m1.q: {invalid_values: [nan], address: {table: holding, "
               "register: 2, type: float32}}"}));
}

// B6 as the lead corrected it: keys the schema types as strings
// (`write.values`) are compared as text, so `16` and `0x10` stay two keys.
TEST(Expand, StringKeysMergeAsText) {
    EXPECT_EQ(
        expand({{"templates/base_a.yaml",
                 "template: base_a\nprotocol: modbus\n"
                 "points: [ { name: c, access: rw, address: { table: holding, "
                 "register: 1, type: uint16, write: { values: { 16: 1 } } } } "
                 "]\n"},
                {"templates/meter_a.yaml",
                 "template: meter_a\nextends: base_a\nprotocol: modbus\n"
                 "points: [ { name: c, address: { write: { values: { 0x10: 2 } "
                 "} } } ]\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        Lines{"ps1.m1.c: {access: rw, address: {table: holding, register: 1, "
              "type: uint16, write: {values: {16: 1, 0x10: 2}}}}"});
}

// B6: map keys of the integer form merge by their value: a child's `0xF`
// of `bits` is the parent's 15, and its spelling and value win.
TEST(Expand, IntegerKeysMergeByValue) {
    EXPECT_EQ(
        expand({{"templates/base_a.yaml",
                 "template: base_a\n"
                 "points: [ { name: w, tag: false, bits: { 0: run, 15: hi } "
                 "}, { name: s, enum: { 16: a, 1: b } } ]\n"},
                {"templates/meter_a.yaml",
                 "template: meter_a\nextends: base_a\nprotocol: modbus\n"
                 "bit_order: lsb0\n"
                 "points:\n"
                 "  - { name: w, address: { table: holding, register: 1, "
                 "type: uint16 }, bits: { 0xF: top } }\n"
                 "  - { name: s, enum: { 0x10: c }, address: { table: "
                 "holding, register: 2, type: uint16 } }\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        (Lines{"ps1.m1.w: {tag: false, bits: {0: run, 0xF: top}, address: "
               "{table: holding, register: 1, type: uint16}}",
               "ps1.m1.s: {enum: {0x10: c, 1: b}, address: {table: holding, "
               "register: 2, type: uint16}}"}));
}

// B7: a name made by expansion follows 2.1 (CF-212), and the message
// names a `{...}` that no variable bound to the point fills.
TEST(Expand, NamesMadeByExpansion) {
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\n"
                 "params: { x: { type: int, default: 1 } }\n"
                 "points:\n"
                 "  - { name: \"s{n}\", address: { table: holding, "
                 "register: 1, type: uint16 } }\n"
                 "repeat:\n"
                 "  - for: { n: \"-1..1\" }\n"
                 "    points:\n"
                 "      - { name: \"x.{n}\", address: { table: holding, "
                 "register: \"10 + n\", type: uint16 } }\n"
                 "      - { name: \"t{n}.u{x}\", address: { table: "
                 "holding, register: \"20 + n\", type: uint16 } }\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        (Lines{
            // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): long lines
            "drivers/modbus/line1.yaml:7:11: error CF-212: \"s{n}\" is not a "
            "valid point name: {n} names no variable bound to this point\n  "
            "from template templates/meter_a.yaml:5:13 (point s{n})",
            "drivers/modbus/line1.yaml:7:11: error CF-212: \"x.-1\" is not a "
            "valid point name\n  from template templates/meter_a.yaml:9:17 "
            "(point x.-1)",
            "drivers/modbus/line1.yaml:7:11: error CF-212: \"t-1.u{x}\" is not "
            "a valid point name: {x} names no variable bound to this point\n  "
            "from template templates/meter_a.yaml:10:17 (point t-1.u{x})",
            "drivers/modbus/line1.yaml:7:11: error CF-212: \"t0.u{x}\" is not "
            "a valid point name: {x} names no variable bound to this point\n  "
            "from template templates/meter_a.yaml:10:17 (point t0.u{x})",
            "drivers/modbus/line1.yaml:7:11: error CF-212: \"t1.u{x}\" is not "
            "a valid point name: {x} names no variable bound to this point\n  "
            "from template templates/meter_a.yaml:10:17 (point t1.u{x})"}));
}

// B8: `identify` names what exists: a name that no parameter gives is
// CF-410 at the instance.
TEST(Expand, TheNamesOfIdentify) {
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\n"
                 "identify:\n"
                 "  - { address: { table: holding, register: \"100 + "
                 "zz\", type: uint16 }, equals: 7 }\n"
                 "points:\n"
                 "  - { name: p, address: { table: holding, register: "
                 "1, type: uint16 } }\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        (Lines{
            "drivers/modbus/line1.yaml:7:11: error CF-410: expression \"100 + "
            "zz\": \"zz\" is not a parameter and no variable bound to this "
            "point\n  from template templates/meter_a.yaml:4:51 (identify)"}));
}

// B9 and the ruling O-9: a key the schema types as an integer takes an
// INT; a `number` parameter written as an integer is an INT.
TEST(Expand, AnIntegerKeyTakesAnInt) {
    const std::string meter =
        "template: meter_a\nprotocol: modbus\n"
        "params: { b: { type: number, default: 1.5 } }\n"
        "points:\n"
        "  - { name: p, address: { table: holding, register: \"b + 2\", "
        "type: uint16 } }\n"
        "  - { name: q, address: { table: holding, register: \"10 / 2\", "
        "type: uint16 } }\n"
        "  - { name: r, scale: { divide_by: \"2.5\" }, address: { table: "
        "holding, register: 3, type: uint16 } }\n"
        "  - { name: g, scale: { gain: \"b * 2\" }, address: { table: "
        "holding, register: 4, type: uint16 } }\n";
    EXPECT_EQ(expand({{"templates/meter_a.yaml", meter},
                      {"drivers/modbus/line1.yaml",
                       one_device("    params: { b: 40 }\n")}}),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-410: expression "
                     "\"10 / 2\": the value must be an INT, not REAL\n  from "
                     "template templates/meter_a.yaml:6:57 (point q)",
                     "drivers/modbus/line1.yaml:7:11: error CF-410: expression "
                     "\"2.5\": the value must be an INT, not REAL\n  from "
                     "template templates/meter_a.yaml:7:37 (point r)"}));
}

// B10: `when` in `defaults` or `overrides` is CF-419 (4.4), at the
// template's or the instance's line.
TEST(Expand, WhenInDefaultsOrOverrides) {
    EXPECT_EQ(expand({{"templates/meter_a.yaml",
                       "template: meter_a\nprotocol: modbus\n"
                       "defaults: { when: \"true\" }\n"
                       "points:\n"
                       "  - { name: p, address: { table: holding, register: 1, "
                       "type: uint16 } }\n"},
                      {"templates/meter_b.yaml",
                       "template: meter_b\nprotocol: modbus\n"
                       "points:\n"
                       "  - { name: p, address: { table: holding, register: 1, "
                       "type: uint16 } }\n"},
                      {"drivers/modbus/line1.yaml",
                       channel("  - name: ps1.m1\n    template: meter_b\n"
                               "    unit_id: 1\n"
                               "    defaults: { when: \"true\" }\n"
                               "    overrides: { p: { when: false } }\n")}}),
              (Lines{"drivers/modbus/line1.yaml:10:23: error CF-419: when "
                     "cannot be given in defaults",
                     "drivers/modbus/line1.yaml:11:29: error CF-419: when "
                     "cannot be given in overrides",
                     "templates/meter_a.yaml:3:19: error CF-419: when cannot "
                     "be given in defaults"}));
}

// B11: a base template has no `address`, `read` or `identify` (4.1).
TEST(Expand, ABaseTemplateHasNoAddress) {
    EXPECT_EQ(
        expand({{"templates/base_a.yaml",
                 "template: base_a\n"
                 "read: { max_registers: 10 }\n"
                 "identify: [ { address: { table: holding, register: 1 }, "
                 "equals: 7 } ]\n"
                 "points:\n"
                 "  - { name: p, address: { table: holding, register: 1 } }\n"
                 "repeat:\n"
                 "  - for: { n: \"1..2\" }\n"
                 "    points: [ { name: \"q{n}\", address: { register: 2 } } "
                 "]\n"}}),
        (Lines{"templates/base_a.yaml:2:7: error CF-421: template \"base_a\": "
               "a base template has no read",
               "templates/base_a.yaml:3:11: error CF-421: template \"base_a\": "
               "a base template has no identify",
               "templates/base_a.yaml:5:25: error CF-421: template \"base_a\": "
               "a base template has no address",
               "templates/base_a.yaml:8:40: error CF-421: template \"base_a\": "
               "a base template has no address"}));
}

// The ruling O-3: a `repeat` variable with the name of a parameter of the
// chain is refused at the instance (CF-412): a child's variable would
// change a parent's expression that meant the parameter.
TEST(Expand, AVariableWithTheNameOfAParameter) {
    EXPECT_EQ(
        expand({{"templates/base_a.yaml",
                 "template: base_a\nprotocol: modbus\n"
                 "params: { n: { type: int, default: 100 } }\n"
                 "points:\n"
                 "  - { name: p, address: { table: holding, register: \"n + "
                 "1\", type: uint16 } }\n"},
                {"templates/meter_a.yaml",
                 "template: meter_a\nextends: base_a\nprotocol: modbus\n"
                 "repeat:\n"
                 "  - for: { n: \"1..2\" }\n"
                 "    points: [ { name: \"x{n}\", unit: V, address: { table: "
                 "holding, register: \"n\", type: uint16 } } ]\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-412: parameter \"n\": "
               "a repeat variable cannot have its name\n  from template "
               "templates/meter_a.yaml:5:12 (parameter n)"}));
}

// The ruling O-6: a template's own fault is reported once, at it, even
// with no instance; a cause in a template met by many instances is one
// error at the first, with their count.
TEST(Expand, ATemplatesFaultOnceAndOneCauseOnce) {
    EXPECT_EQ(expand({{"templates/meter_a.yaml",
                       "template: meter_a\nprotocol: modbus\n"
                       "params: { k: { type: int, default: 1 }, k2: { type: "
                       "int } }\n"},
                      {"templates/meter_b.yaml",
                       "template: meter_b\nextends: meter_a\nprotocol: modbus\n"
                       "params: { k: { type: number } }\n"}}),
              (Lines{"templates/meter_b.yaml:4:22: error CF-415: parameter "
                     "\"k\" cannot change its type"}));
    std::string devices;
    for (int unit = 1; unit <= 4; ++unit) {
        devices += "  - { name: ps1.m" + std::to_string(unit) +
                   ", template: meter_a, unit_id: " + std::to_string(unit) +
                   ", params: { k: 0 } }\n";
    }
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\n"
                 "params: { k: { type: int, default: 1 } }\n"
                 "points:\n"
                 "  - { name: p, address: { table: holding, register: "
                 "\"100 DIV k\", type: uint16 } }\n"},
                {"drivers/modbus/line1.yaml", channel(devices)}}),
        (Lines{"drivers/modbus/line1.yaml:7:13: error CF-410: expression \"100 "
               "DIV k\": division by zero (and 3 more instances)\n  from "
               "template templates/meter_a.yaml:5:58 (point p; k = 0)"}));
}

// The rulings O-4 and O-7 on #146, as the code reads them: `AND` evaluates
// both sides, and a second `for` variable's range sees the parameters only.
TEST(Expand, BothSidesOfAndAndTheSecondRange) {
    EXPECT_EQ(expand({{"templates/meter_a.yaml",
                       "template: meter_a\nprotocol: modbus\n"
                       "params: { k: { type: int, default: 0 } }\n"
                       "points:\n"
                       "  - { name: p, when: \"false AND 1 DIV k = 0\", "
                       "address: { table: holding, register: 1, type: uint16 "
                       "} }\n"},
                      {"drivers/modbus/line1.yaml", one_device()}}),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-410: expression "
                     "\"false AND 1 DIV k = 0\": division by zero\n  from "
                     "template templates/meter_a.yaml:5:35 (point p; k = 0)"}));
    EXPECT_EQ(expand({{"templates/meter_a.yaml",
                       "template: meter_a\nprotocol: modbus\n"
                       "repeat:\n"
                       "  - for: { n: \"1..2\", k: \"1..n\" }\n"
                       "    points:\n"
                       "      - { name: \"p{n}_{k}\", address: { table: "
                       "holding, register: \"n + k\", type: uint16 } }\n"},
                      {"drivers/modbus/line1.yaml", one_device()}}),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-410: expression "
                     "\"1..n\": \"n\" is not a parameter and no variable bound "
                     "to this point\n  from template "
                     "templates/meter_a.yaml:4:30 (variable k)"}));
}

// B2 in the expander: an alarm's expressions read tags, also on a bit of a
// template's point; they are not evaluated.
TEST(Expand, AlarmsOnABitAreNotEvaluated) {
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\nbit_order: lsb0\n"
                 "points:\n"
                 "  - name: w\n"
                 "    tag: false\n"
                 "    address: { table: holding, register: 1, type: uint16 }\n"
                 "    bits:\n"
                 "      0: { name: fault, alarms: [ { type: state, alarm_on: "
                 "true, priority: high, suppress_when: ps1.station.maint } ] "
                 "}\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        Lines{"ps1.m1.w: {tag: false, address: {table: holding, register: 1, "
              "type: uint16}, bits: {0: {name: fault, alarms: [{type: state, "
              "alarm_on: true, priority: high, suppress_when: "
              "ps1.station.maint}]}}}"});
}

// A CF-410 in a block scalar is at the scalar, with its place within the
// text, as for CF-520 (D-109; the lead's note on #144).
TEST(Expand, Cf410InABlockScalarNamesItsPlaceInTheText) {
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\n"
                 "params: { k: { type: int, default: 0 } }\n"
                 "points:\n"
                 "  - name: p\n"
                 "    address:\n"
                 "      table: holding\n"
                 "      type: uint16\n"
                 "      register: |\n"
                 "        100 +\n"
                 "        1 DIV k\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        (Lines{
            "drivers/modbus/line1.yaml:7:11: error CF-410: expression "
            "\"100 +\\n1 DIV k\\n\": division by zero at line 2, column 3 "
            "of the expression's text\n"
            "  from template templates/meter_a.yaml:9:17 (point p; k = 0)"}));
}

}  // namespace
}  // namespace scada::conf
