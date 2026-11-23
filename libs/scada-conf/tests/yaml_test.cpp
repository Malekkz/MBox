// The YAML subset parser (configuration schemas contract 1.1, 1.3; test 3
// and the parsing cases of test 4 of section 14).

#include "scada/conf/yaml.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace scada::conf {
namespace {

constexpr std::size_t KIB_64 = std::size_t{64} * 1024;
constexpr std::size_t KEY_LIMIT = 64;
constexpr std::size_t DEPTH_LIMIT = 32;

// NOLINTNEXTLINE(misc-no-recursion): bounded by the depth limit of the parsed document
void dump(const Document& document, NodeId node_id, std::string& out);

void dump_scalar(const Node& node, std::string& out) {
    switch (node.style) {
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
    for (const char chr : node.text) {
        if (chr == '\n') {
            out += "\\n";
        } else if (chr == '\t') {
            out += "\\t";
        } else {
            out.push_back(chr);
        }
    }
    if (node.style != ScalarStyle::PLAIN) {
        out += node.style == ScalarStyle::SINGLE_QUOTED ? "'" : "\"";
    }
}

// A compact, unambiguous text of a node: maps as {k: v, ...}, sequences as
// [a, b], plain scalars as written, others with their quotes ('', "", |"",
// >"") and \n, \t escaped.
// NOLINTNEXTLINE(misc-no-recursion): bounded by the depth limit of the parsed document
void dump(const Document& document, NodeId node_id, std::string& out) {
    const Node& node = document.node(node_id);
    if (node.kind == NodeKind::SCALAR) {
        dump_scalar(node, out);
        return;
    }
    const bool map = node.kind == NodeKind::MAP;
    out += map ? "{" : "[";
    const std::size_t step = map ? 2 : 1;
    for (std::size_t i = 0; i < node.children.size(); i += step) {
        if (i > 0) {
            out += ", ";
        }
        dump(document, node.children.at(i), out);
        if (map) {
            out += ": ";
            dump(document, node.children.at(i + 1), out);
        }
    }
    out += map ? "}" : "]";
}

// The tree of `text`, or "error <line>:<column> CF-<n>: <message>".
std::string parse(std::string_view text, const ParseLimits& limits = {},
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
std::string code_at(std::string_view text, const ParseLimits& limits = {}) {
    const std::string result = parse(text, limits);
    const std::size_t colon = result.find(": ");
    return result.substr(0, colon);
}

// --- What the subset accepts ---------------------------------------------

TEST(Yaml, TheSystemExampleOfSection3) {
    EXPECT_EQ(parse("unit:\n"
                    "  id: site-0042\n"
                    "  name: Pump Station 1\n"
                    "time:\n"
                    "  timezone: Etc/UTC\n"
                    "  sync:\n"
                    "    mode: ntp\n"
                    "    servers: [10.0.0.1, 10.0.0.2]\n"
                    "    alarm_offset_ms: 500\n"
                    "storage:\n"
                    "  free_space_alarm: 10%\n"),
              "{unit: {id: site-0042, name: Pump Station 1}, time: {timezone: "
              "Etc/UTC, sync: {mode: ntp, servers: [10.0.0.1, 10.0.0.2], "
              "alarm_offset_ms: 500}}, storage: {free_space_alarm: 10%}}");
}

TEST(Yaml, TheTemplateExampleOfSection4) {
    EXPECT_EQ(
        parse("# templates/flow_meter_x.yaml\n"
              "template: flow_meter_x\n"
              "protocol: modbus\n"
              "points:\n"
              "  - name: flow\n"
              "    address: { table: holding, register: 100, type: float32, "
              "word_order: big }\n"
              "    scale: { gain: 1.0, offset: 0.0 }\n"
              "    alarms:\n"
              "      - { type: high, limit: 450, deadband: 5, on_delay: 10s }\n"
              "  - name: total\n"
              "    logging: counters\n"),
        "{template: flow_meter_x, protocol: modbus, points: [{name: flow, "
        "address: {table: holding, register: 100, type: float32, word_order: "
        "big}, scale: {gain: 1.0, offset: 0.0}, alarms: [{type: high, limit: "
        "450, deadband: 5, on_delay: 10s}]}, {name: total, logging: "
        "counters}]}");
}

// A flow mapping may run over several lines (the alarms example of 8).
TEST(Yaml, AFlowMappingOverSeveralLines) {
    EXPECT_EQ(parse("alarms:\n"
                    "  - { id: level_hh, tag: ps1.tank.level, type: "
                    "high_high, limit: 4.8,\n"
                    "      priority: critical, message: \"tank level very "
                    "high\" }\n"),
              "{alarms: [{id: level_hh, tag: ps1.tank.level, type: high_high, "
              "limit: 4.8, priority: critical, message: \"tank level very "
              "high\"}]}");
}

TEST(Yaml, ExpressionsAndPatternsAsWritten) {
    EXPECT_EQ(parse("tags:\n"
                    "  - name: site.pv.total_power\n"
                    "    expr: sum(\"ps1.inv*.power\")\n"
                    "    when: \"'meter' in options\"\n"
                    "    register: \"if(n <= 24, 1000 + 2 * (n - 1), 0)\"\n"
                    "    condition: ps1.tank.level > 0.5 AND NOT ps1.trip\n"),
              "{tags: [{name: site.pv.total_power, expr: sum(\"ps1.inv*."
              "power\"), when: \"'meter' in options\", register: \"if(n <= "
              "24, 1000 + 2 * (n - 1), 0)\", condition: ps1.tank.level > 0.5 "
              "AND NOT ps1.trip}]}");
}

TEST(Yaml, ASequenceAtTheIndentationOfItsKey) {
    EXPECT_EQ(parse("a:\n- 1\n- 2\nb: 3\n"), "{a: [1, 2], b: 3}");
}

TEST(Yaml, CompactNestedCollections) {
    EXPECT_EQ(parse("- - a\n  - b\n- c\n- k: v\n  l: w\n"),
              "[[a, b], c, {k: v, l: w}]");
}

TEST(Yaml, ValuesOnTheNextLine) {
    EXPECT_EQ(parse("a:\n    b: 1\nc:\n  - x\nd:\n  plain text\n"),
              "{a: {b: 1}, c: [x], d: plain text}");
}

TEST(Yaml, PlainScalarsKeepTheirText) {
    // Section 1.2: the text decides nothing here; the schema types it.
    EXPECT_EQ(parse("label: 0123\ngain: 10.0\nb: yes\nc: True\nd: +5\ne: .5\n"
                    "f: 1_000\ng: 010\nh: 0x10\n"),
              "{label: 0123, gain: 10.0, b: yes, c: True, d: +5, e: .5, f: "
              "1_000, g: 010, h: 0x10}");
}

TEST(Yaml, PlainScalarsWithIndicatorsInside) {
    EXPECT_EQ(parse("url: http://10.0.0.1:502/x\nneg: -5\nq: a?b\nh: a#b\n"
                    "k: -x\n"),
              "{url: http://10.0.0.1:502/x, neg: -5, q: a?b, h: a#b, k: -x}");
}

TEST(Yaml, AMultiLinePlainScalarIsFolded) {
    EXPECT_EQ(parse("a: one\n  two\n\n  three\nb: x\n"),
              "{a: one two\\nthree, b: x}");
}

TEST(Yaml, CommentsEndPlainScalars) {
    EXPECT_EQ(parse("a: one # two\n# whole line\nb: x#y  # c\n"),
              "{a: one, b: x#y}");
}

TEST(Yaml, ATabIsAllowedInAComment) {
    EXPECT_EQ(parse("a: 1 #\tcomment\n#\tline\n"), "{a: 1}");
}

TEST(Yaml, SingleQuotedScalars) {
    EXPECT_EQ(parse("a: 'it''s'\nb: ''\nc: 'one\n  two'\nd: 'x\n\n  y'\n"),
              "{a: 'it's', b: '', c: 'one two', d: 'x\\ny'}");
}

TEST(Yaml, DoubleQuotedEscapes) {
    EXPECT_EQ(parse("a: \"t\\tn\\nq\\\"b\\\\s\\/x\\x41u\\u00e9\"\n"),
              "{a: \"t\\tn\\nq\"b\\s/xAu\xc3\xa9\"}");
    EXPECT_EQ(parse("a: \"\\U0001F600\"\n"), "{a: \"\xf0\x9f\x98\x80\"}");
}

TEST(Yaml, ADoubleQuotedEscapedLineBreakJoinsWithoutASpace) {
    EXPECT_EQ(parse("a: \"one\\\n   two\"\n"), "{a: \"onetwo\"}");
    EXPECT_EQ(parse("a: \"one   \n   two\"\n"), "{a: \"one two\"}");
}

TEST(Yaml, QuotedKeysAndTheJsonForm) {
    EXPECT_EQ(parse("'a b': 1\n\"c\": 2\nd: {\"e\":3, 'f': 4}\n"),
              "{'a b': 1, \"c\": 2, d: {\"e\": 3, 'f': 4}}");
}

TEST(Yaml, FlowCollections) {
    EXPECT_EQ(parse("a: []\nb: {}\nc: [1, [2, 3], {d: e}]\nf: [x, y, ]\n"),
              "{a: [], b: {}, c: [1, [2, 3], {d: e}], f: [x, y]}");
}

TEST(Yaml, LiteralBlockScalarsWithEachChomping) {
    EXPECT_EQ(parse("a: |\n  one\n   two\n\nb: |-\n  x\n\nc: |+\n  y\n\n"
                    "d: x\n"),
              "{a: |\"one\\n two\\n\", b: |\"x\", c: |\"y\\n\\n\", d: x}");
}

TEST(Yaml, FoldedBlockScalars) {
    EXPECT_EQ(parse("a: >\n  one\n  two\n\n  three\n    more\n  four\n"),
              "{a: >\"one two\\nthree\\n  more\\nfour\\n\"}");
}

TEST(Yaml, ABlockScalarWithAnIndentationIndicator) {
    EXPECT_EQ(parse("a: |2\n    x\n  y\n"), "{a: |\"  x\\ny\\n\"}");
}

TEST(Yaml, ABlockScalarEndsAtALessIndentedLine) {
    EXPECT_EQ(parse("a: |\n  x\n  # not a comment\nb: 1\n"),
              "{a: |\"x\\n# not a comment\\n\", b: 1}");
}

TEST(Yaml, AnEmptyFileIsAnEmptyMap) {
    EXPECT_EQ(parse(""), "{}");
    EXPECT_EQ(parse("# only a comment\n\n   \n"), "{}");
}

TEST(Yaml, CrlfGivesTheSameTreeAsLf) {
    EXPECT_EQ(parse("a: 1\r\nb:\r\n  - 'x\r\n    y'\r\nc: |\r\n  z\r\n"),
              parse("a: 1\nb:\n  - 'x\n    y'\nc: |\n  z\n"));
}

TEST(Yaml, AFileWithoutAFinalLineEnd) {
    EXPECT_EQ(parse("a: 1"), "{a: 1}");
    EXPECT_EQ(parse("a: |\n  x"), "{a: |\"x\"}");
}

// The line and column of node `node_id` as "line:column".
std::string place(const Document& document, NodeId node_id) {
    const Node& node = document.node(node_id);
    return std::to_string(node.line) + ":" + std::to_string(node.column);
}

// The document of `text`; a failed parse is a test failure and gives an
// empty map.
Document parsed_document(std::string_view text) {
    Diagnostics diagnostics;
    std::optional<Document> parsed = parse_yaml(text, "f.yaml", diagnostics);
    EXPECT_TRUE(parsed.has_value());
    if (parsed.has_value()) {
        return std::move(*parsed);
    }
    Document empty({"f.yaml"});
    Node map;
    map.kind = NodeKind::MAP;
    empty.set_root(empty.add(std::move(map)));
    return empty;
}

TEST(Yaml, NodesCarryTheirLineAndColumn) {
    const Document document =
        parsed_document("a:\n  b: [x, 'y']\n# c\n\xc3\xa9: z\n");
    const NodeId root = document.root();
    const NodeId a_value = document.value(root, 0);
    EXPECT_EQ(place(document, document.key(a_value, 0)), "2:3");
    const NodeId list = document.value(a_value, 0);
    const NodeId y_node = document.node(list).children.at(1);
    EXPECT_EQ(place(document, y_node), "2:10");
    EXPECT_EQ(document.node(y_node).style, ScalarStyle::SINGLE_QUOTED);
    // Columns count characters, not bytes (D-106): é is one.
    EXPECT_EQ(place(document, document.value(root, 1)), "4:4");
    EXPECT_EQ(document.location(document.key(root, 1)).file, "f.yaml");
    EXPECT_TRUE(document.find(root, "a"));
    EXPECT_FALSE(document.find(root, "b"));
}

// --- What the subset refuses, with its code and place (test 3) ----------

TEST(Yaml, AnchorsAliasesAndTags) {
    EXPECT_EQ(parse("a: &x 1\n"),
              "error 1:4 CF-110: an anchor is not allowed in configuration "
              "files");
    EXPECT_EQ(parse("a: 1\nb: *x\n"),
              "error 2:4 CF-110: an alias is not allowed in configuration "
              "files");
    EXPECT_EQ(code_at("a: !!str 1\n"), "error 1:4 CF-110");
    EXPECT_EQ(code_at("a: !x 1\n"), "error 1:4 CF-110");
    EXPECT_EQ(code_at("- &a x\n"), "error 1:3 CF-110");
    EXPECT_EQ(code_at("a: [1, *x]\n"), "error 1:8 CF-110");
    EXPECT_EQ(code_at("a: {k: !t v}\n"), "error 1:8 CF-110");
    EXPECT_EQ(code_at("&a k: v\n"), "error 1:1 CF-110");
}

TEST(Yaml, AMergeKey) {
    EXPECT_EQ(parse("a: 1\n<<: {b: 2}\n"),
              "error 2:1 CF-110: a merge key is not allowed in configuration "
              "files");
    EXPECT_EQ(code_at("a: {<<: x}\n"), "error 1:5 CF-110");
}

TEST(Yaml, AComplexKey) {
    EXPECT_EQ(parse("? a\n: b\n"),
              "error 1:1 CF-110: a complex key is not allowed in "
              "configuration files");
    EXPECT_EQ(code_at("a: {? b: c}\n"), "error 1:5 CF-110");
    EXPECT_EQ(code_at("[a]: b\n"), "error 1:4 CF-110");
}

TEST(Yaml, DocumentMarkersAndDirectives) {
    EXPECT_EQ(parse("---\na: 1\n"),
              "error 1:1 CF-110: a document marker is not allowed in "
              "configuration files");
    EXPECT_EQ(code_at("a: 1\n---\nb: 2\n"), "error 2:1 CF-110");
    EXPECT_EQ(code_at("a: 1\n...\n"), "error 2:1 CF-110");
    EXPECT_EQ(parse("%YAML 1.2\na: 1\n"),
              "error 1:1 CF-110: a directive is not allowed in configuration "
              "files");
    EXPECT_EQ(code_at("a: 'x\n---\n  y'\n"), "error 2:1 CF-110");
}

TEST(Yaml, ADuplicateKey) {
    EXPECT_EQ(parse("a: 1\nb: 2\na: 3\n"), "error 3:1 CF-111: duplicate key a");
    // The same text quoted or not is the same key.
    EXPECT_EQ(code_at("a: 1\n'a': 2\n"), "error 2:1 CF-111");
    EXPECT_EQ(code_at("x: {a: 1, \"a\": 2}\n"), "error 1:11 CF-111");
    // 16 and 0x10 are different text: equal only once typed (stage 3).
    EXPECT_EQ(parse("bits: {16: a, 0x10: b}\n"), "{bits: {16: a, 0x10: b}}");
}

TEST(Yaml, NullAndEmptyValues) {
    EXPECT_EQ(parse("a: null\n"), "error 1:4 CF-112: a has no value");
    EXPECT_EQ(code_at("a: ~\n"), "error 1:4 CF-112");
    EXPECT_EQ(code_at("a: Null\n"), "error 1:4 CF-112");
    EXPECT_EQ(parse("a:\nb: 1\n"), "error 1:1 CF-112: a has no value");
    EXPECT_EQ(code_at("x: 1\na:\n"), "error 2:1 CF-112");
    EXPECT_EQ(code_at("a:   # comment\n"), "error 1:1 CF-112");
    EXPECT_EQ(parse("- 1\n-\n"),
              "error 2:1 CF-112: a sequence item has no value");
    EXPECT_EQ(code_at("a: {b: }\n"), "error 1:5 CF-112");
    EXPECT_EQ(code_at("a: {b}\n"), "error 1:5 CF-112");
    EXPECT_EQ(code_at("a: {b: 1, c, d: 2}\n"), "error 1:11 CF-112");
    EXPECT_EQ(code_at("a: [x, null]\n"), "error 1:8 CF-112");
    // Quoted, it is a string.
    EXPECT_EQ(parse("a: 'null'\n"), "{a: 'null'}");
}

TEST(Yaml, AByteOrderMark) {
    EXPECT_EQ(parse("\xef\xbb\xbf"
                    "a: 1\n"),
              "error 1:1 CF-110: a byte order mark is not allowed in "
              "configuration files");
}

TEST(Yaml, ANulByte) {
    EXPECT_EQ(parse(std::string_view("a: 1\nb: x\0y\n", 12)),
              "error 2:5 CF-114: invalid character at byte 9");
}

TEST(Yaml, InvalidUtf8) {
    EXPECT_EQ(code_at("a: \xff\n"), "error 1:4 CF-114");
    EXPECT_EQ(code_at("a: \xc0\x80\n"), "error 1:4 CF-114");      // overlong
    EXPECT_EQ(code_at("a: \xed\xa0\x80\n"), "error 1:4 CF-114");  // surrogate
    EXPECT_EQ(code_at("a: \xf4\x90\x80\x80\n"),
              "error 1:4 CF-114");                            // > U+10FFFF
    EXPECT_EQ(code_at("a: \xe2\x82\n"), "error 1:4 CF-114");  // cut short
    EXPECT_EQ(code_at("a: \xc3"), "error 1:4 CF-114");
}

TEST(Yaml, ControlCharacters) {
    EXPECT_EQ(code_at("a: x\x01y\n"), "error 1:5 CF-114");
    EXPECT_EQ(code_at("a: 'x\x1by'\n"), "error 1:6 CF-114");
    EXPECT_EQ(code_at("a: x\x7f\n"), "error 1:5 CF-114");
    EXPECT_EQ(code_at("a: x\xc2\x85\n"), "error 1:5 CF-114");  // C1 NEL
    EXPECT_EQ(code_at("a: 1\rb: 2\n"), "error 1:5 CF-114");    // lone CR
    // The column counts characters before the bad byte.
    EXPECT_EQ(parse("\xc3\xa9: \x02\n"),
              "error 1:4 CF-114: invalid character at byte 4");
}

TEST(Yaml, TabsOutsideCommentsAndStrings) {
    EXPECT_EQ(code_at("a:\n\tb: 1\n"), "error 2:1 CF-114");
    EXPECT_EQ(code_at("a:\t1\n"), "error 1:3 CF-114");
    EXPECT_EQ(code_at("a: x\ty\n"), "error 1:5 CF-114");
    EXPECT_EQ(code_at("a: [1,\t2]\n"), "error 1:7 CF-114");
    // Inside quotes and block scalars a tab is part of the string.
    EXPECT_EQ(parse("a: 'x\ty'\nb: |\n  \tz\n"),
              "{a: 'x\\ty', b: |\"\\tz\\n\"}");
}

TEST(Yaml, ReservedIndicatorsAndStrayCharacters) {
    EXPECT_EQ(code_at("a: @x\n"), "error 1:4 CF-110");
    EXPECT_EQ(code_at("a: `x\n"), "error 1:4 CF-110");
    EXPECT_EQ(code_at("a: ]\n"), "error 1:4 CF-110");
    EXPECT_EQ(code_at("a: ,x\n"), "error 1:4 CF-110");
    EXPECT_EQ(code_at("a: b: c\n"), "error 1:5 CF-110");
    EXPECT_EQ(code_at("a: 'x'y\n"), "error 1:7 CF-110");
    EXPECT_EQ(code_at("a: - b\n"), "error 1:4 CF-110");
    EXPECT_EQ(code_at(": x\n"), "error 1:1 CF-110");
}

TEST(Yaml, Indentation) {
    EXPECT_EQ(parse("a:\n  b: 1\n c: 2\n"),
              "error 3:2 CF-110: an indentation deeper than its mapping's is "
              "not allowed in configuration files");
    // A more indented "- b" continues the plain scalar, as in YAML.
    EXPECT_EQ(parse("- a\n  - b\n"), "[a - b]");
    EXPECT_EQ(code_at("- a\n  b: c\n"), "error 2:4 CF-110");
    EXPECT_EQ(code_at("  a: 1\nb: 2\n"), "error 2:1 CF-110");
    EXPECT_EQ(code_at("a:\n  - x\n  b: 1\n"), "error 3:3 CF-110");
    EXPECT_EQ(code_at("a: [1,\n2]\n"), "error 2:1 CF-110");
    EXPECT_EQ(code_at("a: 'x\ny'\n"), "error 2:1 CF-110");
}

TEST(Yaml, UnterminatedAndUnclosed) {
    EXPECT_EQ(parse("a: 'x\n"),
              "error 1:4 CF-110: an unterminated quoted scalar is not allowed "
              "in configuration files");
    EXPECT_EQ(code_at("a: \"x\\\"\n"), "error 1:4 CF-110");
    EXPECT_EQ(code_at("a: [1, 2\n"), "error 1:4 CF-110");
    EXPECT_EQ(code_at("a: {b: 1\n"), "error 1:4 CF-110");
    EXPECT_EQ(code_at("a: [\"a\" b]\n"), "error 1:9 CF-110");
    EXPECT_EQ(parse("a: [1 2]\n"), "{a: [1 2]}");
    EXPECT_EQ(code_at("a: [1, , 2]\n"), "error 1:8 CF-110");
    EXPECT_EQ(code_at("a: [b: c]\n"), "error 1:6 CF-110");
}

TEST(Yaml, Escapes) {
    EXPECT_EQ(parse("a: \"x\\0\"\n"),
              "error 1:6 CF-110: an escaped NUL character is not allowed in "
              "configuration files");
    EXPECT_EQ(code_at("a: \"\\x00\"\n"), "error 1:5 CF-110");
    EXPECT_EQ(code_at("a: \"\\q\"\n"), "error 1:5 CF-110");
    EXPECT_EQ(code_at("a: \"\\ud800\"\n"), "error 1:5 CF-110");
    EXPECT_EQ(code_at("a: \"\\U00110000\"\n"), "error 1:5 CF-110");
    EXPECT_EQ(code_at("a: \"\\x4\"\n"), "error 1:5 CF-110");
}

TEST(Yaml, BlockScalarHeaders) {
    EXPECT_EQ(code_at("a: |0\n  x\n"), "error 1:5 CF-110");
    EXPECT_EQ(code_at("a: | x\n"), "error 1:6 CF-110");
    EXPECT_EQ(code_at("a: |\n     \n  x\n"), "error 2:1 CF-110");
    EXPECT_EQ(code_at("a: [|]\n"), "error 1:5 CF-110");
}

TEST(Yaml, AKeyOnTwoLinesOrTooLong) {
    EXPECT_EQ(code_at("a: {'b\n  c': 1}\n"), "error 1:5 CF-110");
    EXPECT_EQ(parse(std::string(KEY_LIMIT, 'k') + ": 1\n"),
              "{" + std::string(KEY_LIMIT, 'k') + ": 1}");
    EXPECT_EQ(parse(std::string(KEY_LIMIT + 1, 'k') + ": 1\n"),
              "error 1:1 CF-113: a key exceeds its limit of 64 bytes");
}

// --- Limits (test 4), each refused before the work it bounds ------------

TEST(Yaml, TheScalarLimit) {
    const std::string at_limit(KIB_64, 'x');
    EXPECT_EQ(parse("a: " + at_limit + "\n"), "{a: " + at_limit + "}");
    const std::string over(KIB_64 + 1, 'x');
    EXPECT_EQ(parse("a: " + over + "\n"),
              "error 1:4 CF-113: a scalar exceeds its limit of 65536 bytes");
    EXPECT_EQ(code_at("a: '" + over + "'\n"), "error 1:4 CF-113");
    EXPECT_EQ(code_at("a: |\n  " + over + "\n"), "error 1:4 CF-113");
    // A very long line is refused once it passes the limit, not at its end.
    EXPECT_EQ(code_at("a: " + std::string(4 * KIB_64, 'y')),
              "error 1:4 CF-113");
}

TEST(Yaml, TheDepthLimit) {
    std::string at_limit;
    for (std::size_t i = 0; i < DEPTH_LIMIT; ++i) {
        at_limit += "[";
    }
    const std::string closed = at_limit + std::string(DEPTH_LIMIT, ']');
    EXPECT_EQ(parse(closed).substr(0, 2), "[[");
    EXPECT_EQ(parse("[" + closed + "]"),
              "error 1:33 CF-113: the nesting depth exceeds its limit of 32");
    // Block collections count the same way.
    std::string block;
    for (std::size_t i = 0; i < DEPTH_LIMIT; ++i) {
        block += std::string(2 * i, ' ') + "k:\n";
    }
    block += std::string(2 * DEPTH_LIMIT, ' ') + "v: 1\n";
    EXPECT_EQ(code_at(block), "error 33:65 CF-113");
}

TEST(Yaml, AVeryDeepFileStopsAtTheLimit) {
    // 100,000 brackets: refused at the limit, without deep recursion.
    constexpr std::size_t DEEP = 100000;
    EXPECT_EQ(code_at(std::string(DEEP, '[')), "error 1:33 CF-113");
}

TEST(Yaml, TheEntryLimit) {
    ParseLimits limits;
    limits.max_entries = 3;
    EXPECT_EQ(parse("[1, 2, 3]", limits), "[1, 2, 3]");
    EXPECT_EQ(parse("[1, 2, 3, 4]", limits),
              "error 1:11 CF-113: a collection exceeds its limit of 3 "
              "entries");
    EXPECT_EQ(code_at("a: 1\nb: 2\nc: 3\nd: 4\n", limits), "error 4:1 CF-113");
    EXPECT_EQ(code_at("- 1\n- 2\n- 3\n- 4\n", limits), "error 4:1 CF-113");
    EXPECT_EQ(ParseLimits{}.max_entries, 65536U);
}

TEST(Yaml, TheNodeLimitOfAFile) {
    ParseLimits limits;
    limits.max_nodes = 4;
    EXPECT_EQ(parse("[1, 2, 3]", limits), "[1, 2, 3]");
    EXPECT_EQ(parse("[1, 2, 3, 4]", limits),
              "error 1:11 CF-113: the file exceeds its limit of 4 nodes");
    EXPECT_EQ(ParseLimits{}.max_nodes, 1000000U);
}

TEST(Yaml, TheNodeLimitOfATree) {
    constexpr std::size_t TREE_LIMIT = 6;
    NodeBudget budget{.remaining = TREE_LIMIT, .limit = TREE_LIMIT};
    EXPECT_EQ(parse("[1, 2]", {}, &budget), "[1, 2]");
    EXPECT_EQ(budget.remaining, 3U);
    EXPECT_EQ(parse("{a: b, c: d}", {}, &budget),
              "error 1:8 CF-113: the tree exceeds its limit of 6 nodes");
    EXPECT_EQ(NodeBudget{}.limit, 4000000U);
}

TEST(Yaml, TheDefaultLimitsAreThoseOfTheContract) {
    const ParseLimits limits;
    EXPECT_EQ(limits.max_depth, DEPTH_LIMIT);
    EXPECT_EQ(limits.max_scalar_bytes, KIB_64);
    EXPECT_EQ(limits.max_key_bytes, KEY_LIMIT);
}

}  // namespace
}  // namespace scada::conf
