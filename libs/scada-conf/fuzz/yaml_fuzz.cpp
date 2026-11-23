// Fuzz target: the YAML subset parser (configuration schemas contract 1.1,
// 1.3). The input is the bytes of one file. Properties checked on every input:
//
// - a parse gives a document and no error, or no document and exactly one
//   error, located inside the input;
// - no limit is exceeded: nodes, depth, entries, scalar and key bytes;
// - every node lies inside the input, and a node's children come after it;
// - round trip: the document written back in the subset (block style, every
//   scalar double-quoted) parses to the same tree.

#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/conf/yaml.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scada::conf::Document;
using scada::conf::Entry;
using scada::conf::NodeId;
using scada::conf::NodeKind;
using scada::conf::ParseLimits;

// Smaller limits than the contract's, so that the fuzzer reaches them.
constexpr std::size_t FUZZ_NODES = 2000;
constexpr std::size_t FUZZ_DEPTH = 8;
constexpr std::size_t FUZZ_ENTRIES = 100;
constexpr std::size_t FUZZ_SCALAR = 1024;
constexpr std::size_t FUZZ_KEY = 16;

constexpr unsigned char FIRST_PRINTABLE = 0x20;
constexpr unsigned char ASCII_DEL = 0x7F;
constexpr unsigned char CONTINUATION_MASK = 0xC0;
constexpr unsigned char CONTINUATION = 0x80;
constexpr unsigned char LEAD_C2 = 0xC2;
constexpr unsigned char LAST_C1_TAIL = 0x9F;
constexpr unsigned char LEAD_EF = 0xEF;
constexpr unsigned char SECOND_BF = 0xBF;
constexpr unsigned char THIRD_BE = 0xBE;
constexpr unsigned NIBBLE_BITS = 4;
constexpr unsigned NIBBLE_MASK = 0xF;
constexpr std::string_view HEX_DIGITS = "0123456789ABCDEF";
constexpr std::size_t INDENT_STEP = 2;
constexpr unsigned NON_CHARACTER_FFFE = 0xFFFE;
constexpr unsigned NON_CHARACTER_FFFF = 0xFFFF;

// Ends the run with the line of the property that failed.
void check(bool condition,
           std::source_location where = std::source_location::current()) {
    if (!condition) {
        const std::string message = "yaml_fuzz: property at line " +
                                    std::to_string(where.line()) + " failed\n";
        static_cast<void>(std::fputs(message.c_str(), stderr));
        std::abort();
    }
}

// The number of characters (code points) of each line of `text`, from line 1.
std::vector<std::size_t> line_lengths(std::string_view text) {
    std::vector<std::size_t> lengths(1, 0);
    for (const char chr : text) {
        const auto byte = static_cast<unsigned char>(chr);
        if (chr == '\n') {
            lengths.push_back(0);
        } else if (chr != '\r' && (byte & CONTINUATION_MASK) != CONTINUATION) {
            ++lengths.back();
        }
    }
    return lengths;
}

// True if line `line`, column `column` lies in the input, or just after the
// end of a line.
bool inside(const std::vector<std::size_t>& lines, std::uint32_t line,
            std::uint32_t column) {
    return line >= 1 && line <= lines.size() && column >= 1 &&
           column <= lines.at(line - 1) + 1;
}

void check_limits_and_positions(const Document& document,
                                const ParseLimits& limits,
                                const std::vector<std::size_t>& lines) {
    check(document.size() > 0 && document.size() <= limits.max_nodes);
    // Depth by a walk with an explicit stack: (node, depth of its
    // collection).
    std::vector<std::pair<NodeId, std::size_t>> stack{{document.root(), 1}};
    std::size_t visited = 0;
    while (!stack.empty()) {
        const auto [node, depth] = stack.back();
        stack.pop_back();
        ++visited;
        const scada::conf::SourcePosition where = document.position(node);
        check(inside(lines, where.line, where.column));
        if (document.kind(node) == NodeKind::SCALAR) {
            check(document.text(node).size() <= limits.max_scalar_bytes);
            continue;
        }
        check(depth <= limits.max_depth);
        check(document.entry_count(node) <= limits.max_entries);
        check(document.text(node).empty());
        for (const Entry& entry : document.entries(node)) {
            check(entry.key > node && entry.value > node);
            check(document.kind(entry.key) == NodeKind::SCALAR);
            check(document.text(entry.key).size() <= limits.max_key_bytes);
            check(document.find(node, document.text(entry.key)) == entry.value);
            ++visited;
            stack.emplace_back(entry.value, depth + 1);
        }
        for (const NodeId item : document.items(node)) {
            check(item > node);
            stack.emplace_back(item, depth + 1);
        }
    }
    check(visited == document.size());
}

