// Stage 3: the schema engine (configuration schemas contract 1.2, 10.2,
// Annex C) and the built-in schema files against the contract's examples.

#include "scada/conf/schema.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/conf/types.h"
#include "scada/conf/yaml.h"

#include <gtest/gtest.h>

#include <ctime>

#include <algorithm>
#include <chrono>
#include <cstddef>
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
                     "2:1 CF-201: unknown key \"c\" in f.yaml"}));
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
              Lines{"1:4 CF-206: \"1.5s\" is not a valid duration"});
    EXPECT_EQ(check(json, "d: 61s\n"),
              Lines{"1:4 CF-204: d must be between 10ms and 60s"});
    EXPECT_EQ(check(json, "s: 2GB\n"),
              Lines{"1:4 CF-206: \"2GB\" is not a valid size"});
    EXPECT_EQ(check(json, "p: 101%\n"),
              Lines{"1:4 CF-204: p must be between 0 and 100"});
    EXPECT_EQ(check(json, "p: 10\n"),
              Lines{"1:4 CF-203: p must be a percentage"});
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
              Lines{"1:5 CF-212: \"Pump\" is not a valid identifier name"});
    EXPECT_EQ(check(json, "id: " + std::string(32, 'a') + "\n"),
              Lines{"1:5 CF-212: \"" + std::string(32, 'a') +
                    "\" is not a valid identifier name"});
    EXPECT_EQ(check(json, "tag: ps1..flow\n"),
              Lines{"1:6 CF-212: \"ps1..flow\" is not a valid tag name"});
    EXPECT_EQ(check(json, "tag: " + std::string(128, 'a') + "\n"),
              // The quoted value is cut at 64 bytes (D-110).
              Lines{"1:6 CF-212: \"" + std::string(64, 'a') +
                    "\"... is not a valid tag name"});
}

TEST(Schema, ReservedKeysAndValuesAreCF210) {
    const std::string json = R"({ "type": "object", "properties": {
        "scan_group": { "x-scada-reserved": true },
        "role": { "type": "string", "enum": ["connect", "listen"], "x-scada-reserved-values": ["listen"] } } })";
    EXPECT_EQ(check(json, "scan_group: fast\n"),
              Lines{"1:1 CF-210: scan_group is reserved and not available in "
                    "this version"});
    EXPECT_EQ(
        check(json, "role: listen\n"),
        Lines{"1:7 CF-210: role: \"listen\" is reserved and not available "
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
              Lines{"2:1 CF-111: duplicate key \"0x10\""});
    EXPECT_EQ(check(json, "'01': a\n"),
              Lines{"1:1 CF-203: key \"01\" in f.yaml must be an integer"});
    EXPECT_EQ(check(json, "x: a\n"),
              Lines{"1:1 CF-203: key \"x\" in f.yaml must be an integer"});
}

TEST(Schema, BooleanKeys) {
    const std::string json =
        R"({ "type": "object", "x-scada-key-type": "boolean",
        "unevaluatedProperties": { "type": "integer", "x-scada-type": "integer" } })";
    EXPECT_EQ(check(json, "true: 0\nfalse: 1\n"), Lines{});
    EXPECT_EQ(check(json, "yes: 0\n"),
              Lines{"1:1 CF-203: key \"yes\" in f.yaml must be true or false"});
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
              Lines{"2:1 CF-201: unknown key \"parity\" in f.yaml"});
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
              Lines{"3:15 CF-210: node.role: \"master\" is reserved and not "
                    "available in this version"});
    EXPECT_EQ(builtin("system.schema.json",
                      base + "data_plane: { event_queue_capacity: 1000 }\n"),
              Lines{"3:37 CF-203: data_plane.event_queue_capacity must be a "
                    "power of two"});
    EXPECT_EQ(builtin("system.schema.json",
                      base + "data_plane: { processes: { web: {} } }\n"),
              Lines{"3:28 CF-212: \"web\" is not a valid process name"});
    EXPECT_EQ(builtin("system.schema.json",
                      base + "email: { server: mail, from: x, password: p }\n"),
              Lines{"3:43 CF-213: email.password must be a secret reference"});
    EXPECT_EQ(
        builtin("system.schema.json", "unit: { id: Site }\n"),
        (Lines{"1:1 CF-202: f.yaml needs time", "1:7 CF-202: unit needs name",
               "1:13 CF-212: \"Site\" is not a valid unit id name"}));
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
        Lines{"2:22 CF-111: duplicate key \"0x1\""});
}

// --- Review of #131 ----------------------------------------------------------

// The types stage 3 gives the values of the root map of `yaml`, in order.
std::vector<ScalarType> root_types(const std::string& json,
                                   std::string_view yaml) {
    const SchemaSet set = one_schema(json);
    Diagnostics diagnostics;
    const std::optional<Document> document =
        parse_yaml(yaml, "f.yaml", diagnostics);
    if (!document.has_value()) {
        ADD_FAILURE() << "no document";
        return {};
    }
    ScalarTypes types(*document);
    set.validate(*document, types, "t.schema.json", "f.yaml", diagnostics);
    std::vector<ScalarType> out;
    for (const Entry& entry : document->entries(document->root())) {
        out.push_back(types.of(entry.value));
    }
    return out;
}

constexpr std::string_view TYPED_BEHIND_REFERENCES = R"({
  "type": "object",
  "$defs": { "i": { "type": "integer", "x-scada-type": "integer", "maximum": 9 } },
  "properties": {
    "a": { "$ref": "#/$defs/i" },
    "b": { "allOf": [{ "$ref": "#/$defs/i" }] },
    "c": { "oneOf": [{ "$ref": "#/$defs/i" }, { "type": "array" }] }
  } })";

