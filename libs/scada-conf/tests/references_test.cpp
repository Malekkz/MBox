// Stage 5 without expansion (decision D-113): every name that must exist,
// every name given twice, the protocol rules of a Modbus channel, and the
// types of a template's configuration expressions. Each test is a whole
// tree, validated as `scada-conf` validates it.

#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/validate.h"
#include "scada/test/temp_directory.h"

#include <gtest/gtest.h>

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

// Validates the tree of `files` (path to content) with a system.yaml, and
// gives its diagnostics as `path:line:column CF-n: message`.
Lines validate(const Files& files) {
    std::optional<test::TempDirectory> temp =
        test::TempDirectory::create("scada-conf-references-");
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
    const Result<Report> report = validate_tree(temp->path());
    EXPECT_TRUE(report.ok());
    if (!report.ok()) {
        return {"not validated"};
    }
    Lines lines;
    for (const Diagnostic& error : report.value().errors) {
        lines.push_back(error.location.file + ":" +
                        std::to_string(error.location.line) + ":" +
                        std::to_string(error.location.column) + " " +
                        code_text(error) + ": " + error.message);
    }
    return lines;
}

constexpr std::string_view AREAS = "areas:\n  ps1: { description: P1 }\n";
constexpr std::string_view TASKS =
    "tasks:\n  normal: { period: 1s, programs: [] }\n";

// One Modbus RTU channel with the devices given, one per line.
std::string channel(std::string_view devices) {
    return "channel:\n"
           "  name: line1\n"
           "  type: rtu\n"
           "  device: /dev/serial/by-path/platform-serial1\n"
           "  baud: 9600\n"
           "devices:\n" +
           std::string(devices);
}

TEST(References, TemplatesAndProtocolsMustExist) {
    EXPECT_EQ(
        validate({{"templates/meter_b.yaml",
                   "template: meter_b\nextends: meter_base\n"
                   "protocol: modbus\n"},
                  {"templates/meter_x.yaml",
                   "template: meter_x\nprotocol: newproto\n"},
                  {"drivers/modbus/line1.yaml",
                   channel("  - { name: ps1.m1, template: meter_b, unit_id: "
                           "1 }\n"
                           "  - { name: ps1.m2, template: meter_c, unit_id: "
                           "2 }\n")}}),
        (Lines{"drivers/modbus/line1.yaml:8:31 CF-310: template \"meter_c\" "
               "does not exist",
               "templates/meter_b.yaml:2:10 CF-310: template \"meter_base\" "
               "does not exist",
               "templates/meter_x.yaml:2:11 CF-310: protocol \"newproto\" "
               "does not exist"}));
}

TEST(References, AreasTasksAndEnumerationsOfTags) {
    EXPECT_EQ(
        validate({{"areas.yaml", std::string(AREAS)},
                  {"control/tasks.yaml", std::string(TASKS)},
                  {"enums.yaml", "enums:\n  mode: { 0: off, 1: on }\n"},
                  {"tags/site.yaml",
                   "tags:\n"
                   "  - { name: a.m, type: manual, value_type: int64, area: "
                   "ps2, enum: mode }\n"
                   "  - { name: a.v, type: virtual, expr: \"1\", task: fast "
                   "}\n"
                   "  - { name: a.i, type: internal, value_type: int64, "
                   "enum: modes }\n"}}),
        (Lines{"tags/site.yaml:2:57 CF-310: area \"ps2\" does not exist",
               "tags/site.yaml:3:50 CF-310: task \"fast\" does not exist",
               "tags/site.yaml:4:59 CF-310: enumeration \"modes\" does not "
               "exist"}));
}

