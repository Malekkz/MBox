// The expression grammar of configuration schemas contract 5.6: the syntax
// cases of conformance test 11 (section 14).

#include "scada/conf/expression.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace scada::conf {
namespace {

std::string op_name(ExprOp oper) {
    switch (oper) {
        case ExprOp::NEGATE:
            return "neg";
        case ExprOp::NOT:
            return "not";
        case ExprOp::POWER:
            return "**";
        case ExprOp::MULTIPLY:
            return "*";
        case ExprOp::DIVIDE:
            return "/";
        case ExprOp::INT_DIVIDE:
            return "div";
        case ExprOp::MODULO:
            return "mod";
        case ExprOp::ADD:
            return "+";
        case ExprOp::SUBTRACT:
            return "-";
        case ExprOp::EQUAL:
            return "=";
        case ExprOp::NOT_EQUAL:
            return "<>";
        case ExprOp::LESS:
            return "<";
        case ExprOp::LESS_EQUAL:
            return "<=";
        case ExprOp::GREATER:
            return ">";
        case ExprOp::GREATER_EQUAL:
            return ">=";
        case ExprOp::IN_RANGE:
            return "in-range";
        case ExprOp::IN_VALUE:
            return "in";
        case ExprOp::AND:
            return "and";
        case ExprOp::OR:
            return "or";
        case ExprOp::XOR:
            return "xor";
        case ExprOp::RANGE:
            return "..";
        default:
            return "?";
    }
}

// A prefix form of the tree: (op a b), literals and names as written.
// NOLINTNEXTLINE(misc-no-recursion): bounded by the parser's depth limit
void print(const Expression& expression, std::size_t index, std::string& out) {
    const ExprNode& node = expression.nodes.at(index);
    switch (node.op) {
        case ExprOp::INTEGER:
        case ExprOp::TIME:
        case ExprOp::BOOLEAN:
        case ExprOp::QUALITY:
            out += std::to_string(node.integer);
            return;
        case ExprOp::REAL:
            out += node.text;
            return;
        case ExprOp::STRING:
            out += "'" + node.text + "'";
            return;
        case ExprOp::PATTERN:
            out += "\"" + node.text + "\"";
            return;
        case ExprOp::REFERENCE:
            out += node.text;
            return;
        case ExprOp::CALL:
            out += node.text + "(";
            for (std::size_t i = 0; i < node.children.size(); ++i) {
                if (i > 0) {
                    out += " ";
                }
                print(expression, node.children[i], out);
            }
            out += ")";
            return;
        default:
            break;
    }
    out += "(" + op_name(node.op);
    for (const std::size_t child : node.children) {
        out += " ";
        print(expression, child, out);
    }
    out += ")";
}

std::string parse(std::string_view text,
                  ExpressionKind kind = ExpressionKind::CONFIGURATION) {
    ExpressionError error;
    const std::optional<Expression> expression =
        parse_expression(text, kind, error);
    if (!expression) {
        return "CF-" + std::to_string(error.code) + " at " +
               std::to_string(error.column) + ": " + error.message;
    }
    std::string out;
    print(*expression, expression->root, out);
    return out;
}

std::string code_of(std::string_view text,
                    ExpressionKind kind = ExpressionKind::CONFIGURATION) {
    ExpressionError error;
    if (parse_expression(text, kind, error)) {
        return "parsed";
    }
    return "CF-" + std::to_string(error.code);
}

std::string run_time(std::string_view text) {
    return parse(text, ExpressionKind::RUN_TIME);
}

// --- Cases of test 11 that must parse ------------------------------------

TEST(Expression, RangesAfterInAndAsForValues) {
    EXPECT_EQ(parse("n IN 1..5"), "(in-range n 1 5)");
    EXPECT_EQ(parse("n IN 1 .. 2 * k"), "(in-range n 1 (* 2 k))");
    EXPECT_EQ(parse("1..inputs", ExpressionKind::FOR_VALUE), "(.. 1 inputs)");
    EXPECT_EQ(parse("0 .. n - 1", ExpressionKind::FOR_VALUE), "(.. 0 (- n 1))");
    EXPECT_EQ(parse("16#10..16#1F", ExpressionKind::FOR_VALUE), "(.. 16 31)");
    EXPECT_EQ(parse("k", ExpressionKind::FOR_VALUE), "k");
    EXPECT_EQ(parse("'meter' in options"), "(in 'meter' options)");
}

TEST(Expression, AttributesAndNamesThatLookLikeFunctions) {
    EXPECT_EQ(run_time("x.quality = GOOD"), "(= x.quality 0)");
    EXPECT_EQ(run_time("x.age > T#5s"), "(> x.age 5000)");
    EXPECT_EQ(run_time("flow.rate > 3.5"), "(> flow.rate 3.5)");
    EXPECT_EQ(run_time("level.min"), "level.min");
    EXPECT_EQ(run_time("power.avg + power.max"), "(+ power.avg power.max)");
    EXPECT_EQ(run_time("energy.sum"), "energy.sum");
    EXPECT_EQ(run_time("in.flow"), "in.flow");
    EXPECT_EQ(run_time("mod.x"), "mod.x");
    EXPECT_EQ(run_time("sum"), "sum");
    EXPECT_EQ(run_time("ps1.true"), "ps1.true");
}

TEST(Expression, Literals) {
    EXPECT_EQ(parse("0x10 + n"), "(+ 16 n)");
    EXPECT_EQ(parse("16#FF"), "255");
    EXPECT_EQ(parse("0XfF"), "255");
    EXPECT_EQ(run_time("t#5s"), "5000");
    EXPECT_EQ(run_time("T#1500ms"), "1500");
    EXPECT_EQ(run_time("T#2h"), "7200000");
    EXPECT_EQ(parse("9223372036854775807"), "9223372036854775807");
    EXPECT_EQ(parse("-9223372036854775807 - 1"),
              "(- (neg 9223372036854775807) 1)");
    EXPECT_EQ(parse("1e3 + 2.5E-1"), "(+ 1e3 2.5E-1)");
    EXPECT_EQ(run_time("TRUE OR false"), "(or 1 0)");
    EXPECT_EQ(run_time("x.quality <> Bad"), "(<> x.quality 2)");
    EXPECT_EQ(parse("'it''s'"), "'it's'");
}

TEST(Expression, PowerAndUnaryOperators) {
    EXPECT_EQ(parse("2 ** -3"), "(** 2 (neg 3))");
    EXPECT_EQ(parse("-x ** 2"), "(neg (** x 2))");
    EXPECT_EQ(parse("2 ** 3 ** 2"), "(** 2 (** 3 2))");
    EXPECT_EQ(parse("a - -b"), "(- a (neg b))");
    EXPECT_EQ(parse("a * -b"), "(* a (neg b))");
}

TEST(Expression, NotBindsTighterThanComparisons) {
    EXPECT_EQ(parse("NOT level < 5"), "(< (not level) 5)");
    EXPECT_EQ(parse("NOT x IN s"), "(in (not x) s)");
    EXPECT_EQ(parse("NOT a = b"), "(= (not a) b)");
}

TEST(Expression, Precedence) {
    EXPECT_EQ(parse("a OR b AND c"), "(or a (and b c))");
    EXPECT_EQ(parse("a XOR b OR c AND d"), "(or (xor a b) (and c d))");
    EXPECT_EQ(parse("a AND b XOR c AND d OR e"),
              "(or (xor (and a b) (and c d)) e)");
    EXPECT_EQ(parse("a + b * c - d / e"), "(- (+ a (* b c)) (/ d e))");
    EXPECT_EQ(parse("a DIV b MOD c"), "(mod (div a b) c)");
    EXPECT_EQ(parse("(a + b) * c"), "(* (+ a b) c)");
    EXPECT_EQ(parse("a < b AND c >= d"), "(and (< a b) (>= c d))");
}

// "ps1.pump-speed" is ps1.pump minus speed; stage 5 reports the unknown
// name with a hint to tag("…").
TEST(Expression, AHyphenEndsAName) {
    EXPECT_EQ(run_time("ps1.pump-speed"), "(- ps1.pump speed)");
    EXPECT_EQ(run_time("tag(\"ps1.pump-2.flow\")"), "tag(\"ps1.pump-2.flow\")");
}

TEST(Expression, CallsAndPatterns) {
    EXPECT_EQ(run_time("Sum(\"ps1.*\")"), "sum(\"ps1.*\")");
    EXPECT_EQ(run_time("sum(\"ps1.inv*.power\")"), "sum(\"ps1.inv*.power\")");
    EXPECT_EQ(run_time("avg(a, b, c)"), "avg(a b c)");
    EXPECT_EQ(parse("if(n <= 24, 1000 + 2 * (n - 1), 1400)"),
              "if((<= n 24) (+ 1000 (* 2 (- n 1))) 1400)");
    EXPECT_EQ(run_time("moving_avg(x, T#5m)"), "moving_avg(x 300000)");
    EXPECT_EQ(run_time("IF(c, 1, 0)"), "if(c 1 0)");
}

// Spaces and line ends separate tokens; a tab is not one (5.6).
TEST(Expression, SpacesAndLineEndsSeparateTokens) {
    EXPECT_EQ(run_time("a\n  AND\r\nb"), "(and a b)");
    EXPECT_EQ(run_time("a AND\tb"),
              "CF-520 at 6: a tab is not allowed in an expression; use spaces");
}

// --- Cases of test 11 that are CF-520, CF-528 and CF-529 ----------------

TEST(Expression, SyntaxErrors) {
    for (const std::string_view text : {"3x",        "5mod 3",
                                        "x = 1or y", "10ms",
                                        "T#5S",      "0x1G",
                                        "a < b < c", "NOT NOT a",
                                        "- -x",      "1 + 2..5",
                                        "1..5",      "x = 1..3",
                                        "in",        "x IN",
                                        "n IN 1..",  "1.",
                                        ".5",        "x.",
                                        "a == b",    "",
                                        "(a",        "a)",
                                        "if(1,",     "a b",
                                        "#x",        "'unterminated",
                                        "\"open",    "a.b(1)",
                                        "\"p\""}) {
        EXPECT_EQ(code_of(text), "CF-520") << text;
    }
    EXPECT_EQ(code_of("1..2..3", ExpressionKind::FOR_VALUE), "CF-520");
}

TEST(Expression, SyntaxErrorsNameTheirColumn) {
    EXPECT_EQ(parse("a < b < c"), "CF-520 at 7: comparisons do not chain");
    EXPECT_EQ(parse("3x"), R"(CF-520 at 2: "3" is followed directly by "x")");
    EXPECT_EQ(parse("NOT NOT a"),
              "CF-520 at 5: only one unary operator is allowed in a row");
    EXPECT_EQ(parse("a == b"),
              R"(CF-520 at 4: unexpected "="; equality is written with one =)");
}

TEST(Expression, ConfigurationFormsAreRefusedAtRunTime) {
    EXPECT_EQ(code_of("'a'", ExpressionKind::RUN_TIME), "CF-520");
    EXPECT_EQ(code_of("n IN 1..5", ExpressionKind::RUN_TIME), "CF-520");
    EXPECT_EQ(code_of("x IN s", ExpressionKind::RUN_TIME), "CF-520");
}

TEST(Expression, LiteralsOutOfRange) {
    for (const std::string_view text :
         {"9223372036854775808", "-9223372036854775808", "1e-400", "1e400",
          "0x8000000000000000", "T#9223372036854775807d"}) {
        EXPECT_EQ(code_of(text), "CF-528") << text;
    }
    EXPECT_EQ(parse("9223372036854775808"),
              R"(CF-528 at 1: literal "9223372036854775808" is out of range)");
}

TEST(Expression, AnUnknownFunction) {
    EXPECT_EQ(parse("foo(1)"), R"(CF-529 at 1: "foo" is not a function)");
    EXPECT_EQ(code_of("sum2(\"a\")"), "CF-529");
}

TEST(Expression, TheDepthLimit) {
    std::string at_limit;
    for (std::size_t i = 0; i + 1 < MAX_EXPRESSION_DEPTH; ++i) {
        at_limit += "(";
    }
    const std::string closed =
        at_limit + "1" + std::string(MAX_EXPRESSION_DEPTH - 1, ')');
    EXPECT_EQ(parse(closed), "1");
    EXPECT_EQ(code_of("(" + closed + ")"), "CF-526");
}

TEST(Expression, TheSizeLimitAndDeepInput) {
    EXPECT_EQ(code_of(std::string(MAX_EXPRESSION_BYTES + 1, '1')), "CF-526");
    // A very deep input is refused at the limit, without deep recursion.
    constexpr std::size_t DEEP = 100000;
    EXPECT_EQ(code_of(std::string(MAX_EXPRESSION_BYTES, '(')), "CF-526");
    EXPECT_EQ(code_of(std::string(DEEP, '-')), "CF-526");
    // A long flat chain is not deep.
    constexpr std::size_t CHAIN = 1000;
    std::string chain = "1";
    for (std::size_t i = 0; i < CHAIN; ++i) {
        chain += "+1";
    }
    EXPECT_EQ(code_of(chain), "parsed");
}

TEST(Expression, TheFunctionTable) {
    for (const std::string_view name :
         {"if",       "limit",   "min",     "max",        "abs",        "sqrt",
          "exp",      "ln",      "log10",   "sin",        "cos",        "tan",
          "asin",     "acos",    "atan",    "atan2",      "round",      "trunc",
          "floor",    "ceil",    "to_real", "to_int",     "band",       "bor",
          "bxor",     "bnot",    "shl",     "shr",        "bit",        "sum",
          "avg",      "min_of",  "max_of",  "count_good", "moving_avg", "rate",
          "totalise", "quality", "age",     "is_good",    "tag",        "IF",
          "Sum"}) {
        EXPECT_TRUE(is_expression_function(name)) << name;
    }
    EXPECT_FALSE(is_expression_function("foo"));
    EXPECT_FALSE(is_expression_function("select"));
}

// The earlier review of the grammar, point 1: it is neither left-recursive
// nor ambiguous. Each binary level is left-associative and iterative, so a
// flat chain at the size limit parses without depth; `**` is the only
// right-associative operator; a comparison does not chain; after IN, one
// token of look-ahead (`..`) tells a range from a value.
TEST(Expression, TheGrammarIsNotLeftRecursiveOrAmbiguous) {
    EXPECT_EQ(parse("a - b - c"), "(- (- a b) c)");
    EXPECT_EQ(parse("a / b / c"), "(/ (/ a b) c)");
    EXPECT_EQ(parse("a MOD b DIV c"), "(div (mod a b) c)");
    EXPECT_EQ(parse("a OR b OR c"), "(or (or a b) c)");
    EXPECT_EQ(parse("a XOR b XOR c"), "(xor (xor a b) c)");
    EXPECT_EQ(parse("a AND b AND c"), "(and (and a b) c)");
    EXPECT_EQ(parse("2 ** 3 ** 2"), "(** 2 (** 3 2))");
}

TEST(Expression, ComparisonsDoNotChainAndInLooksOneTokenAhead) {
    EXPECT_EQ(code_of("a = b = c"), "CF-520");
    EXPECT_EQ(code_of("a < b <> c"), "CF-520");
    EXPECT_EQ(code_of("n IN s IN t"), "CF-520");
    EXPECT_EQ(parse("n IN a + 1"), "(in n (+ a 1))");
    EXPECT_EQ(parse("n IN a + 1 .. b"), "(in-range n (+ a 1) b)");
}

TEST(Expression, AFlatChainAtTheSizeLimitHasNoDepth) {
    std::string chain = "a";
    while (chain.size() + 2 <= MAX_EXPRESSION_BYTES) {
        chain += "-a";
    }
    EXPECT_EQ(chain.size(), MAX_EXPRESSION_BYTES - 1);
    EXPECT_EQ(code_of(chain), "parsed");
}

// Point 2: function names are not reserved, anywhere. A one-segment name is
// a call only when `(` follows; otherwise it is a reference, in a run-time
// expression, a configuration expression and a `for` value alike.
TEST(Expression, FunctionNamesAreNotReservedAnywhere) {
    EXPECT_EQ(run_time("sum + avg"), "(+ sum avg)");
    EXPECT_EQ(run_time("if.x = tag.y"), "(= if.x tag.y)");
    EXPECT_EQ(parse("max - min"), "(- max min)");
    EXPECT_EQ(parse("n IN 1..max"), "(in-range n 1 max)");
    EXPECT_EQ(parse("1..count_good", ExpressionKind::FOR_VALUE),
              "(.. 1 count_good)");
    EXPECT_EQ(parse("round", ExpressionKind::FOR_VALUE), "round");
}

TEST(Expression, ACallNeedsAOneSegmentNameAndParenthesis) {
    EXPECT_EQ(run_time("sum (a, b)"), "sum(a b)");
    EXPECT_EQ(code_of("ps1.sum(a)"), "CF-520");
    // Reserved words are reserved only as a whole one-segment name.
    EXPECT_EQ(run_time("not.x AND ps1.or"), "(and not.x ps1.or)");
    EXPECT_EQ(code_of("in + 1"), "CF-520");
}

// A mistake a commissioning engineer makes gets the right spelling.
TEST(Expression, MistakesGetTheirHints) {
    EXPECT_EQ(run_time("inv.1.p"),
              R"(CF-520 at 4: unexpected character "."; a tag name with a )"
              R"(segment that starts with a digit or holds - is written )"
              R"(tag("..."))");
    EXPECT_EQ(run_time("in > 5"),
              R"(CF-520 at 1: "in" is a reserved word; a tag of this name is )"
              R"(written tag("in"))");
    EXPECT_EQ(parse("in > 5"),
              R"(CF-520 at 1: "in" is a reserved word; a parameter or )"
              R"(variable cannot have this name)");
    EXPECT_EQ(parse("mode = \"auto\""),
              "CF-520 at 8: a pattern string is allowed only as the argument "
              "of a function; a text is written in single quotes ('auto')");
    EXPECT_EQ(run_time("mode = \"auto\""),
              "CF-520 at 8: a pattern string is allowed only as the argument "
              "of a function; a run-time expression has no text values");
    EXPECT_EQ(run_time("a != b"),
              R"(CF-520 at 3: unexpected character "!"; "not equal" is )"
              R"(written <>)");
    EXPECT_EQ(run_time("a && b"),
              R"(CF-520 at 3: unexpected character "&"; "and" is written AND)");
    EXPECT_EQ(run_time("a || b"),
              R"(CF-520 at 3: unexpected character "|"; "or" is written OR)");
    EXPECT_EQ(run_time("a % 2"),
              R"(CF-520 at 3: unexpected character "%"; the remainder is )"
              R"(written MOD)");
    EXPECT_EQ(run_time("!a"),
              R"(CF-520 at 1: unexpected character "!"; negation is written )"
              R"(NOT)");
    EXPECT_EQ(parse("a IN (1..5)"),
              "CF-520 at 8: a range is written without parentheses: n IN "
              "1..5");
    // A character beyond ASCII is shown whole.
    EXPECT_EQ(run_time("a \xC3\xA9 b"),
              "CF-520 at 3: unexpected character \"\xC3\xA9\"");
}

// IN with a range leaves no RANGE node behind it.
TEST(Expression, InWithARangeHasNoOrphanNode) {
    ExpressionError error;
    const std::optional<Expression> tree =
        parse_expression("n IN 1..5", ExpressionKind::CONFIGURATION, error);
    ASSERT_TRUE(tree.has_value());
    const Expression found = tree.value_or(Expression{});
    EXPECT_EQ(found.nodes.size(), 4U);
    EXPECT_TRUE(std::ranges::none_of(found.nodes, [](const ExprNode& node) {
        return node.op == ExprOp::RANGE;
    }));
}

TEST(Expression, ReservedWords) {
    for (const std::string_view word :
         {"and", "OR", "Xor", "not", "mod", "div", "in", "true", "False",
          "good", "UNCERTAIN", "bad"}) {
        EXPECT_TRUE(is_reserved_word(word)) << word;
    }
    for (const std::string_view name : {"band", "inputs", "ps1.in", "", "if"}) {
        EXPECT_FALSE(is_reserved_word(name)) << name;
    }
}
}  // namespace
}  // namespace scada::conf