// B1: a scalar typed through `$ref`, `allOf` or `oneOf` keeps the type the
// referenced schema gave it; one that got an error has none.
TEST(Schema, TypesBehindReferencesAndCompositions) {
    const std::string json(TYPED_BEHIND_REFERENCES);
    EXPECT_EQ(root_types(json, "a: 5\nb: 6\nc: 7\n"),
              (std::vector<ScalarType>{ScalarType::INTEGER, ScalarType::INTEGER,
                                       ScalarType::INTEGER}));
    EXPECT_EQ(root_types(json, "a: 50\nb: 60\nc: 70\n"),
              (std::vector<ScalarType>{ScalarType::NONE, ScalarType::NONE,
                                       ScalarType::NONE}));
}

// The processor time of this thread, which other load does not change.
std::chrono::nanoseconds thread_time() {
    timespec now{};
    static_cast<void>(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now));
    return std::chrono::seconds(now.tv_sec) +
           std::chrono::nanoseconds(now.tv_nsec);
}

// B2: typed keys are checked with a sorted set. From 4,096 to 65,536 keys a
// linearithmic check takes about 17.5 times as long and a quadratic one 256
// times; the bound sits between, in processor time of this thread.
TEST(Schema, TypedKeysAreCheckedInLinearithmicTime) {
    constexpr std::size_t SMALL = 4096;
    constexpr std::size_t LARGE = 65536;
    constexpr int RUNS = 3;
    constexpr double MAX_RATIO = 64.0;
    const SchemaSet set = one_schema(R"({ "type": "object",
        "x-scada-key-type": "integer",
        "unevaluatedProperties": { "type": "string" } })");
    const auto fastest = [&set](std::size_t keys) {
        std::string yaml;
        for (std::size_t i = 0; i < keys; ++i) {
            yaml += std::to_string(i) + ": x\n";
        }
        Diagnostics diagnostics;
        const std::optional<Document> document =
            parse_yaml(yaml, "f.yaml", diagnostics);
        if (!document.has_value()) {
            ADD_FAILURE() << "no document";
            return std::chrono::nanoseconds::max();
        }
        auto best = std::chrono::nanoseconds::max();
        for (int run = 0; run < RUNS; ++run) {
            ScalarTypes types(*document);
            const auto start = thread_time();
            set.validate(*document, types, "t.schema.json", "f.yaml",
                         diagnostics);
            best = std::min(best, thread_time() - start);
        }
        EXPECT_EQ(diagnostics.error_count(), 0U);
        return best;
    };
    const auto small = fastest(SMALL);
    const auto large = fastest(LARGE);
    EXPECT_LT(static_cast<double>(large.count()),
              MAX_RATIO * static_cast<double>(small.count()))
        << "small " << small.count() << " ns, large " << large.count() << " ns";
}

// The number of keys of `number` and its kin for which an integer of the
// form beyond 64 bits does not give CF-204.
std::size_t numbers_out_of_range(const std::string& json) {
    std::size_t wrong = 0;
    for (const char* key : {"n", "p", "e"}) {
        for (const char* text :
             {"18446744073709551616", "-9223372036854775809"}) {
            const std::string yaml = std::string(key) + ": " + text + "\n";
            const Lines expected{"1:4 CF-204: " + std::string(key) +
                                 " must be between -9223372036854775808 and "
                                 "18446744073709551615"};
            if (check(json, yaml) != expected) {
                ADD_FAILURE() << yaml;
                ++wrong;
            }
        }
    }
    return wrong;
}

// B3: an integer of the integer form that does not fit is CF-204, never
// CF-203 and never a decimal (D-107).
TEST(Schema, IntegersTooLargeAreOutOfRange) {
    const std::string json = R"({ "type": "object", "properties": {
        "i": { "type": "integer", "x-scada-type": "integer" },
        "u": { "type": "integer", "x-scada-type": "uint64" },
        "n": { "type": "number", "x-scada-type": "number" },
        "p": { "type": "string", "x-scada-type": "number_or_percentage" },
        "e": { "type": "string", "x-scada-type": "number_or_expression" } } })";
    EXPECT_EQ(check(json, "u: 18446744073709551615\n"), Lines{});
    EXPECT_EQ(check(json, "u: 18446744073709551616\n"),
              Lines{"1:4 CF-204: u must be between 0 and "
                    "18446744073709551615"});
    EXPECT_EQ(check(json, "i: 0x10000000000000000\n"),
              Lines{"1:4 CF-204: i must be between -9223372036854775808 and "
                    "9223372036854775807"});
    EXPECT_EQ(numbers_out_of_range(json), 0U);
    EXPECT_EQ(root_types(json, "n: 18446744073709551615\n"),
              std::vector<ScalarType>{ScalarType::INTEGER});
    const std::string keys = R"({ "type": "object",
        "x-scada-key-type": "integer",
        "unevaluatedProperties": { "type": "string" } })";
    EXPECT_EQ(check(keys, "9223372036854775808: x\n"),
              Lines{"1:1 CF-204: key \"9223372036854775808\" in f.yaml must "
                    "be between -9223372036854775808 and "
                    "9223372036854775807"});
}

