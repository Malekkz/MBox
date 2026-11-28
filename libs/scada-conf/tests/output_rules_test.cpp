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
    "  - { name: dsp, access: rw, storage: volatile, dangerous: true, "
    "address: { table: holding, register: 21, type: uint16 } }\n"
    "  - { name: tenth2, access: rw, storage: volatile, scale: { divide_by: "
    "10 }, address: { table: holding, register: 22, type: uint16 } }\n"
    "  - { name: text, access: rw, storage: volatile, address: { table: "
    "holding, register: 30, type: string, length: 4 } }\n"
    "  - { name: state, address: { table: coil, register: 40 } }\n"
    "  - { name: brk, kind: double_point, access: wo, storage: volatile, "
    "address: { write: { on: { table: holding, register: 60, value: 1 }, "
    "off: { table: holding, register: 61, value: 1 } } } }\n"
    "  - { name: tenth, access: rw, storage: volatile, scale: { divide_by: "
    "10 }, address: { table: holding, register: 62, type: uint16 } }\n"
    "  - { name: reset, access: wo, storage: volatile, address: { table: "
    "holding, register: 63, write: { mask: 0x0001 } } }\n"
    "  - { name: big, access: rw, storage: volatile, value_type: float64, "
    "address: { table: holding, register: 64, type: float64 } }\n"
    "  - { name: m10k, access: rw, storage: volatile, address: { table: "
    "holding, register: 70, type: mod10k, count: 2 } }\n"
    "  - { name: flag, access: rw, storage: volatile, address: { table: "
    "holding, register: 72, bit: 3 } }\n"
    "  - { name: din, access: rw, storage: volatile, address: { table: "
    "discrete, register: 73 } }\n"
    "  - { name: label, access: wo, storage: volatile, address: { table: "
    "holding, register: 80, type: string, length: 2, write: { mask: 0x0001 "
    "} } }\n"
    // M4 to M6 of the review of #185: a point written as another raw type,
    // two with a write encoding a setpoint can carry, and an enumeration.
    "  - { name: signed, access: rw, storage: volatile, address: { table: "
    "holding, register: 90, type: uint16, write: { type: int16 } } }\n"
    "  - { name: coded, access: rw, storage: volatile, address: { table: "
    "holding, register: 91, type: uint16, write: { values: { 0: 1, 1: 2 } "
    "} } }\n"
    "  - { name: fixed, access: wo, storage: volatile, address: { table: "
    "holding, register: 92, type: uint16, write: { value: 1 } } }\n"
    "  - { name: narrow, access: rw, storage: volatile, address: { table: "
    "holding, register: 100, type: uint32, write: { type: uint16 } } }\n"
    "  - { name: mode, access: rw, storage: volatile, enum: { 0: stop, 1: "
    "run }, address: { table: holding, register: 93, type: uint16 } }\n"
    "  - { name: brk2, kind: double_point, access: wo, storage: volatile, "
    "address: { write: { on: { table: holding, register: 94, value: 1 }, "
    "off: { table: holding, register: 95, value: 1 } } } }\n"
    "  - { name: brk3, kind: double_point, access: wo, storage: volatile, "
    "address: { write: { on: { table: holding, register: 96, value: 1 }, "
    "off: { table: holding, register: 97, value: 1 } } } }\n"
    "  - name: status\n"
    "    address: { table: holding, register: 50, type: uint16 }\n"
    "    tag: false\n"
    "    bits: { 0: run }\n"
    "bit_order: lsb0\n";

