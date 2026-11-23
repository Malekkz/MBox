// The parser of the YAML subset of configuration schemas contract 1.1.
//
// A recursive-descent parser over the bytes of one file. Every collection it
// enters is counted against the depth limit first, so recursion is bounded by
// `ParseLimits::max_depth`. The first refusal ends the parse (D-106).

#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/conf/tree.h"
#include "scada/conf/yaml.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scada::conf {

namespace {

constexpr std::uint16_t CF_SYNTAX = 110;
constexpr std::uint16_t CF_DUPLICATE = 111;
constexpr std::uint16_t CF_NO_VALUE = 112;
constexpr std::uint16_t CF_LIMIT = 113;
constexpr std::uint16_t CF_CHARACTER = 114;

// Code point boundaries of UTF-8 and of the characters refused.
constexpr std::uint32_t FIRST_C1 = 0x80;
constexpr std::uint32_t LAST_C1 = 0x9F;
constexpr std::uint32_t FIRST_SURROGATE = 0xD800;
constexpr std::uint32_t LAST_SURROGATE = 0xDFFF;
constexpr std::uint32_t LAST_CODE_POINT = 0x10FFFF;
constexpr unsigned char ASCII_DEL = 0x7F;
constexpr unsigned char FIRST_PRINTABLE = 0x20;

// The indentation of "no line": the end of the file.
constexpr std::int64_t NO_LINE = -1;

// A position in the input.
struct Mark {
    std::size_t pos = 0;
    std::uint32_t line = 1;
    std::uint32_t column = 1;
};

enum class Context : std::uint8_t { BLOCK, FLOW };

enum class Chomping : std::uint8_t { CLIP, STRIP, KEEP };

// UTF-8 (RFC 3629): lead bytes, continuation bytes and the bits each
// carries.
constexpr unsigned char CONTINUATION = 0x80;
constexpr unsigned char CONTINUATION_MASK = 0xC0;
constexpr unsigned char CONTINUATION_LOW = 0x80;
constexpr unsigned char CONTINUATION_HIGH = 0xBF;
constexpr unsigned char LEAD_TWO = 0xC0;
constexpr unsigned char LEAD_TWO_FIRST_VALID = 0xC2;
constexpr unsigned char LEAD_TWO_LAST = 0xDF;
constexpr unsigned char LEAD_THREE = 0xE0;
constexpr unsigned char LEAD_THREE_LAST = 0xEF;
constexpr unsigned char LEAD_THREE_SURROGATES = 0xED;
constexpr unsigned char LEAD_FOUR = 0xF0;
constexpr unsigned char LEAD_FOUR_LAST_VALID = 0xF4;
// The second byte's range where it rules out overlong forms, surrogates and
// code points above U+10FFFF.
constexpr unsigned char AFTER_E0_LOW = 0xA0;
constexpr unsigned char AFTER_ED_HIGH = 0x9F;
constexpr unsigned char AFTER_F0_LOW = 0x90;
constexpr unsigned char AFTER_F4_HIGH = 0x8F;
constexpr std::uint32_t TWO_BYTE_BITS = 0x1F;
constexpr std::uint32_t THREE_BYTE_BITS = 0x0F;
constexpr std::uint32_t FOUR_BYTE_BITS = 0x07;
constexpr std::uint32_t CONTINUATION_BITS = 0x3F;
constexpr unsigned BITS_PER_CONTINUATION = 6;
constexpr std::uint32_t LIMIT_ONE_BYTE = 0x80;
constexpr std::uint32_t LIMIT_TWO_BYTES = 0x800;
constexpr std::uint32_t LIMIT_THREE_BYTES = 0x10000;
constexpr std::size_t TWO = 2;
constexpr std::size_t THREE = 3;
constexpr std::size_t FOUR = 4;

// Code points of the escapes \N, \_, \L and \P (YAML 1.2, 5.7).
constexpr std::uint32_t NEXT_LINE = 0x85;
constexpr std::uint32_t NO_BREAK_SPACE = 0xA0;
constexpr std::uint32_t LINE_SEPARATOR = 0x2028;
constexpr std::uint32_t PARAGRAPH_SEPARATOR = 0x2029;

// Hexadecimal digits of the escapes \x, \u and \U.
constexpr std::size_t HEX_DIGITS_X = 2;
constexpr std::size_t HEX_DIGITS_U = 4;
constexpr std::size_t HEX_DIGITS_BIG_U = 8;
constexpr unsigned BITS_PER_HEX_DIGIT = 4;
constexpr std::uint32_t TEN = 10;

bool is_flow_indicator(char chr) noexcept {
    return chr == ',' || chr == '[' || chr == ']' || chr == '{' || chr == '}';
}

bool is_hex(char chr) noexcept {
    return (chr >= '0' && chr <= '9') || (chr >= 'a' && chr <= 'f') ||
           (chr >= 'A' && chr <= 'F');
}

std::uint32_t hex_value(char chr) noexcept {
    if (chr >= '0' && chr <= '9') {
        return static_cast<std::uint32_t>(chr - '0');
    }
    if (chr >= 'a' && chr <= 'f') {
        return static_cast<std::uint32_t>(chr - 'a') + TEN;
    }
    return static_cast<std::uint32_t>(chr - 'A') + TEN;
}

void append_byte(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(static_cast<unsigned char>(value)));
}

std::uint32_t continuation_byte(std::uint32_t code_point, unsigned shift) {
    return CONTINUATION | ((code_point >> shift) & CONTINUATION_BITS);
}

// Appends the UTF-8 form of `code_point`, which must be a scalar value.
void append_utf8(std::string& out, std::uint32_t code_point) {
    constexpr unsigned SHIFT_1 = BITS_PER_CONTINUATION;
    constexpr unsigned SHIFT_2 = 2 * BITS_PER_CONTINUATION;
    constexpr unsigned SHIFT_3 = 3 * BITS_PER_CONTINUATION;
    if (code_point < LIMIT_ONE_BYTE) {
        append_byte(out, code_point);
    } else if (code_point < LIMIT_TWO_BYTES) {
        append_byte(out, LEAD_TWO | (code_point >> SHIFT_1));
        append_byte(out, continuation_byte(code_point, 0));
    } else if (code_point < LIMIT_THREE_BYTES) {
        append_byte(out, LEAD_THREE | (code_point >> SHIFT_2));
        append_byte(out, continuation_byte(code_point, SHIFT_1));
        append_byte(out, continuation_byte(code_point, 0));
    } else {
        append_byte(out, LEAD_FOUR | (code_point >> SHIFT_3));
        append_byte(out, continuation_byte(code_point, SHIFT_2));
        append_byte(out, continuation_byte(code_point, SHIFT_1));
        append_byte(out, continuation_byte(code_point, 0));
    }
}

// How a UTF-8 sequence starts: its length, the bits of its lead byte, and
// the allowed range of its second byte.
struct Lead {
    std::size_t length = 0;
    std::uint32_t bits = 0;
    unsigned char low = CONTINUATION_LOW;
    unsigned char high = CONTINUATION_HIGH;
};

