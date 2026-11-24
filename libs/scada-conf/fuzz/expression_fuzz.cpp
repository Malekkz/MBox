// Fuzz target: the expression grammar of configuration schemas contract 5.6.
// The input is one expression, parsed in each of its three variants.
//
// The oracle is the grammar written a second time, without the library's
// code: a lexer and a recognizer by precedence climbing over a table of the
// binary levels (the library descends one function per level), and for a
// `for` value a second pass from the start instead of the library's
// continuation. Both must agree on whether the text is accepted, and on a
// refusal's code and column (D-109: the depth CF-526 counts, the column
// CF-520 names). A tree's children point at earlier nodes.

#include "scada/conf/expression.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using scada::conf::ExpressionKind;

constexpr std::uint16_t SYNTAX = 520;
constexpr std::uint16_t TOO_BIG = 526;
constexpr std::uint16_t LITERAL = 528;
constexpr std::uint16_t NOT_FUNCTION = 529;
constexpr std::size_t MAX_BYTES = 4096;
constexpr std::size_t MAX_DEPTH = 32;
constexpr std::uint64_t INT_MAX = 9223372036854775807ULL;
constexpr std::uint64_t TEN = 10;
constexpr std::uint64_t SIXTEEN = 16;
constexpr std::uint64_t MS_PER_S = 1000;
constexpr std::uint64_t S_PER_M = 60;
constexpr std::uint64_t M_PER_H = 60;
constexpr std::uint64_t H_PER_D = 24;

// The binary levels of 5.6, loosest first.
constexpr int OR_LEVEL = 0;
constexpr int XOR_LEVEL = 1;
constexpr int AND_LEVEL = 2;
constexpr int COMPARISON_LEVEL = 3;
constexpr int SUM_LEVEL = 4;
constexpr int PRODUCT_LEVEL = 5;

// The function table of 5.6, as the contract lists it.
constexpr std::array<std::string_view, 41> FUNCTION_TABLE = {
    "if",       "limit",   "min",     "max",        "abs",        "sqrt",
    "exp",      "ln",      "log10",   "sin",        "cos",        "tan",
    "asin",     "acos",    "atan",    "atan2",      "round",      "trunc",
    "floor",    "ceil",    "to_real", "to_int",     "band",       "bor",
    "bxor",     "bnot",    "shl",     "shr",        "bit",        "sum",
    "avg",      "min_of",  "max_of",  "count_good", "moving_avg", "rate",
    "totalise", "quality", "age",     "is_good",    "tag"};

constexpr std::array<std::string_view, 12> RESERVED = {
    "AND", "OR",   "XOR",   "NOT",  "MOD",       "DIV",
    "IN",  "TRUE", "FALSE", "GOOD", "UNCERTAIN", "BAD"};
constexpr std::array<std::string_view, 5> LITERAL_WORDS = {
    "TRUE", "FALSE", "GOOD", "UNCERTAIN", "BAD"};

// The operators, longest first.
constexpr std::array<std::string_view, 15> OPERATORS = {
    "**", "<>", "<=", ">=", "..", "*", "/", "+",
    "-",  "=",  "<",  ">",  "(",  ")", ","};
constexpr std::array<std::string_view, 6> COMPARISONS = {"=",  "<>", "<",
                                                         "<=", ">",  ">="};

void check(bool condition) {
    if (!condition) {
        std::abort();
    }
}

bool letter(char chr) {
    return (chr >= 'a' && chr <= 'z') || (chr >= 'A' && chr <= 'Z');
}
bool digit(char chr) {
    return chr >= '0' && chr <= '9';
}
bool segment_start(char chr) {
    return letter(chr) || chr == '_';
}
bool segment_char(char chr) {
    return segment_start(chr) || digit(chr);
}
bool hex_digit(char chr) {
    return digit(chr) || (chr >= 'a' && chr <= 'f') ||
           (chr >= 'A' && chr <= 'F');
}
std::string upper(std::string_view text) {
    std::string out(text);
    for (char& chr : out) {
        chr = static_cast<char>(std::toupper(static_cast<unsigned char>(chr)));
    }
    return out;
}
std::string lower(std::string_view text) {
    std::string out(text);
    for (char& chr : out) {
        chr = static_cast<char>(std::tolower(static_cast<unsigned char>(chr)));
    }
    return out;
}
bool in_list(std::span<const std::string_view> list, std::string_view text) {
    return std::ranges::find(list, text) != list.end();
}

