// The schema files of templates, channels and their protocols, tags,
// outputs, alarms and programs, against the examples of configuration
// schemas contract sections 4 to 9 and Annexes A and B.

#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/conf/schema.h"
#include "scada/conf/types.h"
#include "scada/conf/yaml.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scada::conf {
namespace {

using Lines = std::vector<std::string>;

Lines check(std::string_view schema, std::string_view yaml) {
    Diagnostics diagnostics;
    std::optional<Document> document = parse_yaml(yaml, "f.yaml", diagnostics);
    if (!document) {
        return {"parse error"};
    }
    ScalarTypes types(*document);
    SchemaSet::builtin().validate(*document, types, schema, "f.yaml",
                                  diagnostics);
    Report report;
    diagnostics.move_into(report);
    Lines lines;
    lines.reserve(report.errors.size());
    for (const Diagnostic& error : report.errors) {
        lines.push_back(std::to_string(error.location.line) + ":" +
                        std::to_string(error.location.column) + " " +
                        code_text(error) + ": " + error.message);
    }
    return lines;
}

constexpr std::string_view MODBUS_TEMPLATE =
    "modbus.schema.json#/$defs/template";
constexpr std::string_view MODBUS_CHANNEL =
    "modbus.schema.json#/$defs/channel_file";
constexpr std::string_view IO_CHANNEL = "io.schema.json#/$defs/channel_file";

TEST(SchemaExamples, TheTemplateOfSection4) {
    EXPECT_EQ(check(MODBUS_TEMPLATE,
                    "template: flow_meter_x\n"
                    "protocol: modbus\n"
                    "points:\n"
                    "  - name: flow\n"
                    "    address: { table: holding, register: 100, type: "
                    "float32, word_order: big }\n"
                    "    unit: m3/h\n"
                    "    scale: { gain: 1.0, offset: 0.0 }\n"
                    "    poll_group: normal\n"
                    "    max_age: 5s\n"
                    "    logging: { policy: process_default, interval: 1s, "
                    "raw_retention: 180d }\n"
                    "    alarms:\n"
                    "      - { type: high, limit: 450, deadband: 5, on_delay: "
                    "10s, priority: medium }\n"
                    "  - name: total\n"
                    "    address: { table: holding, register: 102, type: "
                    "uint32 }\n"
                    "    unit: m3\n"
                    "    poll_group: slow\n"
                    "    kind: counter\n"
                    "    logging: counters\n"),
              Lines{});
}

TEST(SchemaExamples, ParametersAndRepeatOfSection44) {
    EXPECT_EQ(check(MODBUS_TEMPLATE,
                    "template: inverter\n"
                    "protocol: modbus\n"
                    "params:\n"
                    "  inputs:    { type: int, min: 1, max: 32, default: 16 }\n"
                    "  has_meter: { type: bool, default: false }\n"
                    "  options:   { type: set, values: [battery, meter] }\n"
                    "points:\n"
                    "  - name: meter.power\n"
                    "    when: \"'meter' in options\"\n"
                    "    address: { table: holding, register: 300, type: "
                    "int32 }\n"
                    "repeat:\n"
                    "  - for: { n: \"1..inputs\" }\n"
                    "    points:\n"
                    "      - name: \"pv{n}.voltage\"\n"
                    "        address: { table: holding, register: \"if(n <= "
                    "24, 1000 + 2 * (n - 1), 1400 + 2 * (n - 25))\", type: "
                    "int16 }\n"
                    "        scale: { divide_by: 10 }\n"
                    "        unit: V\n"
                    "  - for: { n: [1, 2, \"5..8\"] }\n"
                    "    points: [ { name: \"t{n}\", address: { table: input, "
                    "register: \"n\" } } ]\n"),
              Lines{});
}

TEST(SchemaExamples, ValidityAndQualityOfSection47) {
    EXPECT_EQ(check(MODBUS_TEMPLATE,
                    "template: sensor\n"
                    "protocol: modbus\n"
                    "defaults:\n"
                    "  invalid_values: { uint16: [0xFFFF], float32: [nan] }\n"
                    "  address: { word_order: little }\n"
                    "points:\n"
                    "  - name: temp1\n"
                    "    address: { table: input, register: 40, type: int16 }\n"
                    "    scale: { divide_by: 10 }\n"
                    "    invalid_values: [ { raw: 0x7FFF, flags: "
                    "[OUT_OF_RANGE], reason: \"sensor open\" } ]\n"
                    "    quality_from: [ { point: status1, field: [8, 11], "
                    "map: { 0: good, 5: [bad, TEST], default: bad } } ]\n"
                    "  - name: status1\n"
                    "    address: { table: input, register: 41 }\n"
                    "    on_invalid: { substitute: 0 }\n"),
              Lines{});
}

TEST(SchemaExamples, BitsOfSection54) {
    EXPECT_EQ(check(MODBUS_TEMPLATE,
                    "template: relay\n"
                    "protocol: modbus\n"
                    "bit_order: lsb0\n"
                    "points:\n"
                    "  - name: alarm_word1\n"
                    "    address: { table: holding, register: 50, type: "
                    "uint16 }\n"
                    "    tag: false\n"
                    "    bits:\n"
                    "      0: { name: overtemp, alarms: [ { type: state, "
                    "alarm_on: true, priority: high } ] }\n"
                    "      3: { name: fan_fault }\n"
                    "      7: { name: door_closed, invert: true }\n"
                    "      8: spare_8\n"),
              Lines{});
}

TEST(SchemaExamples, AddressesOfAnnexA2) {
    EXPECT_EQ(
        check(MODBUS_TEMPLATE,
              "template: shapes\n"
              "protocol: modbus\n"
              "bit_order: msb0\n"
              "points:\n"
              "  - { name: a, address: { table: holding, register: 200, "
              "type: float32, word_order: little } }\n"
              "  - { name: b, address: { table: holding, register: 210, "
              "type: uint16, field: [4, 7] } }\n"
              "  - { name: c, address: { table: coil, register: 12 } }\n"
              "  - { name: d, address: { table: holding, parts: [ { register: "
              "31 }, { register: 30 } ], type: uint32 } }\n"
              "  - { name: e, address: { table: holding, register: 300, type: "
              "string, length: 16, encoding: ascii } }\n"
              "  - { name: f, address: { table: input, register: 40, type: "
              "epoch, width: 32, resolution: 1s } }\n"
              "  - name: g\n"
              "    kind: double_point\n"
              "    address: { on: { table: holding, register: 60, bit: 4 }, "
              "off: { table: holding, register: 60, bit: 5 } }\n"
              "  - { name: h, kind: double_point, states: { 0: 0, 1: 1, 2: 2, "
              "3: 3 }, address: { table: holding, register: 61, field: [0, 1] "
              "} }\n"
              "  - name: cb_ctrl\n"
              "    kind: double_point\n"
              "    access: wo\n"
              "    storage: volatile\n"
              "    address:\n"
              "      table: holding\n"
              "      register: 500\n"
              "      write: { function: 6, values: { 1: 0x0002, 2: 0x0001 } }\n"
              "  - { name: reset, access: wo, storage: volatile, address: { "
              "table: holding, register: 500, write: { function: 6, mask: "
              "0x0010 } } }\n"),
        Lines{});
}

TEST(SchemaExamples, AddressRefusals) {
    EXPECT_EQ(
        check(MODBUS_TEMPLATE,
              "template: t\nprotocol: modbus\npoints:\n"
              "  - { name: a, address: { table: holding, register: 1, "
              "parts: [ { register: 2 } ], typo: 1 } }\n"),
        (Lines{"4:56 CF-207: points[0].address.parts cannot be given "
               "with register",
               "4:84 CF-201: unknown key \"typo\" in points[0].address"}));
    EXPECT_EQ(check(MODBUS_TEMPLATE,
                    "template: t\nprotocol: modbus\npoints:\n"
                    "  - { name: a, address: { table: ram, write: { value: 1, "
                    "mask: 2 } } }\n"),
              (Lines{"4:34 CF-205: points[0].address.table must be one of "
                     "coil, discrete, input, holding",
                     "4:58 CF-207: points[0].address.write.mask cannot be "
                     "given with value"}));
}

TEST(SchemaExamples, TheChannelOfSection46) {
    EXPECT_EQ(check(MODBUS_CHANNEL,
                    "channel:\n"
                    "  name: line1\n"
                    "  type: rtu\n"
                    "  device: /dev/serial/by-path/platform-serial1\n"
                    "  baud: 19200\n"
                    "  parity: even\n"
                    "  poll_groups:\n"
                    "    fast:   { period: 200ms, priority: 1 }\n"
                    "    normal: { period: 1s,    priority: 2 }\n"
                    "    slow:   { period: 10s,   priority: 3 }\n"
                    "devices:\n"
                    "  - { name: ps1.fm1, template: flow_meter_x, unit_id: 11, "
                    "timeout: 300ms, retries: 2 }\n"
                    "  - { name: ps1.fm2, template: flow_meter_x, unit_id: 12, "
                    "timeout: 300ms, retries: 2 }\n"),
              Lines{});
}

TEST(SchemaExamples, InstancesWithOverrides) {
    EXPECT_EQ(check(MODBUS_CHANNEL,
                    "channel: { name: gw1, type: tcp, host: 10.0.0.5, port: "
                    "502, connect_timeout: 3s, lost_after_timeouts: 3 }\n"
                    "devices:\n"
                    "  - name: ps1.inv1\n"
                    "    template: inverter\n"
                    "    unit_id: 1\n"
                    "    params: { inputs: 20, options: [meter] }\n"
                    "    overrides: { ia: { address: { replace: true, table: "
                    "input, register: 3, type: int16 } } }\n"
                    "    only: [\"pv*.voltage\", meter.power]\n"
                    "    keepalive: 30s\n"
                    "    identify: [ { address: { table: holding, register: 9 "
                    "}, equals: 42 } ]\n"
                    "  - name: ps1.direct\n"
                    "    unit_id: 2\n"
                    "    points: [ { name: flow, address: { table: holding, "
                    "register: 1 } } ]\n"),
              Lines{});
}

TEST(SchemaExamples, ChannelRefusals) {
    EXPECT_EQ(
        check(MODBUS_CHANNEL,
              "channel: { name: gw1, type: tcp, host: gw.example.org, "
              "role: listen, rs485: {} }\n"
              "devices: [ { name: d, unit_id: 256, acquisition: report, "
              "disable: [a], only: [b], time_sync: { mode: periodic } } "
              "]\n"),
        (Lines{std::string("1:40 CF-210: channel.host: a host name is "
                           "reserved and not available in this version"),
               std::string("1:62 CF-210: channel.role: \"listen\" is reserved "
                           "and not available in this version"),
               std::string("1:70 CF-210: channel.rs485 is reserved and not "
                           "available in this version"),
               std::string("2:32 CF-204: devices[0].unit_id must be "
                           "between 0 and 255"),
               std::string("2:50 CF-210: devices[0].acquisition: \"report\" is "
                           "reserved and not available in this version"),
               std::string("2:72 CF-207: devices[0].only cannot be given "
                           "with disable"),
               std::string("2:102 CF-210: devices[0].time_sync.mode: "
                           "\"periodic\" is reserved and not available in "
                           "this version")}));
}

TEST(SchemaExamples, AnIoChannel) {
    EXPECT_EQ(check(IO_CHANNEL,
                    "channel: { name: local, backend: gpio, chip: pinctrl-a, "
                    "scan: auto }\n"
                    "devices:\n"
                    "  - name: ps1.io\n"
                    "    points:\n"
                    "      - { name: pump_run, access: rw, storage: volatile, "
                    "address: { line: RELAY1, direction: output, at_start: "
                    "hold } }\n"
                    "      - { name: level, address: { channel: in_voltage0, "
                    "filter: 500ms, clamp: [0, 5] }, scale: { gain: 0.01, "
                    "offset: 0 } }\n"),
              Lines{});
}

TEST(SchemaExamples, TheTagsOfSection51) {
    EXPECT_EQ(check("tags.schema.json",
                    "tags:\n"
                    "  - name: site.pv.total_power\n"
                    "    type: virtual\n"
                    "    expr: sum(\"ps1.inv*.power\")\n"
                    "    unit: kW\n"
                    "    task: normal\n"
                    "    quality: ignore_bad\n"
                    "    logging: process_default\n"
                    "  - name: site.net_power\n"
                    "    type: virtual\n"
                    "    expr: site.pv.total_power - site.load.power\n"
                    "    unit: kW\n"
                    "    logging: { policy: process_default, interval: 1s }\n"
                    "  - name: ps1.tank.level_sp\n"
                    "    type: manual\n"
                    "    value_type: float64\n"
                    "    area: ps1\n"
                    "    limits: { min: 0, max: 5 }\n"
                    "    initial: 2.5\n"
                    "    retain: true\n"),
              Lines{});
    EXPECT_EQ(
        check("tags.schema.json", "tags: [ { name: a, type: derived } ]\n"),
        Lines{"1:26 CF-205: tags[0].type must be one of internal, "
              "manual, virtual"});
}

TEST(SchemaExamples, TheOutputsOfSection72) {
    EXPECT_EQ(
        check("outputs.schema.json",
              "interlocks:\n"
              "  - name: ps1.pump2.permissive\n"
              "    condition: ps1.tank.level > 0.5 AND NOT ps1.pump2.trip\n"
              "    message: \"tank level low or pump tripped\"\n"
              "outputs:\n"
              "  - name: ps1.pump2.speed_cmd\n"
              "    area: ps1\n"
              "    kind: setpoint\n"
              "    target: ps1.vfd2.speed_set\n"
              "    feedback: { tag: ps1.vfd2.speed, within: 2.0, settle: "
              "5s }\n"
              "    limits: { min: 0, max: 100, rate: 10, release: { raw: "
              "0x7FFFFFFF } }\n"
              "    interlocks: [ps1.pump2.permissive]\n"
              "    safe_state: { value: 0 }\n"
              "    on_shutdown: hold\n"
              "    on_control_loss: { action: safe, after: 2s }\n"
              "  - name: ps1.cb1.cmd\n"
              "    area: ps1\n"
              "    kind: double\n"
              "    target: ps1.relay1.cb_ctrl\n"
              "    feedback: ps1.relay1.cb_pos\n"
              "    sbo: true\n"
              "    reason_required: true\n"
              "    interlocks: { on: [ps1.cb1.close_permissive], off: [] "
              "}\n"
              "    safe_state: hold\n"
              "    on_shutdown: hold\n"
              "    on_control_loss: { action: hold }\n"),
        Lines{});
}

TEST(SchemaExamples, OutputRefusals) {
    EXPECT_EQ(
        check("outputs.schema.json",
              "outputs:\n"
              "  - { name: o, area: a, kind: latch, target: t, "
              "feedback: f, no_feedback: true, termination_timeout: 1s, "
              "safe_state: { value: 1, raw: 2 } }\n"),
        (Lines{"2:31 CF-210: outputs[0].kind: \"latch\" is reserved and not "
               "available in this version",
               "2:62 CF-207: outputs[0].no_feedback cannot be given with "
               "feedback",
               "2:81 CF-210: outputs[0].termination_timeout is reserved "
               "and not available in this version",
               "2:130 CF-201: unknown key \"raw\" in outputs[0].safe_state"}));
}

TEST(SchemaExamples, TheAlarmsOfSection8) {
    EXPECT_EQ(check("alarms.schema.json",
                    "priorities: [critical, high, medium, low]\n"
                    "max_shelve: { critical: 1h, high: 8h, medium: 24h, low: "
                    "7d }\n"
                    "classes: [process, equipment, system]\n"
                    "alarms:\n"
                    "  - { id: level_hh, tag: ps1.tank.level, type: high_high, "
                    "limit: 4.8, deadband: 0.1,\n"
                    "      priority: critical, area: ps1, message: \"tank "
                    "level very high\", guidance: \"stop pumps P1 and P2\" }\n"
                    "  - id: dry_run\n"
                    "    name: ps1.pump2.dry_run\n"
                    "    type: expression\n"
                    "    expr: ps1.pump2.running AND ps1.pump2.flow < 1.0\n"
                    "    on_delay: 30s\n"
                    "    priority: high\n"
                    "    area: ps1\n"
                    "    suppress_when: ps1.station.maintenance\n"
                    "  - { tag: ps1.tank.level, type: high, limit: "
                    "ps1.tank.high_sp, priority: high }\n"),
              Lines{});
    EXPECT_EQ(check("alarms.schema.json", "alarms: [ { type: high } ]\n"),
              Lines{"1:11 CF-202: alarms[0] needs priority"});
}

TEST(SchemaExamples, TheProgramOfSection92) {
    EXPECT_EQ(check("program.schema.json",
                    "program: ps1_level\n"
                    "task: normal\n"
                    "blocks:\n"
                    "  - id: level_pid\n"
                    "    type: pid\n"
                    "    in:  { pv: ps1.tank.level, sp: ps1.tank.level_sp }\n"
                    "    params: { kp: 2.0, ti: 60s, td: 0s, out_min: 0, "
                    "out_max: 100 }\n"
                    "    tunable: [kp, ti]\n"
                    "    on_bad_input: { action: hold, then_safe_after: 30s, "
                    "safe_value: 0 }\n"
                    "  - id: speed_out\n"
                    "    type: analog_output\n"
                    "    in:  { value: level_pid.out, interlock: "
                    "ps1.pump2.permissive, prev: { from: speed_out.value, "
                    "prev: true } }\n"
                    "    output: ps1.pump2.speed_cmd\n"),
              Lines{});
}

TEST(SchemaExamples, ABaseTemplateHasNoProtocolKeys) {
    EXPECT_EQ(check("template.schema.json",
                    "template: meter_base\n"
                    "points: [ { name: power, unit: kW } ]\n"
                    "only: [power]\n"
                    "bindings: {}\n"
                    "session: {}\n"),
              (Lines{"4:1 CF-210: bindings is reserved and not available in "
                     "this version",
                     "5:1 CF-210: session is reserved and not available in "
                     "this version"}));
}

}  // namespace
}  // namespace scada::conf