std::optional<Lead> lead_of(unsigned char byte) noexcept {
    if (byte >= LEAD_TWO_FIRST_VALID && byte <= LEAD_TWO_LAST) {
        return Lead{.length = TWO,
                    .bits = byte & TWO_BYTE_BITS,
                    .low = CONTINUATION_LOW,
                    .high = CONTINUATION_HIGH};
    }
    if (byte >= LEAD_THREE && byte <= LEAD_THREE_LAST) {
        return Lead{.length = THREE,
                    .bits = byte & THREE_BYTE_BITS,
                    .low = byte == LEAD_THREE ? AFTER_E0_LOW : CONTINUATION_LOW,
                    .high = byte == LEAD_THREE_SURROGATES ? AFTER_ED_HIGH
                                                          : CONTINUATION_HIGH};
    }
    if (byte >= LEAD_FOUR && byte <= LEAD_FOUR_LAST_VALID) {
        return Lead{.length = FOUR,
                    .bits = byte & FOUR_BYTE_BITS,
                    .low = byte == LEAD_FOUR ? AFTER_F0_LOW : CONTINUATION_LOW,
                    .high = byte == LEAD_FOUR_LAST_VALID ? AFTER_F4_HIGH
                                                         : CONTINUATION_HIGH};
    }
    return std::nullopt;
}

// The length of the UTF-8 sequence that starts at `text[where]` and its
// code point, or nothing if it is not valid UTF-8 (overlong forms,
// surrogates and values above U+10FFFF included).
std::optional<std::pair<std::size_t, std::uint32_t>> decode_utf8(
    std::string_view text, std::size_t where) {
    const std::optional<Lead> lead =
        lead_of(static_cast<unsigned char>(text[where]));
    if (!lead || text.size() - where < lead->length) {
        return std::nullopt;
    }
    std::uint32_t code_point = lead->bits;
    for (std::size_t i = 1; i < lead->length; ++i) {
        const auto next = static_cast<unsigned char>(text[where + i]);
        const unsigned char min = i == 1 ? lead->low : CONTINUATION_LOW;
        const unsigned char max = i == 1 ? lead->high : CONTINUATION_HIGH;
        if (next < min || next > max) {
            return std::nullopt;
        }
        code_point =
            (code_point << BITS_PER_CONTINUATION) | (next & CONTINUATION_BITS);
    }
    return std::make_pair(lead->length, code_point);
}

// The number of bytes of the UTF-8 sequence whose lead byte is `byte`, in
// input already checked to be valid UTF-8.
std::size_t sequence_length(unsigned char byte) noexcept {
    if (byte >= LEAD_FOUR) {
        return FOUR;
    }
    if (byte >= LEAD_THREE) {
        return THREE;
    }
    if (byte >= LEAD_TWO) {
        return TWO;
    }
    return 1;
}

bool is_null_word(std::string_view text) noexcept {
    return text == "null" || text == "Null" || text == "NULL" || text == "~";
}

class Parser {
public:
    Parser(std::string_view source, const std::string& path,
           Diagnostics& diagnostics, const ParseLimits& limits,
           NodeBudget* budget)
        : source_(source),
          path_(path),
          diagnostics_(&diagnostics),
          limits_(limits),
          budget_(budget),
          document_(std::vector<std::string>{path}) {}

    std::optional<Document> run() {
        if (source_.size() > MAX_FILE_BYTES) {
            fail_limit(mark_, "the file", "8 MiB");
            return std::nullopt;
        }
        if (!check_characters() || !next_content_line()) {
            return std::nullopt;
        }
        NodeId root = 0;
        if (next_indent_ == NO_LINE) {
            // A file of comments only, or empty: an empty map.
            if (!new_node(NodeKind::MAP, ScalarStyle::PLAIN, mark_, {}, root)) {
                return std::nullopt;
            }
        } else {
            if (!parse_block_node(NO_LINE, 1, root, "the document")) {
                return std::nullopt;
            }
            if (next_indent_ != NO_LINE) {
                fail_syntax(mark_,
                            "a line indented less than the "
                            "document's first line");
                return std::nullopt;
            }
        }
        document_.set_root(root);
        return std::move(document_);
    }

private:
    // --- Errors --------------------------------------------------------

    bool fail(std::uint16_t code, const Mark& where, std::string message) {
        if (!failed_) {
            failed_ = true;
            diagnostics_->error(
                code,
                Location{
                    .file = path_, .line = where.line, .column = where.column},
                std::move(message));
        }
        return false;
    }

    bool fail_syntax(const Mark& where, std::string_view construct) {
        std::string message(construct);
        message += " is not allowed in configuration files";
        return fail(CF_SYNTAX, where, std::move(message));
    }

    bool fail_character(const Mark& where) {
        return fail(CF_CHARACTER, where,
                    "invalid character at byte " + std::to_string(where.pos));
    }

    bool fail_limit(const Mark& where, std::string_view what,
                    std::string_view limit) {
        std::string message(what);
        message += " exceeds its limit of ";
        message += limit;
        return fail(CF_LIMIT, where, std::move(message));
    }

    bool fail_no_value(const Mark& where, std::string_view owner) {
        std::string message(owner);
        message += " has no value";
        return fail(CF_NO_VALUE, where, std::move(message));
    }

    // --- Characters ----------------------------------------------------

    // Refuses a byte order mark, invalid UTF-8, a NUL, a carriage return not
    // followed by a line feed, and every control character but tab and line
    // feed (C0, DEL and C1). Where a tab may stand is checked while parsing.
    bool check_characters() {
        if (source_.starts_with("\xEF\xBB\xBF")) {
            return fail_syntax(mark_, "a byte order mark");
        }
        Mark where;
        while (where.pos < source_.size()) {
            const auto byte = static_cast<unsigned char>(source_[where.pos]);
            if (byte < FIRST_C1) {
                if (byte == '\n') {
                    ++where.pos;
                    ++where.line;
                    where.column = 1;
                    continue;
                }
                const bool crlf = byte == '\r' &&
                                  where.pos + 1 < source_.size() &&
                                  source_[where.pos + 1] == '\n';
                if (crlf) {
                    ++where.pos;
                    continue;
                }
                if ((byte < FIRST_PRINTABLE && byte != '\t') ||
                    byte == ASCII_DEL) {
                    return fail_character(where);
                }
                ++where.pos;
                ++where.column;
                continue;
            }
            const auto decoded = decode_utf8(source_, where.pos);
            if (!decoded ||
                (decoded->second >= FIRST_C1 && decoded->second <= LAST_C1)) {
                return fail_character(where);
            }
            where.pos += decoded->first;
            ++where.column;
        }
        return true;
    }

    // --- Cursor --------------------------------------------------------

    [[nodiscard]] bool at_end() const noexcept {
        return mark_.pos >= source_.size();
    }

