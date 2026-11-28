// Stage 5 on run-time expressions (configuration schemas contract 5.6;
// decision D-117; slice 7b of #100): virtual tags typed in the order their
// inputs need, their cycles and derived types, and every other run-time
// expression against the tags of the tree. Invented examples only.

#include "expand_cases.h"
#include "scada/conf/expand.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
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

// An interlock with a condition, for the cases of interlock cycles.
std::string interlock(std::string_view name, std::string_view condition,
                      std::string_view more = {}) {
    return "  - { name: " + std::string(name) + ", condition: \"" +
           std::string(condition) + "\", message: m" + std::string(more) +
           " }\n";
}

Lines with_interlocks(std::string_view interlocks, std::string_view tags = {}) {
    return with_tags(tags, {{"outputs/ps1.yaml",
                             "interlocks:\n" + std::string(interlocks)}});
}

// N16 of the delta check: a pattern's walk goes past a name it does not
// match, so a later one closes the cycle; a pattern of an interlock's
// condition that matches the interlock itself is a cycle too.
TEST(RunTimeExpressions, CyclesThroughPatterns) {
    EXPECT_EQ(with_tags(virtual_tag("v.a", R"(sum(\"v.*z\") + 1)") +
                        virtual_tag("v.bz", "v.a + 1")),
              Lines{"tags/site.yaml:5:13: error CF-525: virtual tags form a "
                    "cycle: v.a, v.bz"});
    EXPECT_EQ(with_interlocks(interlock("il.p", R"(count_good(\"il.*\") > 0)")),
              Lines{"outputs/ps1.yaml:2:13: error CF-525: virtual tags form a "
                    "cycle: il.p"});
}

// R1 of the review of #163: an interlock is evaluated like a virtual tag
// (7.3), so a cycle through interlocks, or through interlocks and virtual
// tags, is CF-525 as for virtual tags.
TEST(RunTimeExpressions, ACycleThroughInterlocks) {
    EXPECT_EQ(with_interlocks(interlock("il.y", "il.z") +
                              interlock("il.z", "NOT il.y")),
              Lines{"outputs/ps1.yaml:2:13: error CF-525: virtual tags form a "
                    "cycle: il.y, il.z"});
    EXPECT_EQ(with_interlocks(interlock("il.s", "il.s OR a.b")),
              Lines{"outputs/ps1.yaml:2:13: error CF-525: virtual tags form a "
                    "cycle: il.s"});
    EXPECT_EQ(with_interlocks(interlock("il.v", "v.b"),
                              virtual_tag("v.b", "il.v AND a.b")),
              Lines{"outputs/ps1.yaml:2:13: error CF-525: virtual tags form a "
                    "cycle: il.v, v.b"});
    // An interlock that reads a virtual tag, which reads none of them.
    EXPECT_EQ(
        with_interlocks(interlock("il.w", "v.c"), virtual_tag("v.c", "a.b")),
        Lines{});
}

// S2: a cycle of three is reported once, with every name of it in the
// order written.
TEST(RunTimeExpressions, ACycleOfThree) {
    EXPECT_EQ(with_tags(virtual_tag("v.a", "v.b + 1") +
                        virtual_tag("v.b", "v.c + 1") +
                        virtual_tag("v.c", "v.a + 1") +
                        virtual_tag("v.d", "v.c * 2")),
              Lines{"tags/site.yaml:5:13: error CF-525: virtual tags form a "
                    "cycle: v.a, v.b, v.c"});
}

// M4: past `limits.max_tags` the tags are not whole, and no run-time
// expression is checked against them: CF-1001 alone.
TEST(RunTimeExpressions, NothingIsCheckedPastTheTagLimit) {
    // An alarm's expression and an interlock's condition name a tag that
    // is not stored past the limit (item 2 of the delta check): with the
    // check made, each would be a false CF-310.
    const Lines lines = with_tags(
        virtual_tag("v.e", "z.e + 1") + virtual_tag("v.p", R"(sum(\"z.*\"))"),
        {{"system.yaml", std::string(SYSTEM) + "limits: { max_tags: 3 }\n"},
         {"alarms/site.yaml",
          "alarms:\n  - { name: site.hot, type: expression, expr: \"a.i > "
          "3\", priority: high, area: ps1 }\n"},
         {"outputs/ps1.yaml", "interlocks:\n" + interlock("il.z", "a.i > 2")}});
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_NE(lines.front().find("CF-1001"), std::string::npos)
        << lines.front();
}

