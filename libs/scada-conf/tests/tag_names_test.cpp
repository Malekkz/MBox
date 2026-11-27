// Stage 5: the tag names of the expanded tree and the references to them
// outside expressions (configuration schemas contract 2.6, 12.4, 12.5;
// decision D-116; slice 7a of #100).

#include "expand_cases.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace scada::conf {
namespace {

constexpr std::string_view AREAS = "areas:\n  ps1: { description: Station }\n";

// meter_a with a value point, a status word with two bits and no tag of its
// own, and a BOOL point.
constexpr std::string_view METER =
    "template: meter_a\nprotocol: modbus\nbit_order: lsb0\npoints:\n"
    "  - { name: flow, address: { table: holding, register: 1, type: uint16 "
    "} }\n"
    "  - name: status\n"
    "    address: { table: holding, register: 2, type: uint16 }\n"
    "    tag: false\n"
    "    bits: { 0: run, 1: { name: trip } }\n"
    "  - { name: alarm_on, address: { table: coil, register: 3 } }\n";

// An alarm file of one alarm on `tag` with `more` keys.
std::string alarm_on(std::string_view tag, std::string_view more = {}) {
    return "alarms:\n  - { tag: " + std::string(tag) +
           ", type: high, limit: 4, priority: high, area: ps1" +
           std::string(more) + " }\n";
}

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

Lines with_alarms(const std::string& alarms, std::string_view meter = METER,
                  std::string_view device = {}) {
    return errors(expand({{"areas.yaml", std::string(AREAS)},
                          {"templates/meter_a.yaml", std::string(meter)},
                          {"drivers/modbus/line1.yaml", one_device(device)},
                          {"alarms/site.yaml", alarms}}));
}

// A point's tag is `<device>.<point>` unless `tag: false`; a bit's is
// `<device>.<bit>` (5.4, 12.4).
TEST(TagNames, PointAndBitTags) {
    EXPECT_EQ(with_alarms(alarm_on("ps1.m1.flow")), Lines{});
    EXPECT_EQ(
        with_alarms(alarm_on("ps1.m1.status")),
        Lines{"alarms/site.yaml:2:12: error CF-310: tag \"ps1.m1.status\" "
              "does not exist"});
    EXPECT_EQ(with_alarms(alarm_on("ps1.m1.run") + "  - { tag: ps1.m1.trip, "
                                                   "type: state, alarm_on: "
                                                   "true, priority: high, "
                                                   "area: ps1 }\n"),
              Lines{});
}

// A user's tag of a point tag's name is CF-302, at the second.
TEST(TagNames, AUserTagOfAPointTagsName) {
    EXPECT_EQ(expand({{"templates/meter_a.yaml", std::string(METER)},
                      {"drivers/modbus/line1.yaml", one_device()},
                      {"tags/site.yaml",
                       "tags:\n  - { name: ps1.m1.flow, type: internal, "
                       "value_type: int64 }\n"}}),
              Lines{"tags/site.yaml:2:13: error CF-302: tag \"ps1.m1.flow\" is "
                    "defined twice; first at drivers/modbus/line1.yaml:7"});
}

// The generated tags (12.5): of an output, an area, an alarm, a process,
// a channel, a poll group and a device; the identity tags only for a
// device with `identify`, the `refresh` tag only for an output with it, a
// driver process's signals only for a protocol the tree has.
TEST(TagNames, GeneratedTags) {
    const std::string outputs =
        "outputs:\n"
        "  - { name: ps1.pump.cmd, area: ps1, kind: setpoint, target: "
        "ps1.m1.flow, no_feedback: true, interlocks: [], safe_state: hold }\n";
    const auto with = [&outputs](std::string_view tag) {
        return errors(expand({{"areas.yaml", std::string(AREAS)},
                              {"templates/meter_a.yaml", std::string(METER)},
                              {"drivers/modbus/line1.yaml", one_device()},
                              {"outputs/ps1.yaml", outputs},
                              {"alarms/site.yaml", alarm_on(tag)}}));
    };
    for (const std::string_view tag :
         {"ps1.pump.cmd.status", "ps1.pump.cmd.req_auto",
          "sys.area.ps1.authority", "sys.dev.ps1.m1.state",
          "sys.drv.line1.period.normal", "sys.proc.drv-modbus.log_dropped",
          "sys.proc.hist.config_state"}) {
        EXPECT_EQ(with(tag), Lines{}) << tag;
    }
    for (const std::string_view tag :
         {"ps1.pump.cmd.refresh", "sys.dev.ps1.m1.identity",
          "sys.proc.io.realtime", "sys.drv.line1.period.hourly"}) {
        EXPECT_EQ(with(tag),
                  Lines{"alarms/site.yaml:2:12: error CF-310: tag \"" +
                        std::string(tag) + "\" does not exist"})
            << tag;
    }
}

// A user's tag of a generated tag's name is CF-302; two generated tags of
// one name are another rule's cause and are not reported here.
TEST(TagNames, AUserTagOfAGeneratedName) {
    EXPECT_EQ(
        errors(expand({{"areas.yaml", std::string(AREAS)},
                       {"templates/meter_a.yaml", std::string(METER)},
                       {"drivers/modbus/line1.yaml", one_device()},
                       {"tags/site.yaml",
                        "tags:\n  - { name: sys.area.ps1.authority, "
                        "type: internal, value_type: int64 }\n"
                        "  - { name: ps1.m1.alarm_on.alarm.high, type: "
                        "internal, value_type: int64 }\n"},
                       {"alarms/site.yaml", alarm_on("ps1.m1.alarm_on")}})),
        (Lines{"tags/site.yaml:2:13: error CF-301: tag names starting "
               "with sys. are reserved",
               "tags/site.yaml:3:13: error CF-302: tag "
               "\"ps1.m1.alarm_on.alarm.high\" is defined twice; first "
               "at alarms/site.yaml:2"}));
}

// An alarm's `limit` as a tag, and its `setpoint`, name number tags
// (CF-312; D-116).
TEST(TagNames, LimitAndSetpointAreNumbers) {
    EXPECT_EQ(with_alarms("alarms:\n  - { tag: ps1.m1.flow, type: high, "
                          "limit: ps1.m1.alarm_on, priority: high, area: ps1 "
                          "}\n  - { tag: ps1.m1.flow, type: deviation, limit: "
                          "2, setpoint: ps1.m1.run, priority: high, area: ps1 "
                          "}\n"),
              (Lines{"alarms/site.yaml:2:44: error CF-312: "
                     "\"ps1.m1.alarm_on\" must be a number tag",
                     "alarms/site.yaml:3:62: error CF-312: \"ps1.m1.run\" "
                     "must be a number tag"}));
}

// The points a device's rules name exist in the device (2.6), at the
// instance with the template line.
TEST(TagNames, PointsWithinADevice) {
    const std::string meter =
        std::string(METER) +
        "  - { name: amps, address: { table: holding, register: 4, type: "
        "int16 }, exponent_from: { point: amps_exp, direction: multiply }, "
        "quality_from: [ { point: state, bit: 0, map: { 0: good, 1: bad } } "
        "], source_time_from: clock }\n"
        "  - { name: volts, address: { table: holding, register: 5, type: "
        "int16 }, quality_from: [ { point: self, bit: 15, map: { 0: good, 1: "
        "bad } } ] }\n"
        "health: [ { point: heartbeat, good_when: 1, applies_to: all } ]\n"
        "identify: [ { point: model, equals: 7 } ]\n";
    EXPECT_EQ(
        errors(expand({{"templates/meter_a.yaml", meter},
                       {"drivers/modbus/line1.yaml", one_device()}})),
        // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): long messages
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-310: point "
               "\"heartbeat\" does not exist\n  from template "
               "templates/meter_a.yaml:13:20",
               "drivers/modbus/line1.yaml:7:11: error CF-310: point "
               "\"model\" does not exist\n  from template "
               "templates/meter_a.yaml:14:22",
               "drivers/modbus/line1.yaml:7:11: error CF-310: point "
               "\"amps_exp\" does not exist\n  from template "
               "templates/meter_a.yaml:11:98 (point amps)",
               "drivers/modbus/line1.yaml:7:11: error CF-310: point "
               "\"state\" does not exist\n  from template "
               "templates/meter_a.yaml:11:156 (point amps)",
               "drivers/modbus/line1.yaml:7:11: error CF-310: point "
               "\"clock\" does not exist\n  from template "
               "templates/meter_a.yaml:11:219 (point amps)"}));
}

