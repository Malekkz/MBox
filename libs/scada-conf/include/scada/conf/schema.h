#ifndef SCADA_CONF_SCHEMA_H
#define SCADA_CONF_SCHEMA_H

// Stage 3 of validation: the schema of each file (configuration schemas
// contract, sections 1.2, 1.4, 10.2 and Annex C; decisions D-69, D-107).
//
// The schemas are JSON Schema 2020-12 files under libs/scada-conf/schemas/,
// read with a fixed subset of keywords, each mapped to a CF-2xx code, plus
// the project keywords `x-scada-*` of Annex C. To any other JSON Schema
// validator the `x-scada-*` keywords are annotations, so the files stay valid
// JSON Schema that checks structure only.

#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/conf/types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::conf {

// A schema source: its file name (`system.schema.json`) and text.
struct SchemaSource {
    std::string name;
    std::string text;
};

// The schema files built into the library (`libs/scada-conf/schemas/`).
[[nodiscard]] const std::vector<SchemaSource>& builtin_schema_sources();

struct CompiledSchemas;
class SchemaSet;

// The rules a schema gives one key, found once for many values
// (`SchemaSet::key_rules`).
// Only `SchemaSet::key_rules` makes one, never empty, and only the set that
// made it uses it (S4 of the review of #149).
class KeyRules {
private:
    friend class SchemaSet;
    KeyRules() = default;
    const CompiledSchemas* owner_ = nullptr;
    std::vector<std::size_t> schemas_;
};

// One error of a value at its key: its code and message.
struct ValueError {
    std::uint16_t code = 0;
    std::string message;
};

// A compiled set of schema files. Immutable once compiled; may be shared
// between validations.
class SchemaSet {
public:
    // Compiles `sources`. A schema file that is not valid for this compiler
    // (an unknown keyword, a `$ref` that does not resolve, a bad value) is a
    // defect of this software: it fails with INTERNAL, and `problem` names
    // the file and what is wrong.
    [[nodiscard]] static Result<SchemaSet> compile(
        const std::vector<SchemaSource>& sources, std::string& problem);

    // The built-in schemas, compiled once.
    [[nodiscard]] static const SchemaSet& builtin();

    // Validates `document` against the root schema of the file `schema_file`
    // (for example `system.schema.json`). Errors go to `diagnostics`; the
    // type of every scalar that a schema typed is set in `types`, which has
    // an entry per node of `document` (contract 1.2, D-111). `file_label`
    // names the file in messages about its top level.
    void validate(const Document& document, ScalarTypes& types,
                  std::string_view schema_file, std::string_view file_label,
                  Diagnostics& diagnostics) const;

    // The most times validation checks one document node, at most 64: the
    // product of nested `oneOf` branches and `allOf` parts along a schema
    // path, found when the set compiles (D-107). Validation work is at most
    // this times the document's nodes.
    [[nodiscard]] std::size_t checks_per_node() const noexcept;

    // True if a schema file of this name is in the set.
    [[nodiscard]] bool has(std::string_view schema_file) const;

    // Checks `text`, a value computed at expansion (contract 13.3), written
    // as a plain scalar, against every rule that `schema` gives the key at
    // `path` below it, as stage 3 checks a value written there: its type and
    // form, "minimum" and "maximum", "x-scada-min" and "x-scada-max", "enum"
    // and "const", with the same codes and messages, the key named `label`
    // (decision D-115). `schema` names a file (`modbus.schema.json`) or a
    // definition (`modbus.schema.json#/$defs/device`); `path` holds map
    // keys, and "[]" for the items of a sequence. Every schema that a "$ref",
    // "allOf" or "oneOf" joins at a step is followed; where several give the
    // key a rule, the value must meet each. Gives the errors, none for a
    // value that meets the rules, or INTERNAL when `schema` or `path` names
    // no rule: a defect of the caller, not of the configuration.
    [[nodiscard]] Result<std::vector<ValueError>> check_value(
        std::string_view schema, std::span<const std::string> path,
        std::string_view text, std::string_view label) const;

    // The rules `schema` gives the key at `path`, as `check_value` finds
    // them, to check many values with the overload below; INTERNAL when
    // they name no rule, or when the branches of a `oneOf` on the path give
    // the key different rules: stage 3 checks a value against one branch
    // only, which a computed value alone cannot choose (M8 of the review of
    // #149).
    [[nodiscard]] Result<KeyRules> key_rules(
        std::string_view schema, std::span<const std::string> path) const;

    // Each key that the branches of a `oneOf` give different rules, as
    // `<file>: <key>`: the keys `key_rules` refuses. None in the built-in
    // set, which a test checks.
    [[nodiscard]] std::vector<std::string> keys_split_by_one_of() const;

    // `check_value` against rules found by `key_rules` of this set; INTERNAL
    // for rules of another set.
    [[nodiscard]] Result<std::vector<ValueError>> check_value(
        const KeyRules& rules, std::string_view text,
        std::string_view label) const;

    SchemaSet(SchemaSet&&) noexcept;
    SchemaSet& operator=(SchemaSet&&) noexcept;
    SchemaSet(const SchemaSet&) = delete;
    SchemaSet& operator=(const SchemaSet&) = delete;
    ~SchemaSet();

private:
    explicit SchemaSet(std::unique_ptr<CompiledSchemas> compiled);
    std::unique_ptr<CompiledSchemas> compiled_;
};

}  // namespace scada::conf

#endif  // SCADA_CONF_SCHEMA_H
