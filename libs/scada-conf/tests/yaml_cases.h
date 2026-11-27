#ifndef SCADA_CONF_TESTS_YAML_CASES_H
#define SCADA_CONF_TESTS_YAML_CASES_H

// The text of a parsed YAML file, shared by the tests of the parser and of
// its limits (yaml_limits_test.cpp, a test executable of its own, #164).

#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/conf/yaml.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace scada::conf::yaml_cases {

// NOLINTNEXTLINE(misc-no-recursion): bounded by the depth limit of the parsed document
inline void dump(const Document& document, NodeId node_id, std::string& out);

inline void dump_scalar(const Document& document, NodeId node_id,
                        std::string& out) {
    const ScalarStyle style = document.style(node_id);
    switch (style) {
        case ScalarStyle::PLAIN:
            break;
        case ScalarStyle::SINGLE_QUOTED:
            out += "'";
            break;
        case ScalarStyle::DOUBLE_QUOTED:
            out += "\"";
            break;
        case ScalarStyle::LITERAL:
            out += "|\"";
            break;
        case ScalarStyle::FOLDED:
            out += ">\"";
            break;
    }
    for (const char chr : document.text(node_id)) {
        if (chr == '\n') {
            out += "\\n";
        } else if (chr == '\t') {
            out += "\\t";
        } else {
            out.push_back(chr);
        }
    }
    if (style != ScalarStyle::PLAIN) {
        out += style == ScalarStyle::SINGLE_QUOTED ? "'" : "\"";
    }
}

// A compact, unambiguous text of a node: maps as {k: v, ...}, sequences as
// [a, b], plain scalars as written, others with their quotes ('', "", |"",
// >"") and \n, \t escaped.
// NOLINTNEXTLINE(misc-no-recursion): bounded by the depth limit of the parsed document
inline void dump(const Document& document, NodeId node_id, std::string& out) {
    const NodeKind kind = document.kind(node_id);
    if (kind == NodeKind::SCALAR) {
        dump_scalar(document, node_id, out);
        return;
    }
    if (kind == NodeKind::MAP) {
        out += "{";
        bool first = true;
        for (const Entry& entry : document.entries(node_id)) {
            out += first ? "" : ", ";
            first = false;
            dump(document, entry.key, out);
            out += ": ";
            dump(document, entry.value, out);
        }
        out += "}";
        return;
    }
    out += "[";
    bool first = true;
    for (const NodeId item : document.items(node_id)) {
        out += first ? "" : ", ";
        first = false;
        dump(document, item, out);
    }
    out += "]";
}

// The tree of `text`, or "error <line>:<column> CF-<n>: <message>".
inline std::string parse(std::string_view text, const ParseLimits& limits = {},
                         NodeBudget* budget = nullptr) {
    Diagnostics diagnostics;
    const std::optional<Document> document =
        parse_yaml(text, "f.yaml", diagnostics, limits, budget);
    Report report;
    diagnostics.move_into(report);
    if (!document) {
        if (report.errors.size() != 1) {
            return "no single error";
        }
        const Diagnostic& error = report.errors.front();
        return "error " + std::to_string(error.location.line) + ":" +
               std::to_string(error.location.column) + " " + code_text(error) +
               ": " + error.message;
    }
    if (report.error_count != 0) {
        return "errors beside a document";
    }
    std::string out;
    dump(*document, document->root(), out);
    return out;
}

// The error line only up to its code: "error <line>:<column> CF-<n>".
inline std::string code_at(std::string_view text,
                           const ParseLimits& limits = {}) {
    const std::string result = parse(text, limits);
    const std::size_t colon = result.find(": ");
    return result.substr(0, colon);
}

}  // namespace scada::conf::yaml_cases

#endif  // SCADA_CONF_TESTS_YAML_CASES_H
