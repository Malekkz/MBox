// Fuzz target: stage 3, the schema validator (configuration schemas contract
// 1.2, 10.2). The input is one file; once it parses, it is validated against
// every built-in schema and against a test schema that uses every keyword and
// every type of the validator. Properties: every error points inside the
// input and has a message; the count is at least the list; the table of
// types covers the document.

#include "scada/common/panic.h"
#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/conf/schema.h"
#include "scada/conf/types.h"
#include "scada/conf/yaml.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

// Every keyword and type of the validator, so that the fuzzer reaches what
// the built-in schemas do not use yet.
constexpr std::string_view EVERY_KEYWORD = R"({
  "type": "object",
  "$defs": {
    "count": { "type": "integer", "x-scada-type": "integer", "minimum": -5, "maximum": 5 },
    "name": { "type": "string", "x-scada-type": "identifier" }
  },
  "properties": {
    "i": { "$ref": "#/$defs/count" },
    "u": { "type": "integer", "x-scada-type": "uint64", "maximum": 100 },
    "p2": { "type": "integer", "x-scada-type": "power_of_two" },
    "dec": { "type": "number", "x-scada-type": "decimal", "minimum": 0.5 },
    "num": { "type": "number", "x-scada-type": "number" },
    "b": { "type": "boolean", "x-scada-type": "boolean" },
    "d": { "type": "string", "x-scada-type": "duration", "x-scada-min": "10ms", "x-scada-max": "1h" },
    "s": { "type": "string", "x-scada-type": "size", "x-scada-max": "1GiB" },
    "pc": { "type": "string", "x-scada-type": "percentage", "minimum": 0, "maximum": 100 },
    "np": { "type": ["number", "string"], "x-scada-type": "number_or_percentage" },
    "ne": { "type": ["number", "string"], "x-scada-type": "number_or_expression" },
    "ex": { "type": "string", "x-scada-type": "expression" },
    "t": { "type": "string", "x-scada-type": "text", "x-scada-min": "1", "x-scada-max": "16" },
    "names": { "type": "array", "minItems": 1, "maxItems": 4,
               "items": { "oneOf": [
                 { "type": "string", "x-scada-type": "tag_name" },
                 { "type": "string", "x-scada-type": "device_name" },
                 { "type": "string", "x-scada-type": "unit_id" } ] } },
    "host": { "type": "string", "x-scada-type": "host" },
    "hn": { "type": "string", "x-scada-type": "host_name" },
    "ip": { "type": "string", "x-scada-type": "ip_address" },
    "net": { "type": "string", "x-scada-type": "cidr" },
    "tz": { "type": "string", "x-scada-type": "time_zone" },
    "at": { "type": "string", "x-scada-type": "time_of_day" },
    "url": { "type": "string", "x-scada-type": "https_url" },
    "proc": { "type": "string", "x-scada-type": "process_name" },
    "poll": { "type": "string", "x-scada-type": "poll_group_name" },
    "tier": { "type": "string", "x-scada-type": "tier_name" },
    "proto": { "type": "string", "x-scada-type": "protocol_name" },
    "point": { "type": "string", "x-scada-type": "point_name" },
    "tmpl": { "type": "string", "x-scada-type": "point_name_template" },
    "pat": { "type": "string", "x-scada-type": "name_pattern" },
    "dev": { "type": "string", "x-scada-type": "dev_path" },
    "res": { "type": "string", "x-scada-type": "resolution" },
    "raw": { "type": ["integer", "string"], "x-scada-type": "raw_pattern" },
    "rawn": { "type": ["integer", "string"], "x-scada-type": "raw_pattern_or_nan" },
    "nb": { "type": ["number", "boolean"], "x-scada-type": "number_or_boolean" },
    "nt": { "type": ["number", "string"], "x-scada-type": "number_or_tag", "minimum": -10 },
    "ka": { "type": "string", "x-scada-type": "duration_or_off", "x-scada-min": "1s" },
    "ipl": { "type": "string", "x-scada-type": "ip_literal_host" },
    "mode": { "type": "string", "enum": ["auto", "manual", "off"], "x-scada-reserved-values": ["off"] },
    "one": { "const": 1, "type": "integer", "x-scada-type": "integer" },
    "old": { "x-scada-reserved": true },
    "pw": { "x-scada-secret": true },
    "either": { "oneOf": [
      { "type": "object", "properties": { "a": { "$ref": "#/$defs/count" } }, "unevaluatedProperties": false },
      { "type": "array", "items": { "$ref": "#/$defs/name" } },
      { "type": "string", "x-scada-type": "duration" } ] },
    "both": { "allOf": [
      { "type": "object", "properties": { "x": { "$ref": "#/$defs/count" } } },
      { "properties": { "y": { "$ref": "#/$defs/name" } } } ],
      "unevaluatedProperties": false },
    "ints": { "type": "object", "x-scada-key-type": "integer",
              "unevaluatedProperties": { "$ref": "#/$defs/name" } },
    "flags": { "type": "object", "x-scada-key-type": "boolean",
               "unevaluatedProperties": { "type": "string" } },
    "ids": { "type": "object", "x-scada-key-type": "identifier",
             "unevaluatedProperties": { "type": "integer", "x-scada-type": "integer" } }
  },
  "x-scada-exclusive": [["d", "s"], ["u", "p2"]],
  "dependentRequired": { "url": ["host"] },
  "required": ["i"],
  "unevaluatedProperties": false
})";

const scada::conf::SchemaSet& every_keyword() {
    static const scada::conf::SchemaSet SET = [] {
        std::string problem;
        scada::Result<scada::conf::SchemaSet> compiled =
            scada::conf::SchemaSet::compile(
                {scada::conf::SchemaSource{.name = "fuzz.schema.json",
                                           .text = std::string(EVERY_KEYWORD)}},
                problem);
        if (!compiled.ok()) {
            scada::panic("schema_fuzz: the test schema does not compile");
        }
        return std::move(compiled).value();
    }();
    return SET;
}

void check_one(const scada::conf::SchemaSet& set, std::string_view schema,
               const std::string& text, std::size_t lines) {
    scada::conf::Diagnostics diagnostics;
    std::optional<scada::conf::Document> document =
        scada::conf::parse_yaml(text, "f.yaml", diagnostics);
    if (!document) {
        return;
    }
    scada::conf::ScalarTypes types(*document);
    set.validate(*document, types, schema, "f.yaml", diagnostics);
    if (types.size() != document->size()) {
        std::abort();
    }
    scada::conf::Report report;
    diagnostics.move_into(report);
    if (report.error_count < report.errors.size()) {
        std::abort();
    }
    for (const scada::conf::Diagnostic& error : report.errors) {
        if (error.location.line < 1 || error.location.line > lines ||
            error.location.column < 1 || error.message.empty()) {
            std::abort();
        }
    }
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    const std::string text(bytes.begin(), bytes.end());
    std::size_t lines = 1;
    for (const char chr : text) {
        if (chr == '\n') {
            ++lines;
        }
    }
    for (const std::string_view schema :
         {"system.schema.json", "areas.schema.json", "enums.schema.json",
          "logging.schema.json", "tasks.schema.json", "template.schema.json",
          "modbus.schema.json#/$defs/template",
          "modbus.schema.json#/$defs/channel_file",
          "io.schema.json#/$defs/template",
          "io.schema.json#/$defs/channel_file", "tags.schema.json",
          "outputs.schema.json", "alarms.schema.json", "program.schema.json"}) {
        check_one(scada::conf::SchemaSet::builtin(), schema, text, lines);
    }
    check_one(every_keyword(), "fuzz.schema.json", text, lines);
    return 0;
}
