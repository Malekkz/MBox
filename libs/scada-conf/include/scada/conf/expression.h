#ifndef SCADA_CONF_EXPRESSION_H
#define SCADA_CONF_EXPRESSION_H

// The expression grammar of configuration schemas contract 5.6 (D-78): the
// lexer and parser of run-time expressions (virtual tags, alarm expressions,
// `suppress_when`, interlock conditions) and of configuration expressions
// (`when`, addresses, scaling, `repeat` ranges; section 2.4).
//
// This is the syntax: a text gives a tree or one located error (CF-520,
// CF-526, CF-528, CF-529). Names and types are checked against the tree's
// tags and parameters later (stage 5, CF-521 to CF-525, CF-527).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scada::conf {

// Which variant of the grammar: configuration expressions add strings,
// `IN` and ranges (5.6, "configuration expressions only").
enum class ExpressionKind : std::uint8_t {
    RUN_TIME,
    CONFIGURATION,
    // The value of `for` in a `repeat` block: a range or an expression
    // (`for_value`).
    FOR_VALUE,
};

// The limits of section 1.3: 4,096 bytes and a nesting depth of 32.
constexpr std::size_t MAX_EXPRESSION_BYTES = 4096;
constexpr std::size_t MAX_EXPRESSION_DEPTH = 32;

enum class ExprOp : std::uint8_t {
    // Leaves.
    INTEGER,    // `text` is the literal as written; `integer` its value
    REAL,       // `real` its value
    TIME,       // `integer` the milliseconds
    STRING,     // `text` the unquoted string
    BOOLEAN,    // `integer` 1 or 0
    QUALITY,    // `integer` 0 GOOD, 1 UNCERTAIN, 2 BAD
    REFERENCE,  // `text` the name
    PATTERN,    // `text` the pattern string of an aggregate or `tag`
    // Inner nodes; `children` are the operands in order.
    CALL,  // `text` the function name, lower case
    NEGATE,
    NOT,
    POWER,
    MULTIPLY,
    DIVIDE,
    INT_DIVIDE,  // DIV
    MODULO,      // MOD
    ADD,
    SUBTRACT,
    EQUAL,
    NOT_EQUAL,
    LESS,
    LESS_EQUAL,
    GREATER,
    GREATER_EQUAL,
    IN_RANGE,  // value, low, high
    IN_VALUE,  // value, set or string
    AND,
    OR,
    XOR,
    RANGE,  // low, high: a `for` value
};

struct ExprNode {
    ExprOp op = ExprOp::INTEGER;
    // 1-based byte column in the expression text: of its operator for an
    // operator node (`IN` for IN_RANGE and IN_VALUE), of its name for a
    // call, of its low end for a RANGE, of its first character for a leaf.
    std::uint32_t column = 1;
    std::string text;
    std::int64_t integer = 0;
    double real = 0.0;
    std::vector<std::size_t> children;
};

// A parsed expression: nodes in an arena, the root last.
struct Expression {
    std::vector<ExprNode> nodes;
    std::size_t root = 0;
};

// One located syntax error: its code, the column in the expression text
// (1-based) and the reason, for the message "expression: {reason} at column
// {col}" (CF-520) or the message of its code.
struct ExpressionError {
    std::uint16_t code = 0;
    std::uint32_t column = 1;
    std::string message;
};

// True if `name` (any case) is a reserved word of 5.6: AND, OR, XOR, NOT,
// MOD, DIV, IN, TRUE, FALSE, GOOD, UNCERTAIN, BAD. A parameter or `repeat`
// variable of such a name is refused (CF-212).
[[nodiscard]] bool is_reserved_word(std::string_view name) noexcept;

// True if `name` (any case) is a function of the table of 5.6.
[[nodiscard]] bool is_expression_function(std::string_view name) noexcept;

// Parses `text`. Gives the tree, or the first error and nothing.
[[nodiscard]] std::optional<Expression> parse_expression(
    std::string_view text, ExpressionKind kind, ExpressionError& error);

}  // namespace scada::conf

#endif  // SCADA_CONF_EXPRESSION_H