// A point's poll group is one of its channel's (CF-604): the defaults
// `fast`, `normal`, `slow`, or the channel's own.
TEST(TagNames, PollGroups) {
    const std::string meter =
        "template: meter_a\nprotocol: modbus\npoints:\n"
        "  - { name: a, address: { table: holding, register: 1, type: uint16 "
        "}, poll_group: slow }\n"
        "  - { name: b, address: { table: holding, register: 2, type: uint16 "
        "}, poll_group: hourly }\n";
    EXPECT_EQ(errors(expand({{"templates/meter_a.yaml", meter},
                             {"drivers/modbus/line1.yaml", one_device()}})),
              Lines{"drivers/modbus/line1.yaml:7:11: error CF-604: poll group "
                    "\"hourly\" does not exist on channel \"line1\"\n  from "
                    "template templates/meter_a.yaml:5:84 (point b)"});
}

// A program's sources and the tags it writes are tags (2.6).
TEST(TagNames, TheTagsOfAProgram) {
    EXPECT_EQ(
        errors(expand({{"templates/meter_a.yaml", std::string(METER)},
                       {"drivers/modbus/line1.yaml", one_device()},
                       {"tags/site.yaml",
                        "tags:\n  - { name: site.flag, type: internal, "
                        "value_type: bool, task: normal }\n"},
                       {"control/tasks.yaml",
                        "tasks:\n  normal: { period: 1s, programs: [p1] }\n"},
                       {"control/p1.yaml",
                        "program: p1\ntask: normal\nblocks:\n"
                        "  - { id: cmp, type: compare, in: { a: { tag: "
                        "ps1.m1.flow, max_age: 1s }, b: { tag: ps1.m1.nope, "
                        "max_age: 1s } }, out: { q: site.flag, r: site.none } "
                        "}\n"}})),
        (Lines{"control/p1.yaml:4:85: error CF-310: tag \"ps1.m1.nope\" "
               "does not exist",
               "control/p1.yaml:4:139: error CF-310: tag \"site.none\" "
               "does not exist"}));
}

