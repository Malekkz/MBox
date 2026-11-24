// Stage 3 of the template, channel, tag, alarm and output files: the merge
// rules of configuration schemas contract 4.5 as stage 3 applies them, the
// keys one object requires or refuses by the value of another (rules.h),
// and the literal values of keys that also take an expression. Invented
// examples only.

#include "scada/conf/rules.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/conf/schema.h"
#include "scada/conf/tree.h"
#include "scada/conf/types.h"
#include "scada/conf/yaml.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::conf {
namespace {

using Lines = std::vector<std::string>;

constexpr std::string_view MODBUS_TEMPLATE =
    "modbus.schema.json#/$defs/template";
constexpr std::string_view IO_TEMPLATE = "io.schema.json#/$defs/template";
constexpr std::string_view MODBUS_CHANNEL =
    "modbus.schema.json#/$defs/channel_file";
constexpr std::string_view IO_CHANNEL = "io.schema.json#/$defs/channel_file";

// Stage 3 of one file, as validate_tree runs it: the schema, then the rules.
Lines check(FileType type, std::string_view schema, std::string_view protocol,
            std::string_view yaml) {
    Diagnostics diagnostics;
    std::optional<Document> document = parse_yaml(yaml, "f.yaml", diagnostics);
    if (!document) {
        return {"parse error"};
    }
    ScalarTypes types(*document);
    SchemaSet::builtin().validate(*document, types, schema, "f.yaml",
                                  diagnostics);
    check_file_rules(*document, types, type, protocol, diagnostics);
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

Lines check_template(std::string_view yaml) {
    return check(FileType::TEMPLATE, MODBUS_TEMPLATE, "modbus", yaml);
}

Lines check_channel(std::string_view yaml) {
    return check(FileType::CHANNEL, MODBUS_CHANNEL, "modbus", yaml);
}

// A Modbus template of one point with `keys`, one key per line.
std::string one_point(std::string_view keys) {
    return "template: t\nprotocol: modbus\npoints:\n  - name: p\n" +
           std::string(keys);
}

// Section 4.5: `replace` is a reserved key of every map of a point that
// merging merges, and of the template maps merged level by level.
TEST(Rules, ReplaceIsAcceptedInEveryMapThatMerges) {
    const std::vector<std::pair<std::string_view, std::string_view>> cases = {
        {"point", "    replace: true\n"},
        {"address",
         "    address: { replace: true, table: holding, register: 1 }\n"},
        {"double point address",
         "    kind: double_point\n"
         "    address: { on: { replace: true, table: coil, register: 1 },\n"
         "               off: { table: coil, register: 2 } }\n"},
        {"write",
         "    address: { table: holding, register: 1, write: { replace: true, "
         "value: 1 } }\n"},
        {"write.values",
         "    address: { table: holding, register: 1, write: { values: { "
         "replace: true, 1: 2 } } }\n"},
        {"write.on",
         "    address: { table: holding, register: 1, write: { on: { replace: "
         "true, table: coil, register: 3, value: 1 } } }\n"},
        {"address device_time",
         "    address: { table: holding, register: 1, device_time: { replace: "
         "true, zone: utc } }\n"},
        {"scale", "    scale: { replace: true, divide_by: 10 }\n"},
        {"exponent_from",
         "    exponent_from: { replace: true, point: e, direction: divide }\n"},
        {"write_limits",
         "    write_limits: { replace: true, max_per_day: 5 }\n"},
        {"counter", "    counter: { replace: true, limit: 0xFFFF }\n"},
        {"states", "    states: { replace: true, 0: 0, 3: 3 }\n"},
        {"bits", "    bits: { replace: true, 0: run }\n"},
        {"bit definition", "    bits: { 1: { replace: true, name: trip } }\n"},
        {"enum", "    enum: { replace: true, 0: stopped, 1: running }\n"},
        {"on_invalid", "    on_invalid: { replace: true, substitute: 0 }\n"},
        {"logging",
         "    logging: { replace: true, mode: on_change, raw_retention: 90d "
         "}\n"},
        {"logging.tiers",
         "    logging: { policy: p, tiers: { replace: true, 1h: 10y } }\n"},
    };
    for (const auto& [map, keys] : cases) {
        SCOPED_TRACE(map);
        EXPECT_EQ(check_template(one_point(keys)), Lines{});
    }
    EXPECT_EQ(check_template("template: t\nprotocol: modbus\n"
                             "read: { replace: true, max_gap: 0 }\n"
                             "device_time: { replace: true, zone: utc }\n"
                             "defaults: { invalid_values: { replace: true, "
                             "uint16: [0xFFFF] } }\n"),
              Lines{});
    EXPECT_EQ(check(FileType::TEMPLATE, IO_TEMPLATE, "io",
                    "template: t\nprotocol: io\npoints:\n  - name: p\n"
                    "    address: { replace: true, line: 3, profile: { "
                    "replace: true, type: sine, period: 10 } }\n"),
              Lines{});
    // A map in a list is replaced with its list, never merged.
    EXPECT_EQ(check_template(one_point(
                  "    alarms: [ { replace: true, type: high, limit: 1, "
                  "priority: high } ]\n")),
              Lines{"5:17 CF-201: unknown key \"replace\" in "
                    "points[0].alarms[0]"});
}

// Section 4.5 merges these maps, so a key they lack may come from another
// level: stage 3 does not refuse them for it (stage 5 checks the merged map).
TEST(Rules, PartialMapsThatMergingCompletesAreAccepted) {
    EXPECT_EQ(
        check_channel("channel: { name: c1, type: rtu, device: /dev/ttyS1 }\n"
                      "devices:\n"
                      "  - name: d1\n"
                      "    template: t\n"
                      "    unit_id: 1\n"
                      "    overrides:\n"
                      "      p1: { exponent_from: { direction: divide } }\n"
                      "      p2: { bits: { 7: { invert: true } } }\n"
                      "      p3: { profile_free: 1 }\n"),
        Lines{"9:13 CF-201: unknown key \"profile_free\" in "
              "devices[0].overrides.p3"});
    EXPECT_EQ(
        check_template("template: child\nprotocol: modbus\nextends: base\n"
                       "params: { inputs: { default: 8 } }\n"),
        Lines{});
}

// A node of the wrong kind gets one error, though the framework's schema
// and the protocol's both state its kind.
TEST(Rules, OneErrorPerCause) {
    EXPECT_EQ(check_template("template: t\nprotocol: modbus\n"
                             "read: 5\nfunctions: 3\nrepeat: 5\nidentify: 5\n"
                             "points:\n  - name: p\n    address: [1, 2]\n"),
              (Lines{"3:7 CF-203: read must be a map",
                     "4:12 CF-203: functions must be a list",
                     "5:9 CF-203: repeat must be a list",
                     "6:11 CF-203: identify must be a list",
                     "9:14 CF-203: points[0].address must be a map"}));
    EXPECT_EQ(check_template("template: t\nprotocol: modbus\npoints: 5\n"),
              Lines{"3:9 CF-203: points must be a list"});
    EXPECT_EQ(
        check_channel("channel: { name: c1, type: rtu, device: /dev/ttyS1 }\n"
                      "devices: [ { name: d1, unit_id: 1, overrides: 5 } "
                      "]\n"),
        Lines{"2:47 CF-203: devices[0].overrides must be a map"});
}

// A literal number where a key also takes an expression is checked at
// stage 3; only a real expression waits for expansion.
TEST(Rules, LiteralNumbersAreChecked) {
    EXPECT_EQ(
        check_template(one_point(
            "    address: { table: holding, register: 1.5, bit: 16 }\n"
            "    scale: { divide_by: 0 }\n"
            "  - { name: q, address: { table: holding, register: -1, bit: -1 "
            "}, "
            "scale: { divide_by: -5 } }\n"
            "  - { name: r, address: { table: holding, register: 65537, bit: "
            "1.5 }, scale: { divide_by: 2.5 } }\n"
            "  - { name: s, address: { table: holding, register: 2, field: "
            "[7, 4] } }\n"
            "  - { name: u, address: { table: holding, register: 2, field: "
            "[0, 64] } }\n")),
        // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): each message is cut
        (Lines{"5:42 CF-203: points[0].address.register must be an integer "
               "or an expression",
               "5:52 CF-204: points[0].address.bit must be between 0 and 15",
               "6:25 CF-204: points[0].scale.divide_by must be between 1 and "
               "any",
               "7:53 CF-204: points[1].address.register must be between 0 "
               "and 65536",
               "7:62 CF-204: points[1].address.bit must be between 0 and 15",
               "7:88 CF-204: points[1].scale.divide_by must be between 1 and "
               "any",
               "8:53 CF-204: points[2].address.register must be between 0 "
               "and 65536",
               "8:65 CF-203: points[2].address.bit must be an integer or an "
               "expression",
               "8:92 CF-203: points[2].scale.divide_by must be an integer or "
               "an expression",
               "9:64 CF-204: points[3].address.field[0] must be between 0 and "
               "4",
               "10:67 CF-204: points[4].address.field[1] must be between 0 "
               "and 63"}));
    EXPECT_EQ(check_template(one_point(
                  "    address: { table: holding, register: \"100 + n\", bit: "
                  "\"n - 1\" }\n"
                  "    scale: { divide_by: k }\n"
                  "  - { name: q, address: { table: holding, register: 65536, "
                  "field: [\"a\", 3] } }\n"
                  "  - { name: r, address: { table: holding, register: 0, "
                  "field: [4, 4] } }\n")),
              Lines{});
}

// `when` is a configuration expression, `for` takes integers and ranges
// (Annex C, config_expression and for_value).
TEST(Rules, WhenAndForHaveTheirTypes) {
    EXPECT_EQ(
        check_template("template: t\nprotocol: modbus\n"
                       "points: [ { name: a, when: true }, { name: b, "
                       "when: \"has_meter\" } ]\n"
                       "repeat:\n"
                       "  - for: { n: \"1..inputs\", m: [1, 2, \"5..8\"] }\n"
                       "    when: \"n > 1\"\n"
                       "    points: [ { name: \"x{n}.y{m}\" } ]\n"
                       "  - for: { n: 99999999999999999999 }\n"
                       "    points: [ { name: \"z{n}\" } ]\n"),
        Lines{"8:15 CF-204: repeat[1].for.n must be between "
              "-9223372036854775808 and 9223372036854775807"});
}

// The required keys of an object that no merge completes, by the value of
// another key of it (D-108).
TEST(Rules, KindsRequireTheirKeys) {
    EXPECT_EQ(check_channel("channel: { name: c1, type: tcp }\n"),
              Lines{"1:10 CF-202: channel needs host"});
    EXPECT_EQ(check_channel("channel: { name: c1, type: rtu }\n"),
              Lines{"1:10 CF-202: channel needs device"});
    EXPECT_EQ(check_channel("channel: { name: c1, type: rtu_over_tcp }\n"),
              Lines{"1:10 CF-202: channel needs host"});
    EXPECT_EQ(check(FileType::CHANNEL, IO_CHANNEL, "io",
                    "channel: { name: c1, backend: gpio }\n"),
              Lines{"1:10 CF-202: channel needs chip"});
    EXPECT_EQ(check(FileType::CHANNEL, IO_CHANNEL, "io",
                    "channel: { name: c1, backend: iio }\n"),
              Lines{"1:10 CF-202: channel needs device"});
    EXPECT_EQ(check(FileType::CHANNEL, IO_CHANNEL, "io",
                    "channel: { name: c1, backend: simulator }\n"),
              Lines{});
    EXPECT_EQ(
        check_channel("channel: { name: c1, type: tcp, host: 192.0.2.1 }\n"
                      "devices: [ { name: d1 } ]\n"),
        Lines{"2:12 CF-202: devices[0] needs unit_id"});
    EXPECT_EQ(
        check_channel("channel: { name: c1, type: tcp, host: 192.0.2.1 }\n"
                      "devices:\n"
                      "  - { name: d1, unit_id: 1, params: { a: 1 }, only: "
                      "[x], points: [ { name: p } ] }\n"
                      "  - { name: d2, unit_id: 2, template: t, params: { "
                      "a: 1 }, only: [x] }\n"),
        (Lines{"3:29 CF-201: unknown key \"params\" in devices[0] (no "
               "template)",
               "3:47 CF-201: unknown key \"only\" in devices[0] (no "
               "template)"}));
    EXPECT_EQ(check(FileType::TAGS, "tags.schema.json", "",
                    "tags:\n"
                    "  - { name: a.b, type: internal }\n"
                    "  - { name: a.c, type: manual }\n"
                    "  - { name: a.d, type: virtual }\n"
                    "  - { name: a.e, type: manual, value_type: bool, area: a "
                    "}\n"),
              (Lines{"2:5 CF-202: tags[0] (type \"internal\") needs value_type",
                     "3:5 CF-202: tags[1] (type \"manual\") needs value_type",
                     "3:5 CF-202: tags[1] (type \"manual\") needs area",
                     "4:5 CF-202: tags[2] (type \"virtual\") needs expr"}));
    EXPECT_EQ(
        check(FileType::ALARMS, "alarms.schema.json", "",
              "alarms:\n"
              "  - { tag: a.b, type: high, priority: p }\n"
              "  - { tag: a.b, type: deviation, priority: p }\n"
              "  - { tag: a.b, type: state, priority: p }\n"
              "  - { type: expression, priority: p }\n"
              "  - { type: bad_quality, priority: p }\n"
              "  - { tag: a.b, type: low, limit: 1, priority: p }\n"
              "  - { name: a.x, type: expression, expr: \"a.b\", "
              "priority: p }\n"),
        (Lines{"2:5 CF-202: alarms[0] (type \"high\") needs limit",
               "3:5 CF-202: alarms[1] (type \"deviation\") needs limit",
               "3:5 CF-202: alarms[1] (type \"deviation\") needs setpoint",
               "4:5 CF-202: alarms[2] (type \"state\") needs alarm_on",
               "5:5 CF-202: alarms[3] (type \"expression\") needs expr",
               "5:5 CF-202: alarms[3] (type \"expression\") needs name",
               "6:5 CF-202: alarms[4] (type \"bad_quality\") needs tag"}));
    EXPECT_EQ(
        check_template(
            one_point("    alarms: [ { type: rate_of_change, priority: p } ]\n"
                      "    bits: { 1: { name: trip, alarms: [ { type: state, "
                      "priority: p } ] } }\n")),
        (Lines{"5:15 CF-202: points[0].alarms[0] (type \"rate_of_change\") "
               "needs limit",
               "6:40 CF-202: points[0].bits.1.alarms[0] (type \"state\") "
               "needs alarm_on"}));
    EXPECT_EQ(check_template("template: t\nprotocol: modbus\n"
                             "identify: [ { equals: 1 }, { point: p } ]\n"
                             "repeat:\n"
                             "  - { for: {}, points: [] }\n"
                             "  - { for: { a: 1, b: 2, c: 3 }, points: [] }\n"
                             "  - { for: { a: 1, b: 2 }, points: [] }\n"),
              (Lines{"3:13 CF-202: identify[0] needs address or point",
                     "3:28 CF-202: identify[1] needs equals, in, range or "
                     "pattern",
                     "5:12 CF-204: the number of variables of repeat[0].for "
                     "must be between 1 and 2",
                     "6:12 CF-204: the number of variables of repeat[1].for "
                     "must be between 1 and 2"}));
}

// Keys that belong to another kind of the same object (D-108).
TEST(Rules, KindsRefuseTheKeysOfOtherKinds) {
    EXPECT_EQ(
        check_channel("channel: { name: c1, type: tcp, host: 192.0.2.1, "
                      "baud: 9600, parity: odd, rs485: {} }\n"),
        (Lines{"1:50 CF-201: unknown key \"baud\" in channel (type \"tcp\")",
               "1:62 CF-201: unknown key \"parity\" in channel (type \"tcp\")",
               "1:75 CF-210: channel.rs485 is reserved and not available "
               "in this version"}));
    EXPECT_EQ(
        check_channel("channel: { name: c1, type: rtu, device: /dev/ttyS1, "
                      "host: 192.0.2.1, port: 502, lost_after_timeouts: 3 "
                      "}\n"),
        (Lines{"1:53 CF-201: unknown key \"host\" in channel (type \"rtu\")",
               "1:70 CF-201: unknown key \"port\" in channel (type \"rtu\")",
               "1:81 CF-201: unknown key \"lost_after_timeouts\" in "
               "channel (type \"rtu\")"}));
    EXPECT_EQ(check_channel("channel: { name: c1, type: rtu_over_tcp, host: "
                            "192.0.2.1, lost_after_timeouts: 3 }\n"),
              Lines{"1:59 CF-201: unknown key \"lost_after_timeouts\" in "
                    "channel (type \"rtu_over_tcp\")"});
    EXPECT_EQ(check_channel("channel: { name: c1, type: tcp, host: 192.0.2.1, "
                            "port: 1502, connect_timeout: 1s, "
                            "lost_after_timeouts: 3 }\n"),
              Lines{});
    EXPECT_EQ(
        check(FileType::TAGS, "tags.schema.json", "",
              "tags:\n"
              "  - { name: a.b, type: internal, value_type: int64, expr: "
              "\"1\", quality: worst, limits: {}, task: normal, initial: 1 "
              "}\n"
              "  - { name: a.c, type: virtual, expr: \"1\", initial: 1, "
              "limits: {} }\n"
              "  - { name: a.d, type: manual, value_type: int64, area: a, "
              "limits: {}, initial: 1 }\n"),
        // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): each message is cut
        (Lines{"2:53 CF-201: unknown key \"expr\" in tags[0] (type "
               "\"internal\")",
               "2:64 CF-201: unknown key \"quality\" in tags[0] (type "
               "\"internal\")",
               "2:80 CF-201: unknown key \"limits\" in tags[0] (type "
               "\"internal\")",
               "3:44 CF-201: unknown key \"initial\" in tags[1] (type "
               "\"virtual\")",
               "3:56 CF-201: unknown key \"limits\" in tags[1] (type "
               "\"virtual\")"}));
    EXPECT_EQ(check(FileType::OUTPUTS, "outputs.schema.json", "",
                    "outputs:\n"
                    "  - { name: o.a, area: a, kind: setpoint, target: t.a, "
                    "duration: 1s }\n"
                    "  - { name: o.b, area: a, kind: pulse, target: t.b, "
                    "duration: 1s }\n"),
              Lines{"2:56 CF-201: unknown key \"duration\" in outputs[0] "
                    "(kind \"setpoint\")"});
}

// Values and keys the contract does not take.
TEST(Rules, ValuesBeyondTheContractAreRefused) {
    // 7 data bits is CF-622 at stage 5 (Annex A.1): stage 3 lets it pass
    // and does not name it as valid.
    EXPECT_EQ(check_channel("channel: { name: c1, type: rtu, device: "
                            "/dev/ttyS1, data_bits: 7 }\n"),
              Lines{});
    EXPECT_EQ(check_channel("channel: { name: c1, type: rtu, device: "
                            "/dev/ttyS1, data_bits: 9 }\n"),
              Lines{"1:64 CF-205: channel.data_bits must be one of 8"});
    EXPECT_EQ(check_channel("channel: { name: c1, device: /dev/ttyS1, "
                            "data_bits: 0x8 }\n"),
              Lines{"1:10 CF-202: channel needs type"});
    EXPECT_EQ(
        check_template(
            "template: t\nprotocol: modbus\n"
            "params: { n: { type: int, description: count } }\n"
            "defaults: { invalid_values: { uint16: [0], uint17: [1] } }\n"
            "points:\n"
            "  - name: p\n"
            "    quality_from:\n"
            "      - { point: self, map: { 0: [good, bad], 1: [TEST], 2: "
            "[good, COMM_FAIL], 3: [uncertain, TEST], default: bad } }\n"
            "  - name: d\n"
            "    kind: double_point\n"
            "    address: { on: { table: holding, register: 1, bit: 0, "
            "write: {}, parts: [], length: 2 }, off: { table: coil, "
            "register: 2 } }\n"),
        // NOLINTBEGIN(bugprone-suspicious-missing-comma): each message is cut
        (Lines{"3:27 CF-201: unknown key \"description\" in params.n",
               "4:44 CF-201: unknown key \"uint17\" in "
               "defaults.invalid_values",
               "8:34 CF-203: points[0].quality_from[0].map.0 must be "
               "good, uncertain or bad, or a list of one of them and "
               "flags",
               "8:50 CF-203: points[0].quality_from[0].map.1 must be "
               "good, uncertain or bad, or a list of one of them and "
               "flags",
               "8:68 CF-205: points[0].quality_from[0].map.2[1] must be "
               "one of good, uncertain, bad, OUT_OF_RANGE, OVERFLOW, "
               "BLOCKED, TEST",
               "11:59 CF-201: unknown key \"write\" in "
               "points[1].address.on",
               "11:70 CF-201: unknown key \"parts\" in "
               "points[1].address.on",
               "11:81 CF-201: unknown key \"length\" in "
               "points[1].address.on"}));
    // NOLINTEND(bugprone-suspicious-missing-comma)
    EXPECT_EQ(
        check(FileType::TEMPLATE, "template.schema.json", "",
              "template: base\n"
              "defaults: { invalid_values: { uint16: [0], replace: true } "
              "}\n"),
        Lines{"2:31 CF-201: unknown key \"uint16\" in "
              "defaults.invalid_values"});
    EXPECT_EQ(
        check_channel("channel: { name: c1, type: rtu, device: /dev/ttyS1 }\n"
                      "devices:\n"
                      "  - name: d1\n"
                      "    template: t\n"
                      "    unit_id: 1\n"
                      "    overrides:\n"
                      "      \"p{n}\": { unit: V }\n"
                      "      p: { invalid_values: { uint16: [0] } }\n"),
        (Lines{"7:7 CF-212: \"p{n}\" is not a valid point name",
               "8:28 CF-203: devices[0].overrides.p.invalid_values must "
               "be a list"}));
}

// The order of a bit field is checked only where `field` is one: an
// address, a quality rule and a health rule. A key of the same name
// elsewhere, such as a `repeat` variable, is not a bit field.
TEST(Rules, BitFieldsAreCheckedWhereTheyAre) {
    EXPECT_EQ(check_template("template: t\nprotocol: modbus\n"
                             "repeat:\n"
                             "  - for: { field: [5, 2] }\n"
                             "    points: [ { name: \"p{field}\" } ]\n"),
              Lines{});
    EXPECT_EQ(
        check_template(
            "template: t\nprotocol: modbus\n"
            "identify: [ { address: { table: holding, register: 1, field: "
            "[3, 1] }, equals: 1 } ]\n"
            "health: [ { point: s, good_when: { field: [9, 8], equals: 0 }, "
            "applies_to: all } ]\n"
            "points:\n"
            "  - name: p\n"
            "    quality_from: [ { point: self, field: [4, 2], map: { 0: good "
            "} } ]\n"),
        (Lines{"3:63 CF-204: identify[0].address.field[0] must be between 0 "
               "and 1",
               "4:44 CF-204: health[0].good_when.field[0] must be between 0 "
               "and 8",
               "7:44 CF-204: points[0].quality_from[0].field[0] must be "
               "between 0 and 2"}));
}

// A kind the schema refuses gets no rule of a kind: one error, CF-205.
TEST(Rules, AnUnknownKindGetsNoKindRules) {
    EXPECT_EQ(check(FileType::TAGS, "tags.schema.json", "",
                    "tags:\n"
                    "  - { name: a.b, type: virtal, task: normal, expr: \"1\", "
                    "limits: {}, initial: 1 }\n"),
              Lines{"2:24 CF-205: tags[0].type must be one of internal, "
                    "manual, virtual"});
    EXPECT_EQ(check(FileType::OUTPUTS, "outputs.schema.json", "",
                    "outputs:\n"
                    "  - { name: o.a, area: a, kind: sinle, target: t.a, "
                    "duration: 1s }\n"),
              Lines{"2:33 CF-205: outputs[0].kind must be one of single, "
                    "double, pulse, step, setpoint, trigger, latch"});
    EXPECT_EQ(check(FileType::ALARMS, "alarms.schema.json", "",
                    "alarms: [ { type: hihg, priority: p } ]\n"),
              Lines{"1:19 CF-205: alarms[0].type must be one of high_high, "
                    "high, low, low_low, deviation, rate_of_change, state, "
                    "bad_quality, expression"});
}

// `lost_after_timeouts` is for `tcp` channels only (4.6).
TEST(Rules, AnIoChannelHasNoTcpKeys) {
    EXPECT_EQ(check(FileType::CHANNEL, IO_CHANNEL, "io",
                    "channel: { name: c1, backend: simulator, "
                    "lost_after_timeouts: 3 }\n"),
              Lines{"1:42 CF-201: unknown key \"lost_after_timeouts\" in "
                    "channel (protocol \"io\")"});
}

// CF-517 at the edge of an integer divisor: 1 is an integer scaling, 2 is
// not.
TEST(Rules, IntegerScalingAtItsEdge) {
    EXPECT_EQ(check_template(one_point("    value_type: int64\n"
                                       "    scale: { divide_by: 1, offset: 3 "
                                       "}\n")),
              Lines{});
    EXPECT_EQ(check_template(one_point("    value_type: uint64\n"
                                       "    scale: { divide_by: 2 }\n")),
              Lines{"6:25 CF-517: \"p\": value_type uint64 needs an integer "
                    "scaling; use float64"});
}

// A parameter or `repeat` variable named like a reserved word of
// expressions is CF-212 at its declaration (5.6): `100 + bad` would read
// the literal BAD.
TEST(Rules, ReservedWordsAreNotParameterOrVariableNames) {
    EXPECT_EQ(
        check_template("template: t\nprotocol: modbus\n"
                       "params: { bad: { type: int, default: 8 }, in: { type: "
                       "int }, inputs: { type: int } }\n"
                       "repeat:\n"
                       "  - for: { true: \"1..2\", n: \"1..inputs\" }\n"
                       "    points: [ { name: \"p{n}\" } ]\n"),
        (Lines{"3:11 CF-212: \"bad\" is not a valid parameter name: it is a "
               "reserved word of expressions",
               "3:43 CF-212: \"in\" is not a valid parameter name: it is a "
               "reserved word of expressions",
               "5:12 CF-212: \"true\" is not a valid variable name: it is a "
               "reserved word of expressions"}));
}

// An expression error is at its character in the file, columns counted in
// characters, when the scalar is its source on one line; in a block or
// escaped scalar it is at the scalar, with the line and column within the
// expression's text in the message (D-109).
TEST(Rules, ExpressionErrorsAreWhereTheyAre) {
    EXPECT_EQ(
        check_template("template: t\nprotocol: modbus\n"
                       "points:\n"
                       "  - { name: p, when: \"x = '\xC3\xA9' + * 2\" }\n"),
        Lines{"4:33 CF-520: expression: unexpected \"*\" at column 11"});
    EXPECT_EQ(check(FileType::TAGS, "tags.schema.json", "",
                    "tags:\n"
                    "  - name: a.b\n"
                    "    type: virtual\n"
                    "    expr: |\n"
                    "      a > 1 AND\n"
                    "      b < < 2\n"),
              Lines{"4:11 CF-520: expression: unexpected \"<\" at line 2, "
                    "column 5 of the expression's text"});
    EXPECT_EQ(check(FileType::TAGS, "tags.schema.json", "",
                    "tags: [ { name: a.b, type: virtual, expr: \"a +\\u0020* "
                    "1\" } ]\n"),
              Lines{"1:43 CF-520: expression: unexpected \"*\" at line 1, "
                    "column 5 of the expression's text"});
}
}  // namespace
}  // namespace scada::conf