// S6: text without `%` is not a percentage (CF-203); a decimal of the form
// that is not finite is not a number (CF-203).
TEST(Schema, NumbersPercentagesAndExpressions) {
    const std::string json = R"({ "type": "object", "properties": {
        "d": { "type": "string", "x-scada-type": "number_or_percentage" },
        "e": { "type": "string", "x-scada-type": "number_or_expression" } } })";
    EXPECT_EQ(check(json, "d: abc\n"),
              Lines{"1:4 CF-203: d must be a number or a percentage"});
    EXPECT_EQ(check(json, "d: \"5\"\n"),
              Lines{"1:4 CF-203: d must be a number or a percentage"});
    EXPECT_EQ(check(json, "d: 5x%\n"),
              Lines{"1:4 CF-206: \"5x%\" is not a valid percentage"});
    EXPECT_EQ(check(json, "e: 1e400\n"),
              Lines{"1:4 CF-203: e must be a number or an expression; this "
                    "one is not finite or rounds to zero"});
    EXPECT_EQ(
        root_types(json, "d: 5\ne: k * 2\n"),
        (std::vector<ScalarType>{ScalarType::INTEGER, ScalarType::EXPRESSION}));
}

// S4: a duration that validation accepts fits int64 nanoseconds.
TEST(Schema, DurationsFitNanoseconds) {
    const std::string json = R"({ "type": "object", "properties": {
        "d": { "type": "string", "x-scada-type": "duration" } } })";
    EXPECT_EQ(check(json, "d: 292y\n"), Lines{});
    EXPECT_EQ(check(json, "d: 293y\n"),
              Lines{"1:4 CF-204: d must be at most 9223372036854ms"});
    EXPECT_EQ(check(json, "d: 18446744073709551615ms\n"),
              Lines{"1:4 CF-204: d must be at most 9223372036854ms"});
}

// S5: keys and values are quoted, and a long value is cut so the message
// keeps its end.
TEST(Schema, KeysAndValuesAreQuoted) {
    const std::string json = R"({ "type": "object", "properties": {
        "d": { "type": "string", "x-scada-type": "duration" } },
        "unevaluatedProperties": false })";
    EXPECT_EQ(check(json, "'': 1\n"),
              Lines{"1:1 CF-201: unknown key \"\" in f.yaml"});
    EXPECT_EQ(check(json, "d: " + std::string(65000, 'x') + "\n"),
              Lines{"1:4 CF-206: \"" + std::string(64, 'x') +
                    "\"... is not a valid duration"});
}

// S1: errors outside a `oneOf` go to the diagnostics at once; a branch
// holds at most 1,000 and counts the rest.
TEST(Schema, ABranchHoldsAtMostTheReportedLimit) {
    constexpr std::size_t BAD_ITEMS = 1500;
    const SchemaSet set = one_schema(R"({ "oneOf": [
        { "type": "array", "items": { "type": "integer", "x-scada-type": "integer" } },
        { "type": "string" } ] })");
    std::string yaml;
    for (std::size_t i = 0; i < BAD_ITEMS; ++i) {
        yaml += "- x\n";
    }
    Diagnostics diagnostics;
    const std::optional<Document> document =
        parse_yaml(yaml, "f.yaml", diagnostics);
    if (!document.has_value()) {
        ADD_FAILURE() << "no document";
        return;
    }
    ScalarTypes types(*document);
    set.validate(*document, types, "t.schema.json", "f.yaml", diagnostics);
    Report report;
    diagnostics.move_into(report);
    EXPECT_EQ(report.error_count, BAD_ITEMS);
    EXPECT_EQ(report.errors.size(), Diagnostics::REPORTED_LIMIT);
}

// S2: a keyword's value is checked when the schema compiles.
TEST(Schema, KeywordValuesAreCheckedAtCompileTime) {
    for (
        const char* json : {
            R"({ "type": "string", "x-scada-type": "intger" })",
            R"({ "type": "object", "x-scada-key-type": "integr" })",
            R"({ "type": "integer", "x-scada-type": "integer", "minimum": "five" })",
            R"({ "type": "string", "x-scada-type": "duration", "x-scada-min": "10 ms" })",
            R"({ "type": "string", "x-scada-type": "duration", "x-scada-max": "1hour" })",
            R"({ "type": "string", "x-scada-type": "duration", "minimum": 1 })",
            R"({ "type": "integer", "x-scada-type": "integer", "x-scada-min": "1s" })",
            R"({ "type": "object", "enum": ["a"] })",
            R"({ "type": "string", "x-scada-reserved": "yes" })",
            R"({ "type": "string", "items": { "type": "string" } })",
            R"({ "type": "array", "required": ["a"] })",
        }) {
        std::string problem;
        const Result<SchemaSet> set = SchemaSet::compile(
            {SchemaSource{.name = "t.schema.json", .text = json}}, problem);
        EXPECT_FALSE(set.ok()) << json;
        EXPECT_FALSE(problem.empty()) << json;
    }
}

// S3: a cycle through `$ref`, `allOf` or `oneOf` does not compile;
// recursion through `properties` does, and validates nested input.
TEST(Schema, ReferenceCyclesAreRefused) {
    for (
        const char* json : {
            R"({ "$ref": "#/$defs/x", "$defs": { "x": { "$ref": "#/$defs/x" } } })",
            R"({ "$defs": { "x": { "allOf": [{ "$ref": "#/$defs/y" }] },
                             "y": { "oneOf": [{ "$ref": "#/$defs/x" }] } },
                  "$ref": "#/$defs/x" })",
        }) {
        std::string problem;
        const Result<SchemaSet> set = SchemaSet::compile(
            {SchemaSource{.name = "t.schema.json", .text = json}}, problem);
        EXPECT_FALSE(set.ok()) << json;
        EXPECT_NE(problem.find("cycle"), std::string::npos) << problem;
    }
    const std::string tree = R"({ "$ref": "#/$defs/node", "$defs": { "node": {
        "type": "object",
        "properties": { "name": { "type": "string" },
                        "child": { "$ref": "#/$defs/node" } },
        "unevaluatedProperties": false } } })";
    EXPECT_EQ(
        check(tree, "name: a\nchild:\n  name: b\n  child:\n    name: c\n"),
        Lines{});
    EXPECT_EQ(check(tree, "child:\n  child:\n    x: 1\n"),
              Lines{"3:5 CF-201: unknown key \"x\" in child.child"});
}

