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
// in the form of contract 1.4 (with their notes), or its expanded points.
Lines expand(const Files& files) {
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
    return points_of(*result.expanded);
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
              Lines{"drivers/modbus/line1.yaml:7:11: error CF-415: parameter "
                    "\"inputs\" cannot change its type\n"
                    "  from template templates/meter_a.yaml:4:27"});
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
               "variable n is 1 in base_a and 2 in meter_a\n"
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
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-416: point \"flow\" "
               "is defined twice\n"
               "  from template templates/meter_a.yaml:6:13 (point flow)"}));
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
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-401: template "
               "\"meter_a\": extends forms a cycle through \"meter_a\"\n"
               "  from template templates/meter_b.yaml:2:10"}));
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
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-401: template "
                     "\"meter_a\": extends is deeper than 4 parents\n"
                     "  from template templates/t4.yaml:3:10"}));
    EXPECT_EQ(
        expand(
            {{"templates/base_a.yaml", "template: base_a\nextends: meter_b\n"},
             {"templates/meter_b.yaml",
              "template: meter_b\nprotocol: modbus\n"},
             {"templates/meter_a.yaml",
              "template: meter_a\nextends: base_a\nprotocol: modbus\n"},
             {"drivers/modbus/line1.yaml", one_device()}}),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-421: template "
               "\"base_a\": a base template cannot extend a protocol "
               "template\n"
               "  from template templates/base_a.yaml:2:10"}));
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
               "\"meter_a\" is for io, the channel is modbus"}));
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
             {"drivers/modbus/line1.yaml", one_device()}});
    };
    EXPECT_EQ(with("1..32768").size(), 65536U);
    EXPECT_EQ(with("1..32769"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-414: template "
                     "\"meter_a\" expands to more than 65536 points, the "
                     "limit is 65536\n"
                     "  from template templates/meter_a.yaml:6:17"}));
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
                     "  from template templates/meter_a.yaml:5:61"}));
}

// --- Stage 5 on merged points (D-115) -------------------------------------

// A template `meter_a` of Modbus with the points given, and one instance.
Lines merged(std::string_view points, std::string_view more = {},
             std::string_view base = {}) {
    Files files{{"templates/meter_a.yaml",
                 std::string("template: meter_a\nprotocol: modbus\n") +
                     (base.empty() ? "" : "extends: base_a\n") +
                     "points:\n" + std::string(points)},
                {"drivers/modbus/line1.yaml", one_device(more)}};
    if (!base.empty()) {
        files.emplace("templates/base_a.yaml",
                      "template: base_a\npoints:\n" + std::string(base));
    }
    return expand(files);
}

// CF-504 (D-134): an integer value type on a floating raw type, unless the
// merged point is an enumeration or a double point; a counter on one
// always (C5 of the review of #132).
TEST(MergedPoints, IntegerValueTypesOnFloatingRawTypes) {
    EXPECT_EQ(merged("  - { name: p, value_type: int64, address: { table: "
                     "input, register: 10, type: float32 } }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-504: "
                     "\"ps1.m1.p\": value_type int64 cannot hold float32\n"
                     "  from template templates/meter_a.yaml:4:28 (point p)"}));
    // An enumeration of the base template makes it a code: valid.
    EXPECT_EQ(merged("  - { name: p, value_type: int64, address: { table: "
                     "input, register: 10, type: float32 } }\n",
                     {}, "  - { name: p, enum: { 0: off, 1: on } }\n"),
              Lines{"ps1.m1.p: {enum: {0: off, 1: on}, value_type: int64, "
                    "address: {table: input, register: 10, type: float32}}"});
    // An enumeration does not make a counter valid.
    EXPECT_EQ(merged("  - { name: p, enum: { 0: off, 1: on }, counter: { "
                     "limit: 100, at_limit: wrap }, address: { table: input, "
                     "register: 10, type: float32 } }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-504: "
                     "\"ps1.m1.p\": a counter cannot hold float32\n"
                     "  from template templates/meter_a.yaml:4:50 (point p)"}));
    // An override in the instance's file is at its own line.
    EXPECT_EQ(merged("  - { name: p, address: { table: input, register: 10, "
                     "type: float64 } }\n",
                     "    overrides: { p: { value_type: uint64 } }\n"),
              (Lines{"drivers/modbus/line1.yaml:10:35: error CF-504: "
                     "\"ps1.m1.p\": value_type uint64 cannot hold float64"}));
}

// CF-517 at stage 5: keys that merging brought together, and a value type
// derived from an enumeration.
TEST(MergedPoints, IntegerScalingWhereMergingBringsTheKeys) {
    EXPECT_EQ(merged("  - { name: p, scale: { divide_by: 10 }, address: { "
                     "table: input, register: 10, type: int16 } }\n",
                     {}, "  - { name: p, value_type: int64 }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-517: "
                     "\"ps1.m1.p\": value_type int64 needs an integer "
                     "scaling; use float64\n"
                     "  from template templates/meter_a.yaml:5:36 (point p)"}));
    EXPECT_EQ(merged("  - { name: p, enum: { 0: a, 5: b }, scale: { gain: "
                     "0.5 }, address: { table: input, register: 10, type: "
                     "uint16 } }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-517: "
                     "\"ps1.m1.p\": value_type int64 needs an integer "
                     "scaling; use float64\n"
                     "  from template templates/meter_a.yaml:4:53 (point p)"}));
    // An integer raw type with a decimal gain derives float64: valid.
    EXPECT_EQ(merged("  - { name: p, scale: { gain: 0.5 }, address: { table: "
                     "input, register: 10, type: uint16 } }\n"),
              Lines{"ps1.m1.p: {scale: {gain: 0.5}, address: {table: input, "
                    "register: 10, type: uint16}}"});
}