TEST(References, LoggingPoliciesAndTiers) {
    EXPECT_EQ(
        validate({{"logging.yaml",
                   "aggregate_tiers:\n  - { name: 1m, interval: 1m }\n"
                   "default_policy: process\n"
                   "policies:\n"
                   "  counters: { mode: periodic, interval: 1m, "
                   "raw_retention: 1y, tiers: { 1m: 1y, 1h: 10y } }\n"},
                  {"tags/site.yaml",
                   "tags:\n"
                   "  - { name: a.b, type: internal, value_type: int64, "
                   "logging: counter }\n"
                   "  - { name: a.c, type: internal, value_type: int64, "
                   "logging: { policy: counters, tiers: { 15m: 1y } } }\n"
                   "  - { name: a.d, type: internal, value_type: int64, "
                   "logging: none }\n"}}),
        (Lines{"logging.yaml:3:17 CF-310: logging policy \"process\" does "
               "not exist",
               "logging.yaml:5:81 CF-310: aggregate tier \"1h\" does not exist",
               "tags/site.yaml:2:62 CF-310: logging policy \"counter\" does "
               "not exist",
               "tags/site.yaml:3:91 CF-310: aggregate tier \"15m\" does not "
               "exist"}));
    // Without logging.yaml the default tiers 1m and 1h exist, and no policy.
    EXPECT_EQ(
        validate({{"tags/site.yaml",
                   "tags:\n"
                   "  - { name: a.b, type: internal, value_type: int64, "
                   "logging: { mode: on_change, raw_retention: 1y, tiers: { "
                   "1h: 1y } } }\n"
                   "  - { name: a.c, type: internal, value_type: int64, "
                   "logging: counters }\n"}}),
        (Lines{"tags/site.yaml:3:62 CF-310: logging policy \"counters\" does "
               "not exist"}));
}

TEST(References, AlarmPrioritiesClassesAndAreas) {
    EXPECT_EQ(
        validate({{"areas.yaml", std::string(AREAS)},
                  {"alarms/site.yaml",
                   "priorities: [urgent, normal]\n"
                   "max_shelve: { urgent: 1h, high: 2h }\n"
                   "alarms:\n"
                   "  - { tag: ps1.fm1.flow, type: high, limit: 4, priority: "
                   "high, area: ps1 }\n"
                   "  - { tag: ps1.fm1.flow, type: low, limit: 1, priority: "
                   "normal, class: safety, area: ps9 }\n"}}),
        (Lines{"alarms/site.yaml:2:27 CF-310: alarm priority \"high\" does "
               "not exist",
               "alarms/site.yaml:4:58 CF-310: alarm priority \"high\" does "
               "not exist",
               "alarms/site.yaml:5:72 CF-310: alarm class \"safety\" does not "
               "exist",
               "alarms/site.yaml:5:86 CF-310: area \"ps9\" does not exist"}));
}

TEST(References, OutputsInterlocksAndPrograms) {
    EXPECT_EQ(
        validate(
            {{"areas.yaml", std::string(AREAS)},
             {"control/tasks.yaml",
              "tasks:\n  normal: { period: 1s, programs: [ps1_pump, "
              "ps1_fan] }\n"},
             {"control/ps1_pump.yaml",
              "program: ps1_pump\ntask: normal\nblocks:\n"
              "  - { id: run, type: digital_output, output: ps1.pump1.cmd "
              "}\n"
              "  - { id: fan, type: digital_output, output: ps1.fan.cmd }\n"},
             {"outputs/ps1.yaml",
              "interlocks:\n"
              "  - { name: ps1.ok, condition: \"NOT ps1.tank.low\", message: "
              "low, task: slow }\n"
              "outputs:\n"
              "  - name: ps1.pump1.cmd\n"
              "    area: ps1\n"
              "    kind: single\n"
              "    target: ps1.pump1.run_cmd\n"
              "    no_feedback: true\n"
              "    interlocks: [ps1.ok, ps1.dry]\n"
              "    safe_state: off\n"
              "  - name: ps1.valve.cmd\n"
              "    area: ps1\n"
              "    kind: double\n"
              "    target: ps1.valve.run_cmd\n"
              "    no_feedback: true\n"
              "    interlocks: { on: [ps1.ok], off: [ps1.wet] }\n"
              "    safe_state: off\n"}}),
        // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): long messages
        (Lines{"control/ps1_pump.yaml:5:46 CF-310: output \"ps1.fan.cmd\" "
               "does not exist",
               "control/tasks.yaml:2:46 CF-310: program \"ps1_fan\" does not "
               "exist",
               "outputs/ps1.yaml:2:72 CF-310: task \"slow\" does not exist",
               "outputs/ps1.yaml:9:26 CF-310: interlock \"ps1.dry\" does not "
               "exist",
               "outputs/ps1.yaml:16:39 CF-310: interlock \"ps1.wet\" does not "
               "exist"}));
}

