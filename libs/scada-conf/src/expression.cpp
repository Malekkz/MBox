// The expression grammar of configuration schemas contract 5.6: a lexer and
// a recursive-descent parser whose functions follow the EBNF one to one.
// Recursion is bounded: every entry into a nested expression, a unary
// operand or the right side of `**` counts against MAX_EXPRESSION_DEPTH.

#include "scada/conf/expression.h"
#include "scada/conf/diagnostic.h"
#include "utf8.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace scada::conf {

namespace {

constexpr std::uint16_t CF_SYNTAX = 520;
constexpr std::uint16_t CF_TOO_BIG = 526;
constexpr std::uint16_t CF_LITERAL_RANGE = 528;
constexpr std::uint16_t CF_NOT_A_FUNCTION = 529;

constexpr std::uint64_t BASE_TEN = 10;
constexpr std::uint64_t HEX_BASE = 16;
constexpr std::uint64_t INT_MAX_MAGNITUDE =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

constexpr std::array<std::string_view, 33> FUNCTIONS = {
    "if",     "limit", "min",   "max",    "abs",   "sqrt", "exp",
    "ln",     "log10", "sin",   "cos",    "tan",   "asin", "acos",
    "atan",   "atan2", "round", "trunc",  "floor", "ceil", "to_real",
    "to_int", "band",  "bor",   "bxor",   "bnot",  "shl",  "shr",
    "bit",    "sum",   "avg",   "min_of", "max_of"};
constexpr std::array<std::string_view, 8> MORE_FUNCTIONS = {
    "count_good", "moving_avg", "rate",    "totalise",
    "quality",    "age",        "is_good", "tag"};

enum class Token : std::uint8_t {
    END,
    INTEGER,
    REAL,
    TIME,
    STRING,
    PATTERN,
    NAME,
    OPERATOR,
};

struct Lexeme {
    Token token = Token::END;
    std::uint32_t column = 1;
    // The byte offset just past the token.
    std::uint32_t end = 0;
    std::string text;
    std::int64_t integer = 0;
    double real = 0.0;
    bool single_segment = false;
};

bool is_letter(char chr) noexcept {
    return (chr >= 'a' && chr <= 'z') || (chr >= 'A' && chr <= 'Z');
}
bool is_digit(char chr) noexcept {
    return chr >= '0' && chr <= '9';
}
bool starts_segment(char chr) noexcept {
    return is_letter(chr) || chr == '_';
}
bool continues_segment(char chr) noexcept {
    return is_letter(chr) || is_digit(chr) || chr == '_';
}
bool is_hex(char chr) noexcept {
    return is_digit(chr) || (chr >= 'a' && chr <= 'f') ||
           (chr >= 'A' && chr <= 'F');
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](char chr) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(chr)));
    });
    return out;
}

class Lexer {
public:
    Lexer(std::string_view text, ExpressionKind kind)
        : text_(text), kind_(kind) {}