// An expression alarm's state tag is `<name>.alarm.<id>`; two alarms of
// one id on a tag are CF-812's (slice 7c), not CF-302; a string tag is no
// number for a setpoint; a feedback given as `{ tag, within, settle }`
// names a tag.
TEST(TagNames, AlarmStatesAndFeedbackForms) {
    EXPECT_EQ(with_alarms("alarms:\n"
                          "  - { type: expression, name: site.hot, expr: "
                          "\"ps1.m1.flow > 3\", priority: high, area: ps1 }\n"
                          "  - { tag: site.hot.alarm.expression, type: state, "
                          "alarm_on: 1, priority: high, area: ps1 }\n"
                          "  - { tag: ps1.m1.flow, type: high, limit: 4, "
                          "priority: high, area: ps1 }\n"
                          "  - { tag: ps1.m1.flow, type: high, limit: 5, "
                          "priority: high, area: ps1 }\n"),
              Lines{});
    EXPECT_EQ(
        errors(expand({{"areas.yaml", std::string(AREAS)},
                       {"templates/meter_a.yaml", std::string(METER)},
                       {"drivers/modbus/line1.yaml", one_device()},
                       {"tags/site.yaml",
                        "tags:\n  - { name: site.label, type: manual, "
                        "value_type: string, area: ps1 }\n"},
                       {"alarms/site.yaml",
                        "alarms:\n  - { tag: ps1.m1.flow, type: deviation, "
                        "limit: 2, setpoint: site.label, priority: high, area: "
                        "ps1 }\n"},
                       {"outputs/ps1.yaml",
                        "outputs:\n  - { name: ps1.out, area: ps1, kind: "
                        "setpoint, target: ps1.m1.flow, feedback: { tag: "
                        "ps1.m1.gone, within: 1, settle: 1s }, interlocks: [], "
                        "safe_state: hold }\n"}})),
        (Lines{"alarms/site.yaml:2:62: error CF-312: \"site.label\" "
               "must be a number tag",
               "outputs/ps1.yaml:2:87: error CF-310: tag \"ps1.m1.gone\" "
               "does not exist"}));
}