TEST(References, NamesOfATemplatesPoints) {
    EXPECT_EQ(
        validate({{"enums.yaml", "enums:\n  mode: { 0: off, 1: on }\n"},
                  {"templates/base_a.yaml",
                   "template: base_a\nenums:\n  state: { 0: idle, 1: run }\n"},
                  {"templates/meter_a.yaml",
                   "template: meter_a\nextends: base_a\nprotocol: modbus\n"
                   "enums:\n  local: { 0: a, 1: b }\n"
                   "points:\n"
                   "  - { name: s, enum: state, address: { table: holding, "
                   "register: 1, type: uint16 } }\n"
                   "  - { name: l, enum: local, address: { table: holding, "
                   "register: 2, type: uint16 } }\n"
                   "  - { name: g, enum: mode, address: { table: holding, "
                   "register: 3, type: uint16 } }\n"
                   "  - { name: x, enum: none_such, logging: counters, "
                   "address: { table: holding, register: 4, type: uint16 } "
                   "}\n"}}),
        (Lines{"templates/meter_a.yaml:10:22 CF-310: enumeration \"none_such\" "
               "does not exist",
               "templates/meter_a.yaml:10:42 CF-310: logging policy "
               "\"counters\" does not exist"}));
}

// An instance's own points, defaults and overrides name enumerations of the
// template's chain or of enums.yaml, and logging policies.
TEST(References, NamesOfAnInstance) {
    EXPECT_EQ(
        validate({{"templates/meter_a.yaml",
                   "template: meter_a\nprotocol: modbus\n"
                   "enums:\n  local: { 0: a, 1: b }\n"},
                  {"drivers/modbus/line1.yaml",
                   channel("  - name: ps1.m1\n"
                           "    template: meter_a\n"
                           "    unit_id: 1\n"
                           "    defaults: { logging: hourly }\n"
                           "    overrides: { flow: { enum: local, logging: "
                           "daily } }\n"
                           "    points:\n"
                           "      - { name: extra, enum: missing, address: "
                           "{ table: holding, register: 9, type: uint16 } "
                           "}\n")}}),
        (Lines{"drivers/modbus/line1.yaml:10:26 CF-310: logging policy "
               "\"hourly\" does not exist",
               "drivers/modbus/line1.yaml:11:48 CF-310: logging policy "
               "\"daily\" does not exist",
               "drivers/modbus/line1.yaml:13:30 CF-310: enumeration "
               "\"missing\" does not exist"}));
}