// A refusal: its code and 1-based column.
struct Refusal {
    std::uint16_t code = 0;
    std::size_t column = 0;
};

enum class Kind : std::uint8_t { END, NUMBER, STRING, PATTERN, NAME, OPERATOR };

struct Tok {
    Kind kind = Kind::END;
    std::size_t column = 0;
    std::string text;
    bool one_segment = false;
    // A literal's leaf in the tree notation of `tree_text`.
    std::string value;
};

// A double as exact text: its hexadecimal form.
std::string exact(double value) {
    std::array<char, 64> buffer{};
    const std::to_chars_result written =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                      std::chars_format::hex);
    return std::string(buffer.data(), written.ptr);
}

// The recognizer; `refusal` is set on the first error and ends the parse.
class Oracle {
public:
    Oracle(std::string_view text, ExpressionKind kind)
        : text_(text), kind_(kind) {}

    std::optional<Refusal> run() {
        if (text_.size() > MAX_BYTES) {
            return Refusal{.code = TOO_BIG, .column = 1};
        }
        if (!next()) {
            return refusal_;
        }
        if (kind_ == ExpressionKind::FOR_VALUE) {
            for_value();
        } else {
            expression(0);
        }
        if (!refusal_ && tok_.kind != Kind::END) {
            refuse(SYNTAX, tok_.column);
        }
        return refusal_;
    }

private:
    bool refuse(std::uint16_t code, std::size_t column) {
        if (!refusal_) {
            refusal_ = Refusal{.code = code, .column = column};
        }
        return false;
    }

    [[nodiscard]] char at(std::size_t where) const {
        return where < text_.size() ? text_[where] : '\0';
    }

    // A number, time or string must not be followed by a letter, a digit,
    // `_` or `#`.
    bool abuts() {
        if (segment_char(at(pos_)) || at(pos_) == '#') {
            return refuse(SYNTAX, pos_ + 1);
        }
        return true;
    }

    // Decimal digits to a value up to INT_MAX; nothing beyond.
    static std::optional<std::uint64_t> decimal(std::string_view digits) {
        std::uint64_t value = 0;
        for (const char chr : digits) {
            const auto part = static_cast<std::uint64_t>(chr - '0');
            if (value > (INT_MAX - part) / TEN) {
                return std::nullopt;
            }
            value = (value * TEN) + part;
        }
        return value;
    }

    bool lex_hex(std::size_t start, std::size_t prefix) {
        pos_ += prefix;
        const std::size_t first = pos_;
        std::uint64_t value = 0;
        bool large = false;
        while (hex_digit(at(pos_))) {
            const char chr = at(pos_);
            const std::uint64_t part =
                digit(chr)
                    ? static_cast<std::uint64_t>(chr - '0')
                    : static_cast<std::uint64_t>(
                          std::tolower(static_cast<unsigned char>(chr)) - 'a') +
                          TEN;
            large = large || value > (INT_MAX - part) / SIXTEEN;
            if (!large) {
                value = (value * SIXTEEN) + part;
            }
            ++pos_;
        }
        if (pos_ == first) {
            return refuse(SYNTAX, pos_ + 1);
        }
        if (!abuts()) {
            return false;
        }
        if (large) {
            return refuse(LITERAL, start + 1);
        }
        tok_.value = "i" + std::to_string(value);
        return true;
    }

    bool lex_number() {
        const std::size_t start = pos_;
        if (at(pos_) == '0' && (at(pos_ + 1) == 'x' || at(pos_ + 1) == 'X')) {
            return lex_hex(start, 2);
        }
        if (text_.substr(pos_, 3) == "16#") {
            return lex_hex(start, 3);
        }
        if (at(pos_) == '0') {
            ++pos_;
        } else {
            while (digit(at(pos_))) {
                ++pos_;
            }
        }
        const std::size_t whole = pos_;
        bool real = false;
        if (at(pos_) == '.' && digit(at(pos_ + 1))) {
            real = true;
            pos_ += 2;
            while (digit(at(pos_))) {
                ++pos_;
            }
        }
        const std::size_t mantissa = pos_;
        const bool sign = at(pos_ + 1) == '+' || at(pos_ + 1) == '-';
        if ((at(pos_) == 'e' || at(pos_) == 'E') &&
            digit(at(pos_ + (sign ? 2 : 1)))) {
            real = true;
            pos_ += sign ? 3 : 2;
            while (digit(at(pos_))) {
                ++pos_;
            }
        }
        if (!abuts()) {
            return false;
        }
        if (!real) {
            const std::optional<std::uint64_t> value =
                decimal(text_.substr(start, whole - start));
            if (!value) {
                return refuse(LITERAL, start + 1);
            }
            tok_.value = "i" + std::to_string(*value);
            return true;
        }
        return real_in_range(start, mantissa);
    }