// Three devices of `template_text` (named meter_a) on line1, with `more`
// keys after the third.
Lines three_meters(std::string_view template_text, const Files& more = {},
                   std::string_view third = {}) {
    std::string devices;
    for (int unit = 1; unit <= 3; ++unit) {
        devices +=
            "  - name: ps1.m" + std::to_string(unit) +
            "\n    template: meter_a\n    unit_id: " + std::to_string(unit) +
            "\n";
    }
    devices += third;
    Files files = more;
    files.try_emplace("areas.yaml", std::string(AREAS));
    files.try_emplace("templates/meter_a.yaml", std::string(template_text));
    files.try_emplace("drivers/modbus/line1.yaml", channel(devices));
    files.try_emplace("tags/site.yaml",
                      "tags:\n  - { name: site.label, type: manual, "
                      "value_type: string, area: ps1 }\n");
    return errors(expand(files));
}

// M1 and M4 of the review of #153: one cause of a template at three
// instances is one error, naming the others; the instance's own part is
// its own error.
TEST(TagNames, OneCauseAtManyInstancesIsOneError) {
    const std::string meter =
        "template: meter_a\nprotocol: modbus\npoints:\n"
        "  - name: flow\n"
        "    address: { table: holding, register: 1, type: uint16 }\n"
        "    poll_group: hourly\n"
        "    alarms: [ { tag: site.nope, type: high, limit: 4, priority: "
        "high, area: ps1 }, { type: deviation, limit: 1, setpoint: "
        "site.label, priority: high, area: ps1 } ]\n"
        "health: [ { point: heartbeat, good_when: 1, applies_to: all } ]\n";
    EXPECT_EQ(
        three_meters(meter, {},
                     "    identify: [ { point: model, equals: 7 } ]\n"),
        // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): long messages
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-310: point "
               "\"heartbeat\" does not exist (and 2 more instances: ps1.m2, "
               "ps1.m3)\n  from template templates/meter_a.yaml:8:20",
               "drivers/modbus/line1.yaml:7:11: error CF-310: tag "
               "\"site.nope\" does not exist (and 2 more instances: ps1.m2, "
               "ps1.m3)\n  from template templates/meter_a.yaml:7:22 (point "
               "flow)",
               "drivers/modbus/line1.yaml:7:11: error CF-312: \"site.label\" "
               "must be a number tag (and 2 more instances: ps1.m2, "
               "ps1.m3)\n  from template templates/meter_a.yaml:7:123 (point "
               "flow)",
               "drivers/modbus/line1.yaml:7:11: error CF-604: poll group "
               "\"hourly\" does not exist on channel \"line1\" (and 2 more "
               "instances: ps1.m2, ps1.m3)\n  from template "
               "templates/meter_a.yaml:6:17 (point flow)",
               "drivers/modbus/line1.yaml:16:26: error CF-310: point "
               "\"model\" does not exist"}));
}

// M2 and S5: a generated name over 127 bytes made by a template is CF-403
// once, at the instance; a point tag of an output's generated name is
// CF-302 at the point's instance.
TEST(TagNames, GeneratedNamesAtTheirInstances) {
    // 7 + 70 + 7 + 31 + 14 bytes: the state tag fits, its shelved_until
    // does not. An alarm id is at most 31 bytes (2.1).
    const std::string point(70, 'f');
    const std::string alarm_id(31, 'x');
    const std::string meter =
        "template: meter_a\nprotocol: modbus\npoints:\n"
        "  - name: " +
        point +
        "\n"
        "    address: { table: holding, register: 1, type: uint16 }\n"
        "    alarms: [ { type: high, id: " +
        alarm_id +
        ", limit: 4, priority: high, area: ps1 } ]\n"
        "  - { name: mode, address: { table: holding, register: 2, type: "
        "uint16 } }\n";
    EXPECT_EQ(
        three_meters(meter,
                     {{"outputs/ps1.yaml",
                       "outputs:\n  - { name: ps1.m2, area: ps1, kind: "
                       "setpoint, target: ps1.m1.mode, no_feedback: true, "
                       "interlocks: [], safe_state: hold }\n"}}),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-403: generated tag "
               "\"ps1.m1." +
                   point.substr(0, 57) +
                   "\"... is 129 bytes, the limit is 127 (and 2 more "
                   "instances: ps1.m2, ps1.m3)\n  from template "
                   "templates/meter_a.yaml:6:15 (point " +
                   point + ")",
               "drivers/modbus/line1.yaml:10:11: error CF-302: tag "
               "\"ps1.m2.mode\" is defined twice; first at "
               "outputs/ps1.yaml:2\n  from template "
               "templates/meter_a.yaml:7:13 (point mode)"}));
}

