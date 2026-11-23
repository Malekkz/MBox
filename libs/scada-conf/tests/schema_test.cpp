// Stage 3: the schema engine (configuration schemas contract 1.2, 10.2,
// Annex C) and the built-in schema files against the contract's examples.

#include "scada/conf/schema.h"
#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
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

// "<line>:<column> CF-<n>: <message>" for every error, in report order.
Lines report_lines(Diagnostics& diagnostics) {
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

// Validates `yaml` against the schema file `schema` of `set`.
Lines check_with(const SchemaSet& set, std::string_view schema,
                 std::string_view yaml) {
    Diagnostics diagnostics;
    std::optional<Document> document = parse_yaml(yaml, "f.yaml", diagnostics);
    if (!document) {
        return {"parse error"};
    }
    ScalarTypes types(*document);
    set.validate(*document, types, schema, "f.yaml", diagnostics);
    return report_lines(diagnostics);
}

// A set of one schema file, `t.schema.json`, of the text `json`.
SchemaSet one_schema(std::string json) {
    std::string problem;
    Result<SchemaSet> set = SchemaSet::compile(
        {SchemaSource{.name = "t.schema.json", .text = std::move(json)}},
        problem);
    EXPECT_TRUE(set.ok()) << problem;
    if (!set.ok()) {
        std::string unused;
        return std::move(SchemaSet::compile({}, unused)).value();
    }
    return std::move(set).value();
}

Lines check(const std::string& json, std::string_view yaml) {
    const SchemaSet set = one_schema(json);
    return check_with(set, "t.schema.json", yaml);
}

Lines builtin(std::string_view schema, std::string_view yaml) {
    return check_with(SchemaSet::builtin(), schema, yaml);
}

constexpr std::string_view OBJECT_AB = R"({
  "type": "object",
  "properties": {
    "a": { "type": "integer", "x-scada-type": "integer", "minimum": 1, "maximum": 10 },
    "b": { "type": "string", "enum": ["x", "y"] }
  },
  "required": ["a"],
  "unevaluatedProperties": false
})";

// --- The engine: each keyword gives its code -----------------------------

TEST(Schema, AValidMapGivesNothing) {
    EXPECT_EQ(check(std::string(OBJECT_AB), "a: 3\nb: x\n"), Lines{});
}

TEST(Schema, UnknownAndMissingKeys) {
    EXPECT_EQ(check(std::string(OBJECT_AB), "b: x\nc: 1\n"),
              (Lines{"1:1 CF-202: f.yaml needs a",
                     "2:1 CF-201: unknown key c in f.yaml"}));
}

TEST(Schema, IntegersArePlainAndInRange) {
    EXPECT_EQ(check(std::string(OBJECT_AB), "a: '3'\n"),
              Lines{"1:4 CF-203: a must be an integer"});
    EXPECT_EQ(check(std::string(OBJECT_AB), "a: 11\n"),
              Lines{"1:4 CF-204: a must be between 1 and 10"});
    EXPECT_EQ(check(std::string(OBJECT_AB), "a: 0x0A\n"), Lines{});
    for (const std::string_view text :
         {"+5", "010", "1_000", ".5", "1.", "0o17", "True", "5.0"}) {
        EXPECT_EQ(check(std::string(OBJECT_AB), "a: " + std::string(text)),
                  Lines{"1:4 CF-203: a must be an integer"})
            << text;
    }
}

TEST(Schema, EnumValues) {
    EXPECT_EQ(check(std::string(OBJECT_AB), "a: 1\nb: z\n"),
              Lines{"2:4 CF-205: b must be one of x, y"});
    // A quoted string is the same string.
    EXPECT_EQ(check(std::string(OBJECT_AB), "a: 1\nb: 'x'\n"), Lines{});
}

TEST(Schema, AWrongKindIsCF203) {
    EXPECT_EQ(check(std::string(OBJECT_AB), "a: [1]\n"),
              Lines{"1:4 CF-203: a must be an integer"});
    EXPECT_EQ(check(std::string(OBJECT_AB), "[1]\n"),
              Lines{"1:1 CF-203: f.yaml must be a map"});
}

TEST(Schema, BooleansAreTrueAndFalseOnly) {
    const std::string json = R"({ "type": "object", "properties": {
        "on": { "type": "boolean", "x-scada-type": "boolean" } } })";
    EXPECT_EQ(check(json, "on: true\n"), Lines{});
    for (const std::string_view text : {"True", "yes", "on", "1", "'true'"}) {
        EXPECT_EQ(check(json, "on: " + std::string(text) + "\n"),
                  Lines{"1:5 CF-203: on must be true or false"})
            << text;
    }
}