TEST(References, NamesGivenTwice) {
    EXPECT_EQ(
        validate({{"templates/meter_a.yaml",
                   "template: meter_a\nprotocol: modbus\n"},
                  {"drivers/modbus/line1.yaml",
                   channel("  - { name: ps1.m1, template: meter_a, unit_id: "
                           "1 }\n")},
                  {"drivers/modbus/line2.yaml",
                   "channel:\n  name: line2\n  type: rtu\n"
                   "  device: /dev/serial/by-path/platform-serial2\n"
                   "  baud: 9600\n"
                   "devices:\n"
                   "  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n"},
                  {"drivers/io/line1.yaml",
                   "channel: { name: line1, backend: gpio, chip: c1 }\n"},
                  {"outputs/a.yaml",
                   "interlocks:\n  - { name: ps1.ok, condition: \"TRUE\", "
                   "message: m }\n"},
                  {"outputs/b.yaml",
                   "interlocks:\n  - { name: ps1.ok, condition: \"TRUE\", "
                   "message: m }\n"}}),
        (Lines{"drivers/modbus/line1.yaml:2:9 CF-303: channel \"line1\" is "
               "defined twice; first at drivers/io/line1.yaml:1",
               "drivers/modbus/line2.yaml:7:13 CF-303: device \"ps1.m1\" is "
               "defined twice; first at drivers/modbus/line1.yaml:7",
               "outputs/b.yaml:2:13 CF-302: tag \"ps1.ok\" is defined twice; "
               "first at outputs/a.yaml:2",
               "outputs/b.yaml:2:13 CF-303: interlock \"ps1.ok\" is defined "
               "twice; first at outputs/a.yaml:2"}));
}

TEST(References, TagsInSysAndTagsGivenTwice) {
    EXPECT_EQ(
        validate({{"tags/a.yaml",
                   "tags:\n"
                   "  - { name: sys.mine, type: internal, value_type: int64 "
                   "}\n"
                   "  - { name: a.b, type: internal, value_type: int64 }\n"},
                  {"tags/b.yaml",
                   "tags:\n"
                   "  - { name: a.b, type: internal, value_type: int64 }\n"}}),
        (Lines{"tags/a.yaml:2:13 CF-301: tag names starting with sys. are "
               "reserved",
               "tags/b.yaml:2:13 CF-302: tag \"a.b\" is defined twice; first "
               "at tags/a.yaml:3"}));
}

// A reference to a tag waits for the expander (#103), which makes the point
// tags: no error yet, however wrong it is.
TEST(References, AReferenceToATagGivesNoErrorYet) {
    EXPECT_EQ(
        validate(
            {{"areas.yaml",
              "areas:\n  ps1:\n    authority:\n      local_panel: { input: "
              "ps1.panel.local, active: true }\n"},
             {"tags/site.yaml",
              "tags:\n  - { name: a.v, type: virtual, expr: \"ps1.fm1.flow * "
              "2 + sum(\\\"ps9.*.x\\\")\" }\n"},
             {"alarms/site.yaml",
              "alarms:\n  - { tag: ps1.fm9.flow, type: deviation, limit: 4, "
              "setpoint: ps1.sp9, priority: high, area: ps1 }\n"},
             {"outputs/ps1.yaml",
              "outputs:\n"
              "  - name: ps1.pump1.cmd\n"
              "    area: ps1\n"
              "    kind: single\n"
              "    target: ps1.nothing.here\n"
              "    feedback: ps1.nothing.either\n"
              "    interlocks: []\n"
              "    safe_state: off\n"}}),
        Lines{});
}

TEST(References, ProtocolRulesOfAModbusChannel) {
    EXPECT_EQ(validate({{"drivers/modbus/line1.yaml",
                         "channel:\n  name: line1\n  type: rtu\n"
                         "  device: /dev/serial/by-path/platform-serial1\n"
                         "  baud: 9600\n  data_bits: 7\n"}}),
              Lines{"drivers/modbus/line1.yaml:6:14 CF-622: channel.data_bits: "
                    "7 data bits are Modbus ASCII, which is not available in "
                    "this version"});
}

// The diagnostics of a Modbus TCP channel to `address`.
Lines tcp_channel_to(std::string_view address) {
    return validate({{"drivers/modbus/net1.yaml",
                      "channel:\n  name: net1\n  type: tcp\n  host: \"" +
                          std::string(address) + "\"\n"}});
}