// CF-516: every raw value times the gain plus the offset fits the integer
// value type; without a scaling, a raw type it cannot hold is CF-504.
TEST(MergedPoints, IntegerScalingRanges) {
    EXPECT_EQ(merged("  - { name: p, scale: { gain: 1000, offset: -5 }, "
                     "address: { table: input, register: 10, type: int32 } "
                     "}\n"),
              Lines{"ps1.m1.p: {scale: {gain: 1000, offset: -5}, address: "
                    "{table: input, register: 10, type: int32}}"});
    // 2^31 x 2^32 is 2^63: the gain 2^32 just fits; 2^32 + 1 does not.
    EXPECT_EQ(merged("  - { name: p, scale: { gain: 4294967297 }, address: { "
                     "table: input, register: 10, type: int32 } }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-516: "
                     "\"ps1.m1.p\": raw x gain + offset can exceed int64\n"
                     "  from template templates/meter_a.yaml:4:23 (point p)"}));
    EXPECT_EQ(merged("  - { name: p, scale: { offset: 1 }, address: { table: "
                     "input, register: 10, type: uint64 } }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-516: "
                     "\"ps1.m1.p\": raw x gain + offset can exceed uint64\n"
                     "  from template templates/meter_a.yaml:4:23 (point p)"}));
    EXPECT_EQ(merged("  - { name: p, value_type: int64, address: { table: "
                     "input, register: 10, type: uint64 } }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-504: "
                     "\"ps1.m1.p\": value_type int64 cannot hold uint64\n"
                     "  from template templates/meter_a.yaml:4:28 (point p)"}));
    EXPECT_EQ(merged("  - { name: p, value_type: uint64, address: { table: "
                     "input, register: 10, type: int16 } }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-504: "
                     "\"ps1.m1.p\": value_type uint64 cannot hold int16\n"
                     "  from template templates/meter_a.yaml:4:28 (point p)"}));
    // A bit field of a signed raw type is sign-extended: -8 to 7 here.
    EXPECT_EQ(merged("  - { name: p, value_type: uint64, address: { table: "
                     "input, register: 10, type: int16, field: [0, 3] } }\n",
                     "    params: {}\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-504: "
                     "\"ps1.m1.p\": value_type uint64 cannot hold int16\n"
                     "  from template templates/meter_a.yaml:4:28 (point p)"}));
}

// CF-202: a merged map that still lacks a key no level gave.
TEST(MergedPoints, IncompleteMergedMaps) {
    EXPECT_EQ(merged("  - { name: p, address: { register: 10 } }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-202: the "
                     "address of \"ps1.m1.p\" needs table\n"
                     "  from template templates/meter_a.yaml:4:25 (point p)"}));
    // `defaults` complete it.
    EXPECT_EQ(merged("  - { name: p, address: { register: 10 } }\n",
                     "    defaults: { address: { table: holding } }\n"),
              Lines{"ps1.m1.p: {address: {table: holding, register: 10}}"});
    EXPECT_EQ(merged("  - { name: p, address: { table: holding, register: "
                     "10, type: string } }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-202: the "
                     "address of \"ps1.m1.p\" needs length\n"
                     "  from template templates/meter_a.yaml:4:25 (point p)"}));
    EXPECT_EQ(merged("  - { name: p, exponent_from: { point: e }, address: { "
                     "table: holding, register: 10 } }\n"
                     "  - { name: e, address: { table: holding, register: "
                     "11 } }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-202: "
                     "exponent_from of \"ps1.m1.p\" needs direction\n"
                     "  from template templates/meter_a.yaml:4:31 (point p)"}));
    EXPECT_EQ(merged("  - { name: p, address: { table: holding, register: "
                     "10, device_time: { zone: local } } }\n"),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-202: "
                     "device_time of \"ps1.m1.p\" needs tz\n"
                     "  from template templates/meter_a.yaml:4:70 (point p)"}));
}

// A parameter that no level of the chain gives a type is CF-202.
TEST(MergedPoints, AParameterNeedsAType) {
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml",
                 "template: meter_a\nprotocol: modbus\n"
                 "params: { inputs: { default: 4 } }\n"},
                {"drivers/modbus/line1.yaml", one_device()}}),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-202: parameter "
               "\"inputs\" needs type\n"
               "  from template templates/meter_a.yaml:3:11 (parameter "
               "inputs)"}));
}

// An `io` address needs the keys of its channel's backend (Annex B.2).
TEST(MergedPoints, AnIoAddressNeedsTheKeysOfItsBackend) {
    EXPECT_EQ(
        expand({{"drivers/io/local.yaml",
                 "channel: { name: local, backend: gpio, chip: main-gpio }\n"
                 "devices:\n"
                 "  - name: ps1.io\n"
                 "    points:\n"
                 "      - { name: run, value_type: bool, access: rw, storage: "
                 "volatile, address: { line: 3, direction: output } }\n"}}),
        (Lines{"drivers/io/local.yaml:5:80: error CF-202: the address of "
               "\"ps1.io.run\" needs at_start"}));
}

}  // namespace
}  // namespace scada::conf