    // The byte `ahead` bytes after the cursor, or NUL past the end (NUL is
    // refused in the input, so it never stands for a real byte).
    [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept {
        const std::size_t where = mark_.pos + ahead;
        return where < source_.size() ? source_[where] : '\0';
    }

    [[nodiscard]] bool is_break(std::size_t ahead = 0) const noexcept {
        const char chr = peek(ahead);
        return chr == '\n' || chr == '\r';
    }

    [[nodiscard]] bool is_end(std::size_t ahead) const noexcept {
        return mark_.pos + ahead >= source_.size();
    }

    // A space, a tab, a line end or the end of the file.
    [[nodiscard]] bool is_space_end(std::size_t ahead) const noexcept {
        const char chr = peek(ahead);
        return is_end(ahead) || chr == ' ' || chr == '\t' || chr == '\n' ||
               chr == '\r';
    }

    // After `-`, `?` or `:`: true if the indicator starts a plain scalar.
    [[nodiscard]] bool is_plain_safe(std::size_t ahead,
                                     Context context) const noexcept {
        if (is_space_end(ahead)) {
            return false;
        }
        return context == Context::BLOCK || !is_flow_indicator(peek(ahead));
    }

    // Moves past one byte, keeping line and column (columns count
    // characters, D-106).
    void step(Mark& where) const noexcept {
        const auto byte = static_cast<unsigned char>(source_[where.pos]);
        ++where.pos;
        if (byte == '\n') {
            ++where.line;
            where.column = 1;
        } else if (byte != '\r' && (byte & CONTINUATION_MASK) != CONTINUATION) {
            ++where.column;
        }
    }

    void advance() noexcept {
        if (!at_end()) {
            step(mark_);
        }
    }

    // Moves past one character and appends its bytes to `out`.
    void take(std::string& out) {
        const std::size_t length =
            sequence_length(static_cast<unsigned char>(peek()));
        for (std::size_t i = 0; i < length && !at_end(); ++i) {
            out.push_back(peek());
            advance();
        }
    }

    void skip_line_break() noexcept {
        if (peek() == '\r') {
            advance();
        }
        if (peek() == '\n') {
            advance();
        }
    }

    // Skips spaces. A tab here is outside a comment and a string (CF-114).
    bool skip_spaces() {
        while (peek() == ' ') {
            advance();
        }
        if (peek() == '\t') {
            return fail_character(mark_);
        }
        return true;
    }

    // True if a `#` at the cursor starts a comment: at a line's start or
    // after a space.
    [[nodiscard]] bool at_comment() const noexcept {
        if (peek() != '#') {
            return false;
        }
        if (mark_.pos == 0) {
            return true;
        }
        const char before = source_[mark_.pos - 1];
        return before == ' ' || before == '\t' || before == '\n' ||
               before == '\r';
    }

    void skip_comment() noexcept {
        while (!at_end() && !is_break()) {
            advance();
        }
    }

    // --- Lines ---------------------------------------------------------

    // From the start of a line: skips empty lines and comment lines and
    // stops at the first character of the next content line, whose
    // indentation it stores in `next_indent_` (NO_LINE at the end).
    bool next_content_line() {
        while (true) {
            if (at_end()) {
                next_indent_ = NO_LINE;
                return true;
            }
            while (peek() == ' ') {
                advance();
            }
            if (peek() == '\t') {
                return fail_character(mark_);
            }
            if (is_break()) {
                skip_line_break();
                continue;
            }
            if (at_end()) {
                next_indent_ = NO_LINE;
                return true;
            }
            if (at_comment()) {
                skip_comment();
                skip_line_break();
                continue;
            }
            next_indent_ = static_cast<std::int64_t>(mark_.column) - 1;
            if (mark_.column == 1) {
                if (is_document_marker()) {
                    return fail_syntax(mark_, "a document marker");
                }
                if (peek() == '%') {
                    return fail_syntax(mark_, "a directive");
                }
            }
            return true;
        }
    }

    [[nodiscard]] bool is_document_marker() const noexcept {
        const bool three =
            (peek() == '-' && peek(1) == '-' && peek(2) == '-') ||
            (peek() == '.' && peek(1) == '.' && peek(2) == '.');
        return three && is_space_end(3);
    }

    // After a value on a line: the rest of the line may hold spaces and a
    // comment only. Then moves to the next content line.
    bool finish_line() {
        if (!skip_spaces()) {
            return false;
        }
        if (at_comment()) {
            skip_comment();
        }
        if (!at_end() && !is_break()) {
            return fail_syntax(mark_, describe_unexpected());
        }
        skip_line_break();
        return next_content_line();
    }

    [[nodiscard]] std::string describe_unexpected() const {
        const char chr = peek();
        if (chr == ':') {
            return "a ':' after a value on the same line";
        }
        if (chr == '#') {
            return "a '#' without a space before it";
        }
        return "text after a value on the same line";
    }

    // --- Nodes and limits ----------------------------------------------

    bool new_node(NodeKind kind, ScalarStyle style, const Mark& where,
                  std::string text, NodeId& out) {
        if (document_.size() >= limits_.max_nodes) {
            return fail_limit(where, "the file",
                              std::to_string(limits_.max_nodes) + " nodes");
        }
        if (budget_ != nullptr) {
            if (budget_->remaining == 0) {
                return fail_limit(where, "the tree",
                                  std::to_string(budget_->limit) + " nodes");
            }
            --budget_->remaining;
        }
        out = document_.add(Node{.kind = kind,
                                 .style = style,
                                 .file = 0,
                                 .line = where.line,
                                 .column = where.column,
                                 .text = std::move(text),
                                 .children = {}});
        return true;
    }

    bool check_depth(std::size_t depth, const Mark& where) {
        if (depth > limits_.max_depth) {
            return fail_limit(where, "the nesting depth",
                              std::to_string(limits_.max_depth));
        }
        return true;
    }

    bool count_entry(std::size_t& entries, const Mark& where) {
        if (entries >= limits_.max_entries) {
            return fail_limit(where, "a collection",
                              std::to_string(limits_.max_entries) + " entries");
        }
        ++entries;
        return true;
    }

    bool check_scalar_size(const std::string& text, const Mark& start) {
        if (text.size() > limits_.max_scalar_bytes) {
            return fail_limit(
                start, "a scalar",
                std::to_string(limits_.max_scalar_bytes) + " bytes");
        }
        return true;
    }

    // --- Block structure -----------------------------------------------

    [[nodiscard]] bool is_sequence_entry() const noexcept {
        return peek() == '-' && is_space_end(1);
    }

    // The byte at `where`, or NUL past the end.
    [[nodiscard]] char byte_at(std::size_t where) const noexcept {
        return where < source_.size() ? source_[where] : '\0';
    }

    // A space, a tab, a line end or the end of the file at `where`.
    [[nodiscard]] bool space_end_at(std::size_t where) const noexcept {
        const char chr = byte_at(where);
        return where >= source_.size() || chr == ' ' || chr == '\t' ||
               chr == '\n' || chr == '\r';
    }

    // From the opening quote at `where`: the position after its closing
    // quote, if it is on the same line.
    [[nodiscard]] std::optional<std::size_t> quoted_end(
        std::size_t where) const noexcept {
        const char quote = byte_at(where);
        ++where;
        while (where < source_.size()) {
            const char chr = byte_at(where);
            if (chr == '\n' || chr == '\r') {
                return std::nullopt;
            }
            const bool doubled =
                quote == '\'' && chr == '\'' && byte_at(where + 1) == '\'';
            const bool escaped = quote == '"' && chr == '\\';
            if (doubled || escaped) {
                where += 2;
                continue;
            }
            if (chr == quote) {
                return where + 1;
            }
            ++where;
        }
        return std::nullopt;
    }

    // From a plain scalar's start at `where`: true if a `:` followed by a
    // space or the line end comes before the line end or a comment.
    [[nodiscard]] bool plain_key_ahead(std::size_t where) const noexcept {
        const std::size_t start = where;
        while (where < source_.size()) {
            const char chr = byte_at(where);
            if (chr == '\n' || chr == '\r') {
                return false;
            }
            if (chr == ':' && space_end_at(where + 1)) {
                return where > start;
            }
            if ((chr == ' ' || chr == '\t') && byte_at(where + 1) == '#') {
                return false;
            }
            ++where;
        }
        return false;
    }

    // Looks ahead on the current line: true if it holds a mapping key, a
    // scalar followed by `:` and a space or the line's end. Also true for a
    // complex key (`? `), so that the mapping refuses it. Changes nothing.
    [[nodiscard]] bool looks_like_key() const noexcept {
        const std::size_t where = mark_.pos;
        const char first = byte_at(where);
        if (first == '?' && space_end_at(where + 1)) {
            return true;
        }
        if (first == '\'' || first == '"') {
            const std::optional<std::size_t> end = quoted_end(where);
            if (!end) {
                return false;
            }
            std::size_t after = *end;
            while (byte_at(after) == ' ') {
                ++after;
            }
            return byte_at(after) == ':' && space_end_at(after + 1);
        }
        if (first == '[' || first == '{') {
            return false;
        }
        return plain_key_ahead(where);
    }

    // At the first character of a content line indented more than
    // `parent_indent`: a sequence, a mapping, or a value standing alone.
    // `depth` is the depth a collection here would have.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by ParseLimits::max_depth, checked before each collection is entered
    bool parse_block_node(std::int64_t parent_indent, std::size_t depth,
                          NodeId& out, std::string_view owner) {
        const std::int64_t indent = next_indent_;
        if (is_sequence_entry()) {
            return parse_block_sequence(indent, depth, out);
        }
        if (looks_like_key()) {
            return parse_block_mapping(indent, depth, out);
        }
        return parse_value_on_line(parent_indent, depth, out, owner);
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by ParseLimits::max_depth, checked before each collection is entered
    bool parse_block_mapping(std::int64_t indent, std::size_t depth,
                             NodeId& out) {
        const Mark start = mark_;
        NodeId map = 0;
        if (!check_depth(depth, start) ||
            !new_node(NodeKind::MAP, ScalarStyle::PLAIN, start, {}, map)) {
            return false;
        }
        std::unordered_set<std::string> keys;
        std::size_t entries = 0;
        while (true) {
            const Mark key_at = mark_;
            if (peek() == '?' && is_space_end(1)) {
                return fail_syntax(key_at, "a complex key");
            }
            if (is_sequence_entry()) {
                return fail_syntax(key_at, "a sequence entry inside a mapping");
            }
            NodeId key = 0;
            if (!parse_key(Context::BLOCK, indent, key)) {
                return false;
            }
            if (!skip_spaces()) {
                return false;
            }
            if (peek() != ':' || !is_space_end(1)) {
                return fail_syntax(mark_, "a key without ': '");
            }
            advance();
            // A copy: parsing the value adds nodes and may move this one.
            const std::string key_text = document_.node(key).text;
            if (!keys.insert(key_text).second) {
                return fail(CF_DUPLICATE, key_at, "duplicate key " + key_text);
            }
            if (!count_entry(entries, key_at)) {
                return false;
            }
            document_.append_child(map, key);
            NodeId value = 0;
            if (!parse_map_value(indent, depth, key_at, key_text, value)) {
                return false;
            }
            document_.append_child(map, value);
            if (next_indent_ < indent) {
                break;
            }
            if (next_indent_ > indent) {
                return fail_syntax(mark_,
                                   "an indentation deeper than its "
                                   "mapping's");
            }
        }
        out = map;
        return true;
    }

    // After `key:` at a mapping indented `indent`.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by ParseLimits::max_depth, checked before each collection is entered
    bool parse_map_value(std::int64_t indent, std::size_t depth,
                         const Mark& key_at, const std::string& key_text,
                         NodeId& out) {
        if (!skip_spaces()) {
            return false;
        }
        if (at_end() || is_break() || at_comment()) {
            if (!finish_line()) {
                return false;
            }
            if (next_indent_ > indent) {
                return parse_block_node(indent, depth + 1, out, key_text);
            }
            if (next_indent_ == indent && is_sequence_entry()) {
                // A sequence at the indentation of its key.
                return parse_block_sequence(indent, depth + 1, out);
            }
            return fail_no_value(key_at, key_text);
        }
        return parse_value_on_line(indent, depth + 1, out, key_text);
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by ParseLimits::max_depth, checked before each collection is entered
    bool parse_block_sequence(std::int64_t indent, std::size_t depth,
                              NodeId& out) {
        const Mark start = mark_;
        NodeId sequence = 0;
        if (!check_depth(depth, start) ||
            !new_node(NodeKind::SEQUENCE, ScalarStyle::PLAIN, start, {},
                      sequence)) {
            return false;
        }
        std::size_t entries = 0;
        while (true) {
            const Mark dash = mark_;
            if (!count_entry(entries, dash)) {
                return false;
            }
            advance();
            if (!skip_spaces()) {
                return false;
            }
            NodeId item = 0;
            if (!parse_sequence_item(indent, depth, dash, item)) {
                return false;
            }
            document_.append_child(sequence, item);
            if (next_indent_ < indent ||
                (next_indent_ == indent && !is_sequence_entry())) {
                break;
            }
            if (next_indent_ > indent) {
                return fail_syntax(mark_,
                                   "an indentation deeper than its "
                                   "sequence's");
            }
        }
        out = sequence;
        return true;
    }

    // After `- ` of a sequence indented `indent`.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by ParseLimits::max_depth, checked before each collection is entered
    bool parse_sequence_item(std::int64_t indent, std::size_t depth,
                             const Mark& dash, NodeId& out) {
        constexpr std::string_view OWNER = "a sequence item";
        if (at_end() || is_break() || at_comment()) {
            if (!finish_line()) {
                return false;
            }
            if (next_indent_ > indent) {
                return parse_block_node(indent, depth + 1, out, OWNER);
            }
            return fail_no_value(dash, OWNER);
        }
        // A compact collection starts on the line of its `-`; its own
        // indentation is its column.
        const std::int64_t item_indent =
            static_cast<std::int64_t>(mark_.column) - 1;
        if (is_sequence_entry()) {
            return parse_block_sequence(item_indent, depth + 1, out);
        }
        if (looks_like_key()) {
            return parse_block_mapping(item_indent, depth + 1, out);
        }
        return parse_value_on_line(indent, depth + 1, out, OWNER);
    }

    // A value that starts on the current line, in block context, and the
    // rest of its line(s). Ends at the next content line.
    bool parse_value_on_line(std::int64_t parent_indent, std::size_t depth,
                             NodeId& out, std::string_view owner) {
        if (!refuse_node_properties()) {
            return false;
        }
        const char chr = peek();
        if (chr == '-' && is_space_end(1)) {
            return fail_syntax(mark_,
                               "a sequence entry after a key on the "
                               "same line");
        }
        if (chr == '|' || chr == '>') {
            return parse_block_scalar(parent_indent, out);
        }
        if (chr == '[' || chr == '{') {
            if (!parse_flow_collection(parent_indent, depth, out)) {
                return false;
            }
            return finish_line();
        }
        if (chr == '\'' || chr == '"') {
            if (!parse_quoted(Context::BLOCK, parent_indent, out)) {
                return false;
            }
            return finish_line();
        }
        if (!parse_plain_value(Context::BLOCK, parent_indent, out, owner)) {
            return false;
        }
        return finish_line();
    }

    // Anchors, aliases and tags are refused wherever a node may start.
    bool refuse_node_properties() {
        switch (peek()) {
            case '&':
                return fail_syntax(mark_, "an anchor");
            case '*':
                return fail_syntax(mark_, "an alias");
            case '!':
                return fail_syntax(mark_, "a tag");
            default:
                return true;
        }
    }

    // A map key: a plain or quoted scalar on one line.
    bool parse_key(Context context, std::int64_t parent_indent, NodeId& out) {
        const Mark start = mark_;
        if (!refuse_node_properties()) {
            return false;
        }
        const char chr = peek();
        if (chr == '[' || chr == '{') {
            return fail_syntax(start, "a collection as a key");
        }
        if (chr == '\'' || chr == '"') {
            if (!parse_quoted(context, parent_indent, out)) {
                return false;
            }
        } else {
            if (!parse_plain(context, parent_indent, out)) {
                return false;
            }
            if (document_.node(out).text == "<<") {
                return fail_syntax(start, "a merge key");
            }
        }
        if (mark_.line != start.line) {
            return fail_syntax(start, "a key on several lines");
        }
        if (document_.node(out).text.size() > limits_.max_key_bytes) {
            return fail_limit(start, "a key",
                              std::to_string(limits_.max_key_bytes) + " bytes");
        }
        return true;
    }

    // --- Scalars -------------------------------------------------------

    // A plain scalar as a value: `null` and `~` are refused (CF-112).
    bool parse_plain_value(Context context, std::int64_t parent_indent,
                           NodeId& out, std::string_view owner) {
        const Mark start = mark_;
        if (!parse_plain(context, parent_indent, out)) {
            return false;
        }
        if (is_null_word(document_.node(out).text)) {
            return fail_no_value(start, owner);
        }
        return true;
    }

    bool check_plain_start(Context context) {
        const char chr = peek();
        if (chr == ',' || chr == ']' || chr == '}' || chr == '[' ||
            chr == '{') {
            return fail_syntax(mark_, std::string("a '") + chr + "' here");
        }
        if (chr == '|' || chr == '>') {
            return fail_syntax(mark_,
                               "a block scalar inside a flow "
                               "collection");
        }
        if (chr == '%' || chr == '@' || chr == '`') {
            return fail_syntax(
                mark_, std::string("a reserved indicator '") + chr + "'");
        }
        if ((chr == '-' || chr == '?' || chr == ':') &&
            !is_plain_safe(1, context)) {
            if (chr == '?') {
                return fail_syntax(mark_, "a complex key");
            }
            if (chr == ':') {
                return fail_syntax(mark_, "an empty key");
            }
            return fail_syntax(mark_, "a sequence entry here");
        }
        if (chr == '#') {
            return fail_syntax(mark_, "a comment without a value before it");
        }
        if (at_end() || is_break()) {
            return fail_syntax(mark_, "a missing value");
        }
        return true;
    }

    // Moves `where` past the line break it stands on, if any.
    void step_line_break(Mark& where) const noexcept {
        if (where.pos < source_.size() && source_[where.pos] == '\r') {
            step(where);
        }
        if (where.pos < source_.size() && source_[where.pos] == '\n') {
            step(where);
        }
    }

    // True if a plain scalar continues on the line whose first character
    // (after its spaces) is at `where`.
    [[nodiscard]] bool continues_plain(const Mark& where, Context context,
                                       std::int64_t parent_indent) const {
        const char chr = source_[where.pos];
        const auto indent = static_cast<std::int64_t>(where.column) - 1;
        if (chr == '\t' || chr == '#' || indent <= parent_indent) {
            return false;
        }
        if (context == Context::FLOW) {
            return !is_flow_indicator(chr);
        }
        const std::string_view rest = source_.substr(where.pos);
        return where.column != 1 ||
               !(rest.starts_with("---") || rest.starts_with("..."));
    }

    // Looks ahead from the line end at `from`: if the next non-empty line
    // continues a plain scalar indented more than `parent_indent`, returns
    // the mark of its first character and the number of empty lines
    // between.
    [[nodiscard]] std::optional<std::pair<Mark, std::size_t>> continuation(
        Mark from, Context context, std::int64_t parent_indent) const {
        std::size_t empty_lines = 0;
        step_line_break(from);
        while (true) {
            while (from.pos < source_.size() && source_[from.pos] == ' ') {
                step(from);
            }
            if (from.pos >= source_.size()) {
                return std::nullopt;
            }
            const char chr = source_[from.pos];
            if (chr != '\n' && chr != '\r') {
                break;
            }
            ++empty_lines;
            step_line_break(from);
        }
        if (!continues_plain(from, context, parent_indent)) {
            return std::nullopt;
        }
        return std::make_pair(from, empty_lines);
    }

    // True if the cursor stands where a plain scalar's line ends: `: `, ` #`
    // or, in flow context, a flow indicator.
    [[nodiscard]] bool ends_plain_here(Context context) const noexcept {
        const char chr = peek();
        if (chr == ':') {
            return is_space_end(1) ||
                   (context == Context::FLOW && is_flow_indicator(peek(1)));
        }
        if (chr == ' ' && peek(1) == '#') {
            return true;
        }
        return context == Context::FLOW && is_flow_indicator(chr);
    }

    // The text of one line of a plain scalar, up to its end (`: `, ` #`, a
    // flow indicator or the line end). `content_end` and `content_length`
    // follow the last character that is not a space.
    bool scan_plain_line(Context context, const Mark& start, std::string& text,
                         Mark& content_end, std::size_t& content_length) {
        while (!at_end() && !is_break() && !ends_plain_here(context)) {
            const char chr = peek();
            if (chr == '\t') {
                return fail_character(mark_);
            }
            take(text);
            if (chr != ' ') {
                content_end = mark_;
                content_length = text.size();
            }
            if (!check_scalar_size(text, start)) {
                return false;
            }
        }
        return true;
    }

    bool parse_plain(Context context, std::int64_t parent_indent, NodeId& out) {
        const Mark start = mark_;
        if (!check_plain_start(context)) {
            return false;
        }
        std::string text;
        Mark content_end = mark_;
        std::size_t content_length = 0;
        while (true) {
            if (!scan_plain_line(context, start, text, content_end,
                                 content_length)) {
                return false;
            }
            text.resize(content_length);
            if (!at_end() && !is_break()) {
                break;
            }
            const auto next = continuation(mark_, context, parent_indent);
            if (!next) {
                break;
            }
            if (next->second == 0) {
                text.push_back(' ');
            } else {
                text.append(next->second, '\n');
            }
            mark_ = next->first;
        }
        mark_ = content_end;
        return new_node(NodeKind::SCALAR, ScalarStyle::PLAIN, start,
                        std::move(text), out);
    }

    // Folds a line break inside a quoted scalar: trailing white space of the
    // line is dropped (`text` back to `keep`), empty lines give line feeds,
    // a single break a space, and the next line's leading white space is
    // skipped. False at the end of the file, or at a line not indented
    // more than `parent_indent` in block context.
    bool fold_quoted_break(std::string& text, std::size_t keep, Context context,
                           std::int64_t parent_indent, bool escaped,
                           const Mark& start) {
        text.resize(keep);
        skip_line_break();
        std::size_t empty_lines = 0;
        while (true) {
            while (peek() == ' ' || peek() == '\t') {
                advance();
            }
            if (is_break()) {
                ++empty_lines;
                skip_line_break();
                continue;
            }
            break;
        }
        if (at_end()) {
            return fail_syntax(start, "an unterminated quoted scalar");
        }
        if (context == Context::BLOCK &&
            static_cast<std::int64_t>(mark_.column) - 1 <= parent_indent) {
            return fail_syntax(mark_,
                               "a quoted scalar's line that is not "
                               "indented");
        }
        if (mark_.column == 1 && is_document_marker()) {
            return fail_syntax(mark_, "a document marker");
        }
        if (empty_lines > 0) {
            text.append(empty_lines, '\n');
        } else if (!escaped) {
            text.push_back(' ');
        }
        return true;
    }

    bool parse_escape(std::string& text) {
        const Mark where = mark_;
        advance();  // the backslash
        const char chr = peek();
        auto simple = [this, &text](char value) {
            text.push_back(value);
            advance();
            return true;
        };
        switch (chr) {
            case '0':
                return fail_syntax(where, "an escaped NUL character");
            case 'a':
                return simple('\a');
            case 'b':
                return simple('\b');
            case 't':
            case '\t':
                return simple('\t');
            case 'n':
                return simple('\n');
            case 'v':
                return simple('\v');
            case 'f':
                return simple('\f');
            case 'r':
                return simple('\r');
            case 'e':
                return simple('\x1B');
            case ' ':
            case '"':
            case '/':
            case '\\':
                return simple(chr);
            case 'N':
                advance();
                append_utf8(text, NEXT_LINE);
                return true;
            case '_':
                advance();
                append_utf8(text, NO_BREAK_SPACE);
                return true;
            case 'L':
                advance();
                append_utf8(text, LINE_SEPARATOR);
                return true;
            case 'P':
                advance();
                append_utf8(text, PARAGRAPH_SEPARATOR);
                return true;
            case 'x':
                return parse_hex_escape(where, HEX_DIGITS_X, text);
            case 'u':
                return parse_hex_escape(where, HEX_DIGITS_U, text);
            case 'U':
                return parse_hex_escape(where, HEX_DIGITS_BIG_U, text);
            default:
                return fail_syntax(where, "an unknown escape");
        }
    }

    bool parse_hex_escape(const Mark& where, std::size_t digits,
                          std::string& text) {
        advance();  // x, u or U
        std::uint32_t code_point = 0;
        for (std::size_t i = 0; i < digits; ++i) {
            const char chr = peek();
            if (!is_hex(chr)) {
                return fail_syntax(where, "an invalid escape");
            }
            code_point = (code_point << BITS_PER_HEX_DIGIT) | hex_value(chr);
            advance();
        }
        if (code_point == 0) {
            return fail_syntax(where, "an escaped NUL character");
        }
        if ((code_point >= FIRST_SURROGATE && code_point <= LAST_SURROGATE) ||
            code_point > LAST_CODE_POINT) {
            return fail_syntax(where, "an invalid escape");
        }
        append_utf8(text, code_point);
        return true;
    }

    // What one step inside a quoted scalar found.
    enum class QuotedStep : std::uint8_t { MORE, CLOSED, FAILED };

    // One character, escape or line break of a quoted scalar.
    QuotedStep quoted_step(char quote, Context context,
                           std::int64_t parent_indent, const Mark& start,
                           std::string& text, std::size_t& keep) {
        const char chr = peek();
        if (chr == quote) {
            if (quote == '\'' && peek(1) == '\'') {
                text.push_back('\'');
                advance();
                advance();
                keep = text.size();
                return QuotedStep::MORE;
            }
            advance();
            return QuotedStep::CLOSED;
        }
        bool done = true;
        if (quote == '"' && chr == '\\' && is_break(1)) {
            advance();
            done = fold_quoted_break(text, text.size(), context, parent_indent,
                                     true, start);
            keep = text.size();
        } else if (quote == '"' && chr == '\\') {
            done = parse_escape(text);
            keep = text.size();
        } else if (is_break()) {
            done = fold_quoted_break(text, keep, context, parent_indent, false,
                                     start);
            keep = text.size();
        } else {
            take(text);
            if (chr != ' ' && chr != '\t') {
                keep = text.size();
            }
        }
        return done ? QuotedStep::MORE : QuotedStep::FAILED;
    }

    bool parse_quoted(Context context, std::int64_t parent_indent,
                      NodeId& out) {
        const Mark start = mark_;
        const char quote = peek();
        advance();
        std::string text;
        // Length of `text` up to its last character that is not trailing
        // white space of the current line.
        std::size_t keep = 0;
        while (true) {
            if (at_end()) {
                return fail_syntax(start, "an unterminated quoted scalar");
            }
            const QuotedStep step_result =
                quoted_step(quote, context, parent_indent, start, text, keep);
            if (step_result == QuotedStep::FAILED) {
                return false;
            }
            if (step_result == QuotedStep::CLOSED) {
                break;
            }
            if (!check_scalar_size(text, start)) {
                return false;
            }
        }
        return new_node(NodeKind::SCALAR,
                        quote == '\'' ? ScalarStyle::SINGLE_QUOTED
                                      : ScalarStyle::DOUBLE_QUOTED,
                        start, std::move(text), out);
    }

    // The header of a block scalar: chomping and indentation indicators,
    // then a comment or nothing up to the line end.
    bool parse_block_header(Chomping& chomping, std::int64_t& indicator) {
        bool chomping_given = false;
        for (int i = 0; i < 2; ++i) {
            const char chr = peek();
            if ((chr == '-' || chr == '+') && !chomping_given) {
                chomping = chr == '-' ? Chomping::STRIP : Chomping::KEEP;
                chomping_given = true;
                advance();
            } else if (chr >= '1' && chr <= '9' && indicator == 0) {
                indicator = chr - '0';
                advance();
            } else if (chr == '0') {
                return fail_syntax(mark_, "an indentation indicator of 0");
            } else {
                break;
            }
        }
        if (!skip_spaces()) {
            return false;
        }
        if (at_comment()) {
            skip_comment();
        }
        if (!at_end() && !is_break()) {
            return fail_syntax(mark_, "text after a block scalar's header");
        }
        skip_line_break();
        return true;
    }

    // The indentation of a block scalar's content, found from its first
    // line that is not empty (`parent_indent` + 1 if it has none). Leading
    // empty lines may not be indented more than it.
    bool detect_block_indent(std::int64_t parent_indent,
                             std::int64_t& content_indent) {
        Mark where = mark_;
        std::int64_t widest_empty = 0;
        while (true) {
            std::int64_t spaces = 0;
            while (where.pos < source_.size() && source_[where.pos] == ' ') {
                step(where);
                ++spaces;
            }
            if (where.pos >= source_.size()) {
                content_indent = parent_indent + 1;
                return true;
            }
            const char chr = source_[where.pos];
            if (chr == '\n' || chr == '\r') {
                widest_empty = std::max(widest_empty, spaces);
                if (chr == '\r') {
                    step(where);
                }
                step(where);
                continue;
            }
            if (spaces <= parent_indent) {
                content_indent = parent_indent + 1;
            } else {
                content_indent = spaces;
            }
            break;
        }
        if (widest_empty > content_indent) {
            return fail_syntax(mark_,
                               "a leading empty line indented more "
                               "than its block scalar");
        }
        return true;
    }

    struct BlockLine {
        std::string text;
        bool empty = true;
        bool ends_with_break = false;
    };

    // Reads the lines of a block scalar's content, up to the first line
    // that is not empty and indented less than `content_indent`.
    bool read_block_lines(std::int64_t content_indent,
                          std::vector<BlockLine>& lines, const Mark& start) {
        std::size_t total = 0;
        while (!at_end()) {
            const Mark line_start = mark_;
            std::int64_t spaces = 0;
            while (peek() == ' ' && spaces < content_indent) {
                advance();
                ++spaces;
            }
            if (spaces < content_indent) {
                if (is_break()) {
                    skip_line_break();
                    lines.push_back(BlockLine{
                        .text = {}, .empty = true, .ends_with_break = true});
                    continue;
                }
                if (at_end()) {
                    break;
                }
                // Less indented: the scalar ends before this line.
                mark_ = line_start;
                break;
            }
            BlockLine line;
            while (!at_end() && !is_break()) {
                take(line.text);
            }
            line.empty = line.text.empty();
            line.ends_with_break = is_break();
            skip_line_break();
            total += line.text.size() + 1;
            if (total > limits_.max_scalar_bytes + 1) {
                return fail_limit(
                    start, "a scalar",
                    std::to_string(limits_.max_scalar_bytes) + " bytes");
            }
            lines.push_back(std::move(line));
        }
        return true;
    }

    static std::string fold_lines(const std::vector<BlockLine>& lines,
                                  std::size_t content_lines, bool literal) {
        std::string value;
        if (literal) {
            for (std::size_t i = 0; i < content_lines; ++i) {
                if (i > 0) {
                    value.push_back('\n');
                }
                value += lines[i].text;
            }
            return value;
        }
        bool have_previous = false;
        bool previous_normal = false;
        std::size_t pending_empty = 0;
        for (std::size_t i = 0; i < content_lines; ++i) {
            const BlockLine& line = lines[i];
            if (line.empty) {
                ++pending_empty;
                continue;
            }
            const bool normal =
                line.text.front() != ' ' && line.text.front() != '\t';
            if (have_previous) {
                if (previous_normal && normal) {
                    if (pending_empty == 0) {
                        value.push_back(' ');
                    } else {
                        value.append(pending_empty, '\n');
                    }
                } else {
                    value.append(pending_empty + 1, '\n');
                }
            } else {
                value.append(pending_empty, '\n');
            }
            value += line.text;
            have_previous = true;
            previous_normal = normal;
            pending_empty = 0;
        }
        return value;
    }

    bool parse_block_scalar(std::int64_t parent_indent, NodeId& out) {
        const Mark start = mark_;
        const bool literal = peek() == '|';
        advance();
        Chomping chomping = Chomping::CLIP;
        std::int64_t indicator = 0;
        if (!parse_block_header(chomping, indicator)) {
            return false;
        }
        std::int64_t content_indent = 0;
        if (indicator > 0) {
            content_indent = parent_indent + indicator;
        } else if (!detect_block_indent(parent_indent, content_indent)) {
            return false;
        }
        std::vector<BlockLine> lines;
        if (!read_block_lines(content_indent, lines, start)) {
            return false;
        }
        std::size_t content_lines = lines.size();
        while (content_lines > 0 && lines[content_lines - 1].empty) {
            --content_lines;
        }
        std::string value = fold_lines(lines, content_lines, literal);
        const std::size_t trailing = lines.size() - content_lines;
        if (content_lines > 0) {
            const bool had_break = lines[content_lines - 1].ends_with_break;
            if (chomping == Chomping::CLIP && had_break) {
                value.push_back('\n');
            } else if (chomping == Chomping::KEEP) {
                if (had_break) {
                    value.push_back('\n');
                }
                value.append(trailing, '\n');
            }
        } else if (chomping == Chomping::KEEP) {
            value.append(trailing, '\n');
        }
        if (!check_scalar_size(value, start) ||
            !new_node(NodeKind::SCALAR,
                      literal ? ScalarStyle::LITERAL : ScalarStyle::FOLDED,
                      start, std::move(value), out)) {
            return false;
        }
        return next_content_line();
    }

    // --- Flow collections ----------------------------------------------

    // Skips white space, line breaks and comments inside a flow collection.
    // A line inside it must be indented more than `parent_indent`.
    bool skip_flow_space(std::int64_t parent_indent) {
        bool new_line = false;
        while (!at_end()) {
            const char chr = peek();
            if (chr == ' ') {
                advance();
            } else if (chr == '\t') {
                return fail_character(mark_);
            } else if (at_comment()) {
                skip_comment();
            } else if (is_break()) {
                skip_line_break();
                new_line = true;
            } else {
                break;
            }
        }
        if (new_line && !at_end()) {
            const auto indent = static_cast<std::int64_t>(mark_.column) - 1;
            if (mark_.column == 1 && is_document_marker()) {
                return fail_syntax(mark_, "a document marker");
            }
            if (indent <= parent_indent) {
                return fail_syntax(mark_,
                                   "a flow collection's line that is "
                                   "not indented");
            }
        }
        return true;
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by ParseLimits::max_depth, checked before each collection is entered
    bool parse_flow_collection(std::int64_t parent_indent, std::size_t depth,
                               NodeId& out) {
        if (!check_depth(depth, mark_)) {
            return false;
        }
        return peek() == '[' ? parse_flow_sequence(parent_indent, depth, out)
                             : parse_flow_mapping(parent_indent, depth, out);
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by ParseLimits::max_depth, checked before each collection is entered
    bool parse_flow_sequence(std::int64_t parent_indent, std::size_t depth,
                             NodeId& out) {
        const Mark start = mark_;
        NodeId sequence = 0;
        if (!new_node(NodeKind::SEQUENCE, ScalarStyle::PLAIN, start, {},
                      sequence)) {
            return false;
        }
        advance();
        std::size_t entries = 0;
        while (true) {
            if (!skip_flow_space(parent_indent)) {
                return false;
            }
            if (at_end()) {
                return fail_syntax(start, "an unclosed flow sequence");
            }
            if (peek() == ']') {
                advance();
                break;
            }
            const Mark item_at = mark_;
            if (!count_entry(entries, item_at)) {
                return false;
            }
            NodeId item = 0;
            if (!parse_flow_node(parent_indent, depth, item,
                                 "a sequence item")) {
                return false;
            }
            document_.append_child(sequence, item);
            if (!skip_flow_space(parent_indent)) {
                return false;
            }
            if (at_end()) {
                return fail_syntax(start, "an unclosed flow sequence");
            }
            const char chr = peek();
            if (chr == ':') {
                return fail_syntax(mark_, "a mapping inside a flow sequence");
            }
            if (chr == ',') {
                advance();
                continue;
            }
            if (chr == ']') {
                advance();
                break;
            }
            return fail_syntax(mark_, "a missing ',' between entries");
        }
        out = sequence;
        return true;
    }

    // Skips flow space; false, with an error naming the collection that
    // starts at `start`, if the file ends first.
    bool skip_flow_space_in(std::int64_t parent_indent, const Mark& start,
                            std::string_view unclosed) {
        if (!skip_flow_space(parent_indent)) {
            return false;
        }
        if (at_end()) {
            return fail_syntax(start, unclosed);
        }
        return true;
    }

    // After an entry of a flow collection closed by `close`: true at `,`
    // (consumed), or at the closing bracket (not consumed); else an error.
    bool flow_separator(char close) {
        if (peek() == ',') {
            advance();
            return true;
        }
        if (peek() == close) {
            return true;
        }
        return fail_syntax(mark_, "a missing ',' between entries");
    }

    // One `key: value` entry of a flow mapping, the cursor at its key.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by ParseLimits::max_depth, checked before each collection is entered
    bool parse_flow_entry(std::int64_t parent_indent, std::size_t depth,
                          const Mark& start, NodeId map,
                          std::unordered_set<std::string>& keys,
                          std::size_t& entries) {
        constexpr std::string_view UNCLOSED = "an unclosed flow mapping";
        const Mark key_at = mark_;
        if (peek() == '?' && !is_plain_safe(1, Context::FLOW)) {
            return fail_syntax(key_at, "a complex key");
        }
        if (peek() == ',') {
            return fail_syntax(key_at, "an empty entry");
        }
        NodeId key = 0;
        if (!parse_key(Context::FLOW, parent_indent, key) ||
            !skip_flow_space_in(parent_indent, start, UNCLOSED)) {
            return false;
        }
        const std::string key_text = document_.node(key).text;
        if (peek() == ',' || peek() == '}') {
            return fail_no_value(key_at, key_text);
        }
        if (peek() != ':') {
            return fail_syntax(mark_, "a key without ':'");
        }
        advance();
        if (!keys.insert(key_text).second) {
            return fail(CF_DUPLICATE, key_at, "duplicate key " + key_text);
        }
        if (!count_entry(entries, key_at) ||
            !skip_flow_space_in(parent_indent, start, UNCLOSED)) {
            return false;
        }
        if (peek() == ',' || peek() == '}') {
            return fail_no_value(key_at, key_text);
        }
        NodeId value = 0;
        if (!parse_flow_node(parent_indent, depth, value, key_text)) {
            return false;
        }
        document_.append_child(map, key);
        document_.append_child(map, value);
        return true;
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by ParseLimits::max_depth, checked before each collection is entered
    bool parse_flow_mapping(std::int64_t parent_indent, std::size_t depth,
                            NodeId& out) {
        constexpr std::string_view UNCLOSED = "an unclosed flow mapping";
        const Mark start = mark_;
        NodeId map = 0;
        if (!new_node(NodeKind::MAP, ScalarStyle::PLAIN, start, {}, map)) {
            return false;
        }
        advance();
        std::unordered_set<std::string> keys;
        std::size_t entries = 0;
        while (true) {
            if (!skip_flow_space_in(parent_indent, start, UNCLOSED)) {
                return false;
            }
            if (peek() == '}') {
                advance();
                break;
            }
            if (!parse_flow_entry(parent_indent, depth, start, map, keys,
                                  entries) ||
                !skip_flow_space_in(parent_indent, start, UNCLOSED) ||
                !flow_separator('}')) {
                return false;
            }
        }
        out = map;
        return true;
    }

    // A node inside a flow collection at depth `depth`: its own collections
    // are one deeper.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by ParseLimits::max_depth, checked before each collection is entered
    bool parse_flow_node(std::int64_t parent_indent, std::size_t depth,
                         NodeId& out, std::string_view owner) {
        if (!refuse_node_properties()) {
            return false;
        }
        const char chr = peek();
        if (chr == '[' || chr == '{') {
            return parse_flow_collection(parent_indent, depth + 1, out);
        }
        if (chr == '\'' || chr == '"') {
            return parse_quoted(Context::FLOW, parent_indent, out);
        }
        if (chr == ',') {
            return fail_syntax(mark_, "an empty entry");
        }
        return parse_plain_value(Context::FLOW, parent_indent, out, owner);
    }

    std::string_view source_;
    std::string path_;
    Diagnostics* diagnostics_;
    ParseLimits limits_;
    NodeBudget* budget_;
    Document document_;
    Mark mark_;
    std::int64_t next_indent_ = NO_LINE;
    bool failed_ = false;
};

}  // namespace

std::optional<Document> parse_yaml(std::string_view content,
                                   const std::string& path,
                                   Diagnostics& diagnostics,
                                   const ParseLimits& limits,
                                   NodeBudget* budget) {
    Parser parser(content, path, diagnostics, limits, budget);
    return parser.run();
}

}  // namespace scada::conf