    // The next token, or nothing with `error` set.
    std::optional<Lexeme> next(ExpressionError& error) {
        std::optional<Lexeme> lexeme = scan(error);
        if (lexeme) {
            lexeme->end = static_cast<std::uint32_t>(pos_);
        }
        return lexeme;
    }

private:
    std::optional<Lexeme> scan(ExpressionError& error) {
        // Spaces and line ends separate tokens (5.6); a tab is not one.
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\n' ||
                text_[pos_] == '\r')) {
            ++pos_;
        }
        if (pos_ < text_.size() && text_[pos_] == '\t') {
            fail(error, CF_SYNTAX, pos_,
                 "a tab is not allowed in an expression; use spaces");
            return std::nullopt;
        }
        Lexeme lexeme;
        lexeme.column = static_cast<std::uint32_t>(pos_ + 1);
        if (pos_ >= text_.size()) {
            return lexeme;
        }
        const char chr = text_[pos_];
        if (is_digit(chr)) {
            return number(lexeme, error);
        }
        if ((chr == 'T' || chr == 't') && peek(1) == '#') {
            return time(lexeme, error);
        }
        if (starts_segment(chr)) {
            return name(lexeme);
        }
        if (chr == '\'') {
            return string(lexeme, error);
        }
        if (chr == '"') {
            return pattern(lexeme, error);
        }
        return operator_token(lexeme, error);
    }

    [[nodiscard]] char peek(std::size_t ahead) const noexcept {
        return pos_ + ahead < text_.size() ? text_[pos_ + ahead] : '\0';
    }

    static bool fail(ExpressionError& error, std::uint16_t code,
                     std::size_t where, std::string message) {
        error = ExpressionError{.code = code,
                                .column = static_cast<std::uint32_t>(where + 1),
                                .message = std::move(message)};
        return false;
    }

    // 5.6, "tokens that abut": a number, time or string followed directly
    // by a letter, a digit, `_` or `#`.
    bool check_abut(ExpressionError& error, std::size_t start) const {
        const char chr = peek(0);
        if (continues_segment(chr) || chr == '#') {
            return fail(error, CF_SYNTAX, pos_,
                        quote_text(text_.substr(start, pos_ - start)) +
                            " is followed directly by " +
                            quote_text(std::string(1, chr)));
        }
        return true;
    }

    std::optional<Lexeme> number(Lexeme& lexeme, ExpressionError& error) {
        const bool hex_c =
            text_[pos_] == '0' && (peek(1) == 'x' || peek(1) == 'X');
        if (hex_c || text_.substr(pos_).starts_with("16#")) {
            return hex_number(lexeme, error, hex_c);
        }
        // decimal_int: "0" or a digit other than 0 and more digits.
        const std::size_t start = pos_;
        if (text_[pos_] == '0') {
            ++pos_;
        } else {
            while (pos_ < text_.size() && is_digit(text_[pos_])) {
                ++pos_;
            }
        }
        bool real = false;
        if (peek(0) == '.' && is_digit(peek(1))) {
            real = true;
            ++pos_;
            while (pos_ < text_.size() && is_digit(text_[pos_])) {
                ++pos_;
            }
        }
        if ((peek(0) == 'e' || peek(0) == 'E') &&
            (is_digit(peek(1)) ||
             ((peek(1) == '+' || peek(1) == '-') && is_digit(peek(2))))) {
            real = true;
            pos_ += 2;
            while (pos_ < text_.size() && is_digit(text_[pos_])) {
                ++pos_;
            }
        }
        if (!check_abut(error, start)) {
            return std::nullopt;
        }
        lexeme.text = std::string(text_.substr(start, pos_ - start));
        return real ? real_value(lexeme, start, error)
                    : integer_value(lexeme, start, error);
    }

    // `0x...` or `16#...`.
    std::optional<Lexeme> hex_number(Lexeme& lexeme, ExpressionError& error,
                                     bool c_form) {
        const std::size_t start = pos_;
        pos_ += c_form ? 2 : 3;
        const std::size_t digits = pos_;
        std::uint64_t value = 0;
        bool too_big = false;
        while (pos_ < text_.size() && is_hex(text_[pos_])) {
            const char digit = text_[pos_];
            const std::uint64_t part =
                is_digit(digit)
                    ? static_cast<std::uint64_t>(digit - '0')
                    : static_cast<std::uint64_t>(std::tolower(digit) - 'a') +
                          BASE_TEN;
            too_big = too_big || value > (INT_MAX_MAGNITUDE - part) / HEX_BASE;
            value = too_big ? value : (value * HEX_BASE) + part;
            ++pos_;
        }
        if (pos_ == digits) {
            fail(error, CF_SYNTAX, pos_, "a hexadecimal digit is missing");
            return std::nullopt;
        }
        if (!check_abut(error, start)) {
            return std::nullopt;
        }
        lexeme.text = std::string(text_.substr(start, pos_ - start));
        if (too_big) {
            fail(error, CF_LITERAL_RANGE, start,
                 "literal " + quote_text(lexeme.text) + " is out of range");
            return std::nullopt;
        }
        lexeme.token = Token::INTEGER;
        lexeme.integer = static_cast<std::int64_t>(value);
        return lexeme;
    }

    static std::optional<Lexeme> integer_value(Lexeme& lexeme,
                                               std::size_t start,
                                               ExpressionError& error) {
        std::uint64_t value = 0;
        for (const char digit : lexeme.text) {
            const auto part = static_cast<std::uint64_t>(digit - '0');
            if (value > (INT_MAX_MAGNITUDE - part) / BASE_TEN) {
                fail(error, CF_LITERAL_RANGE, start,
                     "literal " + quote_text(lexeme.text) + " is out of range");
                return std::nullopt;
            }
            value = (value * BASE_TEN) + part;
        }
        lexeme.token = Token::INTEGER;
        lexeme.integer = static_cast<std::int64_t>(value);
        return lexeme;
    }

    static std::optional<Lexeme> real_value(Lexeme& lexeme, std::size_t start,
                                            ExpressionError& error) {
        double value = 0.0;
        const auto [end, code] =
            std::from_chars(std::to_address(lexeme.text.cbegin()),
                            std::to_address(lexeme.text.cend()), value);
        const bool nonzero_digit = std::ranges::any_of(
            lexeme.text.substr(0, lexeme.text.find_first_of("eE")),
            [](char chr) { return chr >= '1' && chr <= '9'; });
        if (code != std::errc() || !std::isfinite(value) ||
            (value == 0.0 && nonzero_digit)) {
            fail(error, CF_LITERAL_RANGE, start,
                 "literal " + quote_text(lexeme.text) + " is out of range");
            return std::nullopt;
        }
        static_cast<void>(end);
        lexeme.token = Token::REAL;
        lexeme.real = value;
        return lexeme;
    }

    std::optional<Lexeme> time(Lexeme& lexeme, ExpressionError& error) {
        constexpr std::uint64_t SECOND = 1000;
        constexpr std::uint64_t MINUTE = 60 * SECOND;
        constexpr std::uint64_t HOUR = 60 * MINUTE;
        constexpr std::uint64_t DAY = 24 * HOUR;
        const std::size_t start = pos_;
        pos_ += 2;
        const std::size_t digits = pos_;
        if (peek(0) == '0') {
            ++pos_;
        } else {
            while (pos_ < text_.size() && is_digit(text_[pos_])) {
                ++pos_;
            }
        }
        if (pos_ == digits) {
            fail(error, CF_SYNTAX, pos_, "a time needs a number");
            return std::nullopt;
        }
        const std::string_view number = text_.substr(digits, pos_ - digits);
        std::uint64_t unit = 0;
        if (text_.substr(pos_).starts_with("ms")) {
            unit = 1;
            pos_ += 2;
        } else if (peek(0) == 's' || peek(0) == 'm' || peek(0) == 'h' ||
                   peek(0) == 'd') {
            switch (peek(0)) {
                case 's':
                    unit = SECOND;
                    break;
                case 'm':
                    unit = MINUTE;
                    break;
                case 'h':
                    unit = HOUR;
                    break;
                default:
                    unit = DAY;
                    break;
            }
            ++pos_;
        } else {
            fail(error, CF_SYNTAX, pos_,
                 "a time needs a unit ms, s, m, h or d in lower case");
            return std::nullopt;
        }
        if (!check_abut(error, start)) {
            return std::nullopt;
        }
        lexeme.text = std::string(text_.substr(start, pos_ - start));
        std::uint64_t value = 0;
        for (const char digit : number) {
            const auto part = static_cast<std::uint64_t>(digit - '0');
            if (value > (INT_MAX_MAGNITUDE - part) / BASE_TEN) {
                fail(error, CF_LITERAL_RANGE, start,
                     "literal " + quote_text(lexeme.text) + " is out of range");
                return std::nullopt;
            }
            value = (value * BASE_TEN) + part;
        }
        if (value > INT_MAX_MAGNITUDE / unit) {
            fail(error, CF_LITERAL_RANGE, start,
                 "literal " + quote_text(lexeme.text) + " is out of range");
            return std::nullopt;
        }
        lexeme.token = Token::TIME;
        lexeme.integer = static_cast<std::int64_t>(value * unit);
        return lexeme;
    }

    Lexeme name(Lexeme& lexeme) {
        const std::size_t start = pos_;
        bool single = true;
        while (true) {
            while (pos_ < text_.size() && continues_segment(text_[pos_])) {
                ++pos_;
            }
            // A "." is part of a name only when a segment follows.
            if (peek(0) == '.' && starts_segment(peek(1))) {
                single = false;
                ++pos_;
                continue;
            }
            break;
        }
        lexeme.token = Token::NAME;
        lexeme.text = std::string(text_.substr(start, pos_ - start));
        lexeme.single_segment = single;
        return lexeme;
    }

    std::optional<Lexeme> string(Lexeme& lexeme, ExpressionError& error) {
        const std::size_t start = pos_;
        if (kind_ == ExpressionKind::RUN_TIME) {
            fail(error, CF_SYNTAX, start,
                 "a string is allowed in configuration expressions only");
            return std::nullopt;
        }
        ++pos_;
        std::string value;
        while (true) {
            if (pos_ >= text_.size()) {
                fail(error, CF_SYNTAX, start, "a string is not closed");
                return std::nullopt;
            }
            if (text_[pos_] == '\'') {
                if (peek(1) == '\'') {
                    value.push_back('\'');
                    pos_ += 2;
                    continue;
                }
                ++pos_;
                break;
            }
            value.push_back(text_[pos_]);
            ++pos_;
        }
        if (!check_abut(error, start)) {
            return std::nullopt;
        }
        lexeme.token = Token::STRING;
        lexeme.text = std::move(value);
        return lexeme;
    }

    std::optional<Lexeme> pattern(Lexeme& lexeme, ExpressionError& error) {
        const std::size_t start = pos_;
        const std::size_t close = text_.find('"', pos_ + 1);
        if (close == std::string_view::npos) {
            fail(error, CF_SYNTAX, start, "a pattern string is not closed");
            return std::nullopt;
        }
        lexeme.token = Token::PATTERN;
        lexeme.text = std::string(text_.substr(pos_ + 1, close - pos_ - 1));
        pos_ = close + 1;
        return lexeme;
    }

    std::optional<Lexeme> operator_token(Lexeme& lexeme,
                                         ExpressionError& error) {
        static constexpr std::array<std::string_view, 15> OPERATORS = {
            "**", "<>", "<=", ">=", "..", "*", "/", "+",
            "-",  "=",  "<",  ">",  "(",  ")", ","};
        for (const std::string_view oper : OPERATORS) {
            if (text_.substr(pos_).starts_with(oper)) {
                lexeme.token = Token::OPERATOR;
                lexeme.text = std::string(oper);
                pos_ += oper.size();
                return lexeme;
            }
        }
        fail(error, CF_SYNTAX, pos_,
             "unexpected character " + quote_text(character()) + hint());
        return std::nullopt;
    }

    // The whole character at `pos_`: one byte, or a UTF-8 sequence.
    [[nodiscard]] std::string character() const {
        const std::optional<std::pair<std::size_t, std::uint32_t>> decoded =
            utf8::decode(text_, pos_);
        const std::size_t length = decoded ? decoded->first : 1;
        return std::string(text_.substr(pos_, length));
    }

    // The right spelling of a character another language uses here.
    [[nodiscard]] std::string hint() const {
        switch (text_[pos_]) {
            case '!':
                return peek(1) == '=' ? "; \"not equal\" is written <>"
                                      : "; negation is written NOT";
            case '&':
                return "; \"and\" is written AND";
            case '|':
                return "; \"or\" is written OR";
            case '%':
                return "; the remainder is written MOD";
            case '.':
                return "; a tag name with a segment that starts with a digit "
                       "or holds - is written tag(\"...\")";
            default:
                return {};
        }
    }

    std::string_view text_;
    ExpressionKind kind_;
    std::size_t pos_ = 0;
};