// --- Delta check of #131 ----------------------------------------------------

constexpr std::string_view PARTS_IN_BOTH_ORDERS = R"({
  "type": "object",
  "$defs": { "i": { "type": "integer", "x-scada-type": "integer" } },
  "properties": {
    "a": { "allOf": [{ "$ref": "#/$defs/i" }, { "enum": ["1", "5"] }] },
    "b": { "allOf": [{ "enum": ["1", "5"] }, { "$ref": "#/$defs/i" }] },
    "c": { "allOf": [{ "$ref": "#/$defs/i" }, { "type": "integer" }] },
    "d": { "allOf": [{ "type": "integer" }, { "$ref": "#/$defs/i" }] }
  } })";

// 1: a part without `x-scada-type` does not type over a sibling part, in
// either order.
TEST(Schema, AllOfPartsTypeTheSameInEitherOrder) {
    EXPECT_EQ(root_types(std::string(PARTS_IN_BOTH_ORDERS),
                         "a: 5\nb: 5\nc: 5\nd: 5\n"),
              std::vector<ScalarType>(4, ScalarType::INTEGER));
}

// 2: more keyword checks at compile time.
TEST(Schema, KeyTypesAndTypesAreCheckedAtCompileTime) {
    for (
        const char* json : {
            R"({ "type": "object", "x-scada-key-type": "integer" })",
            R"({ "type": "object", "x-scada-key-type": "integer", "unevaluatedProperties": false })",
            R"({ "properties": { "a": {} }, "items": {} })",
            R"({ "enum": ["a"], "required": ["a"] })",
            R"({ "type": "integer", "x-scada-type": "duration" })",
            R"({ "type": "string", "x-scada-type": "integer" })",
            R"({ "type": "boolean", "x-scada-type": "number" })",
        }) {
        std::string problem;
        const Result<SchemaSet> set = SchemaSet::compile(
            {SchemaSource{.name = "t.schema.json", .text = json}}, problem);
        EXPECT_FALSE(set.ok()) << json;
    }
    for (
        const char* json : {
            R"({ "type": ["number", "string"], "x-scada-type": "number_or_percentage" })",
            R"({ "type": "number", "x-scada-type": "decimal" })",
            R"({ "properties": { "a": {} }, "required": ["a"] })",
        }) {
        std::string problem;
        const Result<SchemaSet> set = SchemaSet::compile(
            {SchemaSource{.name = "t.schema.json", .text = json}}, problem);
        EXPECT_TRUE(set.ok()) << json << ": " << problem;
    }
}

// 3: a duration or a size of the form beyond 64 bits is CF-204, not CF-206.
TEST(Schema, DurationsAndSizesBeyond64BitsAreOutOfRange) {
    const std::string json = R"({ "type": "object", "properties": {
        "d": { "type": "string", "x-scada-type": "duration" },
        "s": { "type": "string", "x-scada-type": "size" } } })";
    EXPECT_EQ(check(json, "d: 5124095576030431h\n"),
              Lines{"1:4 CF-204: d must be at most 9223372036854ms"});
    EXPECT_EQ(check(json, "d: 18446744073709551616ms\n"),
              Lines{"1:4 CF-204: d must be at most 9223372036854ms"});
    EXPECT_EQ(check(json, "s: 16777216TiB\n"),
              Lines{"1:4 CF-204: s must be at most 18446744073709551615B"});
    // EiB is not a unit of section 2.2: not of the form.
    EXPECT_EQ(check(json, "s: 16EiB\n"),
              Lines{"1:4 CF-206: \"16EiB\" is not a valid size"});
}

// 5 (N1): a decimal key takes any integer or decimal form whose nearest
// binary64 is finite (D-107).
TEST(Schema, DecimalKeysTakeTheNearestBinary64) {
    const std::string json = R"({ "type": "object", "properties": {
        "gain": { "type": "number", "x-scada-type": "decimal" } } })";
    EXPECT_EQ(check(json, "gain: 99999999999999999999999\n"), Lines{});
    EXPECT_EQ(check(json, "gain: 0x10000000000000000\n"), Lines{});
    EXPECT_EQ(root_types(json, "gain: 99999999999999999999999\n"),
              std::vector<ScalarType>{ScalarType::DECIMAL});
    EXPECT_EQ(check(json, "gain: 1e400\n"),
              Lines{"1:7 CF-203: gain must be a decimal number"});
}

// 6 (N2): a capped branch keeps the first errors in report order.
TEST(Schema, ACappedBranchKeepsTheFirstErrorsInReportOrder) {
    constexpr std::size_t BAD_ITEMS = 1500;
    const SchemaSet set = one_schema(R"({ "oneOf": [
        { "type": "object", "required": ["need"], "properties": {
            "list": { "type": "array", "items": { "type": "integer", "x-scada-type": "integer" } } } },
        { "type": "string" } ] })");
    std::string yaml = "list:\n";
    for (std::size_t i = 0; i < BAD_ITEMS; ++i) {
        yaml += "  - x\n";
    }
    Diagnostics diagnostics;
    const std::optional<Document> document =
        parse_yaml(yaml, "f.yaml", diagnostics);
    if (!document.has_value()) {
        ADD_FAILURE() << "no document";
        return;
    }
    ScalarTypes types(*document);
    set.validate(*document, types, "t.schema.json", "f.yaml", diagnostics);
    Report report;
    diagnostics.move_into(report);
    EXPECT_EQ(report.error_count, BAD_ITEMS + 1);
    ASSERT_FALSE(report.errors.empty());
    EXPECT_EQ(format(report.errors.front()),
              "f.yaml:1:1: error CF-202: f.yaml needs need");
}