// M3: two generated tags of one name from different causes are CF-302,
// at the second; S3: an output's or an expression alarm's name in `sys`
// is CF-301.
TEST(TagNames, GeneratedNamesOfTwoCauses) {
    EXPECT_EQ(
        errors(expand(
            {{"areas.yaml", std::string(AREAS)},
             {"templates/meter_a.yaml", std::string(METER)},
             {"drivers/modbus/line1.yaml", one_device()},
             {"alarms/site.yaml",
              alarm_on("ps1.m1.flow", ", id: mode") +
                  "  - { type: expression, name: sys.hot, expr: "
                  "\"ps1.m1.flow > 3\", priority: high, area: ps1 }\n"},
             {"outputs/ps1.yaml",
              "outputs:\n"
              "  - { name: ps1.m1.flow.alarm, area: ps1, kind: setpoint, "
              "target: ps1.m1.flow, no_feedback: true, interlocks: [], "
              "safe_state: hold }\n"
              "  - { name: sys.out, area: ps1, kind: setpoint, target: "
              "ps1.m1.flow, no_feedback: true, interlocks: [], safe_state: "
              "hold }\n"}})),
        (Lines{"alarms/site.yaml:3:31: error CF-301: tag names starting "
               "with sys. are reserved",
               "outputs/ps1.yaml:2:13: error CF-302: tag "
               "\"ps1.m1.flow.alarm.mode\" is defined twice; first at "
               "alarms/site.yaml:2",
               "outputs/ps1.yaml:3:13: error CF-301: tag names starting "
               "with sys. are reserved"}));
}

// S1 and S2: a device given twice is CF-303 alone, its tags not collected
// twice; a device in `sys` is one CF-301, at its name.
TEST(TagNames, DevicesGivenTwiceOrInSys) {
    EXPECT_EQ(errors(expand({{"templates/meter_a.yaml", std::string(METER)},
                             {"drivers/modbus/line1.yaml",
                              channel("  - { name: ps1.m1, template: meter_a, "
                                      "unit_id: 1 }\n"
                                      "  - { name: ps1.m1, template: meter_a, "
                                      "unit_id: 2 }\n"
                                      "  - { name: sys.m3, template: meter_a, "
                                      "unit_id: 3 }\n")}})),
              (Lines{"drivers/modbus/line1.yaml:8:13: error CF-303: device "
                     "\"ps1.m1\" is defined twice; first at "
                     "drivers/modbus/line1.yaml:7",
                     "drivers/modbus/line1.yaml:9:13: error CF-301: tag names "
                     "starting with sys. are reserved"}));
}

// S4: a health rule's `applies_to` names points of the device, or
// patterns that match one (CF-311).
TEST(TagNames, HealthAppliesToPoints) {
    const std::string meter =
        std::string(METER) +
        "health: [ { point: flow, good_when: 1, applies_to: [ alarm_on, "
        "\"fl*\", \"zz*\", nope ] } ]\n";
    EXPECT_EQ(errors(expand({{"templates/meter_a.yaml", meter},
                             {"drivers/modbus/line1.yaml", one_device()}})),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-310: point "
                     "\"nope\" does not exist\n  from template "
                     "templates/meter_a.yaml:11:78",
                     "drivers/modbus/line1.yaml:7:11: error CF-311: pattern "
                     "\"zz*\" matches no tag\n  from template "
                     "templates/meter_a.yaml:11:71"}));
}

