#include "scada/conf/validate.h"

#include "merged_points.h"
#include "runtime_expressions.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/node.h"
#include "scada/conf/references.h"
#include "scada/conf/rules.h"
#include "scada/conf/scalar.h"
#include "scada/conf/schema.h"
#include "scada/conf/tree.h"
#include "scada/conf/types.h"
#include "scada/conf/yaml.h"
#include "tag_names.h"
#include "tag_rules.h"
#include "validate_internal.h"

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

// The tree format this version reads (contract section 11).
constexpr std::uint64_t TREE_FORMAT = 1;

// A file that parsed in stage 2.
struct ParsedFile {
    const TreeFile* source = nullptr;
    Document document;
    // The types stage 3 gives its scalars (D-111).
    ScalarTypes types;
};

// The protocol of a template: its `protocol` key, if this version knows it.
std::string template_protocol(const Document& document) {
    const std::optional<NodeId> protocol =
        document.find(document.root(), "protocol");
    if (!protocol || document.kind(*protocol) != NodeKind::SCALAR ||
        !is_known_protocol(document.text(*protocol))) {
        return {};
    }
    return std::string(document.text(*protocol));
}

// The schema of a file: by its type, and for a channel by its protocol
// directory, for a template by its `protocol` key (section 4.1). A template
// whose `protocol` this version does not know is checked against the
// framework keys alone, with its object open, so that its protocol keys
// give no error: the unknown protocol is one error, at stage 5 (D-108).
// Empty for a file without one.
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
    const std::string protocol = template_protocol(document);
    if (!protocol.empty()) {
        return protocol + ".schema.json#/$defs/template";
    }
    if (document.kind(document.root()) == NodeKind::MAP &&
        document.find(document.root(), "protocol")) {
        return "template.schema.json#/$defs/template_keys";
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
    const FileType type = file.source->path_class.type;
    std::string protocol;
    if (type == FileType::CHANNEL) {
        protocol = file.source->path_class.protocol;
    } else if (type == FileType::TEMPLATE) {
        protocol = template_protocol(file.document);
    }
    check_file_rules(file.document, file.types, type, protocol, diagnostics);
}

}  // namespace

Result<Report> validate_tree(const std::filesystem::path& root,
                             const ScanLimits& limits) {
    Result<Validation> validation = validate_and_expand(root, limits);
    if (!validation.ok()) {
        return validation.status();
    }
    return std::move(validation).value().report;
}

Result<Validation> validate_and_expand(const std::filesystem::path& root,
                                       const ScanLimits& limits,
                                       const ExpandLimits& expand_limits) {
    return validate_and_expand_with(root, limits, expand_limits,
                                    SchemaSet::builtin());
}

Result<Validation> validate_and_expand_with(const std::filesystem::path& root,
                                            const ScanLimits& limits,
                                            const ExpandLimits& expand_limits,
                                            const SchemaSet& stage5_schemas) {
    Validation validation;
    Report& report = validation.report;
    Diagnostics stage1;
    Result<Tree> tree = scan_tree(root, stage1, limits);
    if (!tree.ok()) {
        return tree.status();
    }
    const bool stage1_failed = stage1.error_count() > 0;
    stage1.move_into(report);
    if (stage1_failed) {
        return validation;
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
    const bool stage3_failed =
        stage2.error_count() > 0 || stage3.error_count() > 0;
    stage2.move_into(report);
    stage3.move_into(report);
    if (!stage3_failed) {
        std::vector<CheckedFile> checked;
        checked.reserve(parsed.size());
        for (const ParsedFile& file : parsed) {
            checked.push_back(CheckedFile{.source = file.source,
                                          .document = &file.document,
                                          .types = &file.types});
        }
        // Stage 4: templates and instances (D-114).
        Diagnostics stage4;
        validation.expanded =
            expand_tree(checked, stage4, expand_limits, &validation.expansion);
        const bool stage4_failed = stage4.error_count() > 0;
        stage4.move_into(report);
        if (!stage4_failed) {
            // Stage 5: what needs no point tags (D-113), the merged points
            // (D-115) and the tag references (D-116); the rest comes with
            // slices 7b to 7e of #100.
            Diagnostics stage5;
            check_references(checked, stage5);
            if (validation.expanded) {
                const Status merged = check_merged_points(
                    *validation.expanded, checked, stage5, stage5_schemas);
                if (!merged.ok()) {
                    return merged;
                }
                // The tags of the expanded tree and the references to them
                // (D-116, slice 7a of #100).
                TagNames tags =
                    collect_tags(*validation.expanded, checked, stage5);
                // Virtual tags get their types from their expressions before
                // any reference reads them (D-117, slice 7b of #100).
                check_run_time_expressions(tags, *validation.expanded, checked,
                                           stage5, expand_limits.kept_run_time,
                                           expand_limits.run_time_pattern_work,
                                           &validation.run_time);
                check_tag_references(tags, *validation.expanded, checked,
                                     stage5,
                                     expand_limits.applies_to_pattern_work);
                check_tag_rules(tags, *validation.expanded, checked, stage5);
            }
            stage5.move_into(report);
        }
    }
    if (!passed(report)) {
        // Section 1.4: warnings are reported only when no stage gave an
        // error.
        report.warnings.clear();
        report.warning_count = 0;
        validation.expanded.reset();
    }
    return validation;
}

}  // namespace scada::conf