// The special types of slice 4 follow the integer rules of D-107.
TEST(Schema, SpecialTypesFollowTheIntegerRules) {
    const std::string json = R"({ "type": "object", "properties": {
        "raw": { "type": ["integer", "string"], "x-scada-type": "raw_pattern" },
        "nb": { "type": ["number", "boolean"], "x-scada-type": "number_or_boolean" },
        "nt": { "type": ["number", "string"], "x-scada-type": "number_or_tag" } } })";
    EXPECT_EQ(check(json, "raw: -1\nnb: true\nnt: ps1.flow\n"), Lines{});
    EXPECT_EQ(check(json, "raw: 18446744073709551616\n"),
              Lines{"1:6 CF-204: raw must be between -9223372036854775808 and "
                    "18446744073709551615"});
    EXPECT_EQ(check(json, "raw: -9223372036854775809\n"),
              Lines{"1:6 CF-204: raw must be between -9223372036854775808 and "
                    "18446744073709551615"});
    EXPECT_EQ(check(json, "raw: x\n"),
              Lines{"1:6 CF-203: raw must be a raw pattern"});
    EXPECT_EQ(check(json, "nb: 18446744073709551616\n"),
              Lines{"1:5 CF-204: nb must be between -9223372036854775808 and "
                    "18446744073709551615"});
    EXPECT_EQ(check(json, "nb: yes\n"),
              Lines{"1:5 CF-203: nb must be a number or true or false"});
    EXPECT_EQ(check(json, "nt: Bad\n"),
              Lines{"1:5 CF-212: \"Bad\" is not a valid tag name"});
    EXPECT_EQ(root_types(json, "raw: 5\nnb: 1.5\nnt: 2\n"),
              (std::vector<ScalarType>{ScalarType::INTEGER, ScalarType::DECIMAL,
                                       ScalarType::INTEGER}));
}

// The types of Annex C added with the template and channel schemas, at the
// edges of their grammars.
constexpr std::string_view TEMPLATE_TYPES =
    R"({ "type": "object", "properties": {
        "dev": { "type": "string", "x-scada-type": "dev_path" },
        "res": { "type": "string", "x-scada-type": "resolution" },
        "tmpl": { "type": "string", "x-scada-type": "point_name_template" },
        "pat": { "type": "string", "x-scada-type": "name_pattern" },
        "ka": { "type": "string", "x-scada-type": "duration_or_off", "x-scada-min": "1s", "x-scada-max": "1h" },
        "rawn": { "type": ["integer", "string"], "x-scada-type": "raw_pattern_or_nan" },
        "ipl": { "type": "string", "x-scada-type": "ip_literal_host" },
        "ce": { "type": ["string", "boolean"], "x-scada-type": "config_expression" },
        "fv": { "type": ["string", "integer"], "x-scada-type": "for_value" },
        "noe": { "type": ["integer", "string"], "x-scada-type": "number_or_expression", "minimum": 0, "maximum": 15 } } })";

TEST(Schema, TheTypesOfTemplatesAndChannelsAtTheirLimits) {
    const std::string json(TEMPLATE_TYPES);
    const std::string at_limit = "/dev/" + std::string(250, 'a');
    EXPECT_EQ(check(json,
                    "dev: /dev/serial/by-id/usb adapter 1\n"
                    "res: 1s\ntmpl: \"pv{n}.v\"\npat: \"pv*.{n}?\"\n"
                    "ka: off\nrawn: nan\nipl: \"2001:db8::1\"\n"
                    "ce: true\nfv: 9223372036854775807\nnoe: 15\n"),
              Lines{});
    EXPECT_EQ(check(json, "dev: " + at_limit +
                              "\nres: 9223372036854775807ns\n"
                              "ka: 1h\nrawn: 0xFFFFFFFFFFFFFFFF\nfv: \"1..n\"\n"
                              "noe: \"n + 1\"\n"),
              Lines{});
}

TEST(Schema, DevicePathsAreAbsoluteUnderDev) {
    const std::string json(TEMPLATE_TYPES);
    const std::string at_limit = "/dev/" + std::string(250, 'a');
    EXPECT_EQ(check(json, "dev: " + at_limit + "a\n"),
              Lines{"1:6 CF-203: dev must be an absolute path under /dev/"});
    for (const std::string_view path :
         {"/dev", "/dev/", "/dev//a", "/dev/a/", "/dev/./a", "/dev/a/..",
          "dev/a", "/sys/a", R"("/dev/a\tb")"}) {
        SCOPED_TRACE(path);
        EXPECT_EQ(check(json, "dev: " + std::string(path) + "\n").size(), 1U);
    }
}

TEST(Schema, ResolutionsFitInt64Nanoseconds) {
    const std::string json(TEMPLATE_TYPES);
    EXPECT_EQ(check(json, "res: 1ns\n"), Lines{});
    EXPECT_EQ(check(json, "res: 9223372036854775808ns\n"),
              Lines{"1:6 CF-204: res must be between 1ns and "
                    "9223372036854775807ns"});
    EXPECT_EQ(check(json, "res: 18446744073709551616ns\n"),
              Lines{"1:6 CF-204: res must be between 1ns and "
                    "9223372036854775807ns"});
    for (const std::string_view text :
         {"0ns", "0x10ns", "01ns", "2ms", "1us", "ns", "-1ns", "1.5ns"}) {
        SCOPED_TRACE(text);
        EXPECT_EQ(check(json, "res: " + std::string(text) + "\n"),
                  Lines{"1:6 CF-203: res must be 1s, 1ms or <n>ns"});
    }
}