// S3: a virtual tag of an internal tag's name is CF-302 (D-116), and the
// internal tag keeps its type: no second error from the virtual one.
TEST(RunTimeExpressions, ATagGivenTwiceKeepsItsFirstType) {
    EXPECT_EQ(with_tags("  - { name: x.n, type: internal, value_type: int64 "
                        "}\n" +
                        virtual_tag("x.n", "a.b") +
                        virtual_tag("v.u", "x.n + 1", ", value_type: int64")),
              Lines{"tags/site.yaml:6:13: error CF-302: tag \"x.n\" is defined "
                    "twice; first at tags/site.yaml:5"});
}

// S4: a tasks file without `normal` leaves a virtual tag without `task`
// with no task (CF-310).
TEST(RunTimeExpressions, TheDefaultTask) {
    const Files tasks{
        {"control/tasks.yaml", "tasks:\n  fast: { period: 10ms }\n"}};
    EXPECT_EQ(with_tags(virtual_tag("v.t", "a.i + 1") +
                            virtual_tag("v.f", "a.i + 2", ", task: fast"),
                        tasks),
              Lines{"tags/site.yaml:5:13: error CF-310: task \"normal\" does "
                    "not exist"});
}

// S4 and the delta check: a shift count written as a number is 0 to 63
// (5.6), also a negative one.
TEST(RunTimeExpressions, TheShiftCount) {
    EXPECT_EQ(with_tags(virtual_tag("v.s", "shl(a.i, 64)")),
              Lines{"tags/site.yaml:5:49: error CF-523: expression: shl: the "
                    "count must be from 0 to 63"});
    EXPECT_EQ(with_tags(virtual_tag("v.s", "shr(a.i, 63)")), Lines{});
    // A negative count written as a number is `-` of a literal; a count
    // that is not a number is checked at run time.
    for (const std::string_view call :
         {"shl(a.i, -1)", "shr(a.i, -64)", "shr(a.i, 64)"}) {
        EXPECT_EQ(with_tags(virtual_tag("v.s", call)),
                  Lines{"tags/site.yaml:5:49: error CF-523: expression: " +
                        std::string(call.substr(0, 3)) +
                        ": the count must be from 0 to 63"})
            << call;
    }
    EXPECT_EQ(with_tags(virtual_tag("v.s", "shl(a.i, -0) + shr(a.i, a.i)")),
              Lines{});
}

// N17 of the delta check: an interlock without `task` has no task either
// in a tasks file without `normal`.
TEST(RunTimeExpressions, AnInterlocksDefaultTask) {
    Files interlock{
        {"control/tasks.yaml", "tasks:\n  fast: { period: 10ms }\n"}};
    interlock.emplace("outputs/site.yaml",
                      "interlocks:\n  - { name: il.a, task: fast, condition: "
                      "\"a.i > 1\", message: m }\n  - { name: il.b, "
                      "condition: \"a.i > 2\", message: m }\n");
    EXPECT_EQ(with_tags({}, interlock),
              Lines{"outputs/site.yaml:3:13: error CF-310: task \"normal\" "
                    "does not exist"});
}