// The one-segment reserved words of 5.6, upper case.
std::optional<std::string> reserved_word(const Lexeme& lexeme) {
    if (lexeme.token != Token::NAME || !lexeme.single_segment) {
        return std::nullopt;
    }
    std::string upper(lexeme.text);
    std::ranges::transform(upper, upper.begin(), [](char chr) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(chr)));
    });
    for (const std::string_view word :
         {"AND", "OR", "XOR", "NOT", "MOD", "DIV", "IN", "TRUE", "FALSE",
          "GOOD", "UNCERTAIN", "BAD"}) {
        if (upper == word) {
            return upper;
        }
    }
    return std::nullopt;
}

class Parser {
public:
    Parser(std::string_view text, ExpressionKind kind, ExpressionError& error)
        : lexer_(text, kind), kind_(kind), error_(&error) {}

    std::optional<Expression> run() {
        if (!advance()) {
            return std::nullopt;
        }
        std::optional<std::size_t> root;
        if (kind_ == ExpressionKind::FOR_VALUE) {
            root = for_value();
        } else {
            root = expression(0);
        }
        if (!root) {
            return std::nullopt;
        }
        if (current_.token != Token::END) {
            fail(current_.column, "unexpected " + describe(current_));
            return std::nullopt;
        }
        expression_.root = *root;
        return std::move(expression_);
    }

private:
    bool advance() {
        std::optional<Lexeme> next = lexer_.next(*error_);
        if (!next) {
            return false;
        }
        current_ = std::move(*next);
        return true;
    }