TEST(Schema, NamesAndDurationsOrOff) {
    const std::string json(TEMPLATE_TYPES);
    EXPECT_EQ(
        check(json, "tmpl: \"pv{n\"\npat: \"pv{1n}*\"\n"),
        (Lines{"1:7 CF-212: \"pv{n\" is not a valid point name",
               "2:6 CF-212: \"pv{1n}*\" is not a valid name pattern name"}));
    EXPECT_EQ(check(json, "ka: 999ms\n"),
              Lines{"1:5 CF-204: ka must be between 1s and 1h"});
    EXPECT_EQ(check(json, "ka: 3601s\n"),
              Lines{"1:5 CF-204: ka must be between 1s and 1h"});
    EXPECT_EQ(check(json, "ka: Off\n"),
              Lines{"1:5 CF-206: \"Off\" is not a valid duration"});
}

TEST(Schema, RawPatternsAndHosts) {
    const std::string json(TEMPLATE_TYPES);
    // Plain only: a quoted nan or number is text (Annex C).
    EXPECT_EQ(check(json, "rawn: \"nan\"\n"),
              Lines{"1:7 CF-203: rawn must be a raw pattern"});
    EXPECT_EQ(check(json, "rawn: NaN\n"),
              Lines{"1:7 CF-203: rawn must be a raw pattern"});
    EXPECT_EQ(check(json, "rawn: -9223372036854775809\n"),
              Lines{"1:7 CF-204: rawn must be between -9223372036854775808 "
                    "and 18446744073709551615"});
    EXPECT_EQ(check(json, "ipl: plc.example\n"),
              Lines{"1:6 CF-210: ipl: a host name is reserved and not "
                    "available in this version"});
    EXPECT_EQ(check(json, "ipl: 192.0.2\n"),
              Lines{"1:6 CF-203: ipl must be an IP address"});
}

TEST(Schema, ExpressionsAndRanges) {
    const std::string json(TEMPLATE_TYPES);
    EXPECT_EQ(check(json, "fv: 9223372036854775808\n"),
              Lines{"1:5 CF-204: fv must be between -9223372036854775808 and "
                    "9223372036854775807"});
    EXPECT_EQ(check(json, "noe: 16\n"),
              Lines{"1:6 CF-204: noe must be between 0 and 15"});
    EXPECT_EQ(check(json, "noe: 1.0\n"),
              Lines{"1:6 CF-203: noe must be an integer or an expression"});
    EXPECT_EQ(
        root_types(json, "ce: true\nfv: 3\nnoe: 2\n"),
        (std::vector<ScalarType>{ScalarType::EXPRESSION, ScalarType::INTEGER,
                                 ScalarType::INTEGER}));
}

// `string` and `expression` take any kind of scalar, but their `type` must
// name `string` (S2 of #131, C2 of #132).
TEST(Schema, TextTypesNameStringInTheirType) {
    for (
        const std::string_view json :
        {R"({ "type": "integer", "x-scada-type": "string" })",
         R"({ "type": "boolean", "x-scada-type": "string" })",
         R"({ "type": "number", "x-scada-type": "expression" })",
         R"({ "type": ["integer", "boolean"], "x-scada-type": "config_expression" })"}) {
        SCOPED_TRACE(json);
        std::string problem;
        EXPECT_FALSE(
            SchemaSet::compile({SchemaSource{.name = "t.schema.json",
                                             .text = std::string(json)}},
                               problem)
                .ok());
        EXPECT_NE(problem.find("disagrees with x-scada-type"),
                  std::string::npos)
            << problem;
    }
    EXPECT_EQ(
        check(R"({ "type": ["string", "integer"], "x-scada-type": "string" })",
              "5\n"),
        Lines{});
}

// Inside a `oneOf` branch too, a node of the wrong kind gets one error,
// however many schemas of the branch state its kind.
TEST(Schema, OneKindErrorPerNodeInABranch) {
    EXPECT_EQ(check(R"({ "oneOf": [ { "allOf": [ { "type": "array" },
                                                 { "type": "array" } ] } ] })",
                    "5\n"),
              Lines{"1:1 CF-203: f.yaml must be a list"});
}

// A recursion that passes an `allOf` to a schema with keys of its own does
// not compile, with a message that says why (D-107).
TEST(Schema, ARecursionThroughAnAllOfIsRefused) {
    const std::string recursive = R"({
        "$defs": {
            "common": { "properties": { "name": { "type": "string" } } },
            "block": {
                "allOf": [ { "$ref": "#/$defs/common" } ],
                "properties": { "children": { "type": "array", "items": { "$ref": "#/$defs/block" } } }
            }
        },
        "$ref": "#/$defs/block" })";
    std::string problem;
    EXPECT_FALSE(
        SchemaSet::compile(
            {SchemaSource{.name = "t.schema.json", .text = recursive}}, problem)
            .ok());
    EXPECT_NE(problem.find("reaches itself through properties"),
              std::string::npos)
        << problem;
    EXPECT_NE(problem.find("bound per child key"), std::string::npos)
        << problem;
}

// A `oneOf` of four branches, nested `depth` times around an integer.
std::string nested_one_of(int depth) {
    std::string nested = R"({ "type": "integer" })";
    constexpr int BRANCHES = 4;
    for (int level = 0; level < depth; ++level) {
        std::string next = R"({ "oneOf": [ )";
        for (int branch = 0; branch < BRANCHES; ++branch) {
            next += branch == 0 ? "" : ", ";
            next += nested;
        }
        next += " ] }";
        nested = std::move(next);
    }
    return nested;
}

