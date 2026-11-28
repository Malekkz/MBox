// Stage 5: areas, outputs and their safe states (configuration schemas
// contract 7.1 to 7.4 and 10.7; decision D-124; slice 7e-1 of #100).

#include "expand_cases.h"
#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/validate.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace scada::conf {
namespace {

// NOLINTBEGIN(bugprone-suspicious-missing-comma): long messages

constexpr std::string_view AREAS = "areas:\n  ps1: { description: Station }\n";

// Targets of every kind the rules tell apart, from line 4.
constexpr std::string_view METER =
    "template: meter_a\nprotocol: modbus\npoints:\n"
    "  - { name: ro, address: { table: holding, register: 1, type: uint16 } "
    "}\n"
    "  - { name: sp, access: rw, storage: volatile, address: { table: "
    "holding, register: 2, type: uint16 } }\n"
    "  - { name: sp2, access: rw, storage: volatile, address: { table: "
    "holding, register: 3, type: uint16 } }\n"
    "  - { name: nvm, access: rw, storage: nvm, address: { table: holding, "
    "register: 4, type: uint16 } }\n"
    "  - { name: c1, access: rw, storage: volatile, address: { table: coil, "
    "register: 10 } }\n"
    "  - { name: c2, access: rw, storage: volatile, address: { table: coil, "
    "register: 11 } }\n"
    "  - { name: c3, access: wo, storage: volatile, address: { table: coil, "
    "register: 12 } }\n"
    "  - { name: danger, access: rw, storage: volatile, dangerous: true, "
    "address: { table: coil, register: 20 } }\n"
    "  - { name: text, access: rw, storage: volatile, address: { table: "
    "holding, register: 30, type: string, length: 4 } }\n"
    "  - { name: state, address: { table: coil, register: 40 } }\n"
    "  - name: status\n"
    "    address: { table: holding, register: 50, type: uint16 }\n"
    "    tag: false\n"
    "    bits: { 0: run }\n"
    "bit_order: lsb0\n";

constexpr std::string_view IO_TEMPLATE =
    "template: io_a\nprotocol: io\npoints:\n"
    "  - { name: lamp, access: rw, storage: volatile, address: { line: 1, "
    "direction: output, at_start: safe } }\n";

// The tree of meter_a at ps1.m1, an `io` device ps1.io, the area ps1 and
// `files`.
Files tree_with(Files files) {
    files.try_emplace("areas.yaml", std::string(AREAS));
    files.try_emplace("templates/meter_a.yaml", std::string(METER));
    files.try_emplace("templates/io_a.yaml", std::string(IO_TEMPLATE));
    files.try_emplace("drivers/modbus/line1.yaml", one_device());
    files.try_emplace(
        "drivers/io/local.yaml",
        "channel: { name: local, backend: gpio, chip: gpiochip0 }\n"
        "devices:\n  - { name: ps1.io, template: io_a }\n");
    return files;
}

// The errors of the tree with `outputs` as outputs/ps1.yaml.
Lines errors_of(std::string_view outputs, Files more = {}) {
    more.emplace("outputs/ps1.yaml", "outputs:\n" + std::string(outputs));
    Lines found;
    for (const std::string& line : expand(tree_with(more))) {
        if (line.find(": error ") != std::string::npos) {
            found.push_back(line);
        }
    }
    return found;
}

// The warnings of the tree with `outputs`, which has no error.
Lines warnings_of(std::string_view outputs, Files more = {}) {
    more.emplace("outputs/ps1.yaml", "outputs:\n" + std::string(outputs));
    const Result<Validation> validation = validated(tree_with(more));
    EXPECT_TRUE(validation.ok());
    if (!validation.ok()) {
        return {"not validated"};
    }
    for (const Diagnostic& error : validation.value().report.errors) {
        ADD_FAILURE() << format(error);
    }
    Lines found;
    for (const Diagnostic& warning : validation.value().report.warnings) {
        found.push_back(format(warning));
    }
    return found;
}

// An output of `kind` on `target` with the keys a valid one needs, and
// `more` after them.
std::string output(std::string_view name, std::string_view kind,
                   std::string_view target, std::string_view more = {}) {
    return "  - { name: " + std::string(name) +
           ", area: ps1, kind: " + std::string(kind) +
           ", target: " + std::string(target) +
           ", no_feedback: true, interlocks: [ps1.ok], safe_state: hold" +
           std::string(more) + " }\n";
}

constexpr std::string_view INTERLOCK =
    "interlocks:\n  - { name: ps1.ok, condition: \"ps1.m1.ro > 1\", "
    "message: low }\n";

// Outputs written after the interlock they name.
Lines errors_with_interlock(std::string_view outputs, Files more = {}) {
    more.emplace("outputs/ps1.yaml",
                 std::string(INTERLOCK) + "outputs:\n" + std::string(outputs));
    Lines found;
    for (const std::string& line : expand(tree_with(more))) {
        if (line.find(": error ") != std::string::npos) {
            found.push_back(line);
        }
    }
    return found;
}

// --- CF-702 and CF-703: targets ----------------------------------------

// A target is a point's own tag, writable, and not STRING; each target has
// one output, and an output given twice is CF-302 alone.
TEST(OutputRules, TargetsAreWritablePoints) {
    EXPECT_EQ(
        errors_with_interlock(output("ps1.a", "setpoint", "ps1.m1.ro") +
                                  output("ps1.b", "single", "ps1.m1.run") +
                                  output("ps1.c", "setpoint", "ps1.m1.text") +
                                  output("ps1.d", "setpoint", "site.t") +
                                  output("ps1.e", "setpoint", "ps1.m1.sp") +
                                  output("ps1.f", "setpoint", "ps1.m1.sp") +
                                  output("ps1.g", "single", "ps1.m1.c3") +
                                  output("ps1.h", "single", "ps1.io.lamp"),
                              {{"tags/site.yaml",
                                "tags:\n  - { name: site.t, type: "
                                "internal, value_type: int64 }\n"}}),
        (Lines{"outputs/ps1.yaml:4:55: error CF-702: output \"ps1.a\": target "
               "\"ps1.m1.ro\" is not a writable point",
               "outputs/ps1.yaml:5:53: error CF-702: output \"ps1.b\": target "
               "\"ps1.m1.run\" is not a writable point",
               "outputs/ps1.yaml:6:55: error CF-702: output \"ps1.c\": target "
               "\"ps1.m1.text\" is not a writable point",
               "outputs/ps1.yaml:7:55: error CF-702: output \"ps1.d\": target "
               "\"site.t\" is not a writable point",
               "outputs/ps1.yaml:9:55: error CF-703: tag \"ps1.m1.sp\" is the "
               "target of outputs \"ps1.e\" and \"ps1.f\""}));
    EXPECT_EQ(
        errors_with_interlock(output("ps1.a", "setpoint", "ps1.m1.sp") +
                              output("ps1.a", "setpoint", "ps1.m1.sp")),
        Lines{"outputs/ps1.yaml:5:13: error CF-302: tag \"ps1.a.mode\" is "
              "defined twice; first at outputs/ps1.yaml:4"});
}

// --- CF-709, CF-710, CF-711: keys an output needs ---------------------

TEST(OutputRules, FeedbackSafeStateAndInterlocks) {
    EXPECT_EQ(
        errors_with_interlock(
            "  - { name: ps1.a, area: ps1, kind: setpoint, target: ps1.m1.sp, "
            "safe_state: hold }\n"
            "  - { name: ps1.b, area: ps1, kind: setpoint, target: "
            "ps1.m1.sp2, no_feedback: true }\n"
            "  - { name: ps1.c, area: ps1, kind: single, target: ps1.m1.c1, "
            "no_feedback: true, safe_state: off }\n"
            // A dangerous target holds without being told (7.2).
            "  - { name: ps1.d, area: ps1, kind: single, target: "
            "ps1.m1.danger, feedback: ps1.m1.state, interlocks: [] }\n"
            "  - { name: ps1.e, area: ps1, kind: double, target: ps1.m1.c2, "
            "feedback: { on: ps1.m1.state, off: ps1.m1.run }, interlocks: { "
            "on: [ps1.ok], off: [] }, safe_state: off }\n"),
        (Lines{"outputs/ps1.yaml:4:13: error CF-709: output \"ps1.a\" needs "
               "feedback or no_feedback: true",
               "outputs/ps1.yaml:5:13: error CF-710: output \"ps1.b\" needs a "
               "safe_state",
               "outputs/ps1.yaml:6:13: error CF-711: output \"ps1.c\" needs "
               "interlocks (an empty list if none applies)"}));
}

// --- CF-705, CF-706, CF-714: refresh -----------------------------------

TEST(OutputRules, Refresh) {
    EXPECT_EQ(
        errors_with_interlock(
            output("ps1.a", "single", "ps1.m1.c1", ", refresh: 10s") +
            output("ps1.b", "setpoint", "ps1.m1.sp",
                   ", refresh: 10s, sbo: true") +
            output("ps1.c", "setpoint", "ps1.m1.nvm", ", refresh: 10s") +
            output("ps1.d", "setpoint", "ps1.m1.danger", ", refresh: 10s") +
            output("ps1.e", "setpoint", "ps1.m1.sp2", ", refresh: 10s")),
        (Lines{
            "outputs/ps1.yaml:4:132: error CF-714: output \"ps1.a\": refresh "
            "is only for setpoint outputs",
            "outputs/ps1.yaml:5:134: error CF-714: output \"ps1.b\": refresh "
            "cannot be used with sbo",
            "outputs/ps1.yaml:6:135: error CF-706: output \"ps1.c\" writes "
            "non-volatile memory and cannot be written cyclically",
            "outputs/ps1.yaml:7:138: error CF-705: output \"ps1.d\" is "
            "dangerous: it cannot be refreshed"}));
}

// --- CF-705, CF-719, CF-722, CF-723, CF-714: safe states -------------

// A dangerous output holds in every key; a step, pulse or trigger has no
// safe action; release is for setpoints with limits.release; safe needs a
// safe_state other than hold.
TEST(OutputRules, SafeStatesAndFailSafeKeys) {
    EXPECT_EQ(
        errors_with_interlock(
            "  - { name: ps1.a, area: ps1, kind: single, target: "
            "ps1.m1.danger, no_feedback: true, interlocks: [], safe_state: "
            "off, on_shutdown: safe, on_control_loss: { action: safe }, "
            "on_generation_change: safe }\n" +
            output("ps1.b", "step", "ps1.m1.sp", ", on_shutdown: safe") +
            output("ps1.c", "pulse", "ps1.m1.c1",
                   ", on_control_loss: { action: safe, after: 1s }") +
            "  - { name: ps1.d, area: ps1, kind: trigger, target: ps1.m1.c2, "
            "no_feedback: true, interlocks: [], safe_state: on }\n" +
            output("ps1.e", "single", "ps1.m1.c3", ", on_shutdown: release") +
            output("ps1.f", "setpoint", "ps1.m1.sp2",
                   ", on_control_loss: { action: release }") +
            output("ps1.g", "setpoint", "ps1.m1.nvm",
                   ", on_shutdown: release, limits: { release: { raw: "
                   "0xFFFF } }") +
            output("ps1.h", "single", "ps1.io.lamp",
                   ", on_shutdown: safe, on_generation_change: safe")),
        (Lines{
            "outputs/ps1.yaml:4:115: error CF-705: output \"ps1.a\" is "
            "dangerous: its safe_state must be hold",
            "outputs/ps1.yaml:4:133: error CF-705: output \"ps1.a\" is "
            "dangerous: its on_shutdown must be hold",
            "outputs/ps1.yaml:4:166: error CF-705: output \"ps1.a\" is "
            "dangerous: its on_control_loss must be hold",
            "outputs/ps1.yaml:4:196: error CF-705: output \"ps1.a\" is "
            "dangerous: its on_generation_change must be hold",
            "outputs/ps1.yaml:5:134: error CF-719: output \"ps1.b\": a step "
            "output has no safe action; use hold",
            "outputs/ps1.yaml:6:149: error CF-719: output \"ps1.c\": a pulse "
            "output has no safe action; use hold",
            "outputs/ps1.yaml:7:112: error CF-719: output \"ps1.d\": a "
            "trigger output has no safe action; use hold",
            "outputs/ps1.yaml:8:136: error CF-723: output \"ps1.e\": release "
            "is only for setpoints",
            "outputs/ps1.yaml:9:153: error CF-714: output \"ps1.f\": release "
            "in on_control_loss needs limits.release",
            "outputs/ps1.yaml:11:138: error CF-722: output \"ps1.h\": action "
            "safe needs a safe_state other than hold",
            "outputs/ps1.yaml:11:166: error CF-722: output \"ps1.h\": action "
            "safe needs a safe_state other than hold"}));
}

// --- CF-720: a pulse timed by the unit -----------------------------------

TEST(OutputRules, APulseTimedByTheUnit) {
    EXPECT_EQ(
        errors_with_interlock(
            output("ps1.a", "pulse", "ps1.m1.c1", ", duration: 1s")),
        Lines{"outputs/ps1.yaml:4:132: error CF-720: output \"ps1.a\": the "
              "device must time the pulse"});
}

// --- CW-712 and CW-721: warnings ---------------------------------------

TEST(OutputRules, EmptyInterlocksAndHoldingOnControlLoss) {
    EXPECT_EQ(
        warnings_of(
            "  - { name: ps1.a, area: ps1, kind: single, target: ps1.m1.c1, "
            "no_feedback: true, interlocks: [], safe_state: off }\n"
            "  - { name: ps1.b, area: ps1, kind: double, target: ps1.m1.c2, "
            "no_feedback: true, interlocks: { on: [], off: [] }, safe_state: "
            "on, on_control_loss: { action: hold } }\n"
            // Not a remote output: `io`.
            "  - { name: ps1.c, area: ps1, kind: single, target: ps1.io.lamp, "
            "no_feedback: true, interlocks: [], safe_state: off, "
            "on_control_loss: { action: hold } }\n"
            // A setpoint is no switching output.
            "  - { name: ps1.d, area: ps1, kind: setpoint, target: ps1.m1.sp, "
            "no_feedback: true, safe_state: { value: 0 }, on_control_loss: { "
            "action: hold } }\n"),
        (Lines{"outputs/ps1.yaml:2:95: warning CW-712: output \"ps1.a\" has no "
               "interlock",
               "outputs/ps1.yaml:3:95: warning CW-712: output \"ps1.b\" has no "
               "interlock",
               "outputs/ps1.yaml:3:149: warning CW-721: output \"ps1.b\" holds "
               "on control loss although its safe_state is on",
               "outputs/ps1.yaml:4:97: warning CW-712: output \"ps1.c\" has no "
               "interlock"}));
}

// --- CW-701 and CF-718: areas ------------------------------------------

TEST(OutputRules, AreasAndTheirPanels) {
    const std::string outputs =
        "  - { name: ps1.a, area: ps1, kind: setpoint, target: ps1.m1.sp, "
        "no_feedback: true, safe_state: hold }\n";
    EXPECT_EQ(
        errors_of(outputs,
                  {{"areas.yaml",
                    "areas:\n"
                    "  ps1: { description: Station }\n"
                    "  ps2: { authority: { local_blocks_auto: true } }\n"
                    "  ps3: { authority: { local_panel: { input: ps1.m1.ro, "
                    "active: true } } }\n"
                    "  ps4: { authority: { local_panel: { input: ps1.m1.c3, "
                    "active: true } } }\n"
                    "  ps5: { authority: { local_panel: { input: site.local, "
                    "active: true } } }\n"
                    "  ps6: { authority: { local_panel: { input: ps1.m1.state, "
                    "active: true }, local_blocks_auto: true } }\n"
                    "  ps7: { authority: { local_panel: { input: ps1.m1.run, "
                    "active: false } } }\n"},
                   {"tags/site.yaml",
                    "tags:\n  - { name: site.local, type: internal, "
                    "value_type: bool }\n"}}),
        (Lines{"areas.yaml:3:42: error CF-718: area \"ps2\": local_blocks_auto "
               "needs local_panel",
               "areas.yaml:4:45: error CF-718: area \"ps3\": local_panel.input "
               "\"ps1.m1.ro\" is not a BOOL tag read by a driver",
               "areas.yaml:5:45: error CF-718: area \"ps4\": local_panel.input "
               "\"ps1.m1.c3\" is not a BOOL tag read by a driver",
               "areas.yaml:6:45: error CF-718: area \"ps5\": local_panel.input "
               "\"site.local\" is not a BOOL tag read by a driver"}));
    EXPECT_EQ(
        warnings_of(outputs, {{"areas.yaml",
                               "areas:\n  ps1: { description: Station }\n"
                               "  ps2: { description: Spare }\n"}}),
        Lines{"areas.yaml:3:3: warning CW-701: area \"ps2\" is not used"});
}

// NOLINTEND(bugprone-suspicious-missing-comma)

}  // namespace
}  // namespace scada::conf