// S2: one text in two tasks is checked in each (the task is part of the
// kept result); an interlock without `task` runs in `normal`; a window far
// above 10,000 cycles is CF-524; the given type stays a virtual tag's after
// CF-527, as its readers expect it.
TEST(RunTimeExpressions, TasksWindowsAndGivenTypes) {
    EXPECT_EQ(
        with_tags(virtual_tag("v.m", "rate(a.r, T#500ms)", ", task: fast") +
                  virtual_tag("v.n", "rate(a.r, T#500ms)", ", task: slow")),
        Lines{"tags/site.yaml:6:50: error CF-524: rate: the window must be 1 "
              "to 10000 cycles of task \"slow\""});
    EXPECT_EQ(with_interlocks(interlock("il.r", "rate(a.r, T#1ms) > 1")),
              Lines{"outputs/ps1.yaml:2:41: error CF-524: rate: the window "
                    "must be 1 to 10000 cycles of task \"normal\""});
    // The kept result of a text is the task's (M10 of the delta check): an
    // interlock's text in `fast`, then in `slow`, is checked in each.
    EXPECT_EQ(
        with_interlocks(
            interlock("il.f", "rate(a.r, T#500ms) > 1", ", task: fast") +
            interlock("il.s", "rate(a.r, T#500ms) > 1", ", task: slow")),
        Lines{"outputs/ps1.yaml:3:41: error CF-524: rate: the window must be "
              "1 to 10000 cycles of task \"slow\""});
    EXPECT_EQ(with_tags(virtual_tag("v.l", "rate(a.r, T#100d)")),
              Lines{"tags/site.yaml:5:50: error CF-524: rate: the window must "
                    "be 1 to 10000 cycles of task \"normal\""});
    EXPECT_EQ(with_tags(virtual_tag("v.g", "a.i * 1.5", ", value_type: int64") +
                        virtual_tag("v.h", "v.g AND a.b")),
              (Lines{"tags/site.yaml:5:64: error CF-527: \"v.g\": the "
                     "expression gives float64",
                     "tags/site.yaml:6:44: error CF-523: expression: AND needs "
                     "BOOL, not INT"}));
}

// S2: at most `kept_run_time` pattern summaries and expression results are
// kept (65,536 by default); past them a pattern is resolved, and a text
// checked, at each use, and nothing is lost: 18 interlocks of distinct
// texts over distinct patterns that match no tag are each CF-311, with 16
// of each kept.
TEST(RunTimeExpressions, WhatIsKeptIsCapped) {
    constexpr std::size_t KEPT = 16;
    constexpr std::size_t MORE = KEPT + 2;
    std::string interlocks;
    for (std::size_t i = 0; i < MORE; ++i) {
        interlocks +=
            interlock("il.k" + std::to_string(i),
                      "count_good(\\\"n" + std::to_string(i) + ".*\\\") > 0");
    }
    ExpandLimits limits;
    limits.kept_run_time = KEPT;
    const Expanded expanded =
        expand_limited({{"areas.yaml", std::string(AREAS)},
                        {"tags/site.yaml", "tags:\n" + std::string(BASE_TAGS)},
                        {"outputs/ps1.yaml", "interlocks:\n" + interlocks}},
                       limits);
    EXPECT_EQ(expanded.error_count, MORE);
    EXPECT_EQ(expanded.run_time.kept_patterns, KEPT);
    EXPECT_EQ(expanded.run_time.kept_results, KEPT);
}

constexpr std::size_t BUDGET_TAGS = 100;

// The tree of the budget's tests: BUDGET_TAGS INT tags `w.iN`, `uses`, and
// an interlock whose condition is CF-523.
Expanded with_budget(std::string_view uses, const ExpandLimits& limits) {
    std::string tags;
    for (std::size_t i = 0; i < BUDGET_TAGS; ++i) {
        tags += "  - { name: w.i" + std::to_string(i) +
                ", type: internal, value_type: int64 }\n";
    }
    return expand_limited(
        {{"areas.yaml", std::string(AREAS)},
         {"tags/site.yaml",
          "tags:\n" + std::string(BASE_TAGS) + tags + std::string(uses)},
         {"outputs/ps1.yaml",
          "interlocks:\n" + interlock("il.x", "a.i AND a.b")}},
        limits);
}

// One virtual tag over the pattern `w.*`.
std::string one_use() {
    return virtual_tag("v.s", R"(sum(\"w.*\"))");
}

// The work of `one_use()` with the contract's limits.
std::uint64_t work_of_one_use() {
    const Expanded whole = with_budget(one_use(), {});
    EXPECT_EQ(whole.error_count, 1U);  // the interlock's CF-523
    return whole.run_time.pattern_work;
}

