// Stage 3: the schema compiler and validator (configuration schemas contract
// 1.2, 10.2, Annex C; D-69, D-107).
//
// A schema file is JSON, read with the YAML subset parser (D-70: JSON files
// use the same parser), then compiled into SchemaNodes. Validation walks a
// Document and a schema together and reports CF-2xx errors in the order of
// the document.

#include "scada/conf/schema.h"

#include "expression_site.h"
#include "scada/common/panic.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expression.h"
#include "scada/conf/names.h"
#include "scada/conf/node.h"
#include "scada/conf/scalar.h"
#include "scada/conf/types.h"
#include "scada/conf/yaml.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace scada::conf {

namespace {

// A node as the compiler and the validator read it.
struct NodeView {
    NodeKind kind = NodeKind::SCALAR;
    ScalarStyle style = ScalarStyle::PLAIN;
    std::string text;
};

NodeView view_of(const Document& document, NodeId node) {
    return NodeView{.kind = document.kind(node),
                    .style = document.style(node),
                    .text = std::string(document.text(node))};
}

constexpr std::uint16_t CF_DUPLICATE_KEY = 111;
constexpr std::uint16_t CF_UNKNOWN_KEY = 201;
constexpr std::uint16_t CF_MISSING_KEY = 202;
constexpr std::uint16_t CF_WRONG_TYPE = 203;
constexpr std::uint16_t CF_OUT_OF_RANGE = 204;
constexpr std::uint16_t CF_NOT_ALLOWED = 205;
constexpr std::uint16_t CF_BAD_UNIT_VALUE = 206;
constexpr std::uint16_t CF_EXCLUSIVE = 207;
constexpr std::uint16_t CF_NEEDS = 208;
constexpr std::uint16_t CF_RESERVED = 210;
constexpr std::uint16_t CF_BAD_NAME = 212;
constexpr std::uint16_t CF_NOT_A_SECRET = 213;
constexpr std::uint16_t CF_SECRET = 214;
constexpr std::uint16_t CF_EXPRESSION_SYNTAX = 520;

constexpr std::string_view INT64_MIN_TEXT = "-9223372036854775808";
constexpr std::string_view INT64_MAX_TEXT = "9223372036854775807";
constexpr std::string_view UINT64_MAX_TEXT = "18446744073709551615";
// The most times validation may check one document node: the product of
// nested `oneOf` branches and `allOf` parts along a schema path (D-107).
constexpr std::size_t MAX_CHECKS_PER_NODE = 64;

// The longest duration whose nanoseconds fit int64 (D-107), in ms.
constexpr std::uint64_t MAX_DURATION_MS = 9223372036854;

// The `x-scada-type` names this validator implements (contract Annex C). A
// schema file with another name does not compile, so that a type not
// implemented yet cannot check nothing.
constexpr std::array<std::string_view, 42> KNOWN_SCALAR_TYPES = {
    "string",
    "text",
    "identifier",
    "tag_name",
    "point_name",
    "device_name",
    "poll_group_name",
    "tier_name",
    "protocol_name",
    "unit_id",
    "process_name",
    "integer",
    "uint64",
    "power_of_two",
    "decimal",
    "number",
    "boolean",
    "duration",
    "size",
    "percentage",
    "number_or_percentage",
    "expression",
    "number_or_expression",
    "host",
    "host_name",
    "ip_address",
    "cidr",
    "time_zone",
    "time_of_day",
    "https_url",
    "point_name_template",
    "name_pattern",
    "dev_path",
    "resolution",
    "raw_pattern",
    "raw_pattern_or_nan",
    "number_or_boolean",
    "number_or_tag",
    "duration_or_off",
    "ip_literal_host",
    "config_expression",
    "for_value"};

// The name kind of a name type, and texts joined by commas (below).
std::optional<std::string_view> name_kind(std::string_view type);
std::string join(const std::vector<std::string>& texts);

// The types that "minimum" and "maximum" apply to.
// For `number_or_expression` they apply to a literal number (D-107).
constexpr std::array<std::string_view, 9> NUMBER_TYPES = {
    "integer",
    "uint64",
    "power_of_two",
    "decimal",
    "number",
    "percentage",
    "number_or_percentage",
    "number_or_tag",
    "number_or_expression"};

bool listed(std::span<const std::string_view> list, std::string_view name) {
    return std::ranges::find(list, name) != list.end();
}

using SchemaId = std::size_t;

enum class Unevaluated : std::uint8_t { ALLOWED, REFUSED, SCHEMA };

// One compiled schema object.
struct SchemaNode {
    SchemaId unevaluated_schema = 0;
    std::optional<SchemaId> items;
    std::optional<SchemaId> ref;
    std::optional<std::size_t> min_items;
    std::optional<std::size_t> max_items;
    std::string file;
    // "x-scada-type": the contract type of a scalar ("" for "string").
    std::string scalar_type;
    // "x-scada-key-type": the type of the keys "unevaluatedProperties"
    // applies to.
    std::string key_type;
    std::vector<std::string> enum_texts;
    std::optional<std::string> const_text;
    std::vector<std::pair<std::string, SchemaId>> properties;
    std::vector<std::string> required;
    std::optional<std::string> minimum;
    std::optional<std::string> maximum;
    std::optional<std::string> scada_min;
    std::optional<std::string> scada_max;
    std::vector<SchemaId> one_of;
    std::vector<SchemaId> all_of;
    std::optional<std::string> ref_text;
    std::vector<std::string> reserved_values;
    std::vector<std::vector<std::string>> exclusive;
    std::vector<std::pair<std::string, std::vector<std::string>>>
        dependent_required;
    Unevaluated unevaluated = Unevaluated::ALLOWED;
    // From "type": which node kinds it accepts. None given: every kind.
    bool kind_given = false;
    bool allows_map = false;
    bool allows_sequence = false;
    bool allows_scalar = false;
    // The scalar names of "type" given: "string", "integer", "number",
    // "boolean".
    std::vector<std::string> scalar_kinds;
    bool has_enum = false;
    bool reserved = false;
    bool secret = false;
};

}  // namespace

struct CompiledSchemas {
    std::vector<SchemaNode> nodes;
    // "<file>" and "<file>#/$defs/<name>" to the schema they name.
    std::map<std::string, SchemaId, std::less<>> names;
    // The most times validation checks one document node (D-107).
    std::size_t checks_per_node = 1;
};

namespace {

// --- Compilation -----------------------------------------------------------

class Compiler {
public:
    explicit Compiler(CompiledSchemas& out) : out_(&out) {}

    bool compile_file(const SchemaSource& source, std::string& problem) {
        Diagnostics diagnostics;
        const std::optional<Document> document =
            parse_yaml(source.text, source.name, diagnostics);
        if (!document) {
            Report report;
            diagnostics.move_into(report);
            problem = source.name + ": not readable: " +
                      (report.errors.empty() ? std::string("?")
                                             : format(report.errors.front()));
            return false;
        }
        file_ = source.name;
        document_ = &*document;
        const std::optional<SchemaId> root = compile(document->root(), problem);
        document_ = nullptr;
        if (!root) {
            return false;
        }
        out_->names[source.name] = *root;
        return true;
    }

    // Resolves every "$ref" once all files are compiled.
    bool resolve(std::string& problem) {
        for (SchemaNode& node : out_->nodes) {
            if (!node.ref_text) {
                continue;
            }
            std::string target = *node.ref_text;
            if (target.starts_with('#')) {
                target.insert(0, node.file);
            }
            const auto found = out_->names.find(target);
            if (found == out_->names.end()) {
                problem = node.file + ": $ref " + *node.ref_text +
                          " does not resolve";
                return false;
            }
            node.ref = found->second;
        }
        return refuse_cycles(problem) && bound_checks(problem);
    }

    // Bounds the work of validation (D-107). `check` checks a node once for
    // each schema of its "same node" closure ($ref, allOf, oneOf: every
    // branch in full), and each schema of the closure checks a child with
    // its own property, item or unevaluated schema. So with
    //   g(s) = 1 + the g of the closure's targets of s,
    //   D(s) = the largest B of the schemas below s itself
    //          + the D of the closure's targets of s,
    //   B(s) = max(g(s), D(s)),
    // a document node is checked at most B(root) times. B is found as a
    // fixed point; a set whose B passes MAX_CHECKS_PER_NODE, a recursive one
    // through a oneOf or allOf included, does not compile.
    bool bound_checks(std::string& problem) const {
        const std::vector<SchemaNode>& nodes = out_->nodes;
        const std::size_t count = nodes.size();
        const std::size_t cap = MAX_CHECKS_PER_NODE + 1;
        std::vector<std::size_t> same(count, 1);
        std::vector<std::size_t> below(count, 0);
        std::vector<std::size_t> bound(count, 1);
        bool changed = true;
        // Each pass settles at least one more level; more passes than
        // schemas mean the bound grows without end.
        for (std::size_t pass = 0; changed && pass <= count + 1; ++pass) {
            changed = false;
            for (SchemaId id = 0; id < count; ++id) {
                std::size_t checks = 1;
                std::size_t deeper = largest_below(nodes[id], bound);
                for (const SchemaId target : closure_edges(nodes[id])) {
                    checks = std::min(checks + same[target], cap);
                    deeper = std::min(deeper + below[target], cap);
                }
                const std::size_t total = std::max(checks, deeper);
                changed = changed || checks != same[id] ||
                          deeper != below[id] || total != bound[id];
                same[id] = checks;
                below[id] = deeper;
                bound[id] = total;
            }
        }
        const std::size_t worst =
            count == 0 ? 1 : *std::ranges::max_element(bound);
        if (changed || worst > MAX_CHECKS_PER_NODE) {
            problem =
                "the schemas could check one node more than " +
                std::to_string(MAX_CHECKS_PER_NODE) +
                " times: nested oneOf or allOf, or a schema that reaches "
                "itself through properties, items or unevaluatedProperties "
                "and also refers through $ref, allOf or oneOf to a schema "
                "with keys or items of its own; such a recursion needs a "
                "bound per child key, which this validator does not have "
                "(D-107)";
            return false;
        }
        out_->checks_per_node = worst;
        return true;
    }