// M6: a channel's own poll groups replace the defaults: a point given
// none is in `normal`, CF-604 on a channel without it; the channel's
// period tags are those of its own groups.
TEST(TagNames, AChannelsOwnPollGroups) {
    const std::string line =
        "channel:\n  name: line1\n  type: rtu\n"
        "  device: /dev/serial/by-path/platform-serial1\n  baud: 9600\n"
        "  poll_groups:\n    quick: { period: 1s, priority: 1 }\n"
        "devices:\n  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n";
    const std::string meter =
        "template: meter_a\nprotocol: modbus\npoints:\n"
        "  - { name: a, address: { table: holding, register: 1, type: uint16 "
        "}, poll_group: quick }\n"
        "  - { name: b, address: { table: holding, register: 2, type: uint16 "
        "} }\n";
    EXPECT_EQ(
        errors(expand({{"areas.yaml", std::string(AREAS)},
                       {"templates/meter_a.yaml", meter},
                       {"drivers/modbus/line1.yaml", line},
                       {"alarms/site.yaml",
                        alarm_on("sys.drv.line1.period.quick") +
                            "  - { tag: sys.drv.line1.period.normal, type: "
                            "high, limit: 4, priority: high, area: ps1 }\n"}})),
        (Lines{"alarms/site.yaml:3:12: error CF-310: tag "
               "\"sys.drv.line1.period.normal\" does not exist",
               "drivers/modbus/line1.yaml:9:13: error CF-604: poll group "
               "\"normal\" does not exist on channel \"line1\"\n  from "
               "template templates/meter_a.yaml:5:13 (point b)"}));
}

// M7: the tags of stage 5, generated tags included, count against
// `limits.max_tags` (CF-1001); the references are then not checked.
TEST(TagNames, GeneratedTagsCountAgainstTheLimit) {
    const auto with_limit = [](int limit) {
        return errors(expand(
            {{"system.yaml", std::string(SYSTEM) + "limits: { max_tags: " +
                                 std::to_string(limit) + " }\n"},
             {"areas.yaml", std::string(AREAS)},
             {"templates/meter_a.yaml", std::string(METER)},
             {"drivers/modbus/line1.yaml", one_device()},
             {"alarms/site.yaml", alarm_on("ps1.m1.nope")}}));
    };
    // 21 process tags, the area's, line1's 5 and 3 periods, the device's
    // 17, its 4 point and bit tags: 51, passed at the last point.
    EXPECT_EQ(with_limit(51), Lines{"alarms/site.yaml:2:12: error CF-310: tag "
                                    "\"ps1.m1.nope\" does not exist"});
    EXPECT_EQ(with_limit(50),
              Lines{"drivers/modbus/line1.yaml:7:11: error CF-1001: 51 tags, "
                    "the limit is 50\n  from template "
                    "templates/meter_a.yaml:10:13 (point alarm_on)"});
}

// M5: a program's source given as the tag itself is a tag, unless it is
// a literal or a block's pin.
TEST(TagNames, AProgramsBareSources) {
    EXPECT_EQ(
        errors(expand(
            {{"templates/meter_a.yaml", std::string(METER)},
             {"drivers/modbus/line1.yaml", one_device()},
             {"control/p1.yaml",
              "program: p1\ntask: normal\nblocks:\n"
              "  - { id: run, type: compare, in: { a: ps1.m1.flow, b: 2.5 } }\n"
              "  - { id: cmp, type: compare, in: { a: run.q, b: nope.sp, c: "
              "true, d: 1s } }\n"}})),
        Lines{"control/p1.yaml:5:50: error CF-310: tag \"nope.sp\" does not "
              "exist"});
}

