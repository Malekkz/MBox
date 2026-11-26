// Stage 5: the tag names of the expanded tree and the references to them
// outside expressions (configuration schemas contract 2.6, 12.4, 12.5;
// decision D-116; slice 7a of #100).

#include "expand_cases.h"

#include <gtest/gtest.h>

#include <string>

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
    EXPECT_EQ(errors(expand({{"templates/meter_a.yaml", meter},
                             {"drivers/modbus/line1.yaml", one_device()}})),
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

}  // namespace
}  // namespace scada::conf