    std::nullopt_t fail(std::uint32_t column, std::string message,
                        std::uint16_t code = CF_SYNTAX) {
        *error_ = ExpressionError{
            .code = code, .column = column, .message = std::move(message)};
        return std::nullopt;
    }

    static std::string describe(const Lexeme& lexeme) {
        if (lexeme.token == Token::END) {
            return "end of the expression";
        }
        return quote_text(lexeme.text);
    }

    [[nodiscard]] bool is_operator(std::string_view oper) const {
        return current_.token == Token::OPERATOR && current_.text == oper;
    }

    [[nodiscard]] bool is_word(std::string_view word) const {
        const std::optional<std::string> reserved = reserved_word(current_);
        return reserved && *reserved == word;
    }

    // A node; its extent runs from its column, or its first child, to its
    // last child.
    std::size_t add(ExprOp oper, std::uint32_t column,
                    std::vector<std::size_t> children = {}) {
        ExprNode node;
        node.op = oper;
        node.column = column;
        node.begin = column - 1;
        node.end = column - 1;
        for (const std::size_t child : children) {
            node.begin = std::min(node.begin, expression_.nodes[child].begin);
            node.end = std::max(node.end, expression_.nodes[child].end);
        }
        node.children = std::move(children);
        expression_.nodes.push_back(std::move(node));
        return expression_.nodes.size() - 1;
    }