TEST(Schema, DurationsSizesAndPercentages) {
    const std::string json = R"({ "type": "object", "properties": {
        "d": { "type": "string", "x-scada-type": "duration", "x-scada-min": "10ms", "x-scada-max": "60s" },
        "s": { "type": "string", "x-scada-type": "size" },
        "p": { "type": "string", "x-scada-type": "percentage", "minimum": 0, "maximum": 100 } } })";
    EXPECT_EQ(check(json, "d: 5s\ns: 2GiB\np: 10%\n"), Lines{});
    EXPECT_EQ(check(json, "d: '5s'\n"), Lines{});
    EXPECT_EQ(check(json, "d: 1.5s\n"),
              Lines{"1:4 CF-206: 1.5s is not a valid duration"});
    EXPECT_EQ(check(json, "d: 61s\n"),
              Lines{"1:4 CF-204: d must be between 10ms and 60s"});
    EXPECT_EQ(check(json, "s: 2GB\n"),
              Lines{"1:4 CF-206: 2GB is not a valid size"});
    EXPECT_EQ(check(json, "p: 101%\n"),
              Lines{"1:4 CF-204: p must be between 0 and 100"});
    EXPECT_EQ(check(json, "p: 10\n"),
              Lines{"1:4 CF-206: 10 is not a valid percentage"});
}

TEST(Schema, NumbersKeepTheirWrittenForm) {
    const std::string json = R"({ "type": "object", "properties": {
        "gain": { "type": ["number", "string"], "x-scada-type": "number_or_expression" } } })";
    Diagnostics diagnostics;
    const SchemaSet set = one_schema(json);
    for (const auto& [text, type] :
         {std::pair<std::string_view, ScalarType>{"10", ScalarType::INTEGER},
          {"10.0", ScalarType::DECIMAL},
          {"\"k * 2\"", ScalarType::EXPRESSION},
          {"'10'", ScalarType::EXPRESSION}}) {
        std::optional<Document> document =
            parse_yaml("gain: " + std::string(text), "f.yaml", diagnostics);
        ASSERT_TRUE(document.has_value());
        if (!document) {
            continue;
        }
        ScalarTypes types(*document);
        set.validate(*document, types, "t.schema.json", "f.yaml", diagnostics);
        const NodeId value = document->entries(document->root())[0].value;
        EXPECT_EQ(types.of(value), type) << text;
    }
    EXPECT_EQ(diagnostics.error_count(), 0U);
}

TEST(Schema, NamesBreakingTheirGrammarAreCF212) {
    const std::string json = R"({ "type": "object", "properties": {
        "id": { "type": "string", "x-scada-type": "identifier" },
        "tag": { "type": "string", "x-scada-type": "tag_name" } } })";
    EXPECT_EQ(check(json, "id: pump_1\ntag: ps1.pump-2.flow\n"), Lines{});
    EXPECT_EQ(check(json, "id: Pump\n"),
              Lines{"1:5 CF-212: Pump is not a valid identifier name"});
    EXPECT_EQ(check(json, "id: " + std::string(32, 'a') + "\n"),
              Lines{"1:5 CF-212: " + std::string(32, 'a') +
                    " is not a valid identifier name"});
    EXPECT_EQ(check(json, "tag: ps1..flow\n"),
              Lines{"1:6 CF-212: ps1..flow is not a valid tag name"});
    EXPECT_EQ(check(json, "tag: " + std::string(128, 'a') + "\n"),
              Lines{"1:6 CF-212: " + std::string(128, 'a') +
                    " is not a valid tag name"});
}

TEST(Schema, ReservedKeysAndValuesAreCF210) {
    const std::string json = R"({ "type": "object", "properties": {
        "scan_group": { "x-scada-reserved": true },
        "role": { "type": "string", "enum": ["connect", "listen"], "x-scada-reserved-values": ["listen"] } } })";
    EXPECT_EQ(check(json, "scan_group: fast\n"),
              Lines{"1:1 CF-210: scan_group is reserved and not available in "
                    "this version"});
    EXPECT_EQ(check(json, "role: listen\n"),
              Lines{"1:7 CF-210: role: listen is reserved and not available "
                    "in this version"});
}

TEST(Schema, SecretsAreRefusedInPhase2) {
    const std::string json = R"({ "type": "object", "properties": {
        "password": { "x-scada-secret": true } } })";
    EXPECT_EQ(check(json, "password: hunter2\n"),
              Lines{"1:11 CF-213: password must be a secret reference"});
    EXPECT_EQ(check(json, "password: { secret: mail }\n"),
              Lines{"1:11 CF-214: secrets are not available in this version"});
}

