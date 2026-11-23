// The parser of the YAML subset of configuration schemas contract 1.1.
//
// A recursive-descent parser over the bytes of one file. Every collection it
// enters is counted against the depth limit first, so recursion is bounded by
// `ParseLimits::max_depth`. The first refusal ends the parse (D-106).

#include "scada/common/panic.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/conf/tree.h"
#include "scada/conf/yaml.h"
#include "utf8.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
constexpr std::uint32_t NON_CHARACTER_FFFE = 0xFFFE;
constexpr std::uint32_t NON_CHARACTER_FFFF = 0xFFFF;
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

bool is_null_word(std::string_view text) noexcept {
    return text == "null" || text == "Null" || text == "NULL" || text == "~";
}

// A key as a message names it: in double quotes, so that an empty key or one
// with spaces stays visible. Diagnostics makes the text printable.
std::string named_key(std::string_view key) {
    std::string out = "\"";
    out += key;
    out += '"';
    return out;
}

// A scalar read but not yet added to the document.
struct Scalar {
    std::string text;
    ScalarStyle style = ScalarStyle::PLAIN;
    Mark start;
};

// A block scalar's value as its lines are read (YAML 1.2, 8.1): the lines
// folded so far, and the empty lines not yet written, kept as a count so that
// any number of them costs no memory.
struct BlockFold {
    std::string value;
    std::size_t pending_empty = 0;
    bool have_previous = false;
    bool previous_normal = false;
    bool last_break = false;
};

// The builder refuses only a misuse by this parser, never outside input.
void built(const Result<NodeId>& node) noexcept {
    if (!node.ok()) {
        panic("yaml parser: the document builder refused a node");
    }
}

void built(Status status) noexcept {
    if (!status.ok()) {
        panic("yaml parser: the document builder refused a collection");
    }
}

class Parser {
public:
    Parser(std::string_view source, std::string path, Diagnostics& diagnostics,
           const ParseLimits& limits, NodeBudget* budget)
        : source_(source),
          path_(std::move(path)),
          diagnostics_(&diagnostics),
          limits_(limits),
          budget_(budget) {}

