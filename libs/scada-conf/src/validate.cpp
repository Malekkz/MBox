#include "scada/conf/validate.h"

#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/conf/scalar.h"
#include "scada/conf/schema.h"
#include "scada/conf/tree.h"
#include "scada/conf/types.h"
#include "scada/conf/yaml.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::conf {

namespace {

constexpr std::uint16_t CF_STRUCTURED_TEXT = 920;
constexpr std::uint16_t CF_FILE_NAME = 102;
constexpr std::uint16_t CF_MISSING_KEY = 202;
constexpr std::uint16_t CF_FORMAT = 211;
constexpr std::uint16_t CF_VALUE_TYPE = 504;
constexpr std::uint16_t CF_INTEGER_SCALING = 517;

// The tree format this version reads (contract section 11).
constexpr std::uint64_t TREE_FORMAT = 1;

// A file that parsed in stage 2.
struct ParsedFile {
    const TreeFile* source = nullptr;
    Document document;
    // The types stage 3 gives its scalars (D-111).
    ScalarTypes types;
};

// The schema of a file: by its type, and for a channel by its protocol
// directory, for a template by its `protocol` key (section 4.1). A template
// of a protocol this version does not know is checked as a base template;
// the unknown protocol is a stage 5 error. Empty for a file without one.
std::string schema_of(const ParsedFile& file) {
    const PathClass& path_class = file.source->path_class;
    switch (path_class.type) {
        case FileType::SYSTEM:
            return "system.schema.json";
        case FileType::AREAS:
            return "areas.schema.json";
        case FileType::ENUMS:
            return "enums.schema.json";
        case FileType::LOGGING:
            return "logging.schema.json";
        case FileType::TASKS:
            return "tasks.schema.json";
        case FileType::TAGS:
            return "tags.schema.json";
        case FileType::OUTPUTS:
            return "outputs.schema.json";
        case FileType::ALARMS:
            return "alarms.schema.json";
        case FileType::PROGRAM:
            return "program.schema.json";
        case FileType::CHANNEL:
            return path_class.protocol + ".schema.json#/$defs/channel_file";
        case FileType::TEMPLATE:
            break;
        case FileType::STRUCTURED_TEXT:
            return {};
    }
    const Document& document = file.document;
    const NodeId root = document.root();
    if (const std::optional<NodeId> protocol = document.find(root, "protocol");
        protocol && document.kind(*protocol) == NodeKind::SCALAR) {
        const std::string name(document.text(*protocol));
        if (is_known_protocol(name)) {
            return name + ".schema.json#/$defs/template";
        }
    }
    return "template.schema.json";
}

// Rules of system.yaml that a schema cannot state (section 3).
void check_system(const Document& document, const ScalarTypes& types,
                  Diagnostics& diagnostics) {
    const NodeId root = document.root();
    if (const std::optional<NodeId> format = document.find(root, "format")) {
        const std::string text(document.text(*format));
        const std::optional<Integer> number = parse_integer(text);
        if (types.of(*format) == ScalarType::INTEGER && number &&
            (number->magnitude != TREE_FORMAT || number->negative)) {
            diagnostics.error(CF_FORMAT, document.location(*format),
                              "format " + text +
                                  " is not supported; this version reads "
                                  "format 1");
        }
    }
    // time.sync.servers is required unless time.sync.mode is none; the mode
    // defaults to ntp.
    const std::optional<NodeId> time = document.find(root, "time");
    if (!time || document.kind(*time) != NodeKind::MAP) {
        return;
    }
    const std::optional<NodeId> sync = document.find(*time, "sync");
    if (!sync) {
        diagnostics.error(CF_MISSING_KEY, document.location(*time),
                          "time needs sync");
        return;
    }
    if (document.kind(*sync) != NodeKind::MAP) {
        return;
    }
    const std::optional<NodeId> mode = document.find(*sync, "mode");
    const bool none = mode && document.text(*mode) == "none";
    if (!none && !document.find(*sync, "servers")) {
        diagnostics.error(CF_MISSING_KEY, document.location(*sync),
                          "time.sync needs servers");
    }
}

// CF-102 (section 1, at stage 3 since contract 0.6): the name key of a
// template, channel or program file must equal its file name. A missing or
// non-scalar key is the schema's error, not this one.
void check_name_key(const ParsedFile& file, Diagnostics& diagnostics) {
    const Document& document = file.document;
    const NodeId root = document.root();
    if (document.kind(root) != NodeKind::MAP) {
        return;
    }
    std::optional<NodeId> name;
    switch (file.source->path_class.type) {
        case FileType::TEMPLATE:
            name = document.find(root, "template");
            break;
        case FileType::PROGRAM:
            name = document.find(root, "program");
            break;
        case FileType::CHANNEL:
            if (const std::optional<NodeId> channel =
                    document.find(root, "channel");
                channel && document.kind(*channel) == NodeKind::MAP) {
                name = document.find(*channel, "name");
            }
            break;
        default:
            return;
    }
    if (!name || document.kind(*name) != NodeKind::SCALAR) {
        return;
    }
    const std::string text(document.text(*name));
    if (text != file.source->path_class.stem) {
        diagnostics.error(CF_FILE_NAME, document.location(*name),
                          file.source->path + ": the file name must be " +
                              quote_text(text + ".yaml"));
    }
}

// The map `key` of `map`, if `map` is a map that has it and it is a map.
std::optional<NodeId> map_entry(const Document& document, NodeId map,
                                std::string_view key) {
    const std::optional<NodeId> value = document.find(map, key);
    if (!value || document.kind(*value) != NodeKind::MAP) {
        return std::nullopt;
    }
    return value;
}

// CF-517 (contract 4.7, 0.6) where a point object gives both keys: an
// integer value type with a scaling that is not integer. A `divide_by`
// written as an expression is checked after expansion (#103).
void check_integer_scaling(const ParsedFile& file, NodeId point,
                           const std::string& name, Diagnostics& diagnostics) {
    const Document& document = file.document;
    const std::optional<NodeId> value_type = document.find(point, "value_type");
    if (!value_type) {
        return;
    }
    const std::string type(document.text(*value_type));
    if (type != "int64" && type != "uint64") {
        return;
    }
    const auto refuse = [&](NodeId where) {
        diagnostics.error(CF_INTEGER_SCALING, document.location(where),
                          name + ": value_type " + type +
                              " needs an integer scaling; use float64");
    };
    if (const std::optional<NodeId> exponent =
            document.find(point, "exponent_from")) {
        refuse(*exponent);
    }
    const std::optional<NodeId> scale = map_entry(document, point, "scale");
    if (!scale) {
        return;
    }
    for (const std::string_view key : {"gain", "offset"}) {
        const std::optional<NodeId> factor = document.find(*scale, key);
        if (factor && file.types.of(*factor) == ScalarType::DECIMAL) {
            refuse(*factor);
        }
    }
    if (const std::optional<NodeId> divide =
            document.find(*scale, "divide_by")) {
        const ScalarType divisor = file.types.of(*divide);
        const std::optional<Integer> number =
            parse_integer(document.text(*divide));
        const bool above_one = divisor == ScalarType::INTEGER && number &&
                               !number->negative && number->magnitude > 1;
        if (above_one || divisor == ScalarType::DECIMAL) {
            refuse(*divide);
        }
    }
}

// CF-504 (contract 4.7; D-134) where one point object gives both keys: an
// integer value type, or a counter, on a floating raw type (the address's
// `type`, Annex A.2). An enumeration and a double point read a floating raw
// value as a code (5.3, 5.5) and are not refused.
void check_raw_value_type(const ParsedFile& file, NodeId point,
                          const std::string& name, Diagnostics& diagnostics) {
    const Document& document = file.document;
    const std::optional<NodeId> address = map_entry(document, point, "address");
    if (!address) {
        return;
    }
    const std::optional<NodeId> raw = document.find(*address, "type");
    if (!raw) {
        return;
    }
    const std::string raw_type(document.text(*raw));
    if (raw_type != "float32" && raw_type != "float64") {
        return;
    }
    const std::optional<NodeId> kind = document.find(point, "kind");
    const std::string kind_text(kind ? document.text(*kind) : "");
    if (document.find(point, "enum") || kind_text == "double_point") {
        return;
    }
    if (const std::optional<NodeId> value_type =
            document.find(point, "value_type")) {
        const std::string type(document.text(*value_type));
        if (type == "int64" || type == "uint64") {
            diagnostics.error(
                CF_VALUE_TYPE, document.location(*value_type),
                name + ": value_type " + type + " cannot hold " + raw_type);
        }
    }
    std::optional<NodeId> counter_at = document.find(point, "counter");
    if (!counter_at && kind_text == "counter") {
        counter_at = kind;
    }
    if (counter_at) {
        diagnostics.error(CF_VALUE_TYPE, document.location(*counter_at),
                          name + ": a counter cannot hold " + raw_type);
    }
}

// The rules of stage 3 for one point object (CF-504, CF-517).
void check_point(const ParsedFile& file, NodeId point, const std::string& name,
                 Diagnostics& diagnostics) {
    check_integer_scaling(file, point, name, diagnostics);
    check_raw_value_type(file, point, name, diagnostics);
}

// The name a message gives a point object: its `name`, else "point".
std::string point_label(const Document& document, NodeId point) {
    const std::optional<NodeId> name = document.find(point, "name");
    if (name && document.kind(*name) == NodeKind::SCALAR) {
        return quote_text(document.text(*name));
    }
    return "point";
}

// The point objects of a template or channel file: points (also of repeat
// blocks and devices), defaults and overrides.
void check_points(const ParsedFile& file, NodeId owner,
                  Diagnostics& diagnostics) {
    const Document& document = file.document;
    const auto each_point = [&](NodeId list) {
        for (const NodeId point : document.items(list)) {
            if (document.kind(point) == NodeKind::MAP) {
                check_point(file, point, point_label(document, point),
                            diagnostics);
            }
        }
    };
    if (const std::optional<NodeId> points = document.find(owner, "points")) {
        each_point(*points);
    }
    if (const std::optional<NodeId> repeat = document.find(owner, "repeat")) {
        for (const NodeId block : document.items(*repeat)) {
            if (const std::optional<NodeId> points =
                    document.find(block, "points")) {
                each_point(*points);
            }
        }
    }
    if (const std::optional<NodeId> defaults =
            map_entry(document, owner, "defaults")) {
        check_point(file, *defaults, "defaults", diagnostics);
    }
    if (const std::optional<NodeId> overrides =
            map_entry(document, owner, "overrides")) {
        for (const Entry& entry : document.entries(*overrides)) {
            if (document.kind(entry.value) == NodeKind::MAP) {
                check_point(file, entry.value,
                            quote_text(document.text(entry.key)), diagnostics);
            }
        }
    }
}

void check_point_rules(const ParsedFile& file, Diagnostics& diagnostics) {
    const Document& document = file.document;
    const NodeId root = document.root();
    if (file.source->path_class.type == FileType::TEMPLATE) {
        check_points(file, root, diagnostics);
        return;
    }
    if (file.source->path_class.type != FileType::CHANNEL) {
        return;
    }
    if (const std::optional<NodeId> devices = document.find(root, "devices")) {
        for (const NodeId device : document.items(*devices)) {
            check_points(file, device, diagnostics);
        }
    }
}

// Stage 3 for one file.
void check_schema(ParsedFile& file, Diagnostics& diagnostics) {
    check_name_key(file, diagnostics);
    const std::string schema = schema_of(file);
    if (schema.empty()) {
        return;
    }
    SchemaSet::builtin().validate(file.document, file.types, schema,
                                  file.source->path, diagnostics);
    if (file.source->path_class.type == FileType::SYSTEM) {
        check_system(file.document, file.types, diagnostics);
    }
    check_point_rules(file, diagnostics);
}

}  // namespace

