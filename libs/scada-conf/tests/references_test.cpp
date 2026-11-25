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
        // An enumeration that the chain upward and enums.yaml lack may be a
        // child's (4.5, step 3.1): it waits for the instance (#103; B5 of
        // the review of #144).
        Lines{"templates/meter_a.yaml:10:42 CF-310: logging policy "
              "\"counters\" does not exist"});
}

// An instance's own points, defaults and overrides name enumerations of the
// template's chain or of enums.yaml, and logging policies.
TEST(References, NamesOfAnInstance) {
    EXPECT_EQ(
        validate({{"templates/meter_a.yaml",
                   "template: meter_a\nprotocol: modbus\n"
                   "enums:\n  local: { 0: a, 1: b }\n"
                   "points:\n  - { name: flow, address: { table: holding, "
                   "register: 1, type: uint16 } }\n"},
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

// A chain that names a template the tree does not have gives that one
// error: the enumerations and parameters the missing template would give
// are not refused as well.
TEST(References, AMissingTemplateIsOneError) {
    EXPECT_EQ(
        validate({{"templates/meter_a.yaml",
                   "template: meter_a\nextends: meter_base\nprotocol: "
                   "modbus\n"
                   "points:\n"
                   "  - { name: s, enum: state, when: \"has_s\", address: { "
                   "table: holding, register: 1, type: uint16 } }\n"},
                  {"drivers/modbus/line1.yaml",
                   channel("  - name: ps1.m1\n"
                           "    template: meter_x\n"
                           "    unit_id: 1\n"
                           "    overrides: { s: { enum: state } }\n")}}),
        (Lines{"drivers/modbus/line1.yaml:8:15 CF-310: template \"meter_x\" "
               "does not exist",
               "templates/meter_a.yaml:2:10 CF-310: template \"meter_base\" "
               "does not exist"}));
}

// A child that changes only a parameter's default keeps the parent's type.
TEST(References, AChildsDefaultKeepsTheParametersType) {
    EXPECT_EQ(
        validate({{"templates/base_a.yaml",
                   "template: base_a\n"
                   "params: { inputs: { type: int, default: 4 } }\n"},
                  {"templates/meter_a.yaml",
                   "template: meter_a\nextends: base_a\nprotocol: modbus\n"
                   "params: { inputs: { default: 6 } }\n"
                   "points:\n"
                   "  - { name: count, address: { table: holding, register: "
                   "\"inputs * 2\", type: uint16 } }\n"}}),
        Lines{});
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
// parameters and `repeat` variables of its chain: a failure is CF-410 in the
// form of 2.4 and 10.4. `cards` and the `n` of `k: "1..n"` resolve nowhere in
// the chain upward, so a child could give them: they are not reported here
// (B5, B6 of the review of #144).
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
        (Lines{"templates/meter_a.yaml:8:30 CF-410: expression \"inputs + "
               "1\": the value must be BOOL, not INT",
               "templates/meter_a.yaml:9:60 CF-410: expression \"model = "
               "'a'\": the value must be a number, not BOOL",
               "templates/meter_a.yaml:11:53 CF-410: expression "
               "\"sum(\\\"x.*\\\")\": sum reads tags, and a configuration "
               "expression has none at line 1, column 1 of the "
               "expression's text",
               "templates/meter_a.yaml:12:74 CF-410: expression "
               "\"round(inputs / 2) + max(inputs)\": max takes 2 or more "
               "arguments, not 1",
               "templates/meter_a.yaml:16:18 CF-410: expression \"n > 1 AND "
               "options\": AND needs BOOL, not SET"}));
}

// B1 of the review of #144: without control/tasks.yaml the tasks fast,
// normal and slow exist (9.1); a file replaces them all.
TEST(References, TheDefaultTasks) {
    const std::string tags =
        "tags:\n"
        "  - { name: a.f, type: virtual, expr: \"1\", task: fast }\n"
        "  - { name: a.n, type: virtual, expr: \"1\", task: normal }\n"
        "  - { name: a.s, type: virtual, expr: \"1\", task: slow }\n";
    EXPECT_EQ(validate({{"tags/site.yaml", tags}}), Lines{});
    EXPECT_EQ(validate({{"tags/site.yaml", tags},
                        {"control/tasks.yaml",
                         "tasks:\n  normal: { period: 1s, programs: [] }\n"}}),
              (Lines{"tags/site.yaml:2:50 CF-310: task \"fast\" does not exist",
                     "tags/site.yaml:4:50 CF-310: task \"slow\" does not "
                     "exist"}));
}

// B2: an alarm's expressions read tags wherever the alarm stands, a point,
// a bit of a point or a tag; they are not configuration expressions.
TEST(References, AlarmExpressionsAreRunTimeWhereverTheAlarmStands) {
    EXPECT_EQ(
        validate(
            {{"templates/meter_a.yaml",
              "template: meter_a\nprotocol: modbus\nbit_order: lsb0\n"
              "points:\n"
              "  - name: word\n"
              "    tag: false\n"
              "    address: { table: holding, register: 1, type: uint16 }\n"
              "    alarms: [ { type: expression, name: w.hot, expr: "
              "\"ps1.t1 > 80\", priority: high, suppress_when: "
              "ps1.station.maint } ]\n"
              "    bits:\n"
              "      0:\n"
              "        name: fault\n"
              "        alarms: [ { type: state, alarm_on: true, priority: "
              "high, suppress_when: ps1.station.maint } ]\n"},
             {"tags/site.yaml",
              "tags:\n"
              "  - { name: a.b, type: internal, value_type: bool, alarms: "
              "[ { type: state, alarm_on: true, priority: high, "
              "suppress_when: ps1.station.maint } ] }\n"}}),
        Lines{});
}

// B3: data_bits by its value, however it is written.
TEST(References, DataBitsByTheirValue) {
    for (const std::string_view written : {"0x7", "0x07"}) {
        EXPECT_EQ(validate({{"drivers/modbus/line1.yaml",
                             "channel:\n  name: line1\n  type: rtu\n"
                             "  device: /dev/serial/by-path/platform-serial1\n"
                             "  baud: 9600\n  data_bits: " +
                                 std::string(written) + "\n"}}),
                  Lines{"drivers/modbus/line1.yaml:6:14 CF-622: "
                        "channel.data_bits: 7 data bits are Modbus ASCII, "
                        "which is not available in this version"})
            << written;
    }
}

// B4: an IPv6 host by its 16 bytes, however it is written; an IPv4-mapped
// address by its IPv4 address.
// What CF-622 says of a Modbus TCP host, or "valid".
std::string host_reason(std::string_view address) {
    const Lines lines = tcp_channel_to(address);
    if (lines.empty()) {
        return "valid";
    }
    constexpr std::string_view SEPARATOR = " is ";
    return lines.front().substr(lines.front().rfind(SEPARATOR) +
                                SEPARATOR.size());
}

TEST(References, IpV6HostsByTheirBytes) {
    const auto reason = host_reason;

    for (const std::string_view valid :
         {"ff::1", "ff0::1", "fff:1::1", "::ffff:192.0.2.1", "2001:db8::1"}) {
        EXPECT_EQ(reason(valid), "valid") << valid;
    }
}

TEST(References, IpV6LoopbacksAndMulticastByTheirBytes) {
    const auto reason = host_reason;
    for (const std::string_view loopback :
         {"::1", "0::1", "0000:0000:0000:0000:0000:0000:0000:0001", "::0.0.0.1",
          "::ffff:127.0.0.1"}) {
        EXPECT_EQ(reason(loopback), "a loopback address") << loopback;
    }
    for (const std::string_view multicast :
         {"ff02::1", "FF05::2", "::ffff:224.0.0.1"}) {
        EXPECT_EQ(reason(multicast), "a multicast address") << multicast;
    }
    EXPECT_EQ(reason("::ffff:255.255.255.255"), "the broadcast address");
}

// B5: a parent template is not checked alone for names a child may give;
// a chain of three templates, the root using what the leaf declares.
TEST(References, AParentMayUseWhatAChildGives) {
    EXPECT_EQ(
        validate({{"templates/meter_base.yaml",
                   "template: meter_base\n"
                   "points: [ { name: v, enum: states, unit: V } ]\n"},
                  {"templates/meter_a.yaml",
                   "template: meter_a\nextends: meter_base\nprotocol: modbus\n"
                   "points:\n"
                   "  - { name: p, address: { table: holding, register: "
                   "\"base_r + 1\", type: uint16 } }\n"
                   "  - { name: v, address: { table: holding, register: 9, "
                   "type: uint16 } }\n"},
                  {"templates/meter_b.yaml",
                   "template: meter_b\nextends: meter_a\nprotocol: modbus\n"
                   "params: { base_r: { type: int, default: 100 } }\n"
                   "enums: { states: { 0: off, 1: on } }\n"},
                  {"drivers/modbus/line1.yaml",
                   channel("  - { name: ps1.m1, template: meter_b, unit_id: "
                           "1 }\n")}}),
        Lines{});
    // In the chain upward, the types are checked: meter_b's own `when`.
    EXPECT_EQ(
        validate({{"templates/meter_a.yaml",
                   "template: meter_a\nprotocol: modbus\n"
                   "params: { inputs: { type: int, default: 2 } }\n"},
                  {"templates/meter_b.yaml",
                   "template: meter_b\nextends: meter_a\nprotocol: modbus\n"
                   "points:\n"
                   "  - { name: p, when: inputs, address: { table: holding, "
                   "register: 1, type: uint16 } }\n"}}),
        Lines{"templates/meter_b.yaml:5:22 CF-410: expression \"inputs\": the "
              "value must be BOOL, not INT"});
}

// B5: without a template nothing can define a name, so each is CF-410; with
// a template, the instance's expressions are left to the expander, which
// binds its variables (#103).
TEST(References, NamesOfAnInstancesExpressions) {
    EXPECT_EQ(
        validate({{"drivers/modbus/line1.yaml",
                   channel("  - name: ps1.m1\n"
                           "    unit_id: 1\n"
                           "    points:\n"
                           "      - { name: p, address: { table: holding, "
                           "register: \"base + 1\", type: uint16 } }\n")}}),
        // The expander (stage 4) finds it first.
        Lines{"drivers/modbus/line1.yaml:10:58 CF-410: expression \"base + "
              "1\": \"base\" is not a parameter and no variable bound to "
              "this point"});
    EXPECT_EQ(
        validate({{"templates/meter_a.yaml",
                   "template: meter_a\nprotocol: modbus\n"
                   "points:\n"
                   "  - { name: p, address: { table: holding, register: 1, "
                   "type: uint16 } }\n"},
                  {"drivers/modbus/line1.yaml",
                   channel("  - name: ps1.m1\n"
                           "    template: meter_a\n"
                           "    unit_id: 1\n"
                           "    overrides: { p: { address: { register: "
                           "\"'a' + n\" } } }\n")}}),
        // Stage 5 leaves it to the expander, which binds the variables of
        // each point: `n` is bound to none here.
        Lines{"drivers/modbus/line1.yaml:10:51 CF-410: expression \"'a' + "
              "n\": \"n\" is not a parameter and no variable bound to this "
              "point"});
}

// S3: a chain cut by its length (or a cycle) is not known; its names and
// types are not checked here (CF-401 comes with #103).
TEST(References, ACutChainIsNotChecked) {
    Files files;
    constexpr int TEMPLATES = 7;
    for (int level = 1; level <= TEMPLATES; ++level) {
        const std::string name = "t" + std::to_string(level);
        std::string text = "template: " + name + "\nprotocol: modbus\n";
        if (level > 1) {
            text += "extends: t" + std::to_string(level - 1) + "\n";
        }
        if (level == 1) {
            text += "params: { inputs: { type: int, default: 2 } }\n";
        }
        if (level == TEMPLATES) {
            text +=
                "points: [ { name: p, when: inputs, address: { table: "
                "holding, register: 1, type: uint16 } } ]\n";
        }
        files.emplace("templates/" + name + ".yaml", text);
    }
    EXPECT_EQ(validate(files), Lines{});
}

// S2: a user tag must not start with the segment `sys` (2.1): `sys` itself
// too; `system.a` is another segment, and valid.
TEST(References, TheSysSegment) {
    EXPECT_EQ(
        validate({{"tags/a.yaml",
                   "tags:\n"
                   "  - { name: sys, type: internal, value_type: int64 }\n"
                   "  - { name: system.a, type: internal, value_type: "
                   "int64 }\n"}}),
        Lines{"tags/a.yaml:2:13 CF-301: tag names starting with sys. "
              "are reserved"});
}

// Stage 5 runs only when the stages before it found no error: a tree with
// a YAML error gives that error alone, not its wrong references too.
TEST(References, StageFiveWaitsForTheStagesBefore) {
    EXPECT_EQ(validate({{"tags/a.yaml",
                         "tags:\n"
                         "  - { name: a.b, type: manual, value_type: int64, "
                         "area: nowhere }\n"},
                        {"tags/b.yaml", "tags: [\n"}}),
              Lines{"tags/b.yaml:1:7 CF-110: an unclosed flow sequence is not "
                    "allowed in configuration files"});
}

// B2: where names are checked, a device without template, an alarm's
// expressions are not: each would otherwise be CF-410 for a tag name. One
// alarm on a point, one on a bit: the two places that skip `alarms`.
TEST(References, AlarmsOfADeviceWithoutTemplateAreRunTime) {
    EXPECT_EQ(
        validate({{"drivers/modbus/line1.yaml",
                   channel("  - name: ps1.m1\n"
                           "    unit_id: 1\n"
                           "    points:\n"
                           "      - name: p\n"
                           "        address: { table: holding, register: 1, "
                           "type: uint16 }\n"
                           "        alarms: [ { type: expression, name: p.hot, "
                           "expr: \"ps1.t1 > 80\", priority: high } ]\n")}}),
        Lines{});
    EXPECT_EQ(
        validate({{"drivers/modbus/line1.yaml",
                   channel("  - name: ps1.m1\n"
                           "    unit_id: 1\n"
                           "    points:\n"
                           "      - name: w\n"
                           "        tag: false\n"
                           "        address: { table: holding, register: 1, "
                           "type: uint16 }\n"
                           "        bits:\n"
                           "          0:\n"
                           "            name: fault\n"
                           "            alarms: [ { type: state, alarm_on: "
                           "true, priority: high, suppress_when: "
                           "ps1.station.maint } ]\n")}}),
        Lines{});
}

// The one CF-523 of a configuration expression: the type of a `for` value
// (5.6).
TEST(References, AForValueOfTheWrongTypeIsCf523) {
    EXPECT_EQ(
        validate({{"templates/meter_a.yaml",
                   "template: meter_a\nprotocol: modbus\n"
                   "params: { flag: { type: bool, default: true } }\n"
                   "repeat:\n"
                   "  - for: { n: \"flag\" }\n"
                   "    points: [ { name: \"p{n}\", address: { table: holding, "
                   "register: n, type: uint16 } } ]\n"}}),
        Lines{"templates/meter_a.yaml:5:16 CF-523: expression: the value must "
              "be a range or an INT, not BOOL at column 1"});
}

// A program's task must exist; a tasks.yaml replaces the default tasks.
TEST(References, AProgramsTask) {
    EXPECT_EQ(
        validate({{"control/tasks.yaml",
                   "tasks:\n  normal: { period: 1s, programs: [ps1_a] }\n"},
                  {"control/ps1_a.yaml",
                   "program: ps1_a\ntask: fast\nblocks: []\n"}}),
        Lines{"control/ps1_a.yaml:2:7 CF-310: task \"fast\" does not "
              "exist"});
}

// Types over a chain of three: the leaf uses the root's parameter.
TEST(References, TypesOverAChainOfThree) {
    EXPECT_EQ(validate({{"templates/t1.yaml",
                         "template: t1\nprotocol: modbus\n"
                         "params: { inputs: { type: int, default: 2 } }\n"},
                        {"templates/t2.yaml",
                         "template: t2\nprotocol: modbus\nextends: t1\n"},
                        {"templates/t3.yaml",
                         "template: t3\nprotocol: modbus\nextends: t2\n"
                         "points: [ { name: p, when: inputs, address: { table: "
                         "holding, register: 1, type: uint16 } } ]\n"}}),
              Lines{"templates/t3.yaml:4:28 CF-410: expression \"inputs\": the "
                    "value must be BOOL, not INT"});
}

// A chain cut by its length is not typed, even with a type error in it.
TEST(References, ACutChainWithATypeError) {
    Files files;
    constexpr int TEMPLATES = 7;
    for (int level = 1; level <= TEMPLATES; ++level) {
        const std::string name = "t" + std::to_string(level);
        std::string text = "template: " + name + "\nprotocol: modbus\n";
        if (level > 1) {
            text += "extends: t" + std::to_string(level - 1) + "\n";
        }
        text += "params: { p" + std::to_string(level) +
                ": { type: int, default: 2 } }\n";
        if (level == TEMPLATES) {
            text +=
                "points: [ { name: p, when: \"p7 + 1\", address: { table: "
                "holding, register: 1, type: uint16 } } ]\n";
        }
        files.emplace("templates/" + name + ".yaml", text);
    }
    EXPECT_EQ(validate(files), Lines{});
}

// 3 of the review of #144: the expressions of `identify` (2.4, 4.8).
TEST(References, IdentityChecksAreTyped) {
    EXPECT_EQ(
        validate({{"templates/meter_a.yaml",
                   "template: meter_a\nprotocol: modbus\n"
                   "params: { inputs: { type: int, default: 2 } }\n"
                   "identify:\n"
                   "  - { address: { table: holding, register: \"1 > 2\", "
                   "type: uint16 }, equals: 7 }\n"
                   "  - { address: { table: holding, register: 2, type: "
                   "uint16 }, range: [\"'a'\", \"inputs * 2\"] }\n"}}),
        (Lines{"templates/meter_a.yaml:5:47 CF-410: expression \"1 > 2\": the "
               "value must be a number, not BOOL",
               "templates/meter_a.yaml:6:72 CF-410: expression \"'a'\": the "
               "value must be a number, not STRING"}));
}

// --- The remaining mutant gaps of the review of #144 ----------------------

// 1: stage 5 does not type an instance's expressions; the expander does,
// where they are used. A default that reaches no point is used nowhere.
TEST(References, AnInstanceIsNotTypedAsATemplate) {
    EXPECT_EQ(
        validate({{"templates/meter_a.yaml",
                   "template: meter_a\nprotocol: modbus\n"
                   "params: { model: { type: string, default: a } }\n"
                   "points:\n"
                   "  - { name: p, address: { table: holding, register: 1, "
                   "type: uint16 } }\n"},
                  {"drivers/modbus/line1.yaml",
                   channel("  - name: ps1.m1\n"
                           "    template: meter_a\n"
                           "    unit_id: 1\n"
                           "    disable: [p]\n"
                           "    defaults: { scale: { gain: \"model * 2\" } "
                           "}\n")}}),
        Lines{});
}

// 2: a block's `when` and a template's `defaults` are typed with the
// variables: `n` is an INT, not a BOOL or an unknown name. A variable hides
// a parameter of its name, as in the expander (D-114).
TEST(References, VariablesOfABlockAndOfDefaults) {
    EXPECT_EQ(
        validate({{"templates/meter_a.yaml",
                   "template: meter_a\nprotocol: modbus\n"
                   "defaults: { when: \"n\" }\n"
                   "repeat:\n"
                   "  - for: { n: \"1..2\" }\n"
                   "    when: \"n\"\n"
                   "    points:\n"
                   "      - { name: \"p{n}\", address: { table: holding, "
                   "register: \"n\", type: uint16 } }\n"}}),
        (Lines{"templates/meter_a.yaml:3:20 CF-410: expression \"n\": the "
               "value must be BOOL, not INT",
               "templates/meter_a.yaml:6:12 CF-410: expression \"n\": the "
               "value must be BOOL, not INT"}));
    EXPECT_EQ(validate({{"templates/meter_a.yaml",
                         "template: meter_a\nprotocol: modbus\n"
                         "params: { n: { type: string, default: a } }\n"
                         "defaults: { when: \"n > 1\" }\n"
                         "repeat:\n"
                         "  - for: { n: \"1..2\" }\n"
                         "    when: \"n > 1\"\n"
                         "    points:\n"
                         "      - { name: \"p{n}\", address: { table: holding, "
                         "register: \"n\", type: uint16 } }\n"}}),
              Lines{});
}

// 3: an address is IPv4-mapped only with both bytes 0xFF before the IPv4
// address; `::ff00:7f00:1` is not, so it is not the loopback 127.0.0.1.
TEST(References, AnAddressWithOneFfByteIsNotIpv4Mapped) {
    EXPECT_EQ(host_reason("::ff00:7f00:1"), "valid");
    EXPECT_EQ(host_reason("::00ff:7f00:1"), "valid");
}

// 4 and 5: `replace` in the tiers of logging settings is no tier name, and
// a bit's logging settings name their policy.
TEST(References, TiersAndTheLoggingOfABit) {
    EXPECT_EQ(
        validate({{"templates/meter_a.yaml",
                   "template: meter_a\nprotocol: modbus\nbit_order: lsb0\n"
                   "points:\n"
                   "  - name: w\n"
                   "    tag: false\n"
                   "    address: { table: holding, register: 1, type: uint16 "
                   "}\n"
                   "    logging: { tiers: { replace: true, 1m: 1y } }\n"
                   "    bits:\n"
                   "      0: { name: fault, logging: { policy: events } }\n"}}),
        Lines{"templates/meter_a.yaml:10:44 CF-310: logging policy \"events\" "
              "does not exist"});
}

// 6 and 7: a file that gives `classes` replaces the default classes, and
// the first file that gives `priorities` defines them (a second is CF-811,
// with slice 7 of #100).
TEST(References, AlarmListsOfAFile) {
    EXPECT_EQ(
        validate({{"areas.yaml", std::string(AREAS)},
                  {"alarms/a.yaml",
                   "priorities: [urgent]\n"
                   "classes: [safety]\n"
                   "alarms:\n"
                   "  - { tag: ps1.fm1.flow, type: high, limit: 4, priority: "
                   "urgent, class: process, area: ps1 }\n"},
                  {"alarms/b.yaml",
                   "priorities: [minor]\n"
                   "alarms:\n"
                   "  - { tag: ps1.fm1.flow, type: low, limit: 1, priority: "
                   "minor, class: safety, area: ps1 }\n"}}),
        (Lines{"alarms/a.yaml:4:73 CF-310: alarm class \"process\" does not "
               "exist",
               "alarms/b.yaml:3:57 CF-310: alarm priority \"minor\" does not "
               "exist"}));
}

// A name the chain upward does not have is not typed in the template, not
// even as a variable: `flag` may be a BOOL parameter of a child (B5).
TEST(References, AnUnknownNameIsNoVariable) {
    EXPECT_EQ(
        validate({{"templates/meter_a.yaml",
                   "template: meter_a\nprotocol: modbus\n"
                   "repeat:\n"
                   "  - for: { n: \"1..2\" }\n"
                   "    points:\n"
                   "      - { name: \"p{n}\", when: flag, address: { table: "
                   "holding, register: \"n\", type: uint16 } }\n"}}),
        Lines{});
}

// 8: one INT is a `for` value (5.6), as a range is.
TEST(References, OneIntIsAForValue) {
    EXPECT_EQ(
        validate({{"templates/meter_a.yaml",
                   "template: meter_a\nprotocol: modbus\n"
                   "params: { inputs: { type: int, default: 2 } }\n"
                   "repeat:\n"
                   "  - for: { n: \"inputs\", k: [1, \"inputs * 2\"] }\n"
                   "    points:\n"
                   "      - { name: \"p{n}_{k}\", address: { table: holding, "
                   "register: \"n + k\", type: uint16 } }\n"}}),
        Lines{});
}

}  // namespace
}  // namespace scada::conf