TEST(Schema, ExclusiveAndDependentKeys) {
    const std::string json = R"({ "type": "object", "properties": {
        "gain": { "type": "number", "x-scada-type": "number" },
        "divide_by": { "type": "integer", "x-scada-type": "integer" },
        "max_gap": { "type": "integer", "x-scada-type": "integer" },
        "gap_policy": { "type": "string" } },
      "x-scada-exclusive": [["gain", "divide_by"]],
      "dependentRequired": { "max_gap": ["gap_policy"] } })";
    EXPECT_EQ(check(json, "gain: 2\ndivide_by: 10\n"),
              Lines{"2:1 CF-207: divide_by cannot be given with gain"});
    EXPECT_EQ(check(json, "max_gap: 2\n"),
              Lines{"1:1 CF-208: max_gap needs gap_policy"});
}

TEST(Schema, ListLengthsAndItems) {
    const std::string json = R"({ "type": "object", "properties": {
        "servers": { "type": "array", "minItems": 1, "maxItems": 2,
                     "items": { "type": "string", "x-scada-type": "host" } } } })";
    EXPECT_EQ(check(json, "servers: [10.0.0.1, ntp.example.org]\n"), Lines{});
    EXPECT_EQ(check(json, "servers: []\n"),
              Lines{"1:10 CF-204: the number of entries of servers must be "
                    "between 1 and 2"});
    EXPECT_EQ(check(json, "servers: [10.0.0.256]\n"),
              Lines{"1:11 CF-203: servers[0] must be a host name or an IP "
                    "address"});
}

// Section 1.2: a map with integer keys types them; two keys that type to the
// same value are CF-111.
TEST(Schema, IntegerKeysAndDuplicatesAfterTyping) {
    const std::string json =
        R"({ "type": "object", "x-scada-key-type": "integer",
        "unevaluatedProperties": { "type": "string", "x-scada-type": "identifier" } })";
    EXPECT_EQ(check(json, "0: stopped\n3: fault\n-1: unknown\n"), Lines{});
    EXPECT_EQ(check(json, "16: a\n0x10: b\n"),
              Lines{"2:1 CF-111: duplicate key 0x10"});
    EXPECT_EQ(check(json, "'01': a\n"),
              Lines{"1:1 CF-203: key 01 in f.yaml must be an integer"});
    EXPECT_EQ(check(json, "x: a\n"),
              Lines{"1:1 CF-203: key x in f.yaml must be an integer"});
}

TEST(Schema, BooleanKeys) {
    const std::string json =
        R"({ "type": "object", "x-scada-key-type": "boolean",
        "unevaluatedProperties": { "type": "integer", "x-scada-type": "integer" } })";
    EXPECT_EQ(check(json, "true: 0\nfalse: 1\n"), Lines{});
    EXPECT_EQ(check(json, "yes: 0\n"),
              Lines{"1:1 CF-203: key yes in f.yaml must be true or false"});
}

// Annex C: a key with several shapes is a `oneOf`; the errors reported are
// those of the shape that came closest.
TEST(Schema, OneOfTakesThePassingShapeOrTheClosest) {
    const std::string json = R"({ "type": "object", "properties": {
        "feedback": { "oneOf": [
          { "type": "string", "x-scada-type": "tag_name" },
          { "type": "object", "properties": {
              "tag": { "type": "string", "x-scada-type": "tag_name" },
              "within": { "type": "number", "x-scada-type": "number" } },
            "required": ["tag"], "unevaluatedProperties": false } ] } } })";
    EXPECT_EQ(check(json, "feedback: ps1.x\n"), Lines{});
    EXPECT_EQ(check(json, "feedback: { tag: ps1.x, within: 2.0 }\n"), Lines{});
    EXPECT_EQ(check(json, "feedback: { tag: ps1.x, within: wide }\n"),
              Lines{"1:33 CF-203: feedback.within must be a number"});
}

// 2020-12: keys of an `allOf` part count as evaluated for the object's own
// `unevaluatedProperties`, so framework and protocol keys can share an object.
TEST(Schema, AllOfPartsShareOneObject) {
    const std::string json = R"({
      "allOf": [
        { "properties": { "name": { "type": "string" } } },
        { "properties": { "baud": { "type": "integer", "x-scada-type": "integer" } } } ],
      "unevaluatedProperties": false })";
    EXPECT_EQ(check(json, "name: a\nbaud: 9600\n"), Lines{});
    EXPECT_EQ(check(json, "name: a\nparity: even\n"),
              Lines{"2:1 CF-201: unknown key parity in f.yaml"});
}