    std::optional<Document> run() {
        if (!builder_.add_file(path_).ok()) {
            panic("yaml parser: the document builder refused the file");
        }
        if (source_.size() > MAX_FILE_BYTES) {
            fail_limit(mark_, "the file", "8 MiB");
            return std::nullopt;
        }
        if (!check_characters() || !next_content_line()) {
            return std::nullopt;
        }
        if (next_indent_ == NO_LINE) {
            // A file of comments only, or empty: an empty map.
            if (!count_node(mark_)) {
                return std::nullopt;
            }
            built(builder_.begin_map(position(mark_)));
            built(builder_.end_map());
        } else {
            const std::int64_t first_indent = next_indent_;
            if (!parse_block_node(NO_LINE, 1, "the document")) {
                return std::nullopt;
            }
            if (next_indent_ != NO_LINE) {
                fail_syntax(mark_, next_indent_ < first_indent
                                       ? "a line indented less than the "
                                         "document's first line"
                                       : "a second node at the top level");
                return std::nullopt;
            }
        }
        Result<Document> document = builder_.finish();
        if (!document.ok()) {
            panic("yaml parser: the document builder refused the document");
        }
        return std::move(document).value();
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

    bool fail_scalar_limit(const Mark& start) {
        return fail_limit(start, "a scalar",
                          std::to_string(limits_.max_scalar_bytes) + " bytes");
    }

    bool fail_no_value(const Mark& where, std::string_view owner) {
        std::string message(owner);
        message += " has no value";
        return fail(CF_NO_VALUE, where, std::move(message));
    }

    // --- Characters ----------------------------------------------------

    // Refuses a byte order mark, invalid UTF-8, a NUL, a carriage return not
    // followed by a line feed, every control character but tab and line
    // feed (C0, DEL and C1), and U+FFFE and U+FFFF. Where a tab may stand is
    // checked while parsing.
    bool check_characters() {
        if (source_.starts_with("\xEF\xBB\xBF")) {
            return fail_syntax(mark_, "a byte order mark");
        }
        Mark where;
        while (where.pos < source_.size()) {
            const auto byte = static_cast<unsigned char>(byte_at(where.pos));
            if (byte < FIRST_C1) {
                if (byte == '\n') {
                    ++where.pos;
                    ++where.line;
                    where.column = 1;
                    continue;
                }
                if (byte == '\r' && byte_at(where.pos + 1) == '\n') {
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
            const auto decoded = utf8::decode(source_, where.pos);
            if (!decoded ||
                (decoded->second >= FIRST_C1 && decoded->second <= LAST_C1) ||
                decoded->second == NON_CHARACTER_FFFE ||
                decoded->second == NON_CHARACTER_FFFF) {
                return fail_character(where);
            }
            where.pos += decoded->first;
            ++where.column;
        }
        return true;
    }

    // --- Cursor --------------------------------------------------------

    // The byte at `where`, or NUL past the end (NUL is refused in the
    // input, so it never stands for a real byte).
    [[nodiscard]] char byte_at(std::size_t where) const noexcept {
        return where < source_.size() ? source_[where] : '\0';
    }

    [[nodiscard]] bool at_end() const noexcept {
        return mark_.pos >= source_.size();
    }

    // The byte `ahead` bytes after the cursor, or NUL past the end.
    [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept {
        return byte_at(mark_.pos + ahead);
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
        const auto byte = static_cast<unsigned char>(byte_at(where.pos));
        ++where.pos;
        if (byte == '\n') {
            ++where.line;
            where.column = 1;
        } else if (byte != '\r' &&
                   (byte & utf8::CONTINUATION_MASK) != utf8::CONTINUATION) {
            ++where.column;
        }
    }

    void advance() noexcept {
        if (!at_end()) {
            step(mark_);
        }
    }

    // The number of bytes of the character at the cursor.
    [[nodiscard]] std::size_t character_length() const noexcept {
        return utf8::sequence_length(static_cast<unsigned char>(peek()));
    }

    // Moves past one character and appends its bytes to `out`.
    void take(std::string& out) {
        const std::size_t length = character_length();
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

    // Moves `where` past the line break it stands on, if any.
    void step_line_break(Mark& where) const noexcept {
        if (byte_at(where.pos) == '\r') {
            step(where);
        }
        if (byte_at(where.pos) == '\n') {
            step(where);
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
        const char before = byte_at(mark_.pos - 1);
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

    static SourcePosition position(const Mark& where) noexcept {
        return SourcePosition{
            .file = 0, .line = where.line, .column = where.column};
    }

    // Counts one node against the file's and the tree's limits, before it
    // is added.
    bool count_node(const Mark& where) {
        if (builder_.size() >= limits_.max_nodes) {
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
        return true;
    }

    bool add_scalar(const Scalar& scalar) {
        if (!count_node(scalar.start)) {
            return false;
        }
        built(
            builder_.scalar(scalar.text, scalar.style, position(scalar.start)));
        return true;
    }

    // Adds a map's key; a key of the same text in the map is CF-111.
    bool add_key(const Scalar& key) {
        if (!count_node(key.start)) {
            return false;
        }
        const Result<NodeId> added =
            builder_.key(key.text, key.style, position(key.start));
        if (!added.ok() &&
            added.status().code() == StatusCode::ALREADY_EXISTS) {
            return fail(CF_DUPLICATE, key.start,
                        "duplicate key " + named_key(key.text));
        }
        built(added);
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

    // True if `bytes` more bytes on a scalar of `size` bytes keep it within
    // the scalar limit.
    [[nodiscard]] bool fits(std::size_t size,
                            std::size_t bytes) const noexcept {
        return size <= limits_.max_scalar_bytes &&
               bytes <= limits_.max_scalar_bytes - size;
    }

    // --- Block structure -----------------------------------------------

    [[nodiscard]] bool is_sequence_entry() const noexcept {
        return peek() == '-' && is_space_end(1);
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
                          std::string_view owner) {
        const std::int64_t indent = next_indent_;
        if (is_sequence_entry()) {
            return parse_block_sequence(indent, depth);
        }
        if (looks_like_key()) {
            return parse_block_mapping(indent, depth);
        }
        return parse_value_on_line(parent_indent, depth, owner);
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by ParseLimits::max_depth, checked before each collection is entered
    bool parse_block_mapping(std::int64_t indent, std::size_t depth) {
        const Mark start = mark_;
        if (!check_depth(depth, start) || !count_node(start)) {
            return false;
        }
        built(builder_.begin_map(position(start)));
        std::size_t entries = 0;
        while (true) {
            const Mark key_at = mark_;
            if (peek() == '?' && is_space_end(1)) {
                return fail_syntax(key_at, "a complex key");
            }
            if (is_sequence_entry()) {
                return fail_syntax(key_at, "a sequence entry inside a mapping");
            }
            Scalar key;
            if (!parse_key(Context::BLOCK, indent, key) || !skip_spaces()) {
                return false;
            }
            if (peek() != ':' || !is_space_end(1)) {
                return fail_syntax(mark_, "a key without ': '");
            }
            advance();
            if (!add_key(key) || !count_entry(entries, key_at) ||
                !parse_map_value(indent, depth, key_at, named_key(key.text))) {
                return false;
            }
            if (next_indent_ < indent) {
                break;
            }
            if (next_indent_ > indent) {
                return fail_syntax(mark_,
                                   "an indentation deeper than its "
                                   "mapping's");
            }
        }
        built(builder_.end_map());
        return true;
    }

    // After `key:` at a mapping indented `indent`.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by ParseLimits::max_depth, checked before each collection is entered
    bool parse_map_value(std::int64_t indent, std::size_t depth,
                         const Mark& key_at, const std::string& owner) {
        if (!skip_spaces()) {
            return false;
        }
        if (at_end() || is_break() || at_comment()) {
            if (!finish_line()) {
                return false;
            }
            if (next_indent_ > indent) {
                return parse_block_node(indent, depth + 1, owner);
            }
            if (next_indent_ == indent && is_sequence_entry()) {
                // A sequence at the indentation of its key.
                return parse_block_sequence(indent, depth + 1);
            }
            return fail_no_value(key_at, owner);
        }
        return parse_value_on_line(indent, depth + 1, owner);
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by ParseLimits::max_depth, checked before each collection is entered
    bool parse_block_sequence(std::int64_t indent, std::size_t depth) {
        const Mark start = mark_;
        if (!check_depth(depth, start) || !count_node(start)) {
            return false;
        }
        built(builder_.begin_sequence(position(start)));
        std::size_t entries = 0;
        while (true) {
            const Mark dash = mark_;
            if (!count_entry(entries, dash)) {
                return false;
            }
            advance();
            if (!skip_spaces() || !parse_sequence_item(indent, depth, dash)) {
                return false;
            }
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
        built(builder_.end_sequence());
        return true;
    }

    // After `- ` of a sequence indented `indent`.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by ParseLimits::max_depth, checked before each collection is entered
    bool parse_sequence_item(std::int64_t indent, std::size_t depth,
                             const Mark& dash) {
        constexpr std::string_view OWNER = "a sequence item";
        if (at_end() || is_break() || at_comment()) {
            if (!finish_line()) {
                return false;
            }
            if (next_indent_ > indent) {
                return parse_block_node(indent, depth + 1, OWNER);
            }
            return fail_no_value(dash, OWNER);
        }
        // A compact collection starts on the line of its `-`; its own
        // indentation is its column.
        const std::int64_t item_indent =
            static_cast<std::int64_t>(mark_.column) - 1;
        if (is_sequence_entry()) {
            return parse_block_sequence(item_indent, depth + 1);
        }
        if (looks_like_key()) {
            return parse_block_mapping(item_indent, depth + 1);
        }
        return parse_value_on_line(indent, depth + 1, OWNER);
    }

    // A value that starts on the current line, in block context, and the
    // rest of its line(s). Ends at the next content line.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by ParseLimits::max_depth, checked before each collection is entered
    bool parse_value_on_line(std::int64_t parent_indent, std::size_t depth,
                             std::string_view owner) {
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
            return parse_block_scalar(parent_indent);
        }
        if (chr == '[' || chr == '{') {
            return parse_flow_collection(parent_indent, depth) && finish_line();
        }
        if (chr == '\'' || chr == '"') {
            Scalar scalar;
            return parse_quoted(Context::BLOCK, parent_indent, scalar) &&
                   add_scalar(scalar) && finish_line();
        }
        return parse_plain_value(Context::BLOCK, parent_indent, owner) &&
               finish_line();
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
    bool parse_key(Context context, std::int64_t parent_indent, Scalar& out) {
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
            if (out.text == "<<") {
                return fail_syntax(start, "a merge key");
            }
        }
        if (mark_.line != start.line) {
            return fail_syntax(start, "a key on several lines");
        }
        if (out.text.size() > limits_.max_key_bytes) {
            return fail_limit(start, "a key",
                              std::to_string(limits_.max_key_bytes) + " bytes");
        }
        return true;
    }

    // --- Plain scalars -------------------------------------------------

    // A plain scalar as a value, added to the document: `null` and `~` are
    // refused (CF-112).
    bool parse_plain_value(Context context, std::int64_t parent_indent,
                           std::string_view owner) {
        Scalar scalar;
        if (!parse_plain(context, parent_indent, scalar)) {
            return false;
        }
        if (is_null_word(scalar.text)) {
            return fail_no_value(scalar.start, owner);
        }
        return add_scalar(scalar);
    }

    bool check_plain_start(Context context) {
        const char chr = peek();
        if (chr == ',' || chr == ']' || chr == '}' || chr == '[' ||
            chr == '{') {
            return fail_syntax(mark_, std::string("a '") + chr + "' here");
        }
        if (chr == '|' || chr == '>') {
            // In block context a value never gets here: this is a key.
            return fail_syntax(mark_, context == Context::BLOCK
                                          ? "a block scalar as a key"
                                          : "a block scalar inside a flow "
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

    // True if a plain scalar continues on the line whose first character
    // (after its spaces) is at `where`.
    [[nodiscard]] bool continues_plain(const Mark& where, Context context,
                                       std::int64_t parent_indent) const {
        const char chr = byte_at(where.pos);
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
            while (byte_at(from.pos) == ' ') {
                step(from);
            }
            if (from.pos >= source_.size()) {
                return std::nullopt;
            }
            const char chr = byte_at(from.pos);
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

    // The separator a plain scalar's text gets before its next character:
    // the spaces read on the line, or the fold of a line break (a space, or
    // a line feed per empty line). Written only when a character follows,
    // so trailing white space never counts or costs memory.
    struct PlainGap {
        std::size_t spaces = 0;
        std::size_t breaks = 0;
    };

    // One line of a plain scalar, up to its end (`: `, ` #`, a flow
    // indicator or the line end). `content_end` follows its last character
    // that is not a space.
    bool scan_plain_line(Context context, Scalar& out, PlainGap& gap,
                         Mark& content_end) {
        while (!at_end() && !is_break() && !ends_plain_here(context)) {
            const char chr = peek();
            if (chr == '\t') {
                return fail_character(mark_);
            }
            if (chr == ' ') {
                ++gap.spaces;
                advance();
                continue;
            }
            if (!fits(out.text.size(),
                      gap.breaks + gap.spaces + character_length())) {
                return fail_scalar_limit(out.start);
            }
            out.text.append(gap.breaks, '\n');
            out.text.append(gap.spaces, ' ');
            gap = PlainGap{};
            take(out.text);
            content_end = mark_;
        }
        return true;
    }

    bool parse_plain(Context context, std::int64_t parent_indent, Scalar& out) {
        out.start = mark_;
        out.style = ScalarStyle::PLAIN;
        if (!check_plain_start(context)) {
            return false;
        }
        Mark content_end = mark_;
        PlainGap gap;
        while (true) {
            if (!scan_plain_line(context, out, gap, content_end)) {
                return false;
            }
            if (!at_end() && !is_break()) {
                break;
            }
            const auto next = continuation(mark_, context, parent_indent);
            if (!next) {
                break;
            }
            // The line's trailing spaces are dropped by the fold.
            gap = next->second == 0
                      ? PlainGap{.spaces = 1, .breaks = 0}
                      : PlainGap{.spaces = 0, .breaks = next->second};
            mark_ = next->first;
        }
        mark_ = content_end;
        return true;
    }

    // --- Quoted scalars ------------------------------------------------

    // Folds a line break inside a quoted scalar (YAML 1.2, 7.3): empty
    // lines give line feeds, a single break a space (none after an escaped
    // break), and the next line's leading white space is skipped. Its
    // indentation, counted in spaces, must be more than `parent_indent`.
    bool fold_quoted_break(Scalar& out, Context context,
                           std::int64_t parent_indent, bool escaped) {
        skip_line_break();
        std::size_t empty_lines = 0;
        std::int64_t spaces = 0;
        while (true) {
            spaces = 0;
            while (peek() == ' ') {
                advance();
                ++spaces;
            }
            while (peek() == ' ' || peek() == '\t') {
                advance();
            }
            if (!is_break()) {
                break;
            }
            ++empty_lines;
            skip_line_break();
        }
        if (at_end()) {
            return fail_syntax(out.start, "an unterminated quoted scalar");
        }
        if (mark_.column == 1 && is_document_marker()) {
            return fail_syntax(mark_, "a document marker");
        }
        if (spaces <= parent_indent) {
            return fail_syntax(mark_,
                               context == Context::BLOCK
                                   ? "a quoted scalar's line that is not "
                                     "indented"
                                   : "a flow collection's line that is not "
                                     "indented");
        }
        std::size_t separator = empty_lines;
        if (empty_lines == 0 && !escaped) {
            separator = 1;
        }
        if (!fits(out.text.size(), separator)) {
            return fail_scalar_limit(out.start);
        }
        if (empty_lines > 0) {
            out.text.append(empty_lines, '\n');
        } else if (!escaped) {
            out.text.push_back(' ');
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
        auto code_point = [this, &text](std::uint32_t value) {
            advance();
            utf8::append(text, value);
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
                return code_point(NEXT_LINE);
            case '_':
                return code_point(NO_BREAK_SPACE);
            case 'L':
                return code_point(LINE_SEPARATOR);
            case 'P':
                return code_point(PARAGRAPH_SEPARATOR);
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
        if ((code_point >= utf8::FIRST_SURROGATE &&
             code_point <= utf8::LAST_SURROGATE) ||
            code_point > utf8::LAST_CODE_POINT) {
            return fail_syntax(where, "an invalid escape");
        }
        utf8::append(text, code_point);
        return true;
    }

    // White space of a quoted scalar read but not yet written: the bytes
    // `from` to `from + count` of the source. Dropped at a line break,
    // written before anything else.
    struct QuotedWhite {
        std::size_t from = 0;
        std::size_t count = 0;
    };

    bool write_white(Scalar& out, QuotedWhite& white) {
        if (!fits(out.text.size(), white.count)) {
            return fail_scalar_limit(out.start);
        }
        out.text.append(source_.substr(white.from, white.count));
        white.count = 0;
        return true;
    }

    // What one step inside a quoted scalar found.
    enum class QuotedStep : std::uint8_t { MORE, CLOSED, FAILED };

    // One character, escape or line break of a quoted scalar.
    QuotedStep quoted_step(char quote, Context context,
                           std::int64_t parent_indent, Scalar& out,
                           QuotedWhite& white) {
        const char chr = peek();
        bool done = true;
        if (chr == ' ' || chr == '\t') {
            if (white.count == 0) {
                white.from = mark_.pos;
            }
            ++white.count;
            advance();
        } else if (is_break()) {
            white.count = 0;
            done = fold_quoted_break(out, context, parent_indent, false);
        } else if (!write_white(out, white)) {
            done = false;
        } else if (chr == quote && (quote != '\'' || peek(1) != '\'')) {
            advance();
            return QuotedStep::CLOSED;
        } else if (chr == quote) {
            advance();
            advance();
            done = fits(out.text.size(), 1) || fail_scalar_limit(out.start);
            if (done) {
                out.text.push_back('\'');
            }
        } else if (quote == '"' && chr == '\\' && is_break(1)) {
            advance();
            done = fold_quoted_break(out, context, parent_indent, true);
        } else if (quote == '"' && chr == '\\') {
            std::string escaped;
            done = parse_escape(escaped) &&
                   (fits(out.text.size(), escaped.size()) ||
                    fail_scalar_limit(out.start));
            out.text += escaped;
        } else {
            done = fits(out.text.size(), character_length()) ||
                   fail_scalar_limit(out.start);
            if (done) {
                take(out.text);
            }
        }
        return done ? QuotedStep::MORE : QuotedStep::FAILED;
    }

    bool parse_quoted(Context context, std::int64_t parent_indent,
                      Scalar& out) {
        out.start = mark_;
        const char quote = peek();
        out.style = quote == '\'' ? ScalarStyle::SINGLE_QUOTED
                                  : ScalarStyle::DOUBLE_QUOTED;
        advance();
        QuotedWhite white;
        while (true) {
            if (at_end()) {
                return fail_syntax(out.start, "an unterminated quoted scalar");
            }
            const QuotedStep result =
                quoted_step(quote, context, parent_indent, out, white);
            if (result == QuotedStep::FAILED) {
                return false;
            }
            if (result == QuotedStep::CLOSED) {
                return true;
            }
        }
    }

    // --- Block scalars -------------------------------------------------

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
            } else if (chr == '0' && indicator == 0) {
                return fail_syntax(mark_, "an indentation indicator of 0");
            } else if (chr >= '1' && chr <= '9' && indicator == 0) {
                indicator = chr - '0';
                advance();
            } else {
                break;
            }
        }
        if (peek() >= '0' && peek() <= '9') {
            return fail_syntax(
                mark_, indicator != 0 ? "an indentation indicator of more than "
                                        "one digit"
                                      : "an indentation indicator after text");
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

    // The indentation of a block scalar's content (YAML 1.2, 8.1.1.1): that
    // of its first line that is not empty, if indented more than
    // `parent_indent`. Otherwise the scalar has no content line, and the
    // indentation is that of its widest empty line, at least
    // `parent_indent` + 1, so that every white-space line is empty. Leading
    // empty lines may not be indented more than the first content line.
    bool detect_block_indent(std::int64_t parent_indent,
                             std::int64_t& content_indent) {
        Mark where = mark_;
        std::int64_t widest_empty = 0;
        while (true) {
            std::int64_t spaces = 0;
            while (byte_at(where.pos) == ' ') {
                step(where);
                ++spaces;
            }
            const bool at_file_end = where.pos >= source_.size();
            const char chr = byte_at(where.pos);
            if (!at_file_end && chr != '\n' && chr != '\r') {
                if (spaces <= parent_indent) {
                    break;  // the scalar has no content line
                }
                if (widest_empty > spaces) {
                    return fail_syntax(mark_,
                                       "a leading empty line indented more "
                                       "than its block scalar");
                }
                content_indent = spaces;
                return true;
            }
            widest_empty = std::max(widest_empty, spaces);
            if (at_file_end) {
                break;
            }
            step_line_break(where);
        }
        content_indent = std::max(parent_indent + 1, widest_empty);
        return true;
    }

    // Adds one content line of a block scalar to `fold`, with the empty
    // lines before it, checked against the scalar limit first.
    bool fold_block_line(BlockFold& fold, std::string_view line, bool literal,
                         const Mark& start) {
        const bool normal = line.front() != ' ' && line.front() != '\t';
        std::size_t breaks = fold.pending_empty;
        bool space = false;
        if (fold.have_previous) {
            if (literal || !(fold.previous_normal && normal)) {
                ++breaks;
            } else if (breaks == 0) {
                space = true;
            }
        }
        if (!fits(fold.value.size(), breaks + (space ? 1 : 0) + line.size())) {
            return fail_scalar_limit(start);
        }
        fold.value.append(breaks, '\n');
        if (space) {
            fold.value.push_back(' ');
        }
        fold.value.append(line);
        fold.have_previous = true;
        fold.previous_normal = normal;
        fold.pending_empty = 0;
        return true;
    }

    // Reads the lines of a block scalar's content into `fold`, up to the
    // first line that is not empty and indented less than `content_indent`.
    bool read_block_lines(std::int64_t content_indent, bool literal,
                          BlockFold& fold, const Mark& start) {
        while (!at_end()) {
            const Mark line_start = mark_;
            if (is_document_marker()) {
                return fail_syntax(mark_, "a document marker");
            }
            std::int64_t spaces = 0;
            while (peek() == ' ' && spaces < content_indent) {
                advance();
                ++spaces;
            }
            if (is_break()) {
                skip_line_break();
                ++fold.pending_empty;
                continue;
            }
            if (at_end()) {
                // White space without a line break is not a line.
                break;
            }
            if (spaces < content_indent) {
                // Less indented: the scalar ends before this line.
                mark_ = line_start;
                break;
            }
            const std::size_t from = mark_.pos;
            while (!at_end() && !is_break()) {
                advance();
            }
            const std::string_view line =
                source_.substr(from, mark_.pos - from);
            fold.last_break = is_break();
            skip_line_break();
            if (!fold_block_line(fold, line, literal, start)) {
                return false;
            }
        }
        return true;
    }

    bool parse_block_scalar(std::int64_t parent_indent) {
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
        BlockFold fold;
        if (!read_block_lines(content_indent, literal, fold, start)) {
            return false;
        }
        std::size_t final_breaks = 0;
        if (chomping == Chomping::KEEP) {
            final_breaks = (fold.have_previous && fold.last_break ? 1 : 0) +
                           fold.pending_empty;
        } else if (chomping == Chomping::CLIP && fold.have_previous &&
                   fold.last_break) {
            final_breaks = 1;
        }
        if (!fits(fold.value.size(), final_breaks)) {
            return fail_scalar_limit(start);
        }
        fold.value.append(final_breaks, '\n');
        return add_scalar(Scalar{.text = std::move(fold.value),
                                 .style = literal ? ScalarStyle::LITERAL
                                                  : ScalarStyle::FOLDED,
                                 .start = start}) &&
               next_content_line();
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
            if (indent <= parent_indent && (peek() == ']' || peek() == '}')) {
                return fail(CF_SYNTAX, mark_,
                            "a closing bracket not indented more than its "
                            "key is not allowed in configuration files; "
                            "indent the bracket, or write the collection as "
                            "a block");
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
    bool parse_flow_collection(std::int64_t parent_indent, std::size_t depth) {
        if (!check_depth(depth, mark_)) {
            return false;
        }
        return peek() == '[' ? parse_flow_sequence(parent_indent, depth)
                             : parse_flow_mapping(parent_indent, depth);
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by ParseLimits::max_depth, checked before each collection is entered
    bool parse_flow_sequence(std::int64_t parent_indent, std::size_t depth) {
        constexpr std::string_view UNCLOSED = "an unclosed flow sequence";
        const Mark start = mark_;
        if (!count_node(start)) {
            return false;
        }
        built(builder_.begin_sequence(position(start)));
        advance();
        std::size_t entries = 0;
        while (true) {
            if (!skip_flow_space_in(parent_indent, start, UNCLOSED)) {
                return false;
            }
            if (peek() == ']') {
                advance();
                break;
            }
            const Mark item_at = mark_;
            if (!count_entry(entries, item_at) ||
                !parse_flow_node(parent_indent, depth, "a sequence item") ||
                !skip_flow_space_in(parent_indent, start, UNCLOSED)) {
                return false;
            }
            if (peek() == ':') {
                return fail_syntax(mark_, "a mapping inside a flow sequence");
            }
            if (!flow_separator(']')) {
                return false;
            }
        }
        built(builder_.end_sequence());
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
                          const Mark& start, std::size_t& entries) {
        constexpr std::string_view UNCLOSED = "an unclosed flow mapping";
        const Mark key_at = mark_;
        if (peek() == '?' && !is_plain_safe(1, Context::FLOW)) {
            return fail_syntax(key_at, "a complex key");
        }
        if (peek() == ',') {
            return fail_syntax(key_at, "an empty entry");
        }
        Scalar key;
        if (!parse_key(Context::FLOW, parent_indent, key) ||
            !skip_flow_space_in(parent_indent, start, UNCLOSED)) {
            return false;
        }
        const std::string owner = named_key(key.text);
        if (peek() == ',' || peek() == '}') {
            return fail_no_value(key_at, owner);
        }
        if (peek() != ':') {
            return fail_syntax(mark_, "a key without ':'");
        }
        advance();
        if (!add_key(key) || !count_entry(entries, key_at) ||
            !skip_flow_space_in(parent_indent, start, UNCLOSED)) {
            return false;
        }
        if (peek() == ',' || peek() == '}') {
            return fail_no_value(key_at, owner);
        }
        return parse_flow_node(parent_indent, depth, owner);
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by ParseLimits::max_depth, checked before each collection is entered
    bool parse_flow_mapping(std::int64_t parent_indent, std::size_t depth) {
        constexpr std::string_view UNCLOSED = "an unclosed flow mapping";
        const Mark start = mark_;
        if (!count_node(start)) {
            return false;
        }
        built(builder_.begin_map(position(start)));
        advance();
        std::size_t entries = 0;
        while (true) {
            if (!skip_flow_space_in(parent_indent, start, UNCLOSED)) {
                return false;
            }
            if (peek() == '}') {
                advance();
                break;
            }
            if (!parse_flow_entry(parent_indent, depth, start, entries) ||
                !skip_flow_space_in(parent_indent, start, UNCLOSED) ||
                !flow_separator('}')) {
                return false;
            }
        }
        built(builder_.end_map());
        return true;
    }

    // A node inside a flow collection at depth `depth`: its own collections
    // are one deeper.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by ParseLimits::max_depth, checked before each collection is entered
    bool parse_flow_node(std::int64_t parent_indent, std::size_t depth,
                         std::string_view owner) {
        if (!refuse_node_properties()) {
            return false;
        }
        const char chr = peek();
        if (chr == '[' || chr == '{') {
            return parse_flow_collection(parent_indent, depth + 1);
        }
        if (chr == '\'' || chr == '"') {
            Scalar scalar;
            return parse_quoted(Context::FLOW, parent_indent, scalar) &&
                   add_scalar(scalar);
        }
        if (chr == ',') {
            return fail_syntax(mark_, "an empty entry");
        }
        return parse_plain_value(Context::FLOW, parent_indent, owner);
    }

    std::string_view source_;
    std::string path_;
    Diagnostics* diagnostics_;
    ParseLimits limits_;
    NodeBudget* budget_;
    DocumentBuilder builder_;
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
