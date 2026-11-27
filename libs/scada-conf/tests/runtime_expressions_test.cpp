// Stage 5 on run-time expressions (configuration schemas contract 5.6;
// decision D-117; slice 7b of #100): virtual tags typed in the order their
// inputs need, their cycles and derived types, and every other run-time
// expression against the tags of the tree. Invented examples only.

#include "expand_cases.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace scada::conf {
namespace {

constexpr std::string_view AREAS = "areas:\n  ps1: { description: Station }\n";

// The tags every case reads: an INT, a REAL and a BOOL.
constexpr std::string_view BASE_TAGS =
    "  - { name: a.i, type: internal, value_type: int64 }\n"
    "  - { name: a.r, type: internal, value_type: float64 }\n"
    "  - { name: a.b, type: internal, value_type: bool }\n";

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

// The errors of a tree of `tags` (after the base tags) and `more` files.
Lines with_tags(std::string_view tags, const Files& more = {}) {
    Files files = more;
    files.try_emplace("areas.yaml", std::string(AREAS));
    files.try_emplace("tags/site.yaml",
                      "tags:\n" + std::string(BASE_TAGS) + std::string(tags));
    return errors(expand(files));
}

// A virtual tag of `name` and `expr`, with `more` keys.
std::string virtual_tag(std::string_view name, std::string_view expr,
                        std::string_view more = {}) {
    return "  - { name: " + std::string(name) + ", type: virtual, expr: \"" +
           std::string(expr) + "\"" + std::string(more) + " }\n";
}

// A virtual tag's type comes from its expression, from inputs typed first,
// whatever the order they are written in (5.6); a given value_type must be
// the derived one (CF-527).
TEST(RunTimeExpressions, AVirtualTagTakesTheTypeOfItsExpression) {
    EXPECT_EQ(with_tags(virtual_tag("v.r", "a.i * 1.5") +
                        virtual_tag("v.i", "a.i + 1", ", value_type: int64")),
              Lines{});
    EXPECT_EQ(with_tags(virtual_tag("v.r", "a.i * 1.5", ", value_type: int64")),
              Lines{"tags/site.yaml:5:64: error CF-527: \"v.r\": the "
                    "expression gives float64"});
    // v.x reads v.y, written after it: v.y is typed first, so v.x's given
    // type is checked against INT.
    EXPECT_EQ(with_tags(virtual_tag("v.x", "v.y + 1", ", value_type: bool") +
                        virtual_tag("v.y", "a.i * 2")),
              Lines{"tags/site.yaml:5:62: error CF-527: \"v.x\": the "
                    "expression gives int64"});
    // TIME gives int64 milliseconds.
    EXPECT_EQ(with_tags(virtual_tag("v.t", "a.r.age", ", value_type: int64")),
              Lines{});
}

// Virtual tags that read each other are CF-525 once, also through a
// pattern that matches the tag itself; a tag that reads one of them gives
// no error of its own.
TEST(RunTimeExpressions, ACycleOfVirtualTags) {
    EXPECT_EQ(with_tags(virtual_tag("v.a", "v.b + 1") +
                        virtual_tag("v.b", "v.a.quality + 1") +
                        virtual_tag("v.c", "v.a * 2")),
              Lines{"tags/site.yaml:5:13: error CF-525: virtual tags form a "
                    "cycle: v.a, v.b"});
    EXPECT_EQ(with_tags(virtual_tag("v.s", "sum(\\\"v.*\\\")")),
              Lines{"tags/site.yaml:5:13: error CF-525: virtual tags form a "
                    "cycle: v.s"});
    EXPECT_EQ(with_tags(virtual_tag("v.n", "tag(\\\"v.n\\\") + 1")),
              Lines{"tags/site.yaml:5:13: error CF-525: virtual tags form a "
                    "cycle: v.n"});
}

// A time function's window is 1 to 10,000 cycles of the task that runs the
// expression: a virtual tag's or an interlock's, `normal` by default, of
// control/tasks.yaml where it is given (9.1). An alarm runs in no task:
// its windows are not checked (D-117, on #136).
TEST(RunTimeExpressions, TheWindowsOfTheirTasks) {
    EXPECT_EQ(with_tags(virtual_tag("v.m", "rate(a.r, T#50ms)")),
              Lines{"tags/site.yaml:5:50: error CF-524: rate: the window must "
                    "be 1 to 10000 "
                    "cycles of task \"normal\""});
    EXPECT_EQ(with_tags(virtual_tag("v.m", "rate(a.r, T#5s)", ", task: slow")),
              Lines{});
    EXPECT_EQ(with_tags(virtual_tag("v.m", "rate(a.r, T#500ms)"),
                        {{"control/tasks.yaml",
                          "tasks:\n  normal: { period: 1s }\n"}}),
              Lines{"tags/site.yaml:5:50: error CF-524: rate: the window must "
                    "be 1 to 10000 "
                    "cycles of task \"normal\""});
    EXPECT_EQ(with_tags("", {{"alarms/site.yaml",
                              "alarms:\n  - { name: a.x, type: expression, "
                              "expr: \"rate(a.r, T#1ms) > 1\", priority: high, "
                              "area: ps1 }\n"}}),
              Lines{});
}

// An alarm's expr, its suppress_when and an interlock's condition are BOOL.
TEST(RunTimeExpressions, BoolResults) {
    EXPECT_EQ(with_tags("", {{"outputs/ps1.yaml",
                              "interlocks:\n  - { name: ps1.ok, condition: "
                              "\"a.i + 1\", message: low }\n"}}),
              Lines{"outputs/ps1.yaml:2:37: error CF-523: expression: the "
                    "result must be "
                    "BOOL, not INT"});
    EXPECT_EQ(with_tags("", {{"alarms/site.yaml",
                              "alarms:\n  - { tag: a.r, type: high, limit: 4, "
                              "priority: high, area: ps1, suppress_when: "
                              "\"a.r\" }\n"}}),
              Lines{"alarms/site.yaml:2:82: error CF-523: expression: the "
                    "result must be "
                    "BOOL, not REAL"});
    EXPECT_EQ(with_tags("  - { name: a.t, type: internal, value_type: float64, "
                        "alarms: [ { name: a.t.hot, type: expression, expr: "
                        "\"a.b AND a.r > 2\", priority: high, area: ps1 } ] "
                        "}\n"),
              Lines{});
}

// A template's alarm expression is checked at each instance, and one cause
// at many instances is one error (D-114). An expression alarm's name is the
// same at every instance, so its state tag is CF-302 at the others (D-116).
TEST(RunTimeExpressions, ATemplatesAlarmAtItsInstances) {
    std::string devices;
    for (int unit = 1; unit <= 3; ++unit) {
        devices += "  - { name: ps1.m" + std::to_string(unit) +
                   ", template: meter_a, unit_id: " + std::to_string(unit) +
                   " }\n";
    }
    EXPECT_EQ(
        with_tags("",
                  {{"templates/meter_a.yaml",
                    "template: meter_a\nprotocol: modbus\npoints:\n"
                    "  - name: flow\n"
                    "    address: { table: holding, register: 1, type: "
                    "uint16 }\n"
                    "    alarms: [ { name: a.flow_hot, type: expression, "
                    "expr: \"a.nope > 1\", priority: high, area: ps1 } ]\n"},
                   {"drivers/modbus/line1.yaml", channel(devices)}}),
        (Lines{"drivers/modbus/line1.yaml:7:13: error CF-310: tag \"a.nope\" "
               "does not exist (and 2 more instances: ps1.m2, ps1.m3)\n  from "
               "template templates/meter_a.yaml:6:60 (point flow)",
               "drivers/modbus/line1.yaml:8:13: error CF-302: tag "
               "\"a.flow_hot.alarm.expression\" is defined twice; first at "
               "drivers/modbus/line1.yaml:7 (and 1 more instance: ps1.m3)\n  "
               "from template templates/meter_a.yaml:6:15 (point flow)"}));
}

// The derived type of a virtual tag is the one later checks read: a
// setpoint must be a number tag (CF-312, D-116).
TEST(RunTimeExpressions, LaterChecksReadTheDerivedType) {
    EXPECT_EQ(with_tags(virtual_tag("v.b", "a.i > 3"),
                        {{"alarms/site.yaml",
                          "alarms:\n  - { tag: a.r, type: deviation, limit: "
                          "1, setpoint: v.b, priority: high, area: ps1 }\n"}}),
              Lines{"alarms/site.yaml:2:54: error CF-312: \"v.b\" must be a "
                    "number tag"});
}

}  // namespace
}  // namespace scada::conf