// S7: the generated tags of an output and of alarms, and their types: a
// setpoint must be a number (CF-312), a missing tag is CF-310.
TEST(TagNames, TheGeneratedTagsAndTheirTypes) {
    const std::string meter =
        "template: meter_a\nprotocol: modbus\nbit_order: lsb0\npoints:\n"
        "  - name: flow\n"
        "    address: { table: holding, register: 1, type: uint16 }\n"
        "    alarms: [ { type: high, id: hi, limit: 4, priority: high, area: "
        "ps1 } ]\n"
        "  - name: status\n"
        "    address: { table: holding, register: 2, type: uint16 }\n"
        "    tag: false\n"
        "    bits: { 0: { name: run, alarms: [ { type: state, alarm_on: "
        "true, priority: high, area: ps1 } ] } }\n"
        "  - { name: label, address: { table: holding, register: 3, type: "
        "string, length: 4 } }\n"
        "  - { name: raw, address: { table: holding, register: 5, type: "
        "bytes, length: 4 } }\n"
        "  - { name: valve, address: { table: coil, register: 7 } }\n"
        "  - { name: breaker, dangerous: true, address: { table: coil, "
        "register: 8 } }\n"
        "identify: [ { point: flow, equals: 7 } ]\n"
        "restart_indication: [ { point: gone, rule: changes } ]\n";
    const std::string outputs =
        "outputs:\n"
        "  - { name: ps1.sp, area: ps1, kind: setpoint, target: ps1.m1.flow, "
        "no_feedback: true, interlocks: [], safe_state: hold, refresh: 10s }\n"
        "  - { name: ps1.v, area: ps1, kind: single, target: ps1.m1.valve, "
        "feedback: { on: ps1.m1.run, off: ps1.m1.off }, interlocks: [], "
        "safe_state: off }\n"
        "  - { name: ps1.b, area: ps1, kind: single, target: ps1.m1.breaker, "
        "no_feedback: true, interlocks: [], safe_state: off }\n"
        "  - { name: ps1.s, area: ps1, kind: setpoint, target: ps1.m1.flow, "
        "no_feedback: true, interlocks: [], safe_state: { value: 0 }, "
        "on_shutdown: safe }\n";
    const auto setpoint = [](std::string_view tag) {
        return "  - { tag: ps1.m1.flow, type: deviation, limit: 1, setpoint: " +
               std::string(tag) + ", priority: high, area: ps1 }\n";
    };
    std::string alarms = "alarms:\n";
    for (const std::string_view tag :
         {"ps1.m1.flow.alarm.hi", "ps1.m1.flow.alarm.hi.shelved_until",
          "ps1.m1.run.alarm.state", "site.t.alarm.low", "ps1.sp.mode",
          "ps1.sp.req_auto", "ps1.sp.refresh", "ps1.v.failsafe",
          "ps1.s.failsafe", "ps1.b.failsafe", "ps1.sp.failsafe",
          "sys.dev.ps1.m1.requests", "sys.dev.ps1.m1.refresh_failures",
          "sys.dev.ps1.m1.identity", "sys.dev.ps1.m1.identity_ok",
          "sys.drv.line1.period.slow", "ps1.m1.label", "ps1.m1.raw",
          "ps1.v.req_manual"}) {
        alarms += setpoint(tag);
    }

    // In the order of the report: by file and line.
    // NOLINTBEGIN(bugprone-suspicious-missing-comma): long messages
    const Lines expected = {
        "alarms/site.yaml:11:62: error CF-310: tag \"ps1.b.failsafe\" does not "
        "exist",
        "alarms/site.yaml:12:62: error CF-310: tag \"ps1.sp.failsafe\" does "
        "not exist",
        "alarms/site.yaml:15:62: error CF-312: \"sys.dev.ps1.m1.identity\" "
        "must be a "
        "number tag",
        "alarms/site.yaml:16:62: error CF-312: \"sys.dev.ps1.m1.identity_ok\" "
        "must be a "
        "number tag",
        "alarms/site.yaml:18:62: error CF-312: \"ps1.m1.label\" must be a "
        "number tag",
        "alarms/site.yaml:19:62: error CF-312: \"ps1.m1.raw\" must be a number "
        "tag",
        "alarms/site.yaml:20:62: error CF-312: \"ps1.v.req_manual\" must be a "
        "number "
        "tag",
        "drivers/modbus/line1.yaml:7:11: error CF-310: point \"gone\" does "
        "not exist\n  from template templates/meter_a.yaml:17:32",
        "outputs/ps1.yaml:3:100: error CF-310: tag \"ps1.m1.off\" does not "
        "exist",
        "tags/site.yaml:5:74: error CF-310: tag \"site.nope\" does not exist",
        "tags/site.yaml:5:175: error CF-312: \"ps1.m1.valve\" must be a "
        "number tag"};
    // NOLINTEND(bugprone-suspicious-missing-comma)
    EXPECT_EQ(
        errors(expand({{"areas.yaml", std::string(AREAS)},
                       {"templates/meter_a.yaml", meter},
                       {"drivers/modbus/line1.yaml", one_device()},
                       {"tags/site.yaml",
                        "tags:\n  - name: site.t\n    type: internal\n"
                        "    value_type: float64\n"
                        "    alarms: [ { type: low, limit: 1, priority: high, "
                        "area: ps1 }, { tag: site.nope, type: high, limit: 2, "
                        "priority: high, area: ps1 }, { type: deviation, "
                        "limit: 1, setpoint: ps1.m1.valve, priority: high, "
                        "area: ps1 } ]\n"},
                       {"outputs/ps1.yaml", outputs},
                       {"alarms/site.yaml", alarms}})),
        expected);
}