    // A real that is finite and, unless written as zero, not rounded to it.
    bool real_in_range(std::size_t start, std::size_t mantissa) {
        const std::string_view lexeme = text_.substr(start, pos_ - start);
        double value = 0;
        const std::from_chars_result read = std::from_chars(
            lexeme.data(), lexeme.data() + lexeme.size(), value);
        const std::string_view digits = text_.substr(start, mantissa - start);
        const bool nonzero =
            digits.find_first_of("123456789") != std::string_view::npos;
        if (read.ec != std::errc() || !std::isfinite(value) ||
            (value == 0 && nonzero)) {
            return refuse(LITERAL, start + 1);
        }
        tok_.value = "r" + exact(value);
        return true;
    }

    bool lex_time() {
        const std::size_t start = pos_;
        pos_ += 2;
        const std::size_t first = pos_;
        if (at(pos_) == '0') {
            ++pos_;
        } else {
            while (digit(at(pos_))) {
                ++pos_;
            }
        }
        if (pos_ == first) {
            return refuse(SYNTAX, pos_ + 1);
        }
        const std::string_view digits = text_.substr(first, pos_ - first);
        std::uint64_t unit = 0;
        if (text_.substr(pos_, 2) == "ms") {
            unit = 1;
            pos_ += 2;
        } else {
            constexpr std::array<std::pair<char, std::uint64_t>, 4> UNITS = {
                {{'s', MS_PER_S},
                 {'m', MS_PER_S * S_PER_M},
                 {'h', MS_PER_S * S_PER_M * M_PER_H},
                 {'d', MS_PER_S * S_PER_M * M_PER_H * H_PER_D}}};
            for (const auto& [name, factor] : UNITS) {
                if (at(pos_) == name) {
                    unit = factor;
                }
            }
            if (unit == 0) {
                return refuse(SYNTAX, pos_ + 1);
            }
            ++pos_;
        }
        if (!abuts()) {
            return false;
        }
        const std::optional<std::uint64_t> value = decimal(digits);
        if (!value || *value > INT_MAX / unit) {
            return refuse(LITERAL, start + 1);
        }
        tok_.value = "t" + std::to_string(*value * unit);
        return true;
    }

    bool lex_string() {
        const std::size_t start = pos_;
        if (kind_ == ExpressionKind::RUN_TIME) {
            return refuse(SYNTAX, start + 1);
        }
        ++pos_;
        std::string unquoted;
        while (true) {
            if (pos_ >= text_.size()) {
                return refuse(SYNTAX, start + 1);
            }
            if (at(pos_) == '\'' && at(pos_ + 1) == '\'') {
                unquoted.push_back('\'');
                pos_ += 2;
            } else if (at(pos_) == '\'') {
                ++pos_;
                break;
            } else {
                unquoted.push_back(at(pos_));
                ++pos_;
            }
        }
        tok_.value = "s'" + unquoted + "'";
        return abuts();
    }

    // A name: segments joined by a "." that a segment follows.
    void lex_name(std::size_t start) {
        tok_.kind = Kind::NAME;
        tok_.one_segment = true;
        while (true) {
            while (segment_char(at(pos_))) {
                ++pos_;
            }
            if (at(pos_) != '.' || !segment_start(at(pos_ + 1))) {
                break;
            }
            tok_.one_segment = false;
            ++pos_;
        }
        tok_.text = std::string(text_.substr(start, pos_ - start));
    }