    bool enter(std::size_t depth) {
        if (depth >= MAX_EXPRESSION_DEPTH) {
            fail(current_.column,
                 "expression exceeds a nesting depth of " +
                     std::to_string(MAX_EXPRESSION_DEPTH),
                 CF_TOO_BIG);
            return false;
        }
        return true;
    }

    // for_value = range | expression
    std::optional<std::size_t> for_value() {
        const std::optional<std::size_t> low = add_expr(0);
        if (!low) {
            return std::nullopt;
        }
        if (!is_operator("..")) {
            return finish_or(*low, 0);
        }
        return range(*low, 0);
    }

    // The rest of an expression whose first add_expr is `left`.
    // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
    std::optional<std::size_t> finish_or(std::size_t left, std::size_t depth) {
        return or_expr(depth, left);
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
    std::optional<std::size_t> range(std::size_t low, std::size_t depth) {
        const std::uint32_t column = current_.column;
        if (kind_ == ExpressionKind::RUN_TIME) {
            return fail(column,
                        "a range is allowed in configuration expressions only");
        }
        if (!advance()) {
            return std::nullopt;
        }
        const std::optional<std::size_t> high = add_expr(depth);
        if (!high) {
            return std::nullopt;
        }
        return add(ExprOp::RANGE, expression_.nodes[low].column, {low, *high});
    }

    // expression = or_expr, as or_expr { OR xor_expr } over the levels.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by MAX_EXPRESSION_DEPTH, checked in `enter`
    std::optional<std::size_t> expression(std::size_t depth) {
        if (!enter(depth)) {
            return std::nullopt;
        }
        const std::optional<std::size_t> left = add_expr(depth);
        if (!left) {
            return std::nullopt;
        }
        return finish_or(*left, depth);
    }

    // One level of a left-associative logical operator: `word` over
    // operands parsed by `next`. `first`, if given, is the first operand's
    // add_expr, already parsed.
    template <typename Next>
    // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
    std::optional<std::size_t> logical_level(std::string_view word, ExprOp oper,
                                             std::optional<std::size_t> first,
                                             Next next) {
        std::optional<std::size_t> left = next(first);
        while (left && is_word(word)) {
            const std::uint32_t column = current_.column;
            if (!advance()) {
                return std::nullopt;
            }
            const std::optional<std::size_t> right = next(std::nullopt);
            if (!right) {
                return std::nullopt;
            }
            left = add(oper, column, {*left, *right});
        }
        return left;
    }

    // or_expr = xor_expr { "OR" xor_expr }
    // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
    std::optional<std::size_t> or_expr(std::size_t depth,
                                       std::optional<std::size_t> first) {
        return logical_level(
            "OR", ExprOp::OR, first,
            // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
            [this, depth](std::optional<std::size_t> start) {
                return xor_expr(depth, start);
            });
    }

    // xor_expr = and_expr { "XOR" and_expr }
    // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
    std::optional<std::size_t> xor_expr(std::size_t depth,
                                        std::optional<std::size_t> first) {
        return logical_level(
            "XOR", ExprOp::XOR, first,
            // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
            [this, depth](std::optional<std::size_t> start) {
                return and_expr(depth, start);
            });
    }

    // and_expr = cmp_expr { "AND" cmp_expr }
    // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
    std::optional<std::size_t> and_expr(std::size_t depth,
                                        std::optional<std::size_t> first) {
        return logical_level(
            "AND", ExprOp::AND, first,
            // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
            [this, depth](std::optional<std::size_t> start) {
                return start ? cmp_rest(*start, depth) : comparison(depth);
            });
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
    std::optional<std::size_t> comparison(std::size_t depth) {
        const std::optional<std::size_t> left = add_expr(depth);
        if (!left) {
            return std::nullopt;
        }
        return cmp_rest(*left, depth);
    }

    [[nodiscard]] std::optional<ExprOp> comparison_operator() const {
        if (current_.token != Token::OPERATOR) {
            return std::nullopt;
        }
        static constexpr std::array<std::pair<std::string_view, ExprOp>, 6>
            OPERATORS = {{{"=", ExprOp::EQUAL},
                          {"<>", ExprOp::NOT_EQUAL},
                          {"<", ExprOp::LESS},
                          {"<=", ExprOp::LESS_EQUAL},
                          {">", ExprOp::GREATER},
                          {">=", ExprOp::GREATER_EQUAL}}};
        for (const auto& [text, oper] : OPERATORS) {
            if (current_.text == text) {
                return oper;
            }
        }
        return std::nullopt;
    }

    // cmp_expr = add_expr [ cmp_op add_expr | IN ( range | add_expr ) ]
    // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
    std::optional<std::size_t> cmp_rest(std::size_t left, std::size_t depth) {
        std::optional<std::size_t> result;
        if (const std::optional<ExprOp> oper = comparison_operator()) {
            const std::uint32_t column = current_.column;
            if (!advance()) {
                return std::nullopt;
            }
            const std::optional<std::size_t> right = add_expr(depth);
            if (!right) {
                return std::nullopt;
            }
            result = add(*oper, column, {left, *right});
        } else if (is_word("IN")) {
            result = in_rest(left, depth);
            if (!result) {
                return std::nullopt;
            }
        } else {
            return left;
        }
        // A comparison does not chain.
        if (comparison_operator() || is_word("IN")) {
            return fail(current_.column, "comparisons do not chain");
        }
        return result;
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
    std::optional<std::size_t> in_rest(std::size_t left, std::size_t depth) {
        const std::uint32_t column = current_.column;
        if (kind_ == ExpressionKind::RUN_TIME) {
            return fail(column,
                        "IN is allowed in configuration expressions only");
        }
        if (!advance()) {
            return std::nullopt;
        }
        const std::optional<std::size_t> first = add_expr(depth);
        if (!first) {
            return std::nullopt;
        }
        if (!is_operator("..")) {
            return add(ExprOp::IN_VALUE, column, {left, *first});
        }
        // The range's ends become the operands: no RANGE node is left over.
        if (!advance()) {
            return std::nullopt;
        }
        const std::optional<std::size_t> high = add_expr(depth);
        if (!high) {
            return std::nullopt;
        }
        return add(ExprOp::IN_RANGE, column, {left, *first, *high});
    }

    // add_expr = mul_expr { ( "+" | "-" ) mul_expr }
    // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
    std::optional<std::size_t> add_expr(std::size_t depth) {
        std::optional<std::size_t> left = mul_expr(depth);
        while (left && (is_operator("+") || is_operator("-"))) {
            const ExprOp oper =
                is_operator("+") ? ExprOp::ADD : ExprOp::SUBTRACT;
            const std::uint32_t column = current_.column;
            if (!advance()) {
                return std::nullopt;
            }
            const std::optional<std::size_t> right = mul_expr(depth);
            if (!right) {
                return std::nullopt;
            }
            left = add(oper, column, {*left, *right});
        }
        return left;
    }

    [[nodiscard]] std::optional<ExprOp> multiplicative() const {
        if (is_operator("*")) {
            return ExprOp::MULTIPLY;
        }
        if (is_operator("/")) {
            return ExprOp::DIVIDE;
        }
        if (is_word("DIV")) {
            return ExprOp::INT_DIVIDE;
        }
        if (is_word("MOD")) {
            return ExprOp::MODULO;
        }
        return std::nullopt;
    }

    // mul_expr = unary { ( "*" | "/" | "DIV" | "MOD" ) unary }
    // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
    std::optional<std::size_t> mul_expr(std::size_t depth) {
        std::optional<std::size_t> left = unary(depth);
        while (left) {
            const std::optional<ExprOp> oper = multiplicative();
            if (!oper) {
                break;
            }
            const std::uint32_t column = current_.column;
            if (!advance()) {
                return std::nullopt;
            }
            const std::optional<std::size_t> right = unary(depth);
            if (!right) {
                return std::nullopt;
            }
            left = add(*oper, column, {*left, *right});
        }
        return left;
    }

    // unary = [ "-" | "NOT" ] pow_expr
    // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
    std::optional<std::size_t> unary(std::size_t depth) {
        if (!is_operator("-") && !is_word("NOT")) {
            return pow_expr(depth);
        }
        if (!enter(depth + 1)) {
            return std::nullopt;
        }
        const ExprOp oper = is_operator("-") ? ExprOp::NEGATE : ExprOp::NOT;
        const std::uint32_t column = current_.column;
        if (!advance()) {
            return std::nullopt;
        }
        // One unary operator in a row: `- -x` and `NOT NOT a` are refused.
        if (is_operator("-") || is_word("NOT")) {
            return fail(current_.column,
                        "only one unary operator is allowed in a row");
        }
        const std::optional<std::size_t> operand = pow_expr(depth + 1);
        if (!operand) {
            return std::nullopt;
        }
        return add(oper, column, {*operand});
    }

    // pow_expr = primary [ "**" unary ]
    // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
    std::optional<std::size_t> pow_expr(std::size_t depth) {
        const std::optional<std::size_t> base = primary(depth);
        if (!base || !is_operator("**")) {
            return base;
        }
        const std::uint32_t column = current_.column;
        if (!advance() || !enter(depth + 1)) {
            return std::nullopt;
        }
        const std::optional<std::size_t> power = unary(depth + 1);
        if (!power) {
            return std::nullopt;
        }
        return add(ExprOp::POWER, column, {*base, *power});
    }

    std::size_t leaf(ExprOp oper) {
        const std::size_t node = add(oper, current_.column);
        expression_.nodes[node].end = current_.end;
        expression_.nodes[node].text = current_.text;
        expression_.nodes[node].integer = current_.integer;
        expression_.nodes[node].real = current_.real;
        return node;
    }

    // A literal word, if `word` is one: TRUE, FALSE, GOOD, UNCERTAIN, BAD.
    std::optional<std::size_t> literal_word(const std::string& word) {
        if (word == "TRUE" || word == "FALSE") {
            const std::size_t node = leaf(ExprOp::BOOLEAN);
            expression_.nodes[node].integer = word == "TRUE" ? 1 : 0;
            return node;
        }
        if (word == "GOOD" || word == "UNCERTAIN" || word == "BAD") {
            const std::size_t node = leaf(ExprOp::QUALITY);
            std::int64_t code = 2;
            if (word == "GOOD") {
                code = 0;
            } else if (word == "UNCERTAIN") {
                code = 1;
            }
            expression_.nodes[node].integer = code;
            return node;
        }
        return std::nullopt;
    }

    // primary = literal | call | reference | "(" expression ")"
    // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
    std::optional<std::size_t> primary(std::size_t depth) {
        std::optional<std::size_t> node;
        switch (current_.token) {
            case Token::INTEGER:
                node = leaf(ExprOp::INTEGER);
                break;
            case Token::REAL:
                node = leaf(ExprOp::REAL);
                break;
            case Token::TIME:
                node = leaf(ExprOp::TIME);
                break;
            case Token::STRING:
                node = leaf(ExprOp::STRING);
                break;
            case Token::NAME:
                return name_primary(depth);
            case Token::OPERATOR:
                if (is_operator("(")) {
                    return parenthesis(depth);
                }
                return fail(
                    current_.column,
                    "unexpected " + describe(current_) +
                        (is_operator("=") ? "; equality is written with one ="
                                          : ""));
            case Token::PATTERN:
                return fail(current_.column,
                            "a pattern string is allowed only as the argument "
                            "of a function" +
                                std::string(kind_ == ExpressionKind::RUN_TIME
                                                ? "; a run-time expression "
                                                  "has no text values"
                                                : "; a text is written in "
                                                  "single quotes ('auto')"));
            case Token::END:
                return fail(current_.column, "an operand is missing");
        }
        if (!advance()) {
            return std::nullopt;
        }
        return node;
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
    std::optional<std::size_t> parenthesis(std::size_t depth) {
        if (!advance()) {
            return std::nullopt;
        }
        const std::optional<std::size_t> inner = expression(depth + 1);
        if (!inner) {
            return std::nullopt;
        }
        if (is_operator("..")) {
            return fail(current_.column,
                        "a range is written without parentheses: n IN 1..5");
        }
        if (!is_operator(")")) {
            return fail(current_.column, "a ')' is missing");
        }
        if (!advance()) {
            return std::nullopt;
        }
        return inner;
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
    std::optional<std::size_t> name_primary(std::size_t depth) {
        if (const std::optional<std::string> word = reserved_word(current_)) {
            const std::optional<std::size_t> literal = literal_word(*word);
            if (!literal) {
                return fail(
                    current_.column,
                    quote_text(current_.text) + " is a reserved word" +
                        (kind_ == ExpressionKind::RUN_TIME
                             ? "; a tag of this name is written tag(\"" +
                                   current_.text + "\")"
                             : "; a parameter or variable cannot have this "
                               "name"));
            }
            if (!advance()) {
                return std::nullopt;
            }
            return literal;
        }
        const Lexeme name = current_;
        if (!advance()) {
            return std::nullopt;
        }
        if (!is_operator("(")) {
            const std::size_t node = add(ExprOp::REFERENCE, name.column);
            expression_.nodes[node].end = name.end;
            expression_.nodes[node].text = name.text;
            return node;
        }
        if (!name.single_segment) {
            return fail(current_.column,
                        quote_text(name.text) + " is not a function name");
        }
        if (!is_expression_function(name.text)) {
            return fail(name.column,
                        quote_text(name.text) + " is not a function",
                        CF_NOT_A_FUNCTION);
        }
        return call(name, depth);
    }

    // call = name_segment "(" [ argument { "," argument } ] ")"
    // NOLINTNEXTLINE(misc-no-recursion): bounded as `expression`
    std::optional<std::size_t> call(const Lexeme& name, std::size_t depth) {
        std::vector<std::size_t> arguments;
        if (!advance()) {
            return std::nullopt;
        }
        if (!is_operator(")")) {
            while (true) {
                std::optional<std::size_t> argument;
                if (current_.token == Token::PATTERN) {
                    argument = leaf(ExprOp::PATTERN);
                    if (!advance()) {
                        return std::nullopt;
                    }
                } else {
                    argument = expression(depth + 1);
                }
                if (!argument) {
                    return std::nullopt;
                }
                arguments.push_back(*argument);
                if (is_operator(",")) {
                    if (!advance()) {
                        return std::nullopt;
                    }
                    continue;
                }
                break;
            }
        }
        if (!is_operator(")")) {
            return fail(current_.column, "a ')' or ',' is missing");
        }
        const std::uint32_t close = current_.end;
        if (!advance()) {
            return std::nullopt;
        }
        const std::size_t node =
            add(ExprOp::CALL, name.column, std::move(arguments));
        expression_.nodes[node].end = close;
        expression_.nodes[node].text = lower(name.text);
        return node;
    }

    Lexer lexer_;
    ExpressionKind kind_;
    ExpressionError* error_;
    Lexeme current_;
    Expression expression_;
};

}  // namespace

bool is_reserved_word(std::string_view name) noexcept {
    constexpr std::array<std::string_view, 12> WORDS = {
        "AND", "OR",   "XOR",   "NOT",  "MOD",       "DIV",
        "IN",  "TRUE", "FALSE", "GOOD", "UNCERTAIN", "BAD"};
    return std::ranges::any_of(WORDS, [name](std::string_view word) {
        return name.size() == word.size() &&
               std::ranges::equal(name, word, [](char lhs, char rhs) {
                   return std::toupper(static_cast<unsigned char>(lhs)) == rhs;
               });
    });
}

bool is_expression_function(std::string_view name) noexcept {
    // Function names are case-insensitive (5.6); the longest is 10 bytes.
    constexpr std::size_t LONGEST = 16;
    if (name.size() > LONGEST) {
        return false;
    }
    std::array<char, LONGEST> buffer{};
    for (std::size_t i = 0; i < name.size(); ++i) {
        buffer.at(i) = static_cast<char>(
            std::tolower(static_cast<unsigned char>(name[i])));
    }
    const std::string_view folded(buffer.data(), name.size());
    return std::ranges::find(FUNCTIONS, folded) != FUNCTIONS.end() ||
           std::ranges::find(MORE_FUNCTIONS, folded) != MORE_FUNCTIONS.end();
}

std::optional<Expression> parse_expression(std::string_view text,
                                           ExpressionKind kind,
                                           ExpressionError& error) {
    if (text.size() > MAX_EXPRESSION_BYTES) {
        error = ExpressionError{
            .code = CF_TOO_BIG,
            .column = 1,
            .message = "expression exceeds " +
                       std::to_string(MAX_EXPRESSION_BYTES) + " bytes"};
        return std::nullopt;
    }
    Parser parser(text, kind, error);
    return parser.run();
}

}  // namespace scada::conf