// S7: an `io` channel's process is `io`; an `io` point is `bool` on
// `gpio`, `float64` on another backend; a local output has a fail-safe tag
// by default, a `step` none; a fail-safe key with `safe` or `release` makes
// one; `refresh_failures` needs a refreshed output.
TEST(TagNames, LocalOutputsAndTheIoProcess) {
    const std::string outputs =
        "outputs:\n"
        "  - { name: ps1.lamp, area: ps1, kind: single, target: ps1.io.lamp, "
        "no_feedback: true, interlocks: [], safe_state: off }\n"
        "  - { name: ps1.step, area: ps1, kind: step, target: ps1.io.lamp, "
        "no_feedback: true, interlocks: [], safe_state: hold, on_shutdown: "
        "hold, on_control_loss: { action: hold } }\n"
        "  - { name: ps1.gen, area: ps1, kind: setpoint, target: ps1.m1.flow, "
        "no_feedback: true, interlocks: [], safe_state: { value: 0 }, "
        "on_generation_change: safe }\n"
        "  - { name: ps1.loss, area: ps1, kind: setpoint, target: "
        "ps1.m1.flow, no_feedback: true, interlocks: [], safe_state: { value: "
        "0 }, on_control_loss: { action: release, after: 1s } }\n";
    std::string alarms = "alarms:\n";
    for (const std::string_view tag :
         {"sys.proc.io.realtime", "ps1.ai.level", "ps1.io.lamp",
          "ps1.lamp.failsafe", "ps1.step.failsafe", "ps1.gen.failsafe",
          "ps1.loss.failsafe", "sys.dev.ps1.m1.refresh_failures",
          "sys.dev.ps1.io.state"}) {
        alarms +=
            "  - { tag: ps1.m1.flow, type: deviation, limit: 1, "
            "setpoint: " +
            std::string(tag) + ", priority: high, area: ps1 }\n";
    }
    EXPECT_EQ(
        errors(expand(
            {{"areas.yaml", std::string(AREAS)},
             {"templates/meter_a.yaml", std::string(METER)},
             {"drivers/modbus/line1.yaml", one_device()},
             {"templates/io_a.yaml",
              "template: io_a\nprotocol: io\npoints:\n"
              "  - { name: lamp, address: { line: 1, direction: output, "
              "at_start: safe } }\n"},
             {"templates/adc_a.yaml",
              "template: adc_a\nprotocol: io\npoints:\n"
              "  - { name: level, address: { channel: in_voltage0 } }\n"},
             {"drivers/io/local.yaml",
              "channel: { name: local, backend: gpio, chip: gpiochip0 }\n"
              "devices:\n  - { name: ps1.io, template: io_a }\n"},
             {"drivers/io/adc.yaml",
              "channel: { name: adc, backend: iio, device: adc0 }\n"
              "devices:\n  - { name: ps1.ai, template: adc_a }\n"},
             {"outputs/ps1.yaml", outputs},
             {"alarms/site.yaml", alarms}})),
        (Lines{"alarms/site.yaml:2:62: error CF-312: \"sys.proc.io.realtime\" "
               "must be a number tag",
               "alarms/site.yaml:4:62: error CF-312: \"ps1.io.lamp\" must be "
               "a number tag",
               "alarms/site.yaml:6:62: error CF-310: tag "
               "\"ps1.step.failsafe\" does not exist",
               "alarms/site.yaml:9:62: error CF-310: tag "
               "\"sys.dev.ps1.m1.refresh_failures\" does not exist"}));
}

}  // namespace
}  // namespace scada::conf