    // The next token into `tok_`; false with the refusal set.
    bool next() {
        // Spaces and line ends separate tokens; a tab is refused.
        while (pos_ < text_.size() && std::string_view(" \n\r").find(
                                          at(pos_)) != std::string_view::npos) {
            ++pos_;
        }
        if (pos_ < text_.size() && at(pos_) == '\t') {
            return refuse(SYNTAX, pos_ + 1);
        }
        tok_ = Tok{.kind = Kind::END,
                   .column = pos_ + 1,
                   .text = {},
                   .one_segment = false,
                   .value = {}};
        if (pos_ >= text_.size()) {
            return true;
        }
        const std::size_t start = pos_;
        const char chr = at(pos_);
        if (digit(chr)) {
            tok_.kind = Kind::NUMBER;
            return lex_number();
        }
        if ((chr == 'T' || chr == 't') && at(pos_ + 1) == '#') {
            tok_.kind = Kind::NUMBER;
            return lex_time();
        }
        if (segment_start(chr)) {
            lex_name(start);
            return true;
        }
        if (chr == '\'') {
            tok_.kind = Kind::STRING;
            return lex_string();
        }
        if (chr == '"') {
            const std::size_t close = text_.find('"', pos_ + 1);
            if (close == std::string_view::npos) {
                return refuse(SYNTAX, start + 1);
            }
            tok_.kind = Kind::PATTERN;
            tok_.value = "p\"" +
                         std::string(text_.substr(pos_ + 1, close - pos_ - 1)) +
                         "\"";
            pos_ = close + 1;
            return true;
        }
        for (const std::string_view oper : OPERATORS) {
            if (text_.substr(pos_).starts_with(oper)) {
                tok_.kind = Kind::OPERATOR;
                tok_.text = std::string(oper);
                pos_ += oper.size();
                return true;
            }
        }
        return refuse(SYNTAX, start + 1);
    }

    [[nodiscard]] bool is(std::string_view oper) const {
        return tok_.kind == Kind::OPERATOR && tok_.text == oper;
    }
    [[nodiscard]] bool word(std::string_view reserved) const {
        return tok_.kind == Kind::NAME && tok_.one_segment &&
               upper(tok_.text) == reserved;
    }
    [[nodiscard]] bool reserved() const {
        return tok_.kind == Kind::NAME && tok_.one_segment &&
               in_list(RESERVED, upper(tok_.text));
    }
    [[nodiscard]] bool comparison() const {
        return tok_.kind == Kind::OPERATOR && in_list(COMPARISONS, tok_.text);
    }