constexpr std::string_view IO_TEMPLATE =
    "template: io_a\nprotocol: io\npoints:\n"
    "  - { name: lamp, access: rw, storage: volatile, address: { line: 1, "
    "direction: output, at_start: safe } }\n"
    "  - { name: held, access: rw, storage: volatile, address: { line: 2, "
    "direction: output, at_start: hold } }\n"
    "  - { name: button, access: rw, storage: volatile, address: { line: 3, "
    "direction: input, at_start: hold } }\n"
    "  - { name: led, access: rw, storage: volatile, address: { line: 4, "
    "direction: output, at_start: hold } }\n"
    // M2 of the review of #185: more lines that start safe.
    "  - { name: lamp2, access: rw, storage: volatile, address: { line: 5, "
    "direction: output, at_start: safe } }\n"
    "  - { name: horn, access: rw, storage: volatile, dangerous: true, "
    "address: { line: 6, direction: output, at_start: safe } }\n"
    "  - { name: horn2, access: rw, storage: volatile, dangerous: true, "
    "address: { line: 7, direction: output, at_start: safe } }\n"
    "  - { name: level, access: rw, storage: volatile, address: { line: 8, "
    "direction: output, at_start: hold } }\n";

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
        errors_with_interlock(
            output("ps1.a", "setpoint", "ps1.m1.ro") +
                output("ps1.b", "single", "ps1.m1.run") +
                output("ps1.c", "setpoint", "ps1.m1.text") +
                output("ps1.d", "setpoint", "site.t") +
                output("ps1.e", "setpoint", "ps1.m1.sp") +
                output("ps1.f", "setpoint", "ps1.m1.sp") +
                output("ps1.g", "single", "ps1.m1.c3") +
                "  - { name: ps1.h, area: ps1, kind: single, target: "
                "ps1.io.held, "
                "no_feedback: true, interlocks: [ps1.ok], safe_state: off }\n",
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
            // A dangerous output names its safe_state too (7.2; M3 of the
            // review of #185).
            "  - { name: ps1.d, area: ps1, kind: single, target: "
            "ps1.m1.danger, feedback: ps1.m1.state, interlocks: [] }\n"
            "  - { name: ps1.e, area: ps1, kind: double, target: ps1.m1.brk, "
            "feedback: { on: ps1.m1.state, off: ps1.m1.run }, interlocks: { "
            "on: [ps1.ok], off: [] }, safe_state: off }\n"),
        (Lines{"outputs/ps1.yaml:4:13: error CF-709: output \"ps1.a\" needs "
               "feedback or no_feedback: true",
               "outputs/ps1.yaml:5:13: error CF-710: output \"ps1.b\" needs a "
               "safe_state",
               "outputs/ps1.yaml:6:13: error CF-711: output \"ps1.c\" needs "
               "interlocks (an empty list if none applies)",
               "outputs/ps1.yaml:7:13: error CF-710: output \"ps1.d\" needs a "
               "safe_state"}));
}

// --- CF-705, CF-706, CF-714: refresh -----------------------------------