TEST(Schema, ReferencesWithinAndAcrossFiles) {
    std::string problem;
    const Result<SchemaSet> set = SchemaSet::compile(
        {SchemaSource{.name = "a.schema.json",
                      .text = R"({ "$defs": { "small": { "type": "integer",
                        "x-scada-type": "integer", "maximum": 3 } } })"},
         SchemaSource{.name = "b.schema.json",
                      .text = R"({ "type": "object", "properties": {
                        "x": { "$ref": "a.schema.json#/$defs/small" },
                        "y": { "$ref": "#/$defs/local" } },
                        "$defs": { "local": { "type": "string", "enum": ["q"] } } })"}},
        problem);
    ASSERT_TRUE(set.ok()) << problem;
    EXPECT_EQ(check_with(set.value(), "b.schema.json", "x: 4\ny: r\n"),
              (Lines{"1:4 CF-204: x must be between any and 3",
                     "2:4 CF-205: y must be one of q"}));
}

TEST(Schema, ABrokenSchemaFileIsAnInternalError) {
    std::string problem;
    EXPECT_FALSE(SchemaSet::compile(
                     {SchemaSource{.name = "x.schema.json",
                                   .text = R"({ "patternProperties": {} })"}},
                     problem)
                     .ok());
    EXPECT_EQ(problem, "x.schema.json:1: unknown keyword patternProperties");
    EXPECT_FALSE(SchemaSet::compile(
                     {SchemaSource{.name = "y.schema.json",
                                   .text = R"({ "$ref": "#/$defs/none" })"}},
                     problem)
                     .ok());
}

// --- The built-in schemas against the contract's examples ---------------

TEST(BuiltinSchemas, AllCompile) {
    for (const std::string_view name :
         {"common.schema.json", "system.schema.json", "areas.schema.json",
          "enums.schema.json", "logging.schema.json", "tasks.schema.json"}) {
        EXPECT_TRUE(SchemaSet::builtin().has(name)) << name;
    }
}

TEST(BuiltinSchemas, TheSystemExampleOfSection3) {
    EXPECT_EQ(builtin("system.schema.json",
                      "unit:\n"
                      "  id: site-0042\n"
                      "  name: Pump Station 1\n"
                      "time:\n"
                      "  timezone: Etc/UTC\n"
                      "  sync:\n"
                      "    mode: ntp\n"
                      "    servers: [10.0.0.1, 10.0.0.2]\n"
                      "    alarm_offset_ms: 500\n"
                      "    alarm_unsynced_s: 600\n"
                      "storage:\n"
                      "  free_space_alarm: 10%\n"
                      "data_plane:\n"
                      "  heartbeat_check_ms: 250\n"),
              Lines{});
}

TEST(BuiltinSchemas, SystemRefusals) {
    const std::string base =
        "unit: { id: site-0042, name: x }\ntime: { timezone: Etc/UTC }\n";
    EXPECT_EQ(builtin("system.schema.json", base + "node: { role: master }\n"),
              Lines{"3:15 CF-210: node.role: master is reserved and not "
                    "available in this version"});
    EXPECT_EQ(builtin("system.schema.json",
                      base + "data_plane: { event_queue_capacity: 1000 }\n"),
              Lines{"3:37 CF-203: data_plane.event_queue_capacity must be a "
                    "power of two"});
    EXPECT_EQ(builtin("system.schema.json",
                      base + "data_plane: { processes: { web: {} } }\n"),
              Lines{"3:28 CF-212: web is not a valid process name"});
    EXPECT_EQ(builtin("system.schema.json",
                      base + "email: { server: mail, from: x, password: p }\n"),
              Lines{"3:43 CF-213: email.password must be a secret reference"});
    EXPECT_EQ(
        builtin("system.schema.json", "unit: { id: Site }\n"),
        (Lines{"1:1 CF-202: f.yaml needs time", "1:7 CF-202: unit needs name",
               "1:13 CF-212: Site is not a valid unit id name"}));
}

// Every key of section 3. Secret references are refused in phase 2.
TEST(BuiltinSchemas, EveryKeyOfSystem) {
    EXPECT_EQ(
        builtin("system.schema.json",
                "format: 1\n"
                "unit: { id: site-0042, name: x }\n"
                "node: { role: standalone }\n"
                "time: { timezone: America/Argentina/Buenos_Aires, sync: { "
                "mode: ptp, servers: [ntp1.example.org, \"2001:db8::1\"] } }\n"
                "network: { hostname: unit1, interfaces: [ { name: eth0, "
                "zone: field, addresses: [192.168.1.2/24, \"fe80::1/64\"], "
                "gateway: 192.168.1.1, dns: [\"::1\"] } ] }\n"
                "storage: { free_space_alarm: 10%, journal_reserve: 2GiB }\n"
                "conf: { keep_versions: 50 }\n"
                "upstream: { url: \"https://x.example.org/in\", order: "
                "oldest_first, rate_limit: 1MiB, window: { from: \"01:00\", "
                "to: \"05:00\" }, auth: { token: { secret: up } } }\n"
                "email: { server: mail.example.org, port: 587, tls: starttls, "
                "username: scada, from: unit@example.org, password: { secret: "
                "mail } }\n"
                "limits: { max_tags: 50000, max_devices: 1000, "
                "max_devices_per_channel: 247, max_channels: 300 }\n"
                "data_plane: { heartbeat_check_ms: 250, retain_interval_ms: "
                "5000, generation_change_limit: 5s, processes: { drv-modbus: { "
                "heartbeat_timeout_ms: 3000, control_attach_grace: 60s }, "
                "control: { heartbeat_timeout_ms: 500 } }, "
                "event_queue_capacity: 1024, command_queue_capacity: 1024 }\n"
                "realtime: { cpus: [2, 3], io_priority: 95 }\n"),
        (Lines{"8:140 CF-214: secrets are not available in this version",
               "9:113 CF-214: secrets are not available in this version"}));
}

TEST(BuiltinSchemas, TheAreasExampleOfSection71) {
    EXPECT_EQ(builtin("areas.schema.json",
                      "areas:\n"
                      "  ps1:\n"
                      "    description: Pump station 1\n"
                      "    authority:\n"
                      "      default: web\n"
                      "      local_panel: { input: ps1.panel.local, active: "
                      "true }\n"
                      "      local_blocks_auto: true\n"
                      "    auto_resume: false\n"),
              Lines{});
}

TEST(BuiltinSchemas, TheLoggingExampleOfSection6) {
    EXPECT_EQ(builtin("logging.schema.json",
                      "aggregate_tiers:\n"
                      "  - { name: 1m,  interval: 1m }\n"
                      "  - { name: 15m, interval: 15m }\n"
                      "  - { name: 1h,  interval: 1h }\n"
                      "policies:\n"
                      "  process_default:\n"
                      "    mode: on_change\n"
                      "    deadband: 0.5%\n"
                      "    max_interval: 5m\n"
                      "    raw_retention: 90d\n"
                      "    tiers: { 1m: 1y, 1h: 10y }\n"
                      "  counters:\n"
                      "    mode: periodic\n"
                      "    interval: 1m\n"
                      "    raw_retention: 1y\n"
                      "    tiers: { 1h: 10y }\n"),
              Lines{});
    EXPECT_EQ(builtin("logging.schema.json",
                      "policies:\n  p:\n    mode: on_change\n"),
              Lines{"3:5 CF-202: policies.p needs raw_retention"});
}

TEST(BuiltinSchemas, TheTasksExampleOfSection91) {
    EXPECT_EQ(builtin("tasks.schema.json",
                      "tasks:\n"
                      "  fast:   { period: 10ms,  priority: 90, rt_required: "
                      "false, programs: [] }\n"
                      "  normal: { period: 100ms, priority: 50, programs: "
                      "[ps1_level, ps1_seq] }\n"
                      "  slow:   { period: 1s,    priority: 20, programs: "
                      "[reports] }\n"),
              Lines{});
    EXPECT_EQ(builtin("tasks.schema.json",
                      "tasks:\n  fast: { period: 5ms, scan_group: g }\n"),
              (Lines{"2:19 CF-204: tasks.fast.period must be between 10ms and "
                     "1h",
                     "2:24 CF-210: tasks.fast.scan_group is reserved and not "
                     "available in this version"}));
}

TEST(BuiltinSchemas, Enumerations) {
    EXPECT_EQ(builtin("enums.schema.json",
                      "enums:\n  run_state: { 0: stopped, 1: running, 3: "
                      "fault, -1: unknown }\n"),
              Lines{});
    EXPECT_EQ(
        builtin("enums.schema.json", "enums:\n  run_state: { 1: a, 0x1: b }\n"),
        Lines{"2:22 CF-111: duplicate key 0x1"});
}

}  // namespace
}  // namespace scada::conf