// The bound on nested `oneOf` and `allOf` (D-107): the built-in schemas
// check one node at most 21 times; a set that could pass 64 does not
// compile, nor a recursion through a `oneOf`.
TEST(Schema, NestedOneOfIsBounded) {
    constexpr std::size_t BUILTIN_CHECKS = 21;
    EXPECT_EQ(SchemaSet::builtin().checks_per_node(), BUILTIN_CHECKS);
    // 4 x 4 x 4 branches, each also checked by the level above: more than
    // 64 checks of the innermost node.
    const std::string nested = nested_one_of(3);
    std::string problem;
    EXPECT_FALSE(
        SchemaSet::compile(
            {SchemaSource{.name = "t.schema.json", .text = nested}}, problem)
            .ok());
    EXPECT_NE(problem.find("more than 64 times"), std::string::npos) << problem;
    // Two levels of four: 4 + 4 x 4 + 1 checks at most, within the bound.
    const std::string two_levels =
        R"({ "oneOf": [ { "oneOf": [ {}, {}, {}, {} ] }, {}, {}, {} ] })";
    const Result<SchemaSet> fits = SchemaSet::compile(
        {SchemaSource{.name = "t.schema.json", .text = two_levels}}, problem);
    ASSERT_TRUE(fits.ok()) << problem;
    EXPECT_LE(fits.value().checks_per_node(), 64U);
    // A recursion down the document through a `oneOf` multiplies at every
    // level: refused.
    const std::string recursive = R"({ "$ref": "#/$defs/n", "$defs": { "n": {
        "oneOf": [ { "type": "object", "properties": { "c": { "$ref": "#/$defs/n" } } },
                   { "type": "object", "properties": { "d": { "$ref": "#/$defs/n" } } } ] } } })";
    EXPECT_FALSE(
        SchemaSet::compile(
            {SchemaSource{.name = "t.schema.json", .text = recursive}}, problem)
            .ok());
}

// --- SchemaSet::check_value: a value computed at expansion (D-115) ---------

// "<code>: <message>" of each error `check_value` gives; the status's message
// when it fails.
Lines value_errors(const SchemaSet& set, std::string_view schema,
                   const std::vector<std::string>& path, std::string_view text,
                   std::string_view label) {
    const Result<std::vector<ValueError>> errors =
        set.check_value(schema, path, text, label);
    if (!errors.ok()) {
        return {std::string(errors.status().message())};
    }
    Lines lines;
    for (const ValueError& error : errors.value()) {
        lines.push_back("CF-" + std::to_string(error.code) + ": " +
                        error.message);
    }
    return lines;
}

constexpr std::string_view MODBUS_DEVICE = "modbus.schema.json#/$defs/device";

// The keys that an expression computes, by their paths below a device: the
// edges pass, a value past them is the CF-204 of stage 3.
TEST(CheckValue, TheKeysOfAComputedValue) {
    const SchemaSet& set = SchemaSet::builtin();
    const std::vector<std::string> reg{"points", "[]", "address", "register"};
    EXPECT_EQ(
        value_errors(set, MODBUS_DEVICE, reg, "65536", "address.register"),
        Lines{});
    EXPECT_EQ(
        value_errors(set, MODBUS_DEVICE, reg, "70000", "address.register"),
        Lines{"CF-204: address.register must be between 0 and 65536"});
    EXPECT_EQ(value_errors(set, MODBUS_DEVICE, reg, "-1", "address.register"),
              Lines{"CF-204: address.register must be between 0 and 65536"});
    const std::vector<std::string> bit{"points", "[]", "address", "bit"};
    EXPECT_EQ(value_errors(set, MODBUS_DEVICE, bit, "15", "address.bit"),
              Lines{});
    EXPECT_EQ(value_errors(set, MODBUS_DEVICE, bit, "16", "address.bit"),
              Lines{"CF-204: address.bit must be between 0 and 15"});
    const std::vector<std::string> divide{"points", "[]", "scale", "divide_by"};
    EXPECT_EQ(value_errors(set, MODBUS_DEVICE, divide, "1", "scale.divide_by"),
              Lines{});
    EXPECT_EQ(value_errors(set, MODBUS_DEVICE, divide, "0", "scale.divide_by"),
              Lines{"CF-204: scale.divide_by must be between 1 and any"});
}

// The integer form is a rule of the key too: a REAL where the key takes an
// integer is CF-203. The device's own identity checks and an override are
// paths of their own, through "unevaluatedProperties".
TEST(CheckValue, TheFormAndOtherPaths) {
    const SchemaSet& set = SchemaSet::builtin();
    EXPECT_EQ(value_errors(set, MODBUS_DEVICE,
                           {"points", "[]", "address", "register"}, "1.5",
                           "address.register"),
              Lines{"CF-203: address.register must be an integer or an "
                    "expression"});
    EXPECT_EQ(value_errors(set, MODBUS_DEVICE,
                           {"identify", "[]", "address", "register"}, "70000",
                           "identify[0].address.register"),
              Lines{"CF-204: identify[0].address.register must be between 0 "
                    "and 65536"});
    EXPECT_EQ(
        value_errors(set, MODBUS_DEVICE, {"overrides", "p", "address", "bit"},
                     "16", "address.bit"),
        Lines{"CF-204: address.bit must be between 0 and 15"});
}