    // The binary levels of 5.6, loosest first: OR, XOR, AND, the
    // comparison, + and -, then * / DIV MOD. True if the token at hand is
    // an operator of `level`.
    [[nodiscard]] bool operator_of(int level) const {
        switch (level) {
            case OR_LEVEL:
                return word("OR");
            case XOR_LEVEL:
                return word("XOR");
            case AND_LEVEL:
                return word("AND");
            case SUM_LEVEL:
                return is("+") || is("-");
            case PRODUCT_LEVEL:
                return is("*") || is("/") || word("DIV") || word("MOD");
            default:
                return false;
        }
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by MAX_DEPTH
    bool expression(std::size_t depth) {
        if (depth >= MAX_DEPTH) {
            return refuse(TOO_BIG, tok_.column);
        }
        return level(OR_LEVEL, depth);
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by MAX_DEPTH
    bool level(int number, std::size_t depth) {
        if (number == COMPARISON_LEVEL) {
            return compare(depth);
        }
        if (!operand(number, depth)) {
            return false;
        }
        while (operator_of(number)) {
            const std::string oper = lower(tok_.text);
            if (!next() || !operand(number, depth)) {
                return false;
            }
            combine(oper, 2);
        }
        return true;
    }

    // Replaces the last `count` subtrees by `(oper ...)`.
    void combine(const std::string& oper, std::size_t count) {
        std::string node = "(" + oper;
        for (std::size_t i = trees_.size() - count; i < trees_.size(); ++i) {
            node += " " + std::move(trees_[i]);
        }
        trees_.resize(trees_.size() - count);
        trees_.push_back(std::move(node) + ")");
    }

    // An operand of `number`: the next level, or a unary below the last.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by MAX_DEPTH
    bool operand(int number, std::size_t depth) {
        return number == PRODUCT_LEVEL ? unary(depth)
                                       : level(number + 1, depth);
    }

    // cmp_expr: one comparison, or IN with a value or a range; no chain.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by MAX_DEPTH
    bool compare(std::size_t depth) {
        if (!level(SUM_LEVEL, depth)) {
            return false;
        }
        if (comparison()) {
            const std::string oper = tok_.text;
            if (!next() || !level(SUM_LEVEL, depth)) {
                return false;
            }
            combine(oper, 2);
        } else if (word("IN")) {
            if (kind_ == ExpressionKind::RUN_TIME) {
                return refuse(SYNTAX, tok_.column);
            }
            if (!next() || !level(SUM_LEVEL, depth)) {
                return false;
            }
            if (!is("..")) {
                combine("in", 2);
            } else if (!next() || !level(SUM_LEVEL, depth)) {
                return false;
            } else {
                combine("in-range", 3);
            }
        } else {
            return true;
        }
        if (comparison() || word("IN")) {
            return refuse(SYNTAX, tok_.column);
        }
        return true;
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by MAX_DEPTH
    bool unary(std::size_t depth) {
        if (!is("-") && !word("NOT")) {
            return power(depth);
        }
        const std::string oper = is("-") ? "neg" : "not";
        if (depth + 1 >= MAX_DEPTH) {
            return refuse(TOO_BIG, tok_.column);
        }
        if (!next()) {
            return false;
        }
        if (is("-") || word("NOT")) {
            return refuse(SYNTAX, tok_.column);
        }
        if (!power(depth + 1)) {
            return false;
        }
        combine(oper, 1);
        return true;
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by MAX_DEPTH
    bool power(std::size_t depth) {
        if (!primary(depth)) {
            return false;
        }
        if (!is("**")) {
            return true;
        }
        if (!next()) {
            return false;
        }
        if (depth + 1 >= MAX_DEPTH) {
            return refuse(TOO_BIG, tok_.column);
        }
        if (!unary(depth + 1)) {
            return false;
        }
        combine("**", 2);
        return true;
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by MAX_DEPTH
    bool primary(std::size_t depth) {
        switch (tok_.kind) {
            case Kind::NUMBER:
            case Kind::STRING:
                trees_.push_back(tok_.value);
                return next();
            case Kind::NAME:
                return name(depth);
            case Kind::OPERATOR:
                if (!is("(")) {
                    return refuse(SYNTAX, tok_.column);
                }
                if (!next() || !expression(depth + 1)) {
                    return false;
                }
                return is(")") ? next() : refuse(SYNTAX, tok_.column);
            case Kind::PATTERN:
            case Kind::END:
                return refuse(SYNTAX, tok_.column);
        }
        return false;
    }

    // A literal word, a reference, or a call of a one-segment name.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by MAX_DEPTH
    bool name(std::size_t depth) {
        if (reserved()) {
            const std::string spelled = upper(tok_.text);
            if (!in_list(LITERAL_WORDS, spelled)) {
                return refuse(SYNTAX, tok_.column);
            }
            trees_.push_back(literal_leaf(spelled));
            return next();
        }
        const Tok called = tok_;
        if (!next()) {
            return false;
        }
        if (!is("(")) {
            trees_.push_back("n:" + called.text);
            return true;
        }
        if (!called.one_segment) {
            return refuse(SYNTAX, tok_.column);
        }
        if (!in_list(FUNCTION_TABLE, lower(called.text))) {
            return refuse(NOT_FUNCTION, called.column);
        }
        const std::size_t first = trees_.size();
        if (!arguments(depth)) {
            return false;
        }
        combine("call:" + lower(called.text), trees_.size() - first);
        return true;
    }

    // TRUE and FALSE are BOOL 1 and 0; GOOD, UNCERTAIN and BAD the INT
    // constants 0, 1, 2 (5.6, the table of functions).
    static std::string literal_leaf(const std::string& word) {
        if (word == "TRUE" || word == "FALSE") {
            return word == "TRUE" ? "b1" : "b0";
        }
        if (word == "GOOD") {
            return "q0";
        }
        return word == "UNCERTAIN" ? "q1" : "q2";
    }

    // The arguments of a call, from its `(`: expressions or pattern strings.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by MAX_DEPTH
    bool arguments(std::size_t depth) {
        if (!next()) {
            return false;
        }
        if (!is(")")) {
            while (true) {
                if (tok_.kind == Kind::PATTERN) {
                    trees_.push_back(tok_.value);
                }
                const bool read =
                    tok_.kind == Kind::PATTERN ? next() : expression(depth + 1);
                if (!read) {
                    return false;
                }
                if (!is(",")) {
                    break;
                }
                if (!next()) {
                    return false;
                }
            }
        }
        return is(")") ? next() : refuse(SYNTAX, tok_.column);
    }

    // for_value = range | expression: a sum and `..` is a range; anything
    // else is read again from the start as an expression.
    void for_value() {
        const std::size_t start = pos_;
        const Tok first = tok_;
        if (!level(SUM_LEVEL, 0)) {
            return;
        }
        if (is("..")) {
            if (next() && level(SUM_LEVEL, 0)) {
                combine("..", 2);
            }
            return;
        }
        pos_ = start;
        tok_ = first;
        trees_.clear();
        expression(0);
    }

    std::string_view text_;
    ExpressionKind kind_;
    std::size_t pos_ = 0;
    Tok tok_;
    std::optional<Refusal> refusal_;
    // The subtrees parsed so far; one, the whole tree, at the end.
    std::vector<std::string> trees_;

public:
    // The tree of an accepted text, in the notation of `tree_text`.
    [[nodiscard]] std::string tree() const {
        return trees_.size() == 1 ? trees_.front() : std::string("?");
    }
};

// The oracle's name of an inner node's operator.
std::string operator_name(const scada::conf::ExprNode& node) {
    using scada::conf::ExprOp;
    constexpr std::array<std::pair<ExprOp, std::string_view>, 22> NAMES = {{
        {ExprOp::NEGATE, "neg"},       {ExprOp::NOT, "not"},
        {ExprOp::POWER, "**"},         {ExprOp::MULTIPLY, "*"},
        {ExprOp::DIVIDE, "/"},         {ExprOp::INT_DIVIDE, "div"},
        {ExprOp::MODULO, "mod"},       {ExprOp::ADD, "+"},
        {ExprOp::SUBTRACT, "-"},       {ExprOp::EQUAL, "="},
        {ExprOp::NOT_EQUAL, "<>"},     {ExprOp::LESS, "<"},
        {ExprOp::LESS_EQUAL, "<="},    {ExprOp::GREATER, ">"},
        {ExprOp::GREATER_EQUAL, ">="}, {ExprOp::IN_RANGE, "in-range"},
        {ExprOp::IN_VALUE, "in"},      {ExprOp::AND, "and"},
        {ExprOp::OR, "or"},            {ExprOp::XOR, "xor"},
        {ExprOp::RANGE, ".."},         {ExprOp::CALL, "call"},
    }};
    for (const auto& [oper, name] : NAMES) {
        if (oper == node.op) {
            return oper == ExprOp::CALL ? "call:" + node.text
                                        : std::string(name);
        }
    }
    return "?";
}

// The library's tree in the oracle's notation: a leaf as its kind and
// value, an inner node as `(operator children...)`. Children come before
// their parent in the arena, so one pass in order builds every node.
std::string tree_text(const scada::conf::Expression& tree) {
    using scada::conf::ExprOp;
    std::vector<std::string> text(tree.nodes.size());
    for (std::size_t i = 0; i < tree.nodes.size(); ++i) {
        const scada::conf::ExprNode& node = tree.nodes[i];
        std::string name;
        switch (node.op) {
            case ExprOp::INTEGER:
                text[i] = "i" + std::to_string(node.integer);
                continue;
            case ExprOp::REAL:
                text[i] = "r" + exact(node.real);
                continue;
            case ExprOp::TIME:
                text[i] = "t" + std::to_string(node.integer);
                continue;
            case ExprOp::STRING:
                text[i] = "s'" + node.text + "'";
                continue;
            case ExprOp::BOOLEAN:
                text[i] = "b" + std::to_string(node.integer);
                continue;
            case ExprOp::QUALITY:
                text[i] = "q" + std::to_string(node.integer);
                continue;
            case ExprOp::REFERENCE:
                text[i] = "n:" + node.text;
                continue;
            case ExprOp::PATTERN:
                text[i] = "p\"" + node.text + "\"";
                continue;
            default:
                name = operator_name(node);
                break;
        }
        std::string joined = "(" + name;
        for (const std::size_t child : node.children) {
            joined += " " + std::move(text[child]);
        }
        text[i] = std::move(joined) + ")";
    }
    return text.at(tree.root);
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    const std::string text(bytes.begin(), bytes.end());
    for (const ExpressionKind kind :
         {ExpressionKind::RUN_TIME, ExpressionKind::CONFIGURATION,
          ExpressionKind::FOR_VALUE}) {
        scada::conf::ExpressionError error;
        const std::optional<scada::conf::Expression> expression =
            scada::conf::parse_expression(text, kind, error);
        Oracle oracle(text, kind);
        const std::optional<Refusal> expected = oracle.run();
        check(expression.has_value() == !expected.has_value());
        if (expression.has_value()) {
            const scada::conf::Expression& tree = *expression;
            check(tree_text(tree) == oracle.tree());
            check(tree.root < tree.nodes.size());
            for (std::size_t i = 0; i < tree.nodes.size(); ++i) {
                for (const std::size_t child : tree.nodes[i].children) {
                    check(child < i);
                }
            }
        } else {
            check(!error.message.empty());
            check(expected.has_value() && error.code == expected->code &&
                  error.column == expected->column);
        }
    }
    return 0;
}