    // The largest bound of the schemas that `node` itself checks a child
    // with: its properties, items and unevaluated properties; 0 if none.
    static std::size_t largest_below(const SchemaNode& node,
                                     const std::vector<std::size_t>& bound) {
        std::size_t largest = 0;
        for (const auto& property : node.properties) {
            largest = std::max(largest, bound[property.second]);
        }
        if (node.items) {
            largest = std::max(largest, bound[*node.items]);
        }
        if (node.unevaluated == Unevaluated::SCHEMA) {
            largest = std::max(largest, bound[node.unevaluated_schema]);
        }
        return largest;
    }

    // The schemas checked on the same document node as `node`.
    static std::vector<SchemaId> closure_edges(const SchemaNode& node) {
        std::vector<SchemaId> out;
        if (node.ref) {
            out.push_back(*node.ref);
        }
        out.insert(out.end(), node.all_of.begin(), node.all_of.end());
        out.insert(out.end(), node.one_of.begin(), node.one_of.end());
        return out;
    }

    // A "$ref", "allOf" or "oneOf" chain that comes back to itself would
    // make validation recurse without end; recursion through "properties"
    // or "items" goes down the document and stays allowed (D-107).
    bool refuse_cycles(std::string& problem) const {
        enum class Mark : std::uint8_t { NEW, ON_PATH, DONE };
        const std::vector<SchemaNode>& nodes = out_->nodes;
        std::vector<Mark> marks(nodes.size(), Mark::NEW);
        const auto edges = [&nodes](SchemaId from) {
            const SchemaNode& node = nodes[from];
            std::vector<SchemaId> out;
            if (node.ref) {
                out.push_back(*node.ref);
            }
            out.insert(out.end(), node.all_of.begin(), node.all_of.end());
            out.insert(out.end(), node.one_of.begin(), node.one_of.end());
            return out;
        };
        for (SchemaId start = 0; start < nodes.size(); ++start) {
            if (marks[start] != Mark::NEW) {
                continue;
            }
            // Depth first with an explicit stack: (node, next edge).
            std::vector<std::pair<SchemaId, std::size_t>> stack{{start, 0}};
            marks[start] = Mark::ON_PATH;
            while (!stack.empty()) {
                auto& [current, next] = stack.back();
                const std::vector<SchemaId> targets = edges(current);
                if (next == targets.size()) {
                    marks[current] = Mark::DONE;
                    stack.pop_back();
                    continue;
                }
                const SchemaId target = targets[next++];
                if (marks[target] == Mark::ON_PATH) {
                    problem =
                        nodes[target].file + ": a $ref, allOf or oneOf cycle";
                    return false;
                }
                if (marks[target] == Mark::NEW) {
                    marks[target] = Mark::ON_PATH;
                    stack.emplace_back(target, 0);
                }
            }
        }
        return true;
    }

private:
    [[nodiscard]] NodeView at(NodeId node_id) const {
        return view_of(*document_, node_id);
    }
    [[nodiscard]] NodeId key_of(NodeId map, std::size_t index) const {
        return document_->entries(map)[index].key;
    }
    [[nodiscard]] NodeId value_of(NodeId map, std::size_t index) const {
        return document_->entries(map)[index].value;
    }

    bool fail(std::string& problem, std::string_view what, NodeId where) const {
        const Location location = document_->location(where);
        problem = file_ + ":" + std::to_string(location.line) + ": " +
                  std::string(what);
        return false;
    }

    bool string_list(NodeId value, std::vector<std::string>& out,
                     std::string& problem) const {
        if (at(value).kind != NodeKind::SEQUENCE) {
            return fail(problem, "a list is expected", value);
        }
        for (const NodeId item : document_->items(value)) {
            if (at(item).kind != NodeKind::SCALAR) {
                return fail(problem, "a scalar is expected", item);
            }
            out.push_back(at(item).text);
        }
        return true;
    }

    bool scalar_text(NodeId value, std::string& out,
                     std::string& problem) const {
        if (at(value).kind != NodeKind::SCALAR) {
            return fail(problem, "a scalar is expected", value);
        }
        out = at(value).text;
        return true;
    }

    std::optional<std::size_t> count(NodeId value, std::string& problem) const {
        const std::optional<Integer> parsed = parse_integer(at(value).text);
        if (at(value).kind != NodeKind::SCALAR || !parsed || parsed->negative) {
            fail(problem, "a count is expected", value);
            return std::nullopt;
        }
        return static_cast<std::size_t>(parsed->magnitude);
    }