// Every rule of the key applies, not only "minimum" and "maximum": "enum",
// and "x-scada-min" and "x-scada-max" of a type with a unit.
TEST(CheckValue, EveryRuleOfTheKey) {
    const SchemaSet set =
        one_schema(R"({"type": "object", "properties": {)"
                   R"("mode": {"type": "string", "enum": ["a", "b"]},)"
                   R"("wait": {"type": "string", "x-scada-type": "duration",)"
                   R"( "x-scada-min": "1s", "x-scada-max": "60s"}}})");
    EXPECT_EQ(value_errors(set, "t.schema.json", {"mode"}, "a", "mode"),
              Lines{});
    EXPECT_EQ(value_errors(set, "t.schema.json", {"mode"}, "c", "mode"),
              Lines{"CF-205: mode must be one of a, b"});
    EXPECT_EQ(value_errors(set, "t.schema.json", {"wait"}, "60s", "wait"),
              Lines{});
    // The message of stage 3 for the value written.
    EXPECT_EQ(check_with(set, "t.schema.json", "wait: 2m\n"),
              Lines{"1:7 CF-204: wait must be between 1s and 60s"});
    EXPECT_EQ(value_errors(set, "t.schema.json", {"wait"}, "2m", "wait"),
              Lines{"CF-204: wait must be between 1s and 60s"});
}

// Where an "allOf" or a "oneOf" joins several schemas that give the key a
// rule, the value meets each.
TEST(CheckValue, EverySchemaJoinedAtAStep) {
    const SchemaSet set = one_schema(
        R"({"$defs": {)"
        R"("low": {"properties": {"n": {"type": "integer",)"
        R"( "x-scada-type": "integer", "maximum": 10}}},)"
        R"("high": {"properties": {"n": {"type": "integer",)"
        R"( "x-scada-type": "integer", "minimum": 5}}}},)"
        R"("type": "object",)"
        R"("allOf": [{"$ref": "#/$defs/low"}], "oneOf": [{"$ref": "#/$defs/high"}]})");
    EXPECT_EQ(value_errors(set, "t.schema.json", {"n"}, "7", "n"), Lines{});
    EXPECT_EQ(value_errors(set, "t.schema.json", {"n"}, "11", "n"),
              Lines{"CF-204: n must be between any and 10"});
    EXPECT_EQ(value_errors(set, "t.schema.json", {"n"}, "4", "n"),
              Lines{"CF-204: n must be between 5 and any"});
}

// The rules of a key are found once and check many values, with the same
// results as one call each.
TEST(CheckValue, TheRulesOfAKeyFoundOnce) {
    const SchemaSet& set = SchemaSet::builtin();
    const std::vector<std::string> bit{"points", "[]", "address", "bit"};
    const Result<KeyRules> rules = set.key_rules(MODBUS_DEVICE, bit);
    ASSERT_TRUE(rules.ok());
    const Result<std::vector<ValueError>> good =
        set.check_value(rules.value(), "15", "address.bit");
    ASSERT_TRUE(good.ok());
    EXPECT_TRUE(good.value().empty());
    const Result<std::vector<ValueError>> errors =
        set.check_value(rules.value(), "16", "address.bit");
    ASSERT_TRUE(errors.ok());
    ASSERT_EQ(errors.value().size(), 1U);
    EXPECT_EQ(errors.value().front().message,
              "address.bit must be between 0 and 15");
    const std::vector<std::string> nonsense{"points", "nonsense"};
    EXPECT_FALSE(set.key_rules(MODBUS_DEVICE, nonsense).ok());
    // Rules of another set are refused, not followed (S4 of the review of
    // #149).
    const SchemaSet other = one_schema(
        R"({"type": "object", "properties": {"n": {"type": "integer",)"
        R"( "x-scada-type": "integer", "maximum": 1}}})");
    const Result<std::vector<ValueError>> foreign =
        other.check_value(rules.value(), "1", "n");
    ASSERT_FALSE(foreign.ok());
    EXPECT_EQ(foreign.status().code(), StatusCode::INTERNAL);
}

// The branches of a `oneOf` that give one key different rules: a computed
// value alone cannot choose its branch, so `key_rules` refuses the key (M8
// of the review of #149); no built-in schema has that shape.
TEST(CheckValue, AKeySplitByAOneOf) {
    const SchemaSet set = one_schema(
        R"({"type": "object", "oneOf": [)"
        R"({"properties": {"t": {"const": "coil"}, "n": {"type": "integer",)"
        R"( "x-scada-type": "integer", "maximum": 10}}},)"
        R"({"properties": {"t": {"const": "holding"}, "n": {"type":)"
        R"( "integer", "x-scada-type": "integer", "minimum": 20}}}]})");
    EXPECT_EQ(value_errors(set, "t.schema.json", {"n"}, "25", "n"),
              Lines{"check_value: a oneOf gives the key different rules"});
    EXPECT_EQ(
        set.keys_split_by_one_of(),
        (std::vector<std::string>{"t.schema.json: n", "t.schema.json: t"}));
    EXPECT_EQ(SchemaSet::builtin().keys_split_by_one_of(),
              std::vector<std::string>{});
}

// A schema or a key that gives no rule is a defect of the caller: INTERNAL,
// never a value taken as valid.
TEST(CheckValue, AKeyWithoutARuleIsInternal) {
    const SchemaSet& set = SchemaSet::builtin();
    EXPECT_EQ(value_errors(set, "none.schema.json", {"n"}, "1", "n"),
              Lines{"check_value: the schema is not in the set"});
    EXPECT_EQ(value_errors(set, MODBUS_DEVICE,
                           {"points", "[]", "address", "nonsense"}, "1",
                           "address.nonsense"),
              Lines{"check_value: the schema gives the key no rule"});
    const Result<std::vector<ValueError>> failed = set.check_value(
        MODBUS_DEVICE, std::vector<std::string>{"points", "x"}, "1", "x");
    ASSERT_FALSE(failed.ok());
    EXPECT_EQ(failed.status().code(), StatusCode::INTERNAL);
}

}  // namespace
}  // namespace scada::conf