TEST(References, AModbusDeviceIsNeverAtALoopbackMulticastOrBroadcastHost) {
    const auto host = tcp_channel_to;
    EXPECT_EQ(host("127.0.0.1"),
              Lines{"drivers/modbus/net1.yaml:4:9 CF-622: channel.host: "
                    "\"127.0.0.1\" is a loopback address"});
    EXPECT_EQ(host("239.1.2.3"),
              Lines{"drivers/modbus/net1.yaml:4:9 CF-622: channel.host: "
                    "\"239.1.2.3\" is a multicast address"});
    EXPECT_EQ(host("255.255.255.255"),
              Lines{"drivers/modbus/net1.yaml:4:9 CF-622: channel.host: "
                    "\"255.255.255.255\" is the broadcast address"});
    EXPECT_EQ(host("::1"),
              Lines{"drivers/modbus/net1.yaml:4:9 CF-622: channel.host: "
                    "\"::1\" is a loopback address"});
    EXPECT_EQ(host("FF02::1"),
              Lines{"drivers/modbus/net1.yaml:4:9 CF-622: channel.host: "
                    "\"FF02::1\" is a multicast address"});
    EXPECT_EQ(host("192.0.2.10"), Lines{});
    EXPECT_EQ(host("2001:db8::10"), Lines{});
}

// The types of a template's configuration expressions (5.6), over the
// parameters and `repeat` variables of its chain.
TEST(References, TypesOfConfigurationExpressions) {
    EXPECT_EQ(
        validate(
            {{"templates/base_a.yaml",
              "template: base_a\n"
              "params:\n"
              "  inputs: { type: int, default: 4 }\n"
              "  model: { type: string, default: a }\n"
              "  options: { type: set, values: [meter, battery] }\n"},
             {"templates/meter_a.yaml",
              "template: meter_a\nextends: base_a\nprotocol: modbus\n"
              "params:\n  scale: { type: number, default: 0.1 }\n"
              "points:\n"
              "  - { name: a, when: \"'meter' IN options\", address: { table: "
              "holding, register: 1, type: uint16 } }\n"
              "  - { name: b, when: \"inputs + 1\", address: { table: "
              "holding, register: 2, type: uint16 } }\n"
              "  - { name: c, address: { table: holding, register: \"model "
              "= 'a'\", type: uint16 } }\n"
              "  - { name: d, address: { table: holding, register: "
              "\"cards * 2\", type: uint16 } }\n"
              "  - { name: e, address: { table: holding, register: "
              "\"sum(\\\"x.*\\\")\", type: uint16 } }\n"
              "  - { name: f, address: { table: holding, register: "
              "\"round(inputs / 2) + max(inputs)\", type: uint16 } }\n"
              "  - { name: g, scale: { gain: \"scale * 2\" }, address: { "
              "table: holding, register: \"if(model = 'b', 10, 20)\", type: "
              "int16 } }\n"
              "repeat:\n"
              "  - for: { n: \"1..inputs\", k: \"1..n\" }\n"
              "    when: \"n > 1 AND options\"\n"
              "    points:\n"
              "      - { name: \"p{n}\", address: { table: holding, "
              "register: \"100 + n\", type: uint16 } }\n"}}),
        // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): long messages
        (Lines{"templates/meter_a.yaml:8:30 CF-523: expression: the value "
               "must be BOOL, not INT at column 8",
               "templates/meter_a.yaml:9:60 CF-523: expression: the value "
               "must be a number, not BOOL at column 7",
               "templates/meter_a.yaml:10:54 CF-310: parameter or variable "
               "\"cards\" does not exist",
               "templates/meter_a.yaml:11:53 CF-523: expression: sum reads "
               "tags, and a configuration expression has none at line 1, "
               "column 1 of the expression's text",
               "templates/meter_a.yaml:12:74 CF-523: expression: max takes 2 "
               "or more arguments, not 1 at column 21",
               "templates/meter_a.yaml:15:35 CF-310: parameter or variable "
               "\"n\" does not exist",
               "templates/meter_a.yaml:16:18 CF-523: expression: AND needs "
               "BOOL, not SET at column 7"}));
}

}  // namespace
}  // namespace scada::conf
