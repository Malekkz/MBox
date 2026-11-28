// Stage 5: the control tasks (configuration schemas contract 9.1 and 10.9;
// decision D-125; slice 7e-2 of #100).

#include "expand_cases.h"

#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/validate.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace scada::conf {
namespace {

// NOLINTBEGIN(bugprone-suspicious-missing-comma): long messages

// The errors of the tree of `files`.
Lines errors_of(const Files& files) {
    Lines found;
    for (const std::string& line : expand(files)) {
        if (line.find(": error ") != std::string::npos) {
            found.push_back(line);
        }
    }
    return found;
}

std::string program(std::string_view name, std::string_view more = {}) {
    return "program: " + std::string(name) + "\n" + std::string(more) +
           "blocks: []\n";
}

// CF-901 and CF-902: each program is in exactly one task, and a program's
// own `task` agrees with it.
TEST(TaskRules, ProgramsAndTheirTasks) {
    EXPECT_EQ(errors_of({{"control/tasks.yaml",
                          "tasks:\n"
                          "  fast: { period: 10ms, programs: [a, b] }\n"
                          "  normal: { period: 100ms, programs: [b, c] }\n"},
                         {"control/a.yaml", program("a", "task: fast\n")},
                         {"control/b.yaml", program("b")},
                         {"control/c.yaml", program("c", "task: fast\n")},
                         {"control/d.yaml", program("d")}}),
              (Lines{"control/b.yaml:1:10: error CF-901: program \"b\" must be "
                     "in exactly one task",
                     "control/c.yaml:2:7: error CF-902: program \"c\": task "
                     "\"fast\" disagrees with tasks.yaml (\"normal\")",
                     "control/d.yaml:1:10: error CF-901: program \"d\" must be "
                     "in exactly one task"}));
}

// Without control/tasks.yaml no task lists a program.
TEST(TaskRules, TheDefaultTasksListNoProgram) {
    EXPECT_EQ(errors_of({{"control/a.yaml", program("a")}}),
              Lines{"control/a.yaml:1:10: error CF-901: program \"a\" must be "
                    "in exactly one task"});
}

// CF-906: a task's priority, given or the default 50, is below the I/O
// scan's (default 95).
TEST(TaskRules, PrioritiesBelowTheScan) {
    EXPECT_EQ(errors_of({{"control/tasks.yaml",
                          "tasks:\n"
                          "  fast: { period: 10ms, priority: 90 }\n"
                          "  normal: { period: 100ms }\n"}}),
              Lines{});
    EXPECT_EQ(errors_of({{"system.yaml", std::string(SYSTEM) +
                                             "realtime: { io_priority: 50 }\n"},
                         {"control/tasks.yaml",
                          "tasks:\n"
                          "  fast: { period: 10ms, priority: 49 }\n"
                          "  normal: { period: 100ms }\n"
                          "  slow: { period: 1s, priority: 60 }\n"}}),
              (Lines{"control/tasks.yaml:3:3: error CF-906: task \"normal\": "
                     "priority 50 must be below 50",
                     "control/tasks.yaml:4:33: error CF-906: task \"slow\": "
                     "priority 60 must be below 50"}));
    EXPECT_EQ(
        errors_of({{"system.yaml",
                    std::string(SYSTEM) + "realtime: { io_priority: 50 }\n"}}),
        (Lines{"system.yaml:3:26: error CF-906: task \"fast\": priority "
               "50 must be below 50",
               "system.yaml:3:26: error CF-906: task \"normal\": priority "
               "50 must be below 50",
               "system.yaml:3:26: error CF-906: task \"slow\": priority "
               "50 must be below 50"}));
}

// A Modbus meter whose `flow` is polled every 1 s (normal) and `fast_flow`
// every 200 ms, an `io` input, and the tasks: `fast` is rt_required.
constexpr std::string_view METER =
    "template: meter_a\nprotocol: modbus\npoints:\n"
    "  - { name: flow, address: { table: holding, register: 1, type: uint16 "
    "} }\n"
    "  - { name: fast_flow, poll_group: fast, address: { table: holding, "
    "register: 2, type: uint16 } }\n"
    "  - { name: run, address: { table: coil, register: 3 } }\n"
    "  - { name: cmd, access: wo, storage: volatile, address: { table: "
    "holding, register: 4, type: uint16 } }\n";
constexpr std::string_view IO_TEMPLATE =
    "template: io_a\nprotocol: io\npoints:\n"
    "  - { name: button, address: { line: 3, direction: input } }\n";
constexpr std::string_view TASKS =
    "tasks:\n"
    "  fast: { period: 10ms, rt_required: true, programs: [p_fast] }\n"
    "  normal: { period: 100ms, programs: [p_normal] }\n"
    "  slow: { period: 1s, programs: [p_slow] }\n";

// The tree of the meter at ps1.m1, the input at ps1.io, the tasks, the
// three programs (given or empty) and `files`.
Files inputs_tree(Files files) {
    files.try_emplace("templates/meter_a.yaml", std::string(METER));
    files.try_emplace("templates/io_a.yaml", std::string(IO_TEMPLATE));
    files.try_emplace("drivers/modbus/line1.yaml", one_device());
    files.try_emplace(
        "drivers/io/local.yaml",
        "channel: { name: local, backend: gpio, chip: gpiochip0 }\n"
        "devices:\n  - { name: ps1.io, template: io_a }\n");
    files.try_emplace("control/tasks.yaml", std::string(TASKS));
    for (const std::string_view name : {"p_fast", "p_normal", "p_slow"}) {
        files.try_emplace("control/" + std::string(name) + ".yaml",
                          program(name));
    }
    return files;
}

constexpr std::uint16_t CW_SLOWER_INPUT = 903;

// The CW-903 warnings of the tree, which has no error.
Lines warnings_of(const Files& files) {
    const Result<Validation> validation = validated(inputs_tree(files));
    EXPECT_TRUE(validation.ok());
    if (!validation.ok()) {
        return {"not validated"};
    }
    for (const Diagnostic& error : validation.value().report.errors) {
        ADD_FAILURE() << format(error);
    }
    Lines found;
    for (const Diagnostic& warning : validation.value().report.warnings) {
        if (warning.code == CW_SLOWER_INPUT) {
            found.push_back(format(warning));
        }
    }
    return found;
}

std::string tags(std::string_view lines) {
    return "tags:\n" + std::string(lines);
}

// A program of one block reading `inputs` and writing `out`.
std::string block_program(std::string_view name, std::string_view inputs,
                          std::string_view out = "{}") {
    return "program: " + std::string(name) +
           "\nblocks:\n  - { id: b1, type: move, in: " + std::string(inputs) +
           ", out: " + std::string(out) + " }\n";
}

// CW-903: the slowest input of each reader, against its task's period. A
// tag of a tag file, and a write-only point, have no poll period.
TEST(TaskRules, InputsPolledMoreSlowlyThanTheTask) {
    EXPECT_EQ(
        warnings_of(
            {{"tags/site.yaml",
              tags("  - { name: v.a, type: virtual, expr: \"ps1.m1.flow + "
                   "ps1.m1.fast_flow\" }\n"
                   "  - { name: v.b, type: virtual, task: slow, expr: "
                   "\"ps1.m1.flow + 1\" }\n"
                   "  - { name: v.c, type: virtual, expr: \"ps1.m1.fast_flow "
                   "* 2\" }\n"
                   "  - { name: v.d, type: virtual, task: slow, expr: "
                   "\"v.a + 1\" }\n"
                   "  - { name: v.e, type: virtual, expr: "
                   "\"sum(\\\"ps1.m1.*flow\\\")\" }\n"
                   "  - { name: v.wo, type: virtual, expr: \"ps1.m1.cmd + 1\" "
                   "}\n")},
             {"control/p_slow.yaml",
              block_program("p_slow", "{ x: ps1.m1.fast_flow }")},
             // An io line is read every scan cycle: no poll period.
             {"control/p_fast.yaml",
              block_program("p_fast", "{ x: ps1.io.button }")},
             {"control/p_normal.yaml",
              block_program("p_normal",
                            "{ x: { tag: ps1.m1.flow, max_age: 2s }, y: "
                            "b1.q, z: 3 }")}}),
        (Lines{"control/p_normal.yaml:3:43: warning CW-903: task \"normal\" "
               "runs every 100ms but reads \"ps1.m1.flow\", polled every 1s",
               "tags/site.yaml:2:39: warning CW-903: task \"normal\" runs "
               "every 100ms but reads \"ps1.m1.flow\", polled every 1s",
               "tags/site.yaml:4:39: warning CW-903: task \"normal\" runs "
               "every 100ms but reads \"ps1.m1.fast_flow\", polled every "
               "200ms",
               "tags/site.yaml:6:39: warning CW-903: task \"normal\" runs "
               "every 100ms but reads \"ps1.m1.flow\", polled every 1s"}));
}

// CF-904 (ruling T2): an rt_required task reads no tag a protocol driver
// owns, directly or through virtual tags of any task; `io` points, tags of
// tag files and interlocks are allowed.
TEST(TaskRules, AnRtRequiredTaskReadsNoDriverTag) {
    EXPECT_EQ(
        errors_of(inputs_tree(
            {{"tags/site.yaml",
              tags("  - { name: v.io, type: virtual, task: fast, expr: "
                   "\"ps1.io.button\" }\n"
                   "  - { name: v.m, type: virtual, task: fast, expr: "
                   "\"ps1.m1.run\" }\n"
                   "  - { name: v.slow, type: virtual, task: slow, expr: "
                   "\"ps1.m1.flow + 1\" }\n"
                   "  - { name: v.mid, type: virtual, task: slow, expr: "
                   "\"v.slow * 2\" }\n"
                   "  - { name: v.through, type: virtual, task: fast, expr: "
                   "\"v.mid > 3\" }\n"
                   "  - { name: v.dev, type: virtual, task: fast, expr: "
                   "\"sys.dev.ps1.m1.state\" }\n"
                   "  - { name: w.x, type: internal, value_type: int64 }\n"
                   "  - { name: v.own, type: virtual, task: fast, expr: "
                   "\"w.x + 1\" }\n"
                   "  - { name: v.pat, type: virtual, task: fast, expr: "
                   "\"sum(\\\"v.sl*\\\") > 1\" }\n"
                   "  - { name: b1.v, type: virtual, task: slow, expr: "
                   "\"ps1.m1.flow * 3\" }\n"
                   "  - { name: v.il, type: virtual, task: fast, expr: "
                   "\"il.c\" }\n")},
             {"outputs/ps1.yaml",
              "interlocks:\n"
              "  - { name: il.a, task: fast, condition: \"v.io AND "
              "ps1.m1.run\", message: m }\n"
              "  - { name: il.b, task: fast, condition: \"v.io\", message: m "
              "}\n"
              // An interlock is allowed: what its condition reads is not
              // followed (ruling T2).
              "  - { name: il.c, task: slow, condition: \"ps1.m1.flow > 1\", "
              "message: m }\n"},
             {"control/p_fast.yaml",
              block_program("p_fast", "{ x: ps1.m1.fast_flow, y: il.b }")},
             // A program reads a virtual tag; `b1.v` is the pin of block b1,
             // not the tag.
             {"control/p_fast2.yaml", block_program("p_fast2", "{ x: v.mid }")},
             {"control/p_fast3.yaml", block_program("p_fast3", "{ y: b1.v }")},
             {"control/tasks.yaml",
              "tasks:\n"
              "  fast: { period: 10ms, rt_required: true, programs: [p_fast, "
              "p_fast2, p_fast3] }\n"
              "  normal: { period: 100ms, programs: [p_normal] }\n"
              "  slow: { period: 1s, programs: [p_slow] }\n"}})),
        (Lines{"control/p_fast.yaml:3:36: error CF-904: task \"fast\" is "
               "rt_required and reads \"ps1.m1.fast_flow\" from drv-modbus",
               "control/p_fast2.yaml:3:36: error CF-904: task \"fast\" is "
               "rt_required and reads \"ps1.m1.flow\" from drv-modbus",
               "outputs/ps1.yaml:2:42: error CF-904: task \"fast\" is "
               "rt_required and reads \"ps1.m1.run\" from drv-modbus",
               "tags/site.yaml:3:51: error CF-904: task \"fast\" is "
               "rt_required and reads \"ps1.m1.run\" from drv-modbus",
               "tags/site.yaml:6:57: error CF-904: task \"fast\" is "
               "rt_required and reads \"ps1.m1.flow\" from drv-modbus",
               "tags/site.yaml:7:53: error CF-904: task \"fast\" is "
               "rt_required and reads \"sys.dev.ps1.m1.state\" from "
               "drv-modbus",
               "tags/site.yaml:10:53: error CF-904: task \"fast\" is "
               "rt_required and reads \"ps1.m1.flow\" from drv-modbus"}));
}

// CF-905 (ruling T3): an internal tag is written by one task, its own when
// it names one; an `out` to a tag that is not internal is phase 6's.
TEST(TaskRules, AnInternalTagHasOneWritingTask) {
    EXPECT_EQ(
        errors_of(inputs_tree(
            {{"tags/site.yaml",
              tags("  - { name: w.x, type: internal, value_type: int64 }\n"
                   "  - { name: w.y, type: internal, value_type: int64, "
                   "task: slow }\n"
                   "  - { name: w.z, type: internal, value_type: int64 }\n"
                   "  - { name: w.m, type: manual, value_type: int64, area: "
                   "ps1 }\n")},
             {"areas.yaml", "areas:\n  ps1: { description: Station }\n"},
             {"control/p_normal.yaml",
              block_program("p_normal", "{}",
                            "{ q: w.x, r: w.y, s: w.z, t: w.m }")},
             {"control/p_slow.yaml",
              block_program("p_slow", "{}", "{ q: w.x, r: w.y, t: w.m }")},
             {"control/p_slow2.yaml",
              "program: p_slow2\ntask: slow\nblocks:\n  - { id: b1, type: "
              "move, in: {}, out: { s: ps1.m1.flow } }\n"}})),
        (Lines{"control/p_normal.yaml:3:53: error CF-905: tag \"w.y\" is "
               "written by \"normal\" and \"slow\"",
               "control/p_slow.yaml:3:45: error CF-905: tag \"w.x\" is "
               "written by \"normal\" and \"slow\"",
               "control/p_slow2.yaml:1:10: error CF-901: program \"p_slow2\" "
               "must be in exactly one task"}));
}

// NOLINTEND(bugprone-suspicious-missing-comma)

}  // namespace
}  // namespace scada::conf