Result<Report> validate_tree(const std::filesystem::path& root,
                             const ScanLimits& limits) {
    Report report;
    Diagnostics stage1;
    Result<Tree> tree = scan_tree(root, stage1, limits);
    if (!tree.ok()) {
        return tree.status();
    }
    const bool stage1_failed = stage1.error_count() > 0;
    stage1.move_into(report);
    if (stage1_failed) {
        return report;
    }

    // Stage 2: every YAML file. Structured text is not available in this
    // version: a `.st` file is refused, so that a program that is not run
    // cannot look accepted (contract 9.3).
    Diagnostics stage2;
    NodeBudget budget;
    std::vector<ParsedFile> parsed;
    for (TreeFile& file : tree.value().files) {
        if (file.path_class.type == FileType::STRUCTURED_TEXT) {
            stage2.error(CF_STRUCTURED_TEXT,
                         Location{.file = file.path, .line = 1, .column = 1},
                         "structured text is not available in this version");
            continue;
        }
        std::optional<Document> document =
            parse_yaml(file.content, file.path, stage2, {}, &budget);
        // The document holds its own text: the file's bytes are freed now,
        // so that a tree at its limits is not held twice (README, memory).
        std::string().swap(file.content);
        if (document) {
            ScalarTypes types(*document);
            parsed.push_back(ParsedFile{.source = &file,
                                        .document = std::move(*document),
                                        .types = std::move(types)});
        }
    }

    // Stage 3 checks every file that parsed, also when another did not
    // (section 1.4).
    Diagnostics stage3;
    for (ParsedFile& file : parsed) {
        check_schema(file, stage3);
    }
    stage2.move_into(report);
    stage3.move_into(report);
    if (!passed(report)) {
        // Section 1.4: warnings are reported only when no stage gave an
        // error.
        report.warnings.clear();
        report.warning_count = 0;
    }
    return report;
}

}  // namespace scada::conf