TEST(OutputRules, Refresh) {
    EXPECT_EQ(
        errors_with_interlock(
            output("ps1.a", "single", "ps1.m1.c1", ", refresh: 10s") +
            output("ps1.b", "setpoint", "ps1.m1.sp",
                   ", refresh: 10s, sbo: true") +
            output("ps1.c", "setpoint", "ps1.m1.nvm", ", refresh: 10s") +
            output("ps1.d", "setpoint", "ps1.m1.dsp", ", refresh: 10s") +
            output("ps1.e", "setpoint", "ps1.m1.sp2", ", refresh: 10s")),
        (Lines{
            "outputs/ps1.yaml:4:132: error CF-714: output \"ps1.a\": refresh "
            "is only for setpoint outputs",
            "outputs/ps1.yaml:5:134: error CF-714: output \"ps1.b\": refresh "
            "cannot be used with sbo",
            "outputs/ps1.yaml:6:135: error CF-706: output \"ps1.c\" writes "
            "non-volatile memory and cannot be written cyclically",
            "outputs/ps1.yaml:7:135: error CF-705: output \"ps1.d\" is "
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
            "outputs/ps1.yaml:11:119: error CF-722: output \"ps1.h\": action "
            "safe needs a safe_state other than hold (safe is the default of "
            "on_control_loss for an io target)",
            "outputs/ps1.yaml:11:119: error CF-722: output \"ps1.h\": action "
            "safe needs a safe_state other than hold (the target's at_start "
            "is safe)",
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
            "  - { name: ps1.b, area: ps1, kind: double, target: ps1.m1.brk, "
            "no_feedback: true, interlocks: { on: [], off: [] }, safe_state: "
            "on, on_control_loss: { action: hold } }\n"
            // Not a remote output: `io`.
            "  - { name: ps1.c, area: ps1, kind: single, target: ps1.io.lamp, "
            "no_feedback: true, interlocks: [], safe_state: off, "
            "on_control_loss: { action: hold }, on_shutdown: hold }\n"
            // A setpoint is no switching output.
            "  - { name: ps1.d, area: ps1, kind: setpoint, target: ps1.m1.sp, "
            "no_feedback: true, safe_state: { value: 0 }, on_control_loss: { "
            "action: hold } }\n"),
        (Lines{"outputs/ps1.yaml:2:95: warning CW-712: output \"ps1.a\" has no "
               "interlock",
               "outputs/ps1.yaml:3:96: warning CW-712: output \"ps1.b\" has no "
               "interlock",
               "outputs/ps1.yaml:3:150: warning CW-721: output \"ps1.b\" holds "
               "on control loss although its safe_state is on",
               "outputs/ps1.yaml:4:97: warning CW-712: output \"ps1.c\" has no "
               "interlock",
               "outputs/ps1.yaml:4:135: warning CW-721: output \"ps1.c\" holds "
               "on control loss although its safe_state is off",
               "outputs/ps1.yaml:4:166: warning CW-721: output \"ps1.c\" holds "
               "on shutdown although its safe_state is off"}));
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

// --- The rulings of D-124 -----------------------------------------------

// CF-708 (ruling Q3): the shapes no write carries; a bit with
// `write.mask` is written.
TEST(OutputRules, ShapesTheProtocolCannotWrite) {
    EXPECT_EQ(
        errors_with_interlock(
            output("ps1.a", "setpoint", "ps1.m1.m10k") +
            output("ps1.b", "single", "ps1.m1.flag") +
            output("ps1.c", "single", "ps1.m1.din") +
            "  - { name: ps1.d, area: ps1, kind: single, target: "
            "ps1.io.button, "
            "no_feedback: true, interlocks: [ps1.ok], safe_state: off }\n" +
            output("ps1.e", "single", "ps1.m1.reset")),
        (Lines{"outputs/ps1.yaml:4:55: error CF-708: output \"ps1.a\": the "
               "protocol cannot write raw type mod10k",
               "outputs/ps1.yaml:5:53: error CF-708: output \"ps1.b\": the "
               "protocol cannot write a bit or field without write.mask",
               "outputs/ps1.yaml:6:53: error CF-708: output \"ps1.c\": the "
               "protocol cannot write table discrete without write.on or "
               "write.off",
               "outputs/ps1.yaml:7:53: error CF-708: output \"ps1.d\": the "
               "protocol cannot write an input line"}));
}

// CF-702 (interim rule (g)): the kind fits the target's type.
// A STRING target is no target, even with a write encoding that would
// fit a switching kind.
TEST(OutputRules, AStringTargetWithAMask) {
    EXPECT_EQ(errors_with_interlock(output("ps1.a", "single", "ps1.m1.label")),
              Lines{"outputs/ps1.yaml:4:53: error CF-702: output \"ps1.a\": "
                    "target \"ps1.m1.label\" is not a writable point"});
}

TEST(OutputRules, KindAgainstTargetType) {
    EXPECT_EQ(
        errors_with_interlock(output("ps1.a", "single", "ps1.m1.sp") +
                              output("ps1.b", "setpoint", "ps1.m1.c1") +
                              output("ps1.c", "double", "ps1.m1.c2") +
                              output("ps1.d", "step", "ps1.m1.sp2") +
                              output("ps1.e", "double", "ps1.m1.brk")),
        (Lines{"outputs/ps1.yaml:4:53: error CF-702: output \"ps1.a\": target "
               "\"ps1.m1.sp\" is not a writable point",
               "outputs/ps1.yaml:5:55: error CF-702: output \"ps1.b\": target "
               "\"ps1.m1.c1\" is not a writable point",
               "outputs/ps1.yaml:6:53: error CF-702: output \"ps1.c\": target "
               "\"ps1.m1.c2\" is not a writable point"}));
}

// CF-705 (ruling Q2): a dangerous output always asks for a reason and a
// re-authentication.
TEST(OutputRules, ADangerousOutputAsksForAReason) {
    EXPECT_EQ(
        errors_with_interlock(
            output("ps1.a", "single", "ps1.m1.danger",
                   ", reason_required: false, reauth: false") +
            output("ps1.b", "single", "ps1.m1.c1", ", reason_required: false")),
        (Lines{"outputs/ps1.yaml:4:144: error CF-705: output \"ps1.a\" is "
               "dangerous: its reason_required must be true",
               "outputs/ps1.yaml:4:159: error CF-705: output \"ps1.a\" is "
               "dangerous: its reauth must be true"}));
}

// CF-707 (ruling Q4): an on-only output has no off value.
TEST(OutputRules, OnOnlyOutputs) {
    EXPECT_EQ(
        errors_with_interlock(
            "  - { name: ps1.a, area: ps1, kind: single, target: ps1.m1.c1, "
            "no_feedback: true, interlocks: [], on_only: true, safe_state: "
            "off }\n"
            "  - { name: ps1.b, area: ps1, kind: trigger, target: ps1.m1.c2, "
            "no_feedback: true, interlocks: [], on_only: false, safe_state: "
            "hold }\n"
            "  - { name: ps1.c, area: ps1, kind: single, target: "
            "ps1.m1.reset, no_feedback: true, interlocks: [], on_only: false, "
            "safe_state: hold }\n"
            "  - { name: ps1.d, area: ps1, kind: single, target: ps1.m1.c3, "
            "no_feedback: true, interlocks: [], on_only: true, safe_state: "
            "on }\n"),
        (Lines{"outputs/ps1.yaml:4:126: error CF-707: output \"ps1.a\" is "
               "on-only",
               "outputs/ps1.yaml:5:109: error CF-707: output \"ps1.b\" is "
               "on-only",
               "outputs/ps1.yaml:6:111: error CF-707: output \"ps1.c\" is "
               "on-only"}));
}

// CF-715 (ruling Q5, interim rules (b) and (i)): the feedback of each
// kind.
TEST(OutputRules, FeedbackOfEachKind) {
    EXPECT_EQ(
        errors_with_interlock(
            "  - { name: ps1.a, area: ps1, kind: single, target: ps1.m1.c1, "
            "feedback: ps1.m1.ro, interlocks: [], safe_state: off }\n"
            "  - { name: ps1.b, area: ps1, kind: double, target: ps1.m1.brk, "
            "feedback: ps1.m1.state, interlocks: [], safe_state: hold }\n"
            "  - { name: ps1.c, area: ps1, kind: setpoint, target: ps1.m1.sp, "
            "feedback: ps1.m1.ro, safe_state: hold }\n"
            "  - { name: ps1.d, area: ps1, kind: step, target: ps1.m1.sp2, "
            "feedback: ps1.m1.ro, interlocks: [], safe_state: hold }\n"
            "  - { name: ps1.e, area: ps1, kind: setpoint, target: ps1.m1.big, "
            "feedback: { tag: ps1.m1.big, within: 0, settle: 1s }, "
            "safe_state: hold }\n"
            "  - { name: ps1.f, area: ps1, kind: setpoint, target: "
            "ps1.m1.tenth, feedback: { tag: ps1.m1.tenth, within: 0.5, "
            "settle: 1s }, safe_state: hold }\n"
            "  - { name: ps1.g, area: ps1, kind: single, target: ps1.m1.c2, "
            "feedback: { on: ps1.m1.state, off: ps1.m1.ro }, interlocks: [], "
            "safe_state: off }\n"),
        (Lines{
            "outputs/ps1.yaml:4:74: error CF-715: output \"ps1.a\": feedback "
            "must be a BOOL tag or { on, off }",
            "outputs/ps1.yaml:5:75: error CF-715: output \"ps1.b\": feedback "
            "must be a double point or { on, off }",
            "outputs/ps1.yaml:6:76: error CF-715: output \"ps1.c\": feedback "
            "must be { tag, within, settle } on a number tag",
            "outputs/ps1.yaml:7:73: error CF-715: output \"ps1.d\": feedback "
            "must be none: a step output has no feedback yet",
            "outputs/ps1.yaml:8:77: error CF-715: output \"ps1.e\": feedback "
            "must be { tag, within, settle } with a within above 0 on a "
            "FLOAT64 tag",
            "outputs/ps1.yaml:10:74: error CF-715: output \"ps1.g\": "
            "feedback must be { on, off } of two BOOL tags"}));
}

// CF-713 (ruling Q8, interim rules (d), (e) and (f)): the values an
// output writes fit its target, by mathematical value.
TEST(OutputRules, ValuesFitTheTarget) {
    EXPECT_EQ(
        errors_with_interlock(
            "  - { name: ps1.a, area: ps1, kind: setpoint, target: ps1.m1.sp, "
            "no_feedback: true, safe_state: { value: 1.5 }, limits: { min: "
            "-1, max: 9223372036854775808 } }\n"
            "  - { name: ps1.b, area: ps1, kind: setpoint, target: "
            "ps1.m1.sp2, no_feedback: true, safe_state: { value: 70000 } }\n"
            "  - { name: ps1.c, area: ps1, kind: setpoint, target: "
            "ps1.m1.tenth, no_feedback: true, safe_state: { value: 7000 }, "
            "limits: { release: { raw: 0x1FFFF } }, on_shutdown: release }\n"
            "  - { name: ps1.d, area: ps1, kind: setpoint, target: "
            "ps1.m1.nvm, no_feedback: true, safe_state: { value: 50 }, "
            "limits: { min: 0, max: 40 } }\n"
            "  - { name: ps1.e, area: ps1, kind: single, target: "
            "ps1.m1.reset, no_feedback: true, interlocks: [], safe_state: { "
            "raw: 1 } }\n"
            "  - { name: ps1.f, area: ps1, kind: setpoint, target: ps1.m1.big, "
            "no_feedback: true, safe_state: { value: 9007199254740993 }, "
            "limits: { ranges: [[0, 9007199254740992]] } }\n"
            "  - { name: ps1.g, area: ps1, kind: setpoint, target: "
            "ps1.m1.tenth2, no_feedback: true, safe_state: { value: 6553.5 }, "
            "limits: { min: 0, max: 6553.5 } }\n"),
        (Lines{"outputs/ps1.yaml:4:106: error CF-713: output \"ps1.a\": "
               "safe_state.value does not fit the target's type",
               "outputs/ps1.yaml:4:137: error CF-713: output \"ps1.a\": "
               "limits.max does not fit the target's type",
               "outputs/ps1.yaml:5:107: error CF-713: output \"ps1.b\": "
               "safe_state.value does not fit the target's type",
               "outputs/ps1.yaml:6:109: error CF-713: output \"ps1.c\": "
               "safe_state.value does not fit the target's type",
               "outputs/ps1.yaml:6:143: error CF-713: output \"ps1.c\": "
               "limits.release.raw does not fit the target's type",
               "outputs/ps1.yaml:7:107: error CF-713: output \"ps1.d\": "
               "safe_state.value does not fit the target's type",
               "outputs/ps1.yaml:8:114: error CF-713: output \"ps1.e\": "
               "safe_state does not fit the target's type",
               "outputs/ps1.yaml:9:107: error CF-713: output \"ps1.f\": "
               "safe_state.value does not fit the target's type"}));
}

// M1 of the review of #185: each kind takes its own safe_state forms,
// CF-713 once at the safe_state, and a pulse's CF-719 alone (S3).
TEST(OutputRules, SafeStateFormsPerKind) {
    EXPECT_EQ(
        errors_with_interlock(
            "  - { name: ps1.a, area: ps1, kind: setpoint, target: ps1.m1.sp, "
            "no_feedback: true, safe_state: off, on_shutdown: safe }\n"
            "  - { name: ps1.b, area: ps1, kind: double, target: ps1.m1.brk, "
            "no_feedback: true, interlocks: [], safe_state: { value: 7 } }\n"
            "  - { name: ps1.c, area: ps1, kind: double, target: ps1.m1.brk2, "
            "no_feedback: true, interlocks: [], safe_state: { value: 2 } }\n"
            "  - { name: ps1.d, area: ps1, kind: single, target: ps1.m1.c1, "
            "no_feedback: true, interlocks: [], safe_state: { value: true } "
            "}\n"
            "  - { name: ps1.e, area: ps1, kind: single, target: ps1.m1.c2, "
            "no_feedback: true, interlocks: [], safe_state: { raw: 1 } }\n"
            "  - { name: ps1.f, area: ps1, kind: pulse, target: ps1.m1.c3, "
            "no_feedback: true, interlocks: [], safe_state: { value: true } "
            "}\n"
            "  - { name: ps1.g, area: ps1, kind: setpoint, target: "
            "ps1.m1.sp2, no_feedback: true, safe_state: { value: 3 } }\n"
            "  - { name: ps1.h, area: ps1, kind: double, target: ps1.m1.brk3, "
            "no_feedback: true, interlocks: [], safe_state: on }\n"),
        (Lines{"outputs/ps1.yaml:4:97: error CF-713: output \"ps1.a\": "
               "safe_state does not fit the target's type",
               "outputs/ps1.yaml:5:112: error CF-713: output \"ps1.b\": "
               "safe_state does not fit the target's type",
               "outputs/ps1.yaml:6:113: error CF-713: output \"ps1.c\": "
               "safe_state does not fit the target's type",
               "outputs/ps1.yaml:7:111: error CF-713: output \"ps1.d\": "
               "safe_state does not fit the target's type",
               "outputs/ps1.yaml:8:111: error CF-713: output \"ps1.e\": "
               "safe_state does not fit the target's type",
               "outputs/ps1.yaml:9:110: error CF-719: output \"ps1.f\": a "
               "pulse output has no safe action; use hold"}));
}

// Interim rule (c) on every output (M2 of the review of #185): a pulse, a
// trigger and a dangerous output that hold cannot target a line that starts
// safe; a dangerous one without safe_state is CF-710 alone (M3).
TEST(OutputRules, HoldOnALineThatStartsSafe) {
    EXPECT_EQ(
        errors_with_interlock(
            "  - { name: ps1.a, area: ps1, kind: pulse, target: ps1.io.lamp, "
            "no_feedback: true, interlocks: [], safe_state: hold }\n"
            "  - { name: ps1.b, area: ps1, kind: trigger, target: "
            "ps1.io.lamp2, no_feedback: true, interlocks: [], safe_state: hold "
            "}\n"
            "  - { name: ps1.c, area: ps1, kind: single, target: ps1.io.horn, "
            "no_feedback: true, interlocks: [], safe_state: hold }\n"
            "  - { name: ps1.d, area: ps1, kind: single, target: "
            "ps1.io.horn2, no_feedback: true, interlocks: [] }\n"),
        (Lines{"outputs/ps1.yaml:4:112: error CF-722: output \"ps1.a\": "
               "action safe needs a safe_state other than hold (the target's "
               "at_start is safe)",
               "outputs/ps1.yaml:5:115: error CF-722: output \"ps1.b\": "
               "action safe needs a safe_state other than hold (the target's "
               "at_start is safe)",
               "outputs/ps1.yaml:6:113: error CF-722: output \"ps1.c\": "
               "action safe needs a safe_state other than hold (the target's "
               "at_start is safe)",
               "outputs/ps1.yaml:7:13: error CF-710: output \"ps1.d\" needs a "
               "safe_state"}));
}

// Interim rule (j) (S1 of the review of #185): a written safe action on
// control loss says when; S3: a setpoint on a BOOL line is CF-702 alone.
TEST(OutputRules, ControlLossSafeNeedsAfter) {
    EXPECT_EQ(
        errors_with_interlock(
            "  - { name: ps1.a, area: ps1, kind: single, target: ps1.m1.c1, "
            "no_feedback: true, interlocks: [], safe_state: off, "
            "on_control_loss: { action: safe } }\n"
            "  - { name: ps1.b, area: ps1, kind: single, target: ps1.m1.c2, "
            "no_feedback: true, interlocks: [], safe_state: off, "
            "on_control_loss: { action: safe, after: 5s } }\n"
            "  - { name: ps1.c, area: ps1, kind: setpoint, target: "
            "ps1.io.level, no_feedback: true, safe_state: { value: 1 }, "
            "limits: { min: 0, max: 1 } }\n"),
        (Lines{"outputs/ps1.yaml:4:143: error CF-714: output \"ps1.a\": "
               "on_control_loss with action safe needs after",
               "outputs/ps1.yaml:6:55: error CF-702: output \"ps1.c\": target "
               "\"ps1.io.level\" is not a writable point"}));
}

// S2 of the review of #185: each bound of `limits.ranges` is a value of
// the target's type, and a safe value outside every range is refused
// although it is within min and max.
TEST(OutputRules, TheRangesOfTheLimits) {
    EXPECT_EQ(
        errors_with_interlock(
            "  - { name: ps1.a, area: ps1, kind: setpoint, target: ps1.m1.sp, "
            "no_feedback: true, safe_state: hold, limits: { ranges: [[0, "
            "1.5]] } }\n"
            "  - { name: ps1.b, area: ps1, kind: setpoint, target: "
            "ps1.m1.sp2, no_feedback: true, safe_state: { value: 5 }, limits: "
            "{ min: 0, max: 10, ranges: [[0, 2], [7, 10]] } }\n"
            "  - { name: ps1.c, area: ps1, kind: setpoint, target: "
            "ps1.m1.tenth2, no_feedback: true, safe_state: { value: 8 }, "
            "limits: { min: 0, max: 10, ranges: [[0, 2], [7, 10]] } }\n"),
        (Lines{"outputs/ps1.yaml:4:126: error CF-713: output \"ps1.a\": "
               "limits.ranges[0] does not fit the target's type",
               "outputs/ps1.yaml:5:107: error CF-713: output \"ps1.b\": "
               "safe_state.value does not fit the target's type"}));
}

// M4 to M6 of the review of #185: a value is compared with the raw type
// written, `{ raw }` is refused with any write encoding, and an
// enumeration's codes are the values its target takes.
TEST(OutputRules, ValuesAgainstTheWrite) {
    EXPECT_EQ(
        errors_with_interlock(
            "  - { name: ps1.a, area: ps1, kind: setpoint, target: "
            "ps1.m1.signed, no_feedback: true, safe_state: { value: 40000 } "
            "}\n"
            "  - { name: ps1.b, area: ps1, kind: setpoint, target: "
            "ps1.m1.coded, no_feedback: true, safe_state: { raw: 1 } }\n"
            "  - { name: ps1.c, area: ps1, kind: setpoint, target: "
            "ps1.m1.fixed, no_feedback: true, safe_state: { raw: 1 } }\n"
            "  - { name: ps1.d, area: ps1, kind: setpoint, target: "
            "ps1.m1.mode, no_feedback: true, safe_state: { value: 5 }, "
            "limits: { release: { value: 1 } } }\n"
            "  - { name: ps1.e, area: ps1, kind: setpoint, target: "
            "ps1.m1.sp, no_feedback: true, safe_state: { raw: 40000 } }\n"
            "  - { name: ps1.f, area: ps1, kind: setpoint, target: "
            "ps1.m1.narrow, no_feedback: true, safe_state: { raw: 70000 } }\n"),
        (Lines{"outputs/ps1.yaml:4:110: error CF-713: output \"ps1.a\": "
               "safe_state.value does not fit the target's type",
               "outputs/ps1.yaml:5:107: error CF-713: output \"ps1.b\": "
               "safe_state.raw does not fit the target's type",
               "outputs/ps1.yaml:6:107: error CF-713: output \"ps1.c\": "
               "safe_state.raw does not fit the target's type",
               "outputs/ps1.yaml:7:108: error CF-713: output \"ps1.d\": "
               "safe_state.value does not fit the target's type",
               "outputs/ps1.yaml:9:108: error CF-713: output \"ps1.f\": "
               "safe_state.raw does not fit the target's type"}));
    // A uint16 point written as int16 takes -5, and its raw form too.
    EXPECT_EQ(errors_with_interlock(
                  "  - { name: ps1.a, area: ps1, kind: setpoint, target: "
                  "ps1.m1.signed, no_feedback: true, safe_state: { value: -5 "
                  "}, limits: { release: { raw: -5 } } }\n"),
              Lines{});
}

// M6 of the review of #185: a named enumeration resolves over the
// template's `enums`, then enums.yaml.
TEST(OutputRules, ValuesOfANamedEnumeration) {
    std::string meter(METER);
    meter.insert(meter.find("  - name: status"),
                 "  - { name: local, access: rw, storage: volatile, enum: "
                 "states, address: { table: holding, register: 98, type: "
                 "uint16 } }\n"
                 "  - { name: shared, access: rw, storage: volatile, enum: "
                 "modes, address: { table: holding, register: 99, type: "
                 "uint16 } }\n");
    meter += "enums:\n  states: { 0: off, 4: on }\n";
    const Files more{{"templates/meter_a.yaml", meter},
                     {"enums.yaml", "enums:\n  modes: { 1: auto, 2: hand }\n"}};
    EXPECT_EQ(
        errors_with_interlock(
            "  - { name: ps1.a, area: ps1, kind: setpoint, target: "
            "ps1.m1.local, no_feedback: true, safe_state: { value: 1 } }\n"
            "  - { name: ps1.b, area: ps1, kind: setpoint, target: "
            "ps1.m1.shared, no_feedback: true, safe_state: { value: 0 } }\n",
            more),
        (Lines{"outputs/ps1.yaml:4:109: error CF-713: output \"ps1.a\": "
               "safe_state.value does not fit the target's type",
               "outputs/ps1.yaml:5:110: error CF-713: output \"ps1.b\": "
               "safe_state.value does not fit the target's type"}));
    EXPECT_EQ(
        errors_with_interlock(
            "  - { name: ps1.a, area: ps1, kind: setpoint, target: "
            "ps1.m1.local, no_feedback: true, safe_state: { value: 4 } }\n"
            "  - { name: ps1.b, area: ps1, kind: setpoint, target: "
            "ps1.m1.shared, no_feedback: true, safe_state: { value: 0x2 } }\n",
            more),
        Lines{});
}

// CF-714 (interim rule (a)): no protocol of phase 2 has a select on the
// device.
TEST(OutputRules, NativeSelectWithoutADeviceSelect) {
    EXPECT_EQ(
        errors_with_interlock(output("ps1.a", "single", "ps1.m1.c1",
                                     ", native_select: at_operate") +
                              "  - { name: ps1.b, area: ps1, kind: "
                              "single, target: ps1.io.led, no_feedback: "
                              "true, interlocks: [], safe_state: off, "
                              "native_select: at_select }\n"),
        (Lines{"outputs/ps1.yaml:4:138: error CF-714: output \"ps1.a\": "
               "native_select needs a select on the device, which protocol "
               "modbus does not have",
               "outputs/ps1.yaml:5:132: error CF-714: output \"ps1.b\": "
               "native_select needs a select on the device, which protocol io "
               "does not have"}));
}

// CF-722 on the defaults (ruling Q7) and on `at_start: safe` (interim
// rule (c)); the kinds with no safe action and a dangerous output hold on
// every protocol by default.
TEST(OutputRules, TheDefaultsOfTheFailSafeKeys) {
    EXPECT_EQ(
        errors_with_interlock(
            output("ps1.a", "single", "ps1.io.held") +
            output("ps1.b", "single", "ps1.io.lamp",
                   ", on_shutdown: hold, on_control_loss: { action: hold }") +
            "  - { name: ps1.c, area: ps1, kind: pulse, target: ps1.io.led, "
            "no_feedback: true, interlocks: [], safe_state: hold }\n" +
            output("ps1.d", "single", "ps1.m1.c1")),
        (Lines{
            "outputs/ps1.yaml:4:119: error CF-722: output \"ps1.a\": action "
            "safe needs a safe_state other than hold (safe is the default of "
            "on_shutdown for an io target)",
            "outputs/ps1.yaml:4:119: error CF-722: output \"ps1.a\": action "
            "safe needs a safe_state other than hold (safe is the default of "
            "on_control_loss for an io target)",
            "outputs/ps1.yaml:5:119: error CF-722: output \"ps1.b\": action "
            "safe needs a safe_state other than hold (the target's at_start "
            "is safe)"}));
}

// NOLINTEND(bugprone-suspicious-missing-comma)

}  // namespace
}  // namespace scada::conf