// Item 1 of the delta check: the work of matching patterns is counted, 32
// units per name visited and one per step of the matcher. Resolving a
// pattern again past the kept summaries counts again.
TEST(RunTimeExpressions, TheWorkOfPatternsIsCounted) {
    const std::uint64_t work = work_of_one_use();
    EXPECT_GT(work, RUN_TIME_NAME_WORK * BUDGET_TAGS);
    EXPECT_LT(work, (RUN_TIME_NAME_WORK + 8) * BUDGET_TAGS);
    const std::string two =
        one_use() + virtual_tag("v.t", R"(sum(\"w.*\") + 1)");
    // Kept, a second use costs nothing; not kept, as much again.
    EXPECT_EQ(with_budget(two, {}).run_time.pattern_work, work);
    ExpandLimits none;
    none.kept_run_time = 0;
    EXPECT_EQ(with_budget(two, none).run_time.pattern_work, 2 * work);
}

// Item 1: past the budget, one CF-113 at the expression being checked,
// and nothing else: neither a CF-311 of its own nor the interlock's
// CF-523, which is checked after it.
TEST(RunTimeExpressions, TheWorkOfPatternsHasABudget) {
    const std::uint64_t work = work_of_one_use();
    ExpandLimits short_of;
    short_of.run_time_pattern_work = work - 1;
    const Expanded stopped = with_budget(one_use(), short_of);
    EXPECT_EQ(stopped.error_count, 1U);
    EXPECT_EQ(stopped.lines,
              Lines{"tags/site.yaml:105:39: error CF-113: the pattern matching "
                    "of run-time expressions exceeds its limit of " +
                    std::to_string(work - 1) + " units"});
    ExpandLimits exact;
    exact.run_time_pattern_work = work;
    EXPECT_EQ(with_budget(one_use(), exact).error_count, 1U);
}

// Item 3 of the budget check: a budget below one name stops the first
// pattern at its first name. Neither it nor a later use of the pattern
// reports a CF-311 of a pattern resolved in part, and nothing after it is
// checked.
TEST(RunTimeExpressions, BelowOneNameNothingIsReportedButCF113) {
    ExpandLimits tiny;
    tiny.run_time_pattern_work = RUN_TIME_NAME_WORK - 1;
    const Expanded stopped = with_budget(
        one_use() + virtual_tag("v.t", R"(sum(\"w.*\") + 1)"), tiny);
    EXPECT_EQ(stopped.error_count, 1U);
    EXPECT_EQ(stopped.lines,
              Lines{"tags/site.yaml:105:39: error CF-113: the pattern matching "
                    "of run-time expressions exceeds its limit of " +
                    std::to_string(RUN_TIME_NAME_WORK - 1) + " units"});
}

// Item 3: the walk of the graph counts too. A pattern over 100 virtual
// tags passes the budget while its vertex is walked: CF-113 at the virtual
// tag whose inputs were being walked, and nothing is told from the graph:
// neither the later virtual tag's CF-523 nor the cycle of v.c1 and v.c2.
TEST(RunTimeExpressions, TheGraphWalkCountsAgainstTheBudget) {
    std::string tags = "tags:\n" + std::string(BASE_TAGS);
    for (std::size_t i = 0; i < BUDGET_TAGS; ++i) {
        tags += virtual_tag("v.w" + std::to_string(i), "1");
    }
    tags += virtual_tag("v.s", R"(sum(\"v.w*\"))") +
            virtual_tag("v.bad", "a.i AND a.b") +
            virtual_tag("v.c1", "v.c2 + 1") + virtual_tag("v.c2", "v.c1 + 1");
    const Files files{{"areas.yaml", std::string(AREAS)},
                      {"tags/site.yaml", tags}};
    const Expanded whole = expand_limited(files, {});
    ASSERT_EQ(whole.lines,
              (Lines{"tags/site.yaml:106:46: error CF-523: expression: AND "
                     "needs BOOL, not INT",
                     "tags/site.yaml:107:13: error CF-525: virtual tags form "
                     "a cycle: v.c1, v.c2"}));
    ExpandLimits short_of;
    short_of.run_time_pattern_work = RUN_TIME_NAME_WORK * BUDGET_TAGS / 2;
    const Expanded stopped = expand_limited(files, short_of);
    EXPECT_EQ(stopped.error_count, 1U);
    EXPECT_EQ(stopped.lines,
              Lines{"tags/site.yaml:105:13: error CF-113: the pattern matching "
                    "of run-time expressions exceeds its limit of " +
                    std::to_string(short_of.run_time_pattern_work) + " units"});
}

}  // namespace
}  // namespace scada::conf