// `text` as a double-quoted scalar of the subset.
std::string quote(std::string_view text) {
    std::string out = "\"";
    const auto escape = [&out](char kind, unsigned value, unsigned digits) {
        out.push_back('\\');
        out.push_back(kind);
        for (unsigned i = digits; i > 0; --i) {
            out.push_back(HEX_DIGITS.at((value >> ((i - 1) * NIBBLE_BITS)) &
                                        NIBBLE_MASK));
        }
    };
    std::size_t where = 0;
    while (where < text.size()) {
        const auto byte = static_cast<unsigned char>(text[where]);
        const auto next = [&text, where](std::size_t ahead) -> unsigned {
            return where + ahead < text.size()
                       ? static_cast<unsigned char>(text[where + ahead])
                       : 0;
        };
        if (byte == '\\' || byte == '"') {
            out.push_back('\\');
            out.push_back(static_cast<char>(byte));
        } else if (byte < FIRST_PRINTABLE || byte == ASCII_DEL) {
            escape('x', byte, 2);
        } else if (byte == LEAD_C2 && next(1) <= LAST_C1_TAIL) {
            // A C1 control character, U+0080 to U+009F.
            escape('u', next(1), 4);
            ++where;
        } else if (byte == LEAD_EF && next(1) == SECOND_BF &&
                   next(2) >= THIRD_BE) {
            // U+FFFE or U+FFFF.
            escape(
                'u',
                next(2) == THIRD_BE ? NON_CHARACTER_FFFE : NON_CHARACTER_FFFF,
                4);
            where += 2;
        } else {
            out.push_back(static_cast<char>(byte));
        }
        ++where;
    }
    out.push_back('"');
    return out;
}

bool is_empty_collection(const Document& document, NodeId node) {
    return document.kind(node) != NodeKind::SCALAR &&
           document.entry_count(node) == 0;
}

void emit(const Document& document, NodeId node, std::size_t indent,
          std::string& out);

// Writes the value of an entry or an item whose indicator stands at
// `indent`: on the same line if it is a scalar or empty, else indented below.
// NOLINTNEXTLINE(misc-no-recursion): bounded by the depth limit of the parsed document
void emit_child(const Document& document, NodeId value, std::size_t indent,
                std::string& out) {
    if (document.kind(value) == NodeKind::SCALAR ||
        is_empty_collection(document, value)) {
        out.push_back(' ');
        emit(document, value, indent, out);
    } else {
        emit(document, value, indent + INDENT_STEP, out);
    }
}

// Writes `node` in block style, its entries indented by `indent` spaces.
// NOLINTNEXTLINE(misc-no-recursion): bounded by the depth limit of the parsed document
void emit(const Document& document, NodeId node, std::size_t indent,
          std::string& out) {
    const NodeKind kind = document.kind(node);
    if (kind == NodeKind::SCALAR) {
        out += quote(document.text(node));
        return;
    }
    if (document.entry_count(node) == 0) {
        out += kind == NodeKind::MAP ? "{}" : "[]";
        return;
    }
    for (const Entry& entry : document.entries(node)) {
        out += '\n' + std::string(indent, ' ') +
               quote(document.text(entry.key)) + ':';
        emit_child(document, entry.value, indent, out);
    }
    for (const NodeId item : document.items(node)) {
        out += '\n' + std::string(indent, ' ') + '-';
        emit_child(document, item, indent, out);
    }
}

// True if the trees under `lhs` and `rhs` have the same kinds, texts and
// order.
// NOLINTNEXTLINE(misc-no-recursion): bounded by the depth limit of the parsed document
bool same_tree(const Document& left, NodeId lhs, const Document& right,
               NodeId rhs) {
    if (left.kind(lhs) != right.kind(rhs) ||
        left.text(lhs) != right.text(rhs) ||
        left.entry_count(lhs) != right.entry_count(rhs)) {
        return false;
    }
    const std::span<const Entry> left_entries = left.entries(lhs);
    const std::span<const Entry> right_entries = right.entries(rhs);
    for (std::size_t i = 0; i < left_entries.size(); ++i) {
        if (left.text(left_entries[i].key) !=
                right.text(right_entries[i].key) ||
            !same_tree(left, left_entries[i].value, right,
                       right_entries[i].value)) {
            return false;
        }
    }
    const std::span<const NodeId> left_items = left.items(lhs);
    const std::span<const NodeId> right_items = right.items(rhs);
    for (std::size_t i = 0; i < left_items.size(); ++i) {
        if (!same_tree(left, left_items[i], right, right_items[i])) {
            return false;
        }
    }
    return true;
}

void parse_once(std::string_view text, const ParseLimits& limits) {
    scada::conf::Diagnostics diagnostics;
    const std::optional<Document> document =
        scada::conf::parse_yaml(text, "f.yaml", diagnostics, limits);
    const std::vector<std::size_t> lines = line_lengths(text);
    scada::conf::Report report;
    diagnostics.move_into(report);
    if (!document) {
        check(report.error_count == 1 && report.errors.size() == 1);
        const scada::conf::Location& where = report.errors.front().location;
        check(inside(lines, where.line, where.column));
        return;
    }
    check(report.error_count == 0);
    check_limits_and_positions(*document, limits, lines);

    std::string written;
    emit(*document, document->root(), 0, written);
    written.push_back('\n');
    scada::conf::Diagnostics again;
    const std::optional<Document> reparsed =
        scada::conf::parse_yaml(written, "f.yaml", again, limits);
    check(reparsed.has_value());
    if (reparsed.has_value()) {
        check(same_tree(*document, document->root(), *reparsed,
                        reparsed->root()));
    }
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    const std::string text(bytes.begin(), bytes.end());
    parse_once(text, ParseLimits{});
    ParseLimits small;
    small.max_nodes = FUZZ_NODES;
    small.max_depth = FUZZ_DEPTH;
    small.max_entries = FUZZ_ENTRIES;
    small.max_scalar_bytes = FUZZ_SCALAR;
    small.max_key_bytes = FUZZ_KEY;
    parse_once(text, small);
    return 0;
}