    bool compile_type(NodeId value, SchemaNode& node,
                      std::string& problem) const {
        std::vector<std::string> names;
        if (at(value).kind == NodeKind::SCALAR) {
            names.push_back(at(value).text);
        } else if (!string_list(value, names, problem)) {
            return false;
        }
        node.kind_given = true;
        for (const std::string& name : names) {
            if (name == "object") {
                node.allows_map = true;
            } else if (name == "array") {
                node.allows_sequence = true;
            } else if (name == "string" || name == "integer" ||
                       name == "number" || name == "boolean") {
                node.allows_scalar = true;
                node.scalar_kinds.push_back(name);
            } else {
                return fail(problem, "unknown type " + name, value);
            }
        }
        return true;
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by the depth of the schema file, which the parser limits
    bool compile_properties(NodeId value, SchemaNode& node,
                            std::string& problem) {
        if (at(value).kind != NodeKind::MAP) {
            return fail(problem, "properties must be an object", value);
        }
        for (std::size_t i = 0; i < document_->entry_count(value); ++i) {
            const std::optional<SchemaId> property =
                compile(value_of(value, i), problem);
            if (!property) {
                return false;
            }
            node.properties.emplace_back(at(key_of(value, i)).text, *property);
        }
        return true;
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by the depth of the schema file, which the parser limits
    bool compile_schema_list(NodeId value, std::vector<SchemaId>& out,
                             std::string& problem) {
        if (at(value).kind != NodeKind::SEQUENCE) {
            return fail(problem, "a list of schemas is expected", value);
        }
        for (const NodeId item : document_->items(value)) {
            const std::optional<SchemaId> compiled = compile(item, problem);
            if (!compiled) {
                return false;
            }
            out.push_back(*compiled);
        }
        return true;
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by the depth of the schema file, which the parser limits
    bool compile_defs(NodeId value, std::string& problem) {
        if (at(value).kind != NodeKind::MAP) {
            return fail(problem, "$defs must be an object", value);
        }
        for (std::size_t i = 0; i < document_->entry_count(value); ++i) {
            const std::optional<SchemaId> def =
                compile(value_of(value, i), problem);
            if (!def) {
                return false;
            }
            out_->names[file_ + "#/$defs/" + at(key_of(value, i)).text] = *def;
        }
        return true;
    }

    bool compile_exclusive(NodeId value, SchemaNode& node,
                           std::string& problem) const {
        if (at(value).kind != NodeKind::SEQUENCE) {
            return fail(problem, "x-scada-exclusive must be a list", value);
        }
        for (const NodeId group : document_->items(value)) {
            std::vector<std::string> keys;
            if (!string_list(group, keys, problem)) {
                return false;
            }
            node.exclusive.push_back(std::move(keys));
        }
        return true;
    }

    bool compile_dependent(NodeId value, SchemaNode& node,
                           std::string& problem) const {
        if (at(value).kind != NodeKind::MAP) {
            return fail(problem, "dependentRequired must be an object", value);
        }
        for (std::size_t i = 0; i < document_->entry_count(value); ++i) {
            std::vector<std::string> needed;
            if (!string_list(value_of(value, i), needed, problem)) {
                return false;
            }
            node.dependent_required.emplace_back(at(key_of(value, i)).text,
                                                 std::move(needed));
        }
        return true;
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by the depth of the schema file, which the parser limits
    bool compile_unevaluated(NodeId value, SchemaNode& node,
                             std::string& problem) {
        if (at(value).kind == NodeKind::SCALAR) {
            if (at(value).text == "false") {
                node.unevaluated = Unevaluated::REFUSED;
                return true;
            }
            if (at(value).text == "true") {
                node.unevaluated = Unevaluated::ALLOWED;
                return true;
            }
            return fail(problem, "unevaluatedProperties must be a schema",
                        value);
        }
        const std::optional<SchemaId> compiled = compile(value, problem);
        if (!compiled) {
            return false;
        }
        node.unevaluated = Unevaluated::SCHEMA;
        node.unevaluated_schema = *compiled;
        return true;
    }

    // A keyword whose value is a list or a map of names; nothing if `name` is
    // not one.
    std::optional<bool> list_keyword(std::string_view name, NodeId value,
                                     SchemaNode& node,
                                     std::string& problem) const {
        if (name == "enum") {
            node.has_enum = true;
            return string_list(value, node.enum_texts, problem);
        }
        if (name == "required") {
            return string_list(value, node.required, problem);
        }
        if (name == "x-scada-reserved-values") {
            return string_list(value, node.reserved_values, problem);
        }
        if (name == "x-scada-exclusive") {
            return compile_exclusive(value, node, problem);
        }
        if (name == "dependentRequired") {
            return compile_dependent(value, node, problem);
        }
        return std::nullopt;
    }

    // `x-scada-reserved` or `x-scada-secret`: plain `true` or `false`.
    bool flag_keyword(std::string_view name, NodeId value, SchemaNode& node,
                      std::string& problem) const {
        const NodeView flag = at(value);
        if (flag.kind != NodeKind::SCALAR || flag.style != ScalarStyle::PLAIN ||
            !parse_boolean(flag.text)) {
            return fail(problem, std::string(name) + " must be true or false",
                        value);
        }
        (name == "x-scada-reserved" ? node.reserved : node.secret) =
            flag.text == "true";
        return true;
    }

    // A keyword whose value is a scalar or a list of scalars.
    bool scalar_keyword(std::string_view name, NodeId value, SchemaNode& node,
                        std::string& problem, bool& known) const {
        known = true;
        if (name == "type") {
            return compile_type(value, node, problem);
        }
        if (name == "x-scada-type") {
            return scalar_text(value, node.scalar_type, problem);
        }
        if (name == "x-scada-key-type") {
            return scalar_text(value, node.key_type, problem);
        }
        if (std::optional<bool> done =
                list_keyword(name, value, node, problem)) {
            return *done;
        }
        if (name == "minItems" || name == "maxItems") {
            const std::optional<std::size_t> limit = count(value, problem);
            (name == "minItems" ? node.min_items : node.max_items) = limit;
            return limit.has_value();
        }
        if (name == "x-scada-reserved" || name == "x-scada-secret") {
            const NodeView flag = at(value);
            if (flag.kind != NodeKind::SCALAR ||
                flag.style != ScalarStyle::PLAIN || !parse_boolean(flag.text)) {
                return fail(problem,
                            std::string(name) + " must be true or false",
                            value);
            }
            (name == "x-scada-reserved" ? node.reserved : node.secret) =
                flag.text == "true";
            return true;
        }
        std::optional<std::string>* text_slot = nullptr;
        if (name == "const") {
            text_slot = &node.const_text;
        } else if (name == "minimum") {
            text_slot = &node.minimum;
        } else if (name == "maximum") {
            text_slot = &node.maximum;
        } else if (name == "x-scada-min") {
            text_slot = &node.scada_min;
        } else if (name == "x-scada-max") {
            text_slot = &node.scada_max;
        } else if (name == "$ref") {
            text_slot = &node.ref_text;
        }
        if (text_slot != nullptr) {
            std::string text;
            const bool done = scalar_text(value, text, problem);
            *text_slot = std::move(text);
            return done;
        }
        known = name == "description" || name == "title" || name == "$schema" ||
                name == "$id" || name == "$comment" || name == "default" ||
                name == "examples";
        return true;
    }

    // A keyword whose value holds schemas.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by the depth of the schema file, which the parser limits
    bool schema_keyword(std::string_view name, NodeId value, SchemaNode& node,
                        std::string& problem) {
        if (name == "properties") {
            return compile_properties(value, node, problem);
        }
        if (name == "unevaluatedProperties") {
            return compile_unevaluated(value, node, problem);
        }
        if (name == "items") {
            node.items = compile(value, problem);
            return node.items.has_value();
        }
        if (name == "oneOf") {
            return compile_schema_list(value, node.one_of, problem);
        }
        if (name == "allOf") {
            return compile_schema_list(value, node.all_of, problem);
        }
        if (name == "$defs") {
            return compile_defs(value, problem);
        }
        return fail(problem, "unknown keyword " + std::string(name), value);
    }

    // Compiles the schema object `schema`. Its children are compiled first
    // and stored before it; the node itself is built here and stored last,
    // at the index reserved for it, so no reference into the vector is held
    // while it grows.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by the depth of the schema file, which the parser limits
    std::optional<SchemaId> compile(NodeId schema, std::string& problem) {
        if (at(schema).kind != NodeKind::MAP) {
            fail(problem, "a schema must be an object", schema);
            return std::nullopt;
        }
        const SchemaId self = out_->nodes.size();
        out_->nodes.emplace_back();
        SchemaNode node;
        node.file = file_;
        for (std::size_t i = 0; i < document_->entry_count(schema); ++i) {
            const std::string name = at(key_of(schema, i)).text;
            const NodeId value = value_of(schema, i);
            bool known = false;
            if (!scalar_keyword(name, value, node, problem, known)) {
                return std::nullopt;
            }
            if (!known && !schema_keyword(name, value, node, problem)) {
                return std::nullopt;
            }
        }
        if (!check_keywords(node, schema, problem)) {
            return std::nullopt;
        }
        out_->nodes[self] = std::move(node);
        return self;
    }

    // The values of the keywords of one schema object, once all are read:
    // a type that the validator knows, bounds that read in the type's own
    // form, and keywords that apply to the kind of node the schema takes
    // (D-107). A schema file that breaks one is a defect of this software.
    bool check_keywords(const SchemaNode& node, NodeId where,
                        std::string& problem) const {
        if (!node.scalar_type.empty() &&
            !listed(KNOWN_SCALAR_TYPES, node.scalar_type)) {
            return fail(problem, "unknown x-scada-type " + node.scalar_type,
                        where);
        }
        if (!node.key_type.empty() && node.key_type != "integer" &&
            node.key_type != "boolean" && node.key_type != "string" &&
            !name_kind(node.key_type)) {
            return fail(problem, "unknown x-scada-key-type " + node.key_type,
                        where);
        }
        // The key type applies to the keys that a schema in
        // "unevaluatedProperties" takes; without one no key is typed.
        if (!node.key_type.empty() && node.unevaluated != Unevaluated::SCHEMA) {
            return fail(problem,
                        "x-scada-key-type needs a schema in "
                        "unevaluatedProperties",
                        where);
        }
        if (!type_agrees(node)) {
            return fail(problem,
                        "type " + join(node.scalar_kinds) +
                            " disagrees with x-scada-type " + node.scalar_type,
                        where);
        }
        if (!check_bounds(node, where, problem)) {
            return false;
        }
        const bool scalar_keywords = node.has_enum || node.const_text ||
                                     !node.scalar_type.empty() ||
                                     !node.reserved_values.empty();
        const bool map_keywords =
            !node.properties.empty() || !node.required.empty() ||
            node.unevaluated != Unevaluated::ALLOWED ||
            !node.exclusive.empty() || !node.dependent_required.empty() ||
            !node.key_type.empty();
        const bool list_keywords =
            node.items || node.min_items || node.max_items;
        // With "type", each keyword must apply to a kind it allows;
        // without it, the keywords must all be of one kind.
        const int kinds_used = static_cast<int>(scalar_keywords) +
                               static_cast<int>(map_keywords) +
                               static_cast<int>(list_keywords);
        const bool misplaced =
            node.kind_given ? (scalar_keywords && !node.allows_scalar) ||
                                  (map_keywords && !node.allows_map) ||
                                  (list_keywords && !node.allows_sequence)
                            : kinds_used > 1;
        if (misplaced) {
            return fail(problem,
                        "a keyword that does not apply to the schema's type",
                        where);
        }
        return true;
    }

    // True if the scalar names of "type" are those the `x-scada-type` is
    // written as: "integer" for the integer types, "number" (or "integer")
    // for decimals and numbers, "boolean" for booleans, "number" and
    // "string" for a number or a percentage or an expression, any kind for
    // `string` and `expression`, "string" for every other type.
    static bool type_agrees(const SchemaNode& node) {
        if (node.scalar_type.empty() || node.scalar_kinds.empty()) {
            return true;
        }
        const std::string& type = node.scalar_type;
        // `string` is any text, also a value whose type another key decides
        // later (D-108): every scalar kind.
        // An expression's text may be a literal number or boolean (5.6).
        if (type == "string" || type == "expression" ||
            type == "config_expression") {
            return std::ranges::find(node.scalar_kinds, "string") !=
                   node.scalar_kinds.end();
        }
        std::vector<std::string_view> allowed;
        if (type == "integer" || type == "uint64" || type == "power_of_two") {
            allowed = {"integer"};
        } else if (type == "raw_pattern" || type == "raw_pattern_or_nan" ||
                   type == "for_value") {
            allowed = {"integer", "string"};
        } else if (type == "number_or_boolean") {
            allowed = {"number", "integer", "boolean"};
        } else if (type == "decimal" || type == "number") {
            allowed = {"number", "integer"};
        } else if (type == "boolean") {
            allowed = {"boolean"};
        } else if (type == "number_or_percentage" ||
                   type == "number_or_expression" || type == "number_or_tag") {
            allowed = {"number", "integer", "string"};
        } else {
            allowed = {"string"};
        }
        return std::ranges::all_of(
            node.scalar_kinds, [&allowed](const std::string& kind) {
                return std::ranges::find(allowed, kind) != allowed.end();
            });
    }

    // "minimum" and "maximum" are numbers and apply to number types;
    // "x-scada-min" and "x-scada-max" are in the form of a duration or a
    // size, or a count of bytes for text.
    bool check_bounds(const SchemaNode& node, NodeId where,
                      std::string& problem) const {
        const std::string& type = node.scalar_type;
        for (const std::optional<std::string>* bound :
             {&node.minimum, &node.maximum}) {
            if (*bound &&
                (!listed(NUMBER_TYPES, type) || !parse_number(**bound))) {
                return fail(problem,
                            "minimum and maximum take a number, on a number "
                            "type",
                            where);
            }
        }
        for (const std::optional<std::string>* bound :
             {&node.scada_min, &node.scada_max}) {
            if (!bound->has_value()) {
                continue;
            }
            const std::string text = bound->value_or(std::string());
            const std::optional<Integer> count = parse_integer(text);
            const bool valid =
                ((type == "duration" || type == "duration_or_off") &&
                 parse_duration_ms(text)) ||
                (type == "size" && parse_size_bytes(text)) ||
                (type == "text" && count.has_value() &&
                 !count.value_or(Integer{}).negative);
            if (!valid) {
                return fail(problem,
                            "x-scada-min and x-scada-max take the form of a "
                            "duration, a size or a count of bytes for text",
                            where);
            }
        }
        return true;
    }

    CompiledSchemas* out_;
    std::string file_;
    const Document* document_ = nullptr;
};

// --- Validation -------------------------------------------------------------

// Where checking puts what it finds. At the top of a file, errors go to the
// file's Diagnostics at once and types to its table, so memory does not grow
// with the number of errors. Inside a `oneOf` branch they are held until the
// branch is known to count, so that a branch that is not taken leaves no
// trace: at most REPORTED_LIMIT errors, and a count of the rest (D-107).
//
// A node whose kind is wrong gets one error, however many schemas state its
// kind (a framework key that a protocol's schema states again): one
// diagnostic per cause.
class Outcome {
public:
    // Held, for a `oneOf` branch.
    Outcome() = default;

    // Given at once to `diagnostics` and `types`.
    Outcome(Diagnostics& diagnostics, ScalarTypes& types)
        : diagnostics_(&diagnostics),
          types_(&types),
          kind_reported_(types.size(), false) {}

    void error(Diagnostic diagnostic) {
        add(std::move(diagnostic), std::nullopt);
    }

    // An error that `node` is of the wrong kind; dropped if one was given.
    void kind_error(NodeId node, Diagnostic diagnostic) {
        if (diagnostics_ != nullptr) {
            if (node >= kind_reported_.size() || kind_reported_[node]) {
                return;
            }
            kind_reported_[node] = true;
        } else if (!held_kinds_.insert(node).second) {
            return;
        }
        add(std::move(diagnostic), node);
    }

    // Records the type of `node`; a later call for the same node wins.
    void type(NodeId node, ScalarType type) {
        if (types_ != nullptr) {
            types_->set(node, type);
        } else {
            held_types_[node] = type;
        }
    }

    // True if `node` has a type here.
    [[nodiscard]] bool typed(NodeId node) const {
        if (types_ != nullptr) {
            return types_->of(node) != ScalarType::NONE;
        }
        const auto found = held_types_.find(node);
        return found != held_types_.end() && found->second != ScalarType::NONE;
    }

    [[nodiscard]] std::size_t error_count() const noexcept {
        return error_count_;
    }

    // Takes what the branch `taken` found, as if found here.
    void take(Outcome& taken) {
        taken.trim(true);
        const std::size_t unlisted =
            taken.error_count_ - taken.held_errors_.size();
        for (Held& held : taken.held_errors_) {
            if (held.kind_node) {
                kind_error(*held.kind_node, std::move(held.diagnostic));
            } else {
                error(std::move(held.diagnostic));
            }
        }
        error_count_ += unlisted;
        if (diagnostics_ != nullptr) {
            diagnostics_->count_unlisted_errors(unlisted);
        }
        for (const auto& [node, type_of_node] : taken.held_types_) {
            type(node, type_of_node);
        }
    }

private:
    struct Held {
        Diagnostic diagnostic;
        // The node, for an error of its kind.
        std::optional<NodeId> kind_node;
    };

    void add(Diagnostic diagnostic, std::optional<NodeId> kind_node) {
        ++error_count_;
        if (diagnostics_ != nullptr) {
            diagnostics_->add(std::move(diagnostic));
        } else {
            held_errors_.push_back(Held{.diagnostic = std::move(diagnostic),
                                        .kind_node = kind_node});
            trim(false);
        }
    }

    // Keeps the first REPORTED_LIMIT held errors in the order of the report
    // (contract 1.4), as Diagnostics does, once twice that many are held.
    void trim(bool force) {
        if (!force && held_errors_.size() < 2 * Diagnostics::REPORTED_LIMIT) {
            return;
        }
        std::stable_sort(held_errors_.begin(), held_errors_.end(),
                         [](const Held& lhs, const Held& rhs) {
                             return reported_before(lhs.diagnostic,
                                                    rhs.diagnostic);
                         });
        if (held_errors_.size() > Diagnostics::REPORTED_LIMIT) {
            held_errors_.resize(Diagnostics::REPORTED_LIMIT);
        }
    }

    Diagnostics* diagnostics_ = nullptr;
    ScalarTypes* types_ = nullptr;
    // The nodes given a kind error: at the top, one bit per node; in a
    // branch, those of the branch.
    std::vector<bool> kind_reported_;
    std::set<NodeId> held_kinds_;
    std::vector<Held> held_errors_;
    std::map<NodeId, ScalarType> held_types_;
    std::size_t error_count_ = 0;
};

std::string join(const std::vector<std::string>& texts) {
    std::string out;
    for (const std::string& text : texts) {
        if (!out.empty()) {
            out += ", ";
        }
        out += text;
    }
    return out;
}

// What a value of a contract type is called in a message: "{key} must be
// {type}" (CF-203).
std::string type_phrase(std::string_view type) {
    static const std::map<std::string_view, std::string_view> PHRASES = {
        {"integer", "an integer"},
        {"uint64", "an integer"},
        {"power_of_two", "an integer"},
        {"decimal", "a decimal number"},
        {"number", "a number"},
        {"boolean", "true or false"},
        {"duration", "a duration"},
        {"size", "a size"},
        {"percentage", "a percentage"},
        {"number_or_percentage", "a number or a percentage"},
        {"number_or_expression", "a number or an expression"},
        {"expression", "an expression"},
        {"config_expression", "an expression"},
        {"for_value", "an integer or a range"},
    };
    const auto found = PHRASES.find(type);
    if (found != PHRASES.end()) {
        return std::string(found->second);
    }
    return "text";
}

// The name kind of a name type, for CF-212 ("{name} is not a valid {kind}
// name"), or nothing for another type.
std::optional<std::string_view> name_kind(std::string_view type) {
    static const std::map<std::string_view, std::string_view> KINDS = {
        {"identifier", "identifier"},      {"tag_name", "tag"},
        {"point_name", "point"},           {"device_name", "device"},
        {"poll_group_name", "poll group"}, {"tier_name", "tier"},
        {"protocol_name", "protocol"},     {"unit_id", "unit id"},
        {"process_name", "process"},       {"point_name_template", "point"},
        {"name_pattern", "name pattern"},
    };
    const auto found = KINDS.find(type);
    if (found == KINDS.end()) {
        return std::nullopt;
    }
    return found->second;
}

// `text` with every `{<identifier>}` (a `repeat` variable, 4.4) replaced by
// `0`, and, for a pattern, every `*` and `?` (2.5) by `a`; nothing if a brace
// is not closed or holds no identifier.
std::optional<std::string> instantiated(std::string_view text, bool pattern) {
    std::string out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const char chr = text[pos];
        if (chr == '{') {
            const std::size_t close = text.find('}', pos);
            if (close == std::string_view::npos ||
                !is_identifier(text.substr(pos + 1, close - pos - 1))) {
                return std::nullopt;
            }
            out.push_back('0');
            pos = close + 1;
            continue;
        }
        out.push_back(pattern && (chr == '*' || chr == '?') ? 'a' : chr);
        ++pos;
    }
    return out;
}

bool name_matches(std::string_view type, std::string_view text) {
    if (type == "point_name_template" || type == "name_pattern") {
        const std::optional<std::string> plain =
            instantiated(text, type == "name_pattern");
        return plain && is_tag_name(*plain);
    }
    if (type == "identifier") {
        return is_identifier(text);
    }
    if (type == "tag_name" || type == "point_name") {
        return is_tag_name(text);
    }
    if (type == "device_name") {
        return is_tag_name(text, MAX_DEVICE_NAME);
    }
    if (type == "poll_group_name" || type == "tier_name") {
        return is_poll_group_name(text);
    }
    if (type == "protocol_name") {
        return is_protocol_name(text);
    }
    if (type == "unit_id") {
        return is_unit_id(text);
    }
    return is_process_name(text);
}

// An absolute path under /dev/ without `..` or empty components, at most
// 255 bytes (Annex A.1).

// Text forms that are strings with a grammar, refused with CF-203 and the
// phrase given.
std::optional<std::pair<bool, std::string_view>> text_form(
    std::string_view type, std::string_view text) {
    if (type == "host") {
        return std::make_pair(is_host_name(text) || is_ip_address(text),
                              "a host name or an IP address");
    }
    if (type == "host_name") {
        return std::make_pair(is_host_name(text), "a host name");
    }
    if (type == "ip_address") {
        return std::make_pair(is_ip_address(text), "an IP address");
    }
    if (type == "cidr") {
        return std::make_pair(is_cidr(text), "an address with a prefix length");
    }
    if (type == "time_zone") {
        return std::make_pair(is_time_zone_name(text), "an IANA zone name");
    }
    if (type == "time_of_day") {
        return std::make_pair(is_time_of_day(text), "a time of day HH:MM");
    }
    if (type == "https_url") {
        return std::make_pair(is_https_url(text), "an https URL");
    }
    if (type == "dev_path") {
        return std::make_pair(is_device_path(text),
                              "an absolute path under /dev/");
    }
    if (type == "resolution") {
        return std::make_pair(
            read_resolution_ns(text).form != IntegerForm::NOT_INTEGER,
            "1s, 1ms or <n>ns");
    }
    return std::nullopt;
}

// Compares two numbers exactly, an integer against a decimal included.
// Negative if `lhs` < `rhs`.
int compare_numbers(const Number& lhs, const Number& rhs) {
    const auto as_long_double = [](const Number& number) {
        if (const auto* integer = std::get_if<Integer>(&number)) {
            const auto magnitude = static_cast<long double>(integer->magnitude);
            return integer->negative ? -magnitude : magnitude;
        }
        return static_cast<long double>(std::get<double>(number));
    };
    const auto* left = std::get_if<Integer>(&lhs);
    const auto* right = std::get_if<Integer>(&rhs);
    if (left != nullptr && right != nullptr) {
        // Exact for every value of 64 bits.
        if (left->negative != right->negative &&
            (left->magnitude != 0 || right->magnitude != 0)) {
            return left->negative ? -1 : 1;
        }
        if (left->magnitude == right->magnitude) {
            return 0;
        }
        const bool smaller = left->magnitude < right->magnitude;
        return (smaller != left->negative) ? -1 : 1;
    }
    const long double left_value = as_long_double(lhs);
    const long double right_value = as_long_double(rhs);
    if (left_value < right_value) {
        return -1;
    }
    return left_value > right_value ? 1 : 0;
}

class Validator {
public:
    Validator(const CompiledSchemas& schemas, const Document& document,
              ScalarTypes& types, std::string_view file_label)
        : schemas_(&schemas),
          document_(&document),
          types_(&types),
          file_label_(file_label) {}

    void run(SchemaId root, Diagnostics& diagnostics) {
        Outcome outcome(diagnostics, *types_);
        std::set<std::string> evaluated;
        check(root, document_->root(), {}, outcome, evaluated);
    }

private:
    [[nodiscard]] const SchemaNode& schema(SchemaId schema_id) const {
        return schemas_->nodes.at(schema_id);
    }
    [[nodiscard]] NodeView node(NodeId node_id) const {
        return view_of(*document_, node_id);
    }
    [[nodiscard]] NodeId key_of(NodeId map, std::size_t index) const {
        return document_->entries(map)[index].key;
    }
    [[nodiscard]] NodeId value_of(NodeId map, std::size_t index) const {
        return document_->entries(map)[index].value;
    }

    // The name of the place `path` in messages: the path, or the file.
    [[nodiscard]] std::string label(const std::string& path) const {
        return path.empty() ? std::string(file_label_) : path;
    }

    void error(Outcome& out, std::uint16_t code, NodeId where,
               std::string message) const {
        out.error(Diagnostic{.severity = Severity::ERROR,
                             .code = code,
                             .location = document_->location(where),
                             .message = std::move(message),
                             .notes = {}});
    }

    static std::string child_path(const std::string& path,
                                  std::string_view key) {
        if (path.empty()) {
            return std::string(key);
        }
        return path + "." + std::string(key);
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by the depth of the document (32, the parser's limit) and of the schemas
    void check(SchemaId schema_id, NodeId node_id, const std::string& path,
               Outcome& out, std::set<std::string>& evaluated) {
        const SchemaNode& rules = schema(schema_id);
        if (rules.secret) {
            check_secret(node_id, path, out);
            return;
        }
        const std::size_t errors_before = out.error_count();
        if (rules.ref) {
            check(*rules.ref, node_id, path, out, evaluated);
        }
        for (const SchemaId part : rules.all_of) {
            check(part, node_id, path, out, evaluated);
        }
        if (!rules.one_of.empty()) {
            check_one_of(rules, node_id, path, out, evaluated);
        }
        const NodeView value = node(node_id);
        if (!kind_allowed(rules, value.kind)) {
            out.kind_error(node_id,
                           Diagnostic{.severity = Severity::ERROR,
                                      .code = CF_WRONG_TYPE,
                                      .location = document_->location(node_id),
                                      .message = label(path) + " must be " +
                                                 kind_phrase(rules),
                                      .notes = {}});
            return;
        }
        if (value.kind == NodeKind::MAP) {
            check_map(rules, node_id, path, out, evaluated);
        } else if (value.kind == NodeKind::SEQUENCE) {
            check_sequence(rules, node_id, path, out);
        } else {
            check_scalar(rules, node_id, path, out);
            // A scalar that got an error has no type (D-107).
            if (out.error_count() > errors_before) {
                out.type(node_id, ScalarType::NONE);
            }
        }
    }

    // True if `schema_id`, or what it refers to, takes a node of `kind`.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by MAX_REFERENCE_DEPTH
    [[nodiscard]] bool admits_kind(SchemaId schema_id, NodeKind kind,
                                   std::size_t depth) const {
        constexpr std::size_t MAX_REFERENCE_DEPTH = 16;
        const SchemaNode& rules = schema(schema_id);
        if (!kind_allowed(rules, kind)) {
            return false;
        }
        if (rules.ref && depth < MAX_REFERENCE_DEPTH) {
            return admits_kind(*rules.ref, kind, depth + 1);
        }
        return true;
    }

    static bool kind_allowed(const SchemaNode& rules, NodeKind kind) {
        if (!rules.kind_given) {
            return true;
        }
        switch (kind) {
            case NodeKind::MAP:
                return rules.allows_map;
            case NodeKind::SEQUENCE:
                return rules.allows_sequence;
            case NodeKind::SCALAR:
                return rules.allows_scalar;
        }
        return false;
    }

    static std::string kind_phrase(const SchemaNode& rules) {
        std::vector<std::string> phrases;
        if (rules.allows_scalar) {
            phrases.push_back(type_phrase(rules.scalar_type));
        }
        if (rules.allows_map) {
            phrases.emplace_back("a map");
        }
        if (rules.allows_sequence) {
            phrases.emplace_back("a list");
        }
        std::string out;
        for (std::size_t i = 0; i < phrases.size(); ++i) {
            if (i > 0) {
                out += i + 1 == phrases.size() ? " or " : ", ";
            }
            out += phrases[i];
        }
        return out;
    }

    void check_secret(NodeId node_id, const std::string& path, Outcome& out) {
        const NodeView value = node(node_id);
        const bool reference = value.kind == NodeKind::MAP &&
                               document_->entry_count(node_id) == 1 &&
                               node(key_of(node_id, 0)).text == "secret";
        if (reference) {
            error(out, CF_SECRET, node_id,
                  "secrets are not available in this version");
            return;
        }
        error(out, CF_NOT_A_SECRET, node_id,
              label(path) + " must be a secret reference");
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded as `check`
    void check_one_of(const SchemaNode& rules, NodeId node_id,
                      const std::string& path, Outcome& out,
                      std::set<std::string>& evaluated) {
        std::optional<std::size_t> best;
        bool best_admits = false;
        std::vector<Outcome> outcomes(rules.one_of.size());
        std::vector<std::set<std::string>> keys(rules.one_of.size());
        const NodeKind kind = node(node_id).kind;
        for (std::size_t i = 0; i < rules.one_of.size(); ++i) {
            check(rules.one_of[i], node_id, path, outcomes[i], keys[i]);
            const bool admits = admits_kind(rules.one_of[i], kind, 0);
            const bool better =
                !best || (admits && !best_admits) ||
                (admits == best_admits &&
                 outcomes[i].error_count() < outcomes[*best].error_count());
            if (better) {
                best = i;
                best_admits = admits;
            }
        }
        // The first branch that passes, else, among the branches that take
        // a node of this kind, the one with the fewest errors (the first of
        // equals): its errors are the most specific.
        out.take(outcomes.at(best.value_or(0)));
        evaluated.insert(keys.at(best.value_or(0)).begin(),
                         keys.at(best.value_or(0)).end());
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded as `check`
    void check_map(const SchemaNode& rules, NodeId map, const std::string& path,
                   Outcome& out, std::set<std::string>& evaluated) {
        const std::size_t count = document_->entry_count(map);
        for (std::size_t i = 0; i < count; ++i) {
            const NodeId key = key_of(map, i);
            const std::string& text = node(key).text;
            for (const auto& [name, property] : rules.properties) {
                if (name != text) {
                    continue;
                }
                evaluated.insert(text);
                if (schema(property).reserved) {
                    error(out, CF_RESERVED, key,
                          child_path(path, text) +
                              " is reserved and not available in this "
                              "version");
                    break;
                }
                std::set<std::string> inner;
                check(property, value_of(map, i), child_path(path, text), out,
                      inner);
                break;
            }
        }
        check_required(rules, map, path, out);
        check_exclusive(rules, map, path, out);
        check_dependent(rules, map, path, out);
        if (rules.unevaluated != Unevaluated::ALLOWED) {
            check_unevaluated(rules, map, path, out, evaluated);
        }
    }

    void check_required(const SchemaNode& rules, NodeId map,
                        const std::string& path, Outcome& out) {
        for (const std::string& key : rules.required) {
            if (!document_->find(map, key)) {
                error(out, CF_MISSING_KEY, map, label(path) + " needs " + key);
            }
        }
    }

    // The index of the entry `key` of `map`, if any.
    [[nodiscard]] std::optional<std::size_t> entry_of(
        NodeId map, std::string_view key) const {
        for (std::size_t i = 0; i < document_->entry_count(map); ++i) {
            if (node(key_of(map, i)).text == key) {
                return i;
            }
        }
        return std::nullopt;
    }

    void check_exclusive(const SchemaNode& rules, NodeId map,
                         const std::string& path, Outcome& out) {
        // A key in two groups (`value` and `mask` of a write) is reported
        // once.
        std::set<std::size_t> reported;
        for (const std::vector<std::string>& group : rules.exclusive) {
            std::optional<std::size_t> first;
            for (std::size_t i = 0; i < document_->entry_count(map); ++i) {
                const std::string& text = node(key_of(map, i)).text;
                if (std::ranges::find(group, text) == group.end()) {
                    continue;
                }
                if (!first) {
                    first = i;
                    continue;
                }
                if (!reported.insert(i).second) {
                    continue;
                }
                error(out, CF_EXCLUSIVE, key_of(map, i),
                      child_path(path, text) + " cannot be given with " +
                          node(key_of(map, *first)).text);
            }
        }
    }

    void check_dependent(const SchemaNode& rules, NodeId map,
                         const std::string& path, Outcome& out) {
        for (const auto& [key, needed] : rules.dependent_required) {
            const std::optional<std::size_t> entry = entry_of(map, key);
            if (!entry.has_value()) {
                continue;
            }
            const NodeId key_node = key_of(map, *entry);
            for (const std::string& other : needed) {
                if (!entry_of(map, other)) {
                    error(out, CF_NEEDS, key_node,
                          child_path(path, key) + " needs " + other);
                }
            }
        }
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded as `check`
    void check_unevaluated(const SchemaNode& rules, NodeId map,
                           const std::string& path, Outcome& out,
                           const std::set<std::string>& evaluated) {
        // Keys typed by "x-scada-key-type" that are equal after typing are
        // CF-111 (contract 1.2; D-106).
        std::set<std::string, std::less<>> typed_keys;
        for (std::size_t i = 0; i < document_->entry_count(map); ++i) {
            const NodeId key = key_of(map, i);
            const std::string& text = node(key).text;
            if (evaluated.contains(text)) {
                continue;
            }
            if (rules.unevaluated == Unevaluated::REFUSED) {
                error(out, CF_UNKNOWN_KEY, key,
                      "unknown key " + quote_text(text) + " in " + label(path));
                continue;
            }
            const std::optional<std::string> canonical =
                check_key(rules.key_type, key, path, out);
            if (!canonical) {
                continue;
            }
            if (!typed_keys.insert(*canonical).second) {
                error(out, CF_DUPLICATE_KEY, key,
                      "duplicate key " + quote_text(text));
                continue;
            }
            std::set<std::string> inner;
            check(rules.unevaluated_schema, value_of(map, i),
                  child_path(path, text), out, inner);
        }
    }

    // Types a map key by `key_type`; its canonical text, or nothing (with
    // an error) if it does not fit.
    std::optional<std::string> check_key(const std::string& key_type,
                                         NodeId key, const std::string& path,
                                         Outcome& out) {
        const NodeView text_node = node(key);
        const std::string& text = text_node.text;
        if (key_type.empty() || key_type == "string") {
            out.type(key, ScalarType::STRING);
            return text;
        }
        const bool plain = text_node.style == ScalarStyle::PLAIN;
        if (key_type == "integer") {
            const IntegerRead read = plain ? read_integer(text) : IntegerRead{};
            if (read.form == IntegerForm::NOT_INTEGER) {
                error(out, CF_WRONG_TYPE, key,
                      "key " + quote_text(text) + " in " + label(path) +
                          " must be an integer");
                return std::nullopt;
            }
            const std::optional<std::int64_t> value =
                read.form == IntegerForm::FITS ? as_int64(read.value)
                                               : std::nullopt;
            if (!value) {
                error(out, CF_OUT_OF_RANGE, key,
                      "key " + quote_text(text) + " in " + label(path) +
                          " must be between " + std::string(INT64_MIN_TEXT) +
                          " and " + std::string(INT64_MAX_TEXT));
                return std::nullopt;
            }
            out.type(key, ScalarType::INTEGER);
            return std::to_string(*value);
        }
        if (key_type == "boolean") {
            const std::optional<bool> value =
                plain ? parse_boolean(text) : std::nullopt;
            if (!value) {
                error(out, CF_WRONG_TYPE, key,
                      "key " + quote_text(text) + " in " + label(path) +
                          " must be true or false");
                return std::nullopt;
            }
            out.type(key, ScalarType::BOOLEAN);
            return text;
        }
        const std::optional<std::string_view> kind = name_kind(key_type);
        if (kind && !name_matches(key_type, text)) {
            error(out, CF_BAD_NAME, key,
                  quote_text(text) + " is not a valid " + std::string(*kind) +
                      " name");
            return std::nullopt;
        }
        out.type(key, ScalarType::STRING);
        return text;
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded as `check`
    void check_sequence(const SchemaNode& rules, NodeId sequence,
                        const std::string& path, Outcome& out) {
        const std::size_t count = document_->items(sequence).size();
        const bool too_few = rules.min_items && count < *rules.min_items;
        const bool too_many = rules.max_items && count > *rules.max_items;
        if (too_few || too_many) {
            const std::string min = std::to_string(rules.min_items.value_or(0));
            const std::string max =
                rules.max_items ? std::to_string(*rules.max_items) : "any";
            error(out, CF_OUT_OF_RANGE, sequence,
                  "the number of entries of " + label(path) +
                      " must be between " + min + " and " + max);
        }
        if (!rules.items) {
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            std::set<std::string> inner;
            check(*rules.items, document_->items(sequence)[i],
                  label(path) + "[" + std::to_string(i) + "]", out, inner);
        }
    }

    // --- Scalars ---------------------------------------------------------

    void check_scalar(const SchemaNode& rules, NodeId scalar,
                      const std::string& path, Outcome& out) {
        const NodeView value = node(scalar);
        const std::string key = label(path);
        if (std::ranges::find(rules.reserved_values, value.text) !=
            rules.reserved_values.end()) {
            error(out, CF_RESERVED, scalar,
                  key + ": " + quote_text(value.text) +
                      " is reserved and not available in this version");
            return;
        }
        // A schema without a type of its own that refers to or composes
        // others leaves the typing to them (D-107).
        const bool composed =
            rules.ref || !rules.all_of.empty() || !rules.one_of.empty();
        // A schema without an `x-scada-type` types a scalar as STRING only if
        // nothing typed it in this check, so the order of `allOf` parts does
        // not matter.
        const bool own_type =
            !rules.scalar_type.empty() || (!composed && !out.typed(scalar));
        std::optional<ScalarType> type;
        if (own_type) {
            type = type_scalar(rules, scalar, key, out);
            if (!type) {
                return;
            }
        }
        if (rules.const_text && value.text != *rules.const_text &&
            !same_number(rules, value.text, *rules.const_text)) {
            error(out, CF_NOT_ALLOWED, scalar,
                  key + " must be one of " + *rules.const_text);
            return;
        }
        if (rules.has_enum && !in_enum(rules, value.text)) {
            error(out, CF_NOT_ALLOWED, scalar,
                  key + " must be one of " + join(rules.enum_texts));
            return;
        }
        if (type) {
            out.type(scalar, *type);
        }
    }

    static bool is_integer_type(std::string_view type) {
        return type == "integer" || type == "uint64" || type == "power_of_two";
    }

    static bool same_number(const SchemaNode& rules, std::string_view text,
                            std::string_view other) {
        if (!is_integer_type(rules.scalar_type)) {
            return false;
        }
        const std::optional<Integer> lhs = parse_integer(text);
        const std::optional<Integer> rhs = parse_integer(other);
        return lhs && rhs && compare_numbers(*lhs, *rhs) == 0;
    }

    static bool in_enum(const SchemaNode& rules, std::string_view text) {
        return std::ranges::any_of(
            rules.enum_texts, [&rules, text](const std::string& allowed) {
                return allowed == text || same_number(rules, text, allowed);
            });
    }

    // Checks that the number `value` of `scalar` lies within the schema's
    // "minimum" and "maximum".
    bool number_in_range(const SchemaNode& rules, const Number& value,
                         NodeId scalar, const std::string& key, Outcome& out) {
        const std::optional<Number> min =
            rules.minimum ? parse_number(*rules.minimum) : std::nullopt;
        const std::optional<Number> max =
            rules.maximum ? parse_number(*rules.maximum) : std::nullopt;
        if ((min && compare_numbers(value, *min) < 0) ||
            (max && compare_numbers(value, *max) > 0)) {
            error(out, CF_OUT_OF_RANGE, scalar,
                  key + " must be between " + rules.minimum.value_or("any") +
                      " and " + rules.maximum.value_or("any"));
            return false;
        }
        return true;
    }

    std::optional<ScalarType> type_integer(const SchemaNode& rules,
                                           NodeId scalar,
                                           const std::string& key,
                                           Outcome& out) {
        const NodeView value = node(scalar);
        const IntegerRead read = value.style == ScalarStyle::PLAIN
                                     ? read_integer(value.text)
                                     : IntegerRead{};
        if (read.form == IntegerForm::NOT_INTEGER) {
            error(out, CF_WRONG_TYPE, scalar, key + " must be an integer");
            return std::nullopt;
        }
        const std::optional<Integer> parsed =
            read.form == IntegerForm::FITS ? std::optional<Integer>(read.value)
                                           : std::nullopt;
        const bool fits = parsed && (rules.scalar_type == "uint64"
                                         ? as_uint64(*parsed).has_value()
                                         : as_int64(*parsed).has_value());
        if (!parsed || !fits) {
            error(
                out, CF_OUT_OF_RANGE, scalar,
                key + " must be between " +
                    rules.minimum.value_or(rules.scalar_type == "uint64"
                                               ? "0"
                                               : std::string(INT64_MIN_TEXT)) +
                    " and " +
                    rules.maximum.value_or(rules.scalar_type == "uint64"
                                               ? std::string(UINT64_MAX_TEXT)
                                               : std::string(INT64_MAX_TEXT)));
            return std::nullopt;
        }
        if (!number_in_range(rules, Number{*parsed}, scalar, key, out)) {
            return std::nullopt;
        }
        if (rules.scalar_type == "power_of_two") {
            const std::uint64_t magnitude = parsed->magnitude;
            if (parsed->negative || magnitude == 0 ||
                (magnitude & (magnitude - 1)) != 0) {
                error(out, CF_WRONG_TYPE, scalar,
                      key + " must be a power of two");
                return std::nullopt;
            }
        }
        return ScalarType::INTEGER;
    }

    // What a plain text gives as a number of `number` and its kin (D-107).
    enum class NumberRead : std::uint8_t { NONE, NUMBER, REFUSED };

    // Reads `text` as a number: an integer of the integer form, which must
    // fit int64 or uint64, else a decimal of the decimal form, which must be
    // finite. NONE, with no error, for a text of neither form; REFUSED, with
    // the error given, for one of a form that does not fit.
    NumberRead read_number(std::string_view text, NodeId scalar,
                           const std::string& key, std::string_view phrase,
                           Outcome& out, Number& number) {
        const IntegerRead integer = read_integer(text);
        if (integer.form != IntegerForm::NOT_INTEGER) {
            const bool fits =
                integer.form == IntegerForm::FITS &&
                (as_int64(integer.value) || as_uint64(integer.value));
            if (!fits) {
                error(out, CF_OUT_OF_RANGE, scalar,
                      key + " must be between " + std::string(INT64_MIN_TEXT) +
                          " and " + std::string(UINT64_MAX_TEXT));
                return NumberRead::REFUSED;
            }
            number = Number{integer.value};
            return NumberRead::NUMBER;
        }
        if (!has_decimal_form(text)) {
            return NumberRead::NONE;
        }
        const std::optional<double> decimal = parse_decimal(text);
        if (!decimal) {
            error(out, CF_WRONG_TYPE, scalar,
                  key + " must be " + std::string(phrase) +
                      "; this one is not finite or rounds to zero");
            return NumberRead::REFUSED;
        }
        number = Number{*decimal};
        return NumberRead::NUMBER;
    }

    static ScalarType type_of(const Number& number) {
        return std::holds_alternative<Integer>(number) ? ScalarType::INTEGER
                                                       : ScalarType::DECIMAL;
    }

    std::optional<ScalarType> type_number(const SchemaNode& rules,
                                          NodeId scalar, const std::string& key,
                                          Outcome& out) {
        const NodeView value = node(scalar);
        const std::string phrase = type_phrase(rules.scalar_type);
        Number number;
        NumberRead read = NumberRead::NONE;
        if (value.style == ScalarStyle::PLAIN) {
            if (rules.scalar_type == "decimal") {
                if (const std::optional<double> decimal =
                        nearest_binary64(value.text)) {
                    number = Number{*decimal};
                    read = NumberRead::NUMBER;
                }
            } else {
                read =
                    read_number(value.text, scalar, key, phrase, out, number);
            }
        }
        if (read == NumberRead::REFUSED) {
            return std::nullopt;
        }
        if (read == NumberRead::NONE) {
            error(out, CF_WRONG_TYPE, scalar, key + " must be " + phrase);
            return std::nullopt;
        }
        if (!number_in_range(rules, number, scalar, key, out)) {
            return std::nullopt;
        }
        return type_of(number);
    }

    // Durations and sizes, with "x-scada-min" and "x-scada-max" of the same
    // form.
    std::optional<ScalarType> type_with_unit(const SchemaNode& rules,
                                             NodeId scalar,
                                             const std::string& key,
                                             Outcome& out) {
        const NodeView value = node(scalar);
        const bool duration = rules.scalar_type == "duration";
        const auto parse = [duration](std::string_view text) {
            return duration ? parse_duration_ms(text) : parse_size_bytes(text);
        };
        const UnitRead read = duration ? read_duration_ms(value.text)
                                       : read_size_bytes(value.text);
        if (read.form == IntegerForm::TOO_LARGE) {
            error(out, CF_OUT_OF_RANGE, scalar,
                  key + " must be at most " +
                      (duration ? std::to_string(MAX_DURATION_MS) + "ms"
                                : std::string(UINT64_MAX_TEXT) + "B"));
            return std::nullopt;
        }
        const std::optional<std::uint64_t> parsed =
            read.form == IntegerForm::FITS
                ? std::optional<std::uint64_t>(read.value)
                : std::nullopt;
        if (!parsed) {
            error(out, CF_BAD_UNIT_VALUE, scalar,
                  quote_text(value.text) + " is not a valid " +
                      (duration ? "duration" : "size"));
            return std::nullopt;
        }
        // A duration that validation accepts is readable in int64
        // nanoseconds (D-107).
        if (duration && *parsed > MAX_DURATION_MS) {
            error(out, CF_OUT_OF_RANGE, scalar,
                  key + " must be at most " + std::to_string(MAX_DURATION_MS) +
                      "ms");
            return std::nullopt;
        }
        const std::optional<std::uint64_t> min =
            rules.scada_min ? parse(*rules.scada_min) : std::nullopt;
        const std::optional<std::uint64_t> max =
            rules.scada_max ? parse(*rules.scada_max) : std::nullopt;
        if ((min && *parsed < *min) || (max && *parsed > *max)) {
            error(out, CF_OUT_OF_RANGE, scalar,
                  key + " must be between " + rules.scada_min.value_or("0") +
                      " and " + rules.scada_max.value_or("any"));
            return std::nullopt;
        }
        return duration ? ScalarType::DURATION : ScalarType::SIZE;
    }

    std::optional<ScalarType> type_percentage(const SchemaNode& rules,
                                              NodeId scalar,
                                              const std::string& key,
                                              Outcome& out) {
        const NodeView value = node(scalar);
        const bool or_number = rules.scalar_type == "number_or_percentage";
        const std::string phrase = type_phrase(rules.scalar_type);
        if (or_number && value.style == ScalarStyle::PLAIN) {
            Number number;
            const NumberRead read =
                read_number(value.text, scalar, key, phrase, out, number);
            if (read == NumberRead::REFUSED) {
                return std::nullopt;
            }
            if (read == NumberRead::NUMBER) {
                return number_in_range(rules, number, scalar, key, out)
                           ? std::optional<ScalarType>(type_of(number))
                           : std::nullopt;
            }
        }
        // Text without `%` is not a percentage at all (CF-203); text with
        // it that does not read is a bad percentage (CF-206).
        if (!value.text.ends_with('%')) {
            error(out, CF_WRONG_TYPE, scalar, key + " must be " + phrase);
            return std::nullopt;
        }
        const std::optional<double> percent = parse_percentage(value.text);
        if (!percent) {
            error(out, CF_BAD_UNIT_VALUE, scalar,
                  quote_text(value.text) + " is not a valid percentage");
            return std::nullopt;
        }
        if (!number_in_range(rules, Number{*percent}, scalar, key, out)) {
            return std::nullopt;
        }
        return ScalarType::PERCENTAGE;
    }

    std::optional<ScalarType> type_text(const SchemaNode& rules, NodeId scalar,
                                        const std::string& key, Outcome& out) {
        const NodeView value = node(scalar);
        if (!is_free_text(value.text)) {
            error(out, CF_WRONG_TYPE, scalar,
                  key + " must be text without control characters");
            return std::nullopt;
        }
        const std::size_t max =
            rules.scada_max
                ? static_cast<std::size_t>(parse_integer(*rules.scada_max)
                                               .value_or(Integer{})
                                               .magnitude)
                : MAX_TEXT;
        const std::size_t min =
            rules.scada_min
                ? static_cast<std::size_t>(parse_integer(*rules.scada_min)
                                               .value_or(Integer{})
                                               .magnitude)
                : 0;
        if (value.text.size() < min || value.text.size() > max) {
            error(out, CF_OUT_OF_RANGE, scalar,
                  key + " must be between " + std::to_string(min) + " and " +
                      std::to_string(max) + " bytes");
            return std::nullopt;
        }
        return ScalarType::STRING;
    }

    std::optional<ScalarType> type_scalar(const SchemaNode& rules,
                                          NodeId scalar, const std::string& key,
                                          Outcome& out) {
        const std::string& type = rules.scalar_type;
        const NodeView value = node(scalar);
        if (is_integer_type(type)) {
            return type_integer(rules, scalar, key, out);
        }
        if (type == "number" || type == "decimal") {
            return type_number(rules, scalar, key, out);
        }
        if (type == "boolean") {
            if (value.style != ScalarStyle::PLAIN ||
                !parse_boolean(value.text)) {
                error(out, CF_WRONG_TYPE, scalar,
                      key + " must be true or false");
                return std::nullopt;
            }
            return ScalarType::BOOLEAN;
        }
        if (type == "duration" || type == "size") {
            return type_with_unit(rules, scalar, key, out);
        }
        if (type == "percentage" || type == "number_or_percentage") {
            return type_percentage(rules, scalar, key, out);
        }
        if (type == "expression") {
            return type_expression(scalar, ExpressionKind::RUN_TIME, out);
        }
        if (type == "config_expression") {
            return type_expression(scalar, ExpressionKind::CONFIGURATION, out);
        }
        if (const std::optional<ScalarType> special =
                type_special(rules, scalar, key, out)) {
            if (*special == ScalarType::NONE) {
                return std::nullopt;
            }
            return special;
        }
        if (type == "number_or_expression") {
            return type_number_or_expression(rules, scalar, key, out);
        }
        if (type == "for_value") {
            return type_for_value(scalar, key, out);
        }
        return type_string_form(rules, scalar, key, out);
    }

    // A plain number, otherwise an expression (Annex C). A literal number is
    // checked at once against "minimum" and "maximum", and must be an
    // integer where "type" names "integer" and not "number": only a real
    // expression waits for expansion (D-107).
    std::optional<ScalarType> type_number_or_expression(const SchemaNode& rules,
                                                        NodeId scalar,
                                                        const std::string& key,
                                                        Outcome& out) {
        const NodeView value = node(scalar);
        const bool integer_only = !rules.scalar_kinds.empty() &&
                                  listed_text(rules.scalar_kinds, "integer") &&
                                  !listed_text(rules.scalar_kinds, "number");
        // An expression of an integer key must give an INT, which the
        // expander checks on its value.
        const auto expression = [&]() -> std::optional<ScalarType> {
            const std::optional<ScalarType> typed =
                type_expression(scalar, ExpressionKind::CONFIGURATION, out);
            if (typed && integer_only) {
                return ScalarType::INTEGER_EXPRESSION;
            }
            return typed;
        };
        if (value.style != ScalarStyle::PLAIN) {
            return expression();
        }
        Number number;
        const NumberRead read =
            read_number(value.text, scalar, key, type_phrase(rules.scalar_type),
                        out, number);
        if (read == NumberRead::REFUSED) {
            return std::nullopt;
        }
        if (read == NumberRead::NONE) {
            return expression();
        }
        if (integer_only && !std::holds_alternative<Integer>(number)) {
            error(out, CF_WRONG_TYPE, scalar,
                  key + " must be an integer or an expression");
            return std::nullopt;
        }
        if (!number_in_range(rules, number, scalar, key, out)) {
            return std::nullopt;
        }
        return type_of(number);
    }

    // An expression's text, checked for its syntax (5.6, D-109). Its names
    // and types are checked against the tree in stage 5.
    std::optional<ScalarType> type_expression(NodeId scalar,
                                              ExpressionKind kind,
                                              Outcome& out) {
        ExpressionError problem;
        const std::string_view text = document_->text(scalar);
        if (parse_expression(text, kind, problem)) {
            return ScalarType::EXPRESSION;
        }
        out.error(expression_diagnostic(scalar, problem));
        return std::nullopt;
    }

    // Where an expression error is (D-109): at the offending character in
    // the file when the scalar's text is its source on one line; otherwise
    // at the scalar, with the line and column within the expression's text
    // in the message. Columns count characters (contract 1.1).
    [[nodiscard]] Diagnostic expression_diagnostic(
        NodeId scalar, const ExpressionError& problem) const {
        ExpressionSite site =
            expression_site(*document_, scalar, problem.column);
        Location location = std::move(site.location);
        const std::string& where = site.where;
        std::string message = problem.message;
        if (problem.code == CF_EXPRESSION_SYNTAX) {
            message = "expression: " + problem.message + where;
        }
        return Diagnostic{.severity = Severity::ERROR,
                          .code = problem.code,
                          .location = std::move(location),
                          .message = std::move(message),
                          .notes = {}};
    }

    static bool listed_text(const std::vector<std::string>& list,
                            std::string_view name) {
        return std::ranges::find(list, name) != list.end();
    }

    // A `repeat` range (4.4): a plain integer, which must fit int64,
    // otherwise the text of a range expression.
    std::optional<ScalarType> type_for_value(NodeId scalar,
                                             const std::string& key,
                                             Outcome& out) {
        const NodeView value = node(scalar);
        const IntegerRead read = value.style == ScalarStyle::PLAIN
                                     ? read_integer(value.text)
                                     : IntegerRead{};
        if (read.form == IntegerForm::NOT_INTEGER) {
            return type_expression(scalar, ExpressionKind::FOR_VALUE, out);
        }
        if (read.form == IntegerForm::TOO_LARGE || !as_int64(read.value)) {
            error(out, CF_OUT_OF_RANGE, scalar,
                  key + " must be between " + std::string(INT64_MIN_TEXT) +
                      " and " + std::string(INT64_MAX_TEXT));
            return std::nullopt;
        }
        return ScalarType::INTEGER;
    }

    // A raw pattern (2.3): 0 to 2^64 - 1, or a negative int64 value. An
    // integer of the form beyond these is CF-204 (D-107).
    std::optional<ScalarType> type_raw_pattern(NodeId scalar,
                                               const std::string& key,
                                               Outcome& out) {
        const NodeView value = node(scalar);
        const IntegerRead read = value.style == ScalarStyle::PLAIN
                                     ? read_integer(value.text)
                                     : IntegerRead{};
        if (read.form == IntegerForm::NOT_INTEGER) {
            error(out, CF_WRONG_TYPE, scalar, key + " must be a raw pattern");
            return std::nullopt;
        }
        if (read.form == IntegerForm::TOO_LARGE ||
            (read.value.negative && !as_int64(read.value))) {
            error(out, CF_OUT_OF_RANGE, scalar,
                  key + " must be between " + std::string(INT64_MIN_TEXT) +
                      " and " + std::string(UINT64_MAX_TEXT));
            return std::nullopt;
        }
        return ScalarType::INTEGER;
    }

    ScalarType type_number_or_boolean(NodeId scalar, const std::string& key,
                                      Outcome& out) {
        const NodeView value = node(scalar);
        const std::string phrase = "a number or true or false";
        if (value.style == ScalarStyle::PLAIN) {
            if (parse_boolean(value.text)) {
                return ScalarType::BOOLEAN;
            }
            Number number;
            const NumberRead read =
                read_number(value.text, scalar, key, phrase, out, number);
            if (read == NumberRead::REFUSED) {
                return ScalarType::NONE;
            }
            if (read == NumberRead::NUMBER) {
                return type_of(number);
            }
        }
        error(out, CF_WRONG_TYPE, scalar, key + " must be " + phrase);
        return ScalarType::NONE;
    }

    ScalarType type_number_or_tag(const SchemaNode& rules, NodeId scalar,
                                  const std::string& key, Outcome& out) {
        const NodeView value = node(scalar);
        if (value.style == ScalarStyle::PLAIN) {
            Number number;
            const NumberRead read = read_number(
                value.text, scalar, key, "a number or a tag name", out, number);
            if (read == NumberRead::REFUSED) {
                return ScalarType::NONE;
            }
            if (read == NumberRead::NUMBER) {
                return number_in_range(rules, number, scalar, key, out)
                           ? type_of(number)
                           : ScalarType::NONE;
            }
        }
        if (!is_tag_name(value.text)) {
            error(out, CF_BAD_NAME, scalar,
                  quote_text(value.text) + " is not a valid tag name");
            return ScalarType::NONE;
        }
        return ScalarType::STRING;
    }

    ScalarType type_ip_literal_host(NodeId scalar, const std::string& key,
                                    Outcome& out) {
        const NodeView value = node(scalar);
        if (is_ip_address(value.text)) {
            return ScalarType::STRING;
        }
        if (is_host_name(value.text)) {
            error(out, CF_RESERVED, scalar,
                  key +
                      ": a host name is reserved and not available in this "
                      "version");
        } else {
            error(out, CF_WRONG_TYPE, scalar, key + " must be an IP address");
        }
        return ScalarType::NONE;
    }

    // The types that combine two forms, and the raw patterns. Nothing for a
    // type that is not one of them; NONE, the error added, if the value
    // does not fit.
    std::optional<ScalarType> type_special(const SchemaNode& rules,
                                           NodeId scalar,
                                           const std::string& key,
                                           Outcome& out) {
        const std::string& type = rules.scalar_type;
        const NodeView value = node(scalar);
        const bool plain_nan =
            value.text == "nan" && value.style == ScalarStyle::PLAIN;
        if (type == "raw_pattern" ||
            (type == "raw_pattern_or_nan" && !plain_nan)) {
            return type_raw_pattern(scalar, key, out)
                .value_or(ScalarType::NONE);
        }
        if (type == "raw_pattern_or_nan") {
            return ScalarType::STRING;
        }
        if (type == "number_or_boolean") {
            return type_number_or_boolean(scalar, key, out);
        }
        if (type == "number_or_tag") {
            return type_number_or_tag(rules, scalar, key, out);
        }
        if (type == "duration_or_off") {
            if (value.text == "off") {
                return ScalarType::STRING;
            }
            SchemaNode as_duration = rules;
            as_duration.scalar_type = "duration";
            return type_with_unit(as_duration, scalar, key, out)
                .value_or(ScalarType::NONE);
        }
        if (type == "ip_literal_host") {
            return type_ip_literal_host(scalar, key, out);
        }
        if (type == "resolution" &&
            read_resolution_ns(value.text).form == IntegerForm::TOO_LARGE) {
            error(out, CF_OUT_OF_RANGE, scalar,
                  key + " must be between 1ns and " +
                      std::string(INT64_MAX_TEXT) + "ns");
            return ScalarType::NONE;
        }
        return std::nullopt;
    }

    // Free text, names and the other string forms.
    std::optional<ScalarType> type_string_form(const SchemaNode& rules,
                                               NodeId scalar,
                                               const std::string& key,
                                               Outcome& out) {
        const std::string& type = rules.scalar_type;
        const NodeView value = node(scalar);
        if (type == "text") {
            return type_text(rules, scalar, key, out);
        }
        if (const std::optional<std::string_view> kind = name_kind(type)) {
            if (!name_matches(type, value.text)) {
                error(out, CF_BAD_NAME, scalar,
                      quote_text(value.text) + " is not a valid " +
                          std::string(*kind) + " name");
                return std::nullopt;
            }
            return ScalarType::STRING;
        }
        if (const auto form = text_form(type, value.text)) {
            if (!form->first) {
                error(out, CF_WRONG_TYPE, scalar,
                      key + " must be " + std::string(form->second));
                return std::nullopt;
            }
        }
        return ScalarType::STRING;
    }

    const CompiledSchemas* schemas_;
    const Document* document_;
    ScalarTypes* types_;
    std::string_view file_label_;
};

}  // namespace

SchemaSet::SchemaSet(std::unique_ptr<CompiledSchemas> compiled)
    : compiled_(std::move(compiled)) {}
SchemaSet::SchemaSet(SchemaSet&&) noexcept = default;
SchemaSet& SchemaSet::operator=(SchemaSet&&) noexcept = default;
SchemaSet::~SchemaSet() = default;

Result<SchemaSet> SchemaSet::compile(const std::vector<SchemaSource>& sources,
                                     std::string& problem) {
    auto compiled = std::make_unique<CompiledSchemas>();
    Compiler compiler(*compiled);
    for (const SchemaSource& source : sources) {
        if (!compiler.compile_file(source, problem)) {
            return Status(StatusCode::INTERNAL, "a schema file is not valid");
        }
    }
    if (!compiler.resolve(problem)) {
        return Status(StatusCode::INTERNAL, "a schema reference is not valid");
    }
    return SchemaSet(std::move(compiled));
}

const SchemaSet& SchemaSet::builtin() {
    // A process-wide resource: compiled once, never changed (CLAUDE.md, "No
    // global mutable state outside clearly named singletons").
    static const SchemaSet INSTANCE = [] {
        std::string problem;
        Result<SchemaSet> compiled =
            SchemaSet::compile(builtin_schema_sources(), problem);
        if (!compiled.ok()) {
            panic("the built-in configuration schemas do not compile");
        }
        return std::move(compiled).value();
    }();
    return INSTANCE;
}

std::size_t SchemaSet::checks_per_node() const noexcept {
    return compiled_->checks_per_node;
}

bool SchemaSet::has(std::string_view schema_file) const {
    return compiled_->names.contains(schema_file);
}

void SchemaSet::validate(const Document& document, ScalarTypes& types,
                         std::string_view schema_file,
                         std::string_view file_label,
                         Diagnostics& diagnostics) const {
    const auto found = compiled_->names.find(schema_file);
    if (found == compiled_->names.end()) {
        panic("SchemaSet::validate: no such schema file");
    }
    Validator validator(*compiled_, document, types, file_label);
    validator.run(found->second, diagnostics);
}

}  // namespace scada::conf
