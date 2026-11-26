// The types of expressions: each row of the operator and function tables of
// contract 5.6, and the refusals, of configuration expressions (D-113) and
// of run-time expressions over tags (D-117).

#include "expression_types.h"

#include "scada/conf/expression.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::conf {
namespace {

// The type of `text`, a configuration expression over the parameters `i`
// (INT), `r` (REAL), `b` (BOOL), `s` (STRING) and `o` (SET); or its error
// as "CF-<code> at <column>: <message>".
std::string type_of(std::string_view text,
                    ExpressionKind kind = ExpressionKind::CONFIGURATION) {
    ExpressionError syntax;
    const std::optional<Expression> tree = parse_expression(text, kind, syntax);
    if (!tree) {
        return "syntax: " + syntax.message;
    }
    const NameTypes names =
        [](std::string_view name) -> std::optional<ValueType> {
        if (name == "i") {
            return ValueType::INT;
        }
        if (name == "r") {
            return ValueType::REAL;
        }
        if (name == "b") {
            return ValueType::BOOL;
        }
        if (name == "s") {
            return ValueType::STRING;
        }
        if (name == "o") {
            return ValueType::SET;
        }
        return std::nullopt;
    };
    TypeError error;
    const std::optional<ValueType> type =
        configuration_type(*tree, names, error);
    if (!type) {
        return "CF-" + std::to_string(error.code) + " at " +
               std::to_string(error.column) + ": " + error.message;
    }
    return std::string(value_type_name(*type));
}

TEST(ExpressionTypes, Literals) {
    EXPECT_EQ(type_of("1"), "INT");
    EXPECT_EQ(type_of("1.5"), "REAL");
    EXPECT_EQ(type_of("T#5s"), "TIME");
    EXPECT_EQ(type_of("'a'"), "STRING");
    EXPECT_EQ(type_of("TRUE"), "BOOL");
    EXPECT_EQ(type_of("BAD"), "INT");
}

TEST(ExpressionTypes, Arithmetic) {
    EXPECT_EQ(type_of("i + i"), "INT");
    EXPECT_EQ(type_of("i - r"), "REAL");
    EXPECT_EQ(type_of("T#1s + T#2s"), "TIME");
    EXPECT_EQ(type_of("i * i"), "INT");
    EXPECT_EQ(type_of("i * r"), "REAL");
    EXPECT_EQ(type_of("T#1s * 2"), "TIME");
    EXPECT_EQ(type_of("2.5 * T#1s"), "TIME");
    EXPECT_EQ(type_of("i / i"), "REAL");
    EXPECT_EQ(type_of("T#1s / T#2s"), "REAL");
    EXPECT_EQ(type_of("T#1s / 2"), "TIME");
    EXPECT_EQ(type_of("i DIV i"), "INT");
    EXPECT_EQ(type_of("i MOD i"), "INT");
    EXPECT_EQ(type_of("i ** i"), "REAL");
    EXPECT_EQ(type_of("-i"), "INT");
    EXPECT_EQ(type_of("-T#1s"), "TIME");
    EXPECT_EQ(type_of("i + b"),
              "CF-523 at 3: expression: + needs numbers or two TIMEs, not "
              "INT and BOOL");
    EXPECT_EQ(type_of("T#1s + 1"),
              "CF-523 at 6: expression: + needs numbers or two TIMEs, not "
              "TIME and INT");
    EXPECT_EQ(type_of("2 / T#1s"),
              "CF-523 at 3: expression: / needs numbers, two TIMEs, or a "
              "TIME and a number, not INT and TIME");
    EXPECT_EQ(type_of("r MOD i"),
              "CF-523 at 3: expression: MOD needs two INTs, not REAL and INT");
    EXPECT_EQ(type_of("s ** 2"),
              "CF-523 at 3: expression: ** needs numbers, not STRING and INT");
    EXPECT_EQ(type_of("-b"),
              "CF-523 at 1: expression: - needs INT, REAL or TIME, not BOOL");
}

TEST(ExpressionTypes, ComparisonsAndLogic) {
    EXPECT_EQ(type_of("i = r"), "BOOL");
    EXPECT_EQ(type_of("s <> 'a'"), "BOOL");
    EXPECT_EQ(type_of("b = TRUE"), "BOOL");
    EXPECT_EQ(type_of("i < r"), "BOOL");
    EXPECT_EQ(type_of("T#1s >= T#2s"), "BOOL");
    EXPECT_EQ(type_of("b AND NOT b OR b XOR b"), "BOOL");
    EXPECT_EQ(type_of("s = i"),
              "CF-523 at 3: expression: = needs two values of one type, not "
              "STRING and INT");
    // Two SETs are two values of one type (5.6; S1 of the review of #144).
    EXPECT_EQ(type_of("o = o"), "BOOL");
    EXPECT_EQ(type_of("o <> o"), "BOOL");
    EXPECT_EQ(type_of("s < 'b'"),
              "CF-523 at 3: expression: a comparison needs numbers or two "
              "TIMEs, not STRING and STRING");
    // BOOL never mixes with a number (5.6): NOT on an INT is refused.
    EXPECT_EQ(type_of("NOT i"),
              "CF-523 at 1: expression: NOT needs BOOL, not INT");
    EXPECT_EQ(type_of("b AND i"),
              "CF-523 at 3: expression: AND needs BOOL, not INT");
}

TEST(ExpressionTypes, InAndRanges) {
    EXPECT_EQ(type_of("s IN o"), "BOOL");
    EXPECT_EQ(type_of("'meter' IN o"), "BOOL");
    EXPECT_EQ(type_of("i IN 1..i + 2"), "BOOL");
    EXPECT_EQ(type_of("1..i", ExpressionKind::FOR_VALUE), "a range");
    EXPECT_EQ(type_of("i * 2", ExpressionKind::FOR_VALUE), "INT");
    EXPECT_EQ(type_of("i IN o"),
              "CF-523 at 3: expression: IN needs a STRING and a SET, not INT "
              "and SET");
    EXPECT_EQ(type_of("i IN 1..r"),
              "CF-523 at 3: expression: IN with a range needs INTs, not REAL");
    EXPECT_EQ(type_of("1..r", ExpressionKind::FOR_VALUE),
              "CF-523 at 1: expression: a range needs two INTs, not INT and "
              "REAL");
}

// The pairs of operand types that each of `*`, `/`, the comparisons and
// equality refuses, one per side (the mutation run on #144's code).
TEST(ExpressionTypes, RefusedPairs) {
    EXPECT_EQ(type_of("T#1s * T#2s"),
              "CF-523 at 6: expression: * needs numbers, or a TIME and a "
              "number, not TIME and TIME");
    EXPECT_EQ(type_of("2 * s"),
              "CF-523 at 3: expression: * needs numbers, or a TIME and a "
              "number, not INT and STRING");
    EXPECT_EQ(type_of("s * 2"),
              "CF-523 at 3: expression: * needs numbers, or a TIME and a "
              "number, not STRING and INT");
    EXPECT_EQ(type_of("s / 2"),
              "CF-523 at 3: expression: / needs numbers, two TIMEs, or a TIME "
              "and a number, not STRING and INT");
    EXPECT_EQ(type_of("2 / T#1s"),
              "CF-523 at 3: expression: / needs numbers, two TIMEs, or a TIME "
              "and a number, not INT and TIME");
    EXPECT_EQ(type_of("i < s"),
              "CF-523 at 3: expression: a comparison needs numbers or two "
              "TIMEs, not INT and STRING");
    EXPECT_EQ(type_of("s < i"),
              "CF-523 at 3: expression: a comparison needs numbers or two "
              "TIMEs, not STRING and INT");
    EXPECT_EQ(type_of("T#1s < 1"),
              "CF-523 at 6: expression: a comparison needs numbers or two "
              "TIMEs, not TIME and INT");
    EXPECT_EQ(type_of("1 < T#1s"),
              "CF-523 at 3: expression: a comparison needs numbers or two "
              "TIMEs, not INT and TIME");
    EXPECT_EQ(type_of("i = s"),
              "CF-523 at 3: expression: = needs two values of one type, not "
              "INT and STRING");
    EXPECT_EQ(type_of("i <> s"),
              "CF-523 at 3: expression: <> needs two values of one type, not "
              "INT and STRING");
}

// The gaps of the review of #144: one INT is a `for` value as a range is
// (5.6); a STRING on the left of IN with a range is refused, as IN with a
// range compares INTs.
TEST(ExpressionTypes, ForValuesAndInOfAString) {
    EXPECT_EQ(type_of("i", ExpressionKind::FOR_VALUE), "INT");
    EXPECT_EQ(type_of("3", ExpressionKind::FOR_VALUE), "INT");
    EXPECT_EQ(type_of("s IN 1..5"),
              "CF-523 at 3: expression: IN with a range needs INTs, not "
              "STRING");
    EXPECT_EQ(type_of("i IN 1..5"), "BOOL");
}

TEST(ExpressionTypes, Functions) {
    EXPECT_EQ(type_of("if(b, i, i)"), "INT");
    EXPECT_EQ(type_of("if(b, i, r)"), "REAL");
    EXPECT_EQ(type_of("if(b, s, 'x')"), "STRING");
    EXPECT_EQ(type_of("limit(0, i, 10)"), "INT");
    EXPECT_EQ(type_of("limit(0, r, 10)"), "REAL");
    EXPECT_EQ(type_of("min(i, i, i)"), "INT");
    EXPECT_EQ(type_of("max(i, r)"), "REAL");
    EXPECT_EQ(type_of("abs(i)"), "INT");
    EXPECT_EQ(type_of("abs(T#1s)"), "TIME");
    EXPECT_EQ(type_of("sqrt(i)"), "REAL");
    EXPECT_EQ(type_of("atan2(r, i)"), "REAL");
    EXPECT_EQ(type_of("round(r)"), "INT");
    EXPECT_EQ(type_of("to_real(i)"), "REAL");
    EXPECT_EQ(type_of("to_int(r)"), "INT");
    EXPECT_EQ(type_of("band(i, 3)"), "INT");
    EXPECT_EQ(type_of("bnot(i)"), "INT");
    EXPECT_EQ(type_of("bit(i, 3)"), "BOOL");
    EXPECT_EQ(type_of("if(i, 1, 2)"),
              "CF-523 at 1: expression: if needs a BOOL condition, not INT");
    EXPECT_EQ(type_of("if(b, 1, 'x')"),
              "CF-523 at 1: expression: if needs two values of one type, not "
              "INT and STRING");
    EXPECT_EQ(type_of("if(b, 1)"),
              "CF-523 at 1: expression: if takes 3 arguments, not 2");
    EXPECT_EQ(type_of("limit(0, i, 10, 2)"),
              "CF-523 at 1: expression: limit takes 3 arguments, not 4");
    EXPECT_EQ(type_of("limit(0, i)"),
              "CF-523 at 1: expression: limit takes 3 arguments, not 2");
    EXPECT_EQ(type_of("max(i)"),
              "CF-523 at 1: expression: max takes 2 or more arguments, not 1");
    EXPECT_EQ(type_of("sqrt(i, i)"),
              "CF-523 at 1: expression: sqrt takes 1 argument, not 2");
    // The rounding functions take REAL (the function table).
    EXPECT_EQ(type_of("round(i)"),
              "CF-523 at 1: expression: round needs REAL as argument 1, not "
              "INT");
    EXPECT_EQ(type_of("shl(i, r)"),
              "CF-523 at 1: expression: shl needs INT as argument 2, not "
              "REAL");
    EXPECT_EQ(type_of("abs(b)"),
              "CF-523 at 1: expression: abs needs INT, REAL or TIME as "
              "argument 1, not BOOL");
}

TEST(ExpressionTypes, NamesAndTags) {
    EXPECT_EQ(type_of("i + k"),
              "CF-310 at 5: parameter or variable \"k\" does not exist");
    for (const std::string_view call :
         {"sum(\"a.*\")", "avg(\"a.*\")", "min_of(\"a.*\")", "max_of(\"a.*\")",
          "count_good(\"a.*\")", "moving_avg(\"a\", T#1m)", "rate(\"a\")",
          "totalise(\"a\")", "quality(\"a\")", "age(\"a\")", "is_good(\"a\")",
          "tag(\"a\")"}) {
        const std::string name(call.substr(0, call.find('(')));
        EXPECT_EQ(type_of(call), "CF-523 at 1: expression: " + name +
                                     " reads tags, and a configuration "
                                     "expression has none")
            << call;
    }
}

// The type of `text`, a run-time expression over the tags `a.b` (BOOL),
// `a.i` (INT), `a.r` (REAL), `a.s` (STRING), `v.x` (a virtual tag of unknown
// type), `q` and `q.quality` (two INT tags), `inv1.power` (REAL) and
// `inv2.power` (INT), `f1` and `f2` (BOOL), in task `normal` of 100 ms; or
// its error as "CF-<code> at <column>: <message>" ("silent" for an error of
// code 0).
std::string run_time_of(std::string_view text) {
    ExpressionError syntax;
    const std::optional<Expression> tree =
        parse_expression(text, ExpressionKind::RUN_TIME, syntax);
    if (!tree) {
        return "syntax: " + syntax.message;
    }
    const std::vector<std::pair<std::string_view, TagRead>> tags = {
        {"a.b", TagRead::BOOL},       {"a.i", TagRead::INT},
        {"a.r", TagRead::REAL},       {"a.s", TagRead::STRING},
        {"v.x", TagRead::UNKNOWN},    {"q", TagRead::INT},
        {"q.quality", TagRead::INT},  {"inv1.power", TagRead::REAL},
        {"inv2.power", TagRead::INT}, {"f1", TagRead::BOOL},
        {"f2", TagRead::BOOL}};
    RunTimeContext context;
    context.tag = [&tags](std::string_view name) -> std::optional<TagRead> {
        for (const auto& [tag, read] : tags) {
            if (tag == name) {
                return read;
            }
        }
        return std::nullopt;
    };
    context.matching = [&tags](std::string_view pattern) {
        std::vector<TagRead> found;
        // The few patterns of these cases, by their text.
        for (const auto& [tag, read] : tags) {
            const bool hit =
                (pattern == "inv*.power" && tag.starts_with("inv")) ||
                (pattern == "f?" && tag.starts_with("f")) ||
                (pattern == "a.*" && tag.starts_with("a.")) ||
                (pattern == "v.*" && tag.starts_with("v."));
            if (hit) {
                found.push_back(read);
            }
        }
        return found;
    };
    context.task = "normal";
    context.period_ms = 100;
    TypeError error;
    const std::optional<ValueType> type = run_time_type(*tree, context, error);
    if (!type) {
        if (error.code == 0) {
            return "silent";
        }
        return "CF-" + std::to_string(error.code) + " at " +
               std::to_string(error.column) + ": " + error.message;
    }
    return std::string(value_type_name(*type));
}

// A tag reads as its type, its `.quality` as INT and its `.age` as TIME
// (5.6); a name that is a tag and an attribute is CF-521.
TEST(RunTimeTypes, TagsAndAttributes) {
    EXPECT_EQ(run_time_of("a.b AND f1"), "BOOL");
    EXPECT_EQ(run_time_of("a.i + 1"), "INT");
    EXPECT_EQ(run_time_of("a.r * 2"), "REAL");
    EXPECT_EQ(run_time_of("a.r.quality = GOOD"), "BOOL");
    EXPECT_EQ(run_time_of("a.r.quality"), "INT");
    EXPECT_EQ(run_time_of("a.r.age"), "TIME");
    EXPECT_EQ(run_time_of("a.r.age > T#5s"), "BOOL");
    EXPECT_EQ(run_time_of("q.quality"),
              "CF-521 at 1: \"q.quality\" is both a tag and an attribute; "
              "use quality() or age()");
    EXPECT_EQ(run_time_of("quality(q)"), "INT");
    EXPECT_EQ(run_time_of("a.nothing > 1"),
              "CF-310 at 1: tag \"a.nothing\" does not exist");
    EXPECT_EQ(run_time_of("a.nothing.quality"),
              "CF-310 at 1: tag \"a.nothing.quality\" does not exist");
    EXPECT_EQ(run_time_of("a.s = 1"),
              "CF-522 at 1: string tag \"a.s\" cannot be used in an "
              "expression");
    EXPECT_EQ(run_time_of("a.s.quality"),
              "CF-522 at 1: string tag \"a.s\" cannot be used in an "
              "expression");
    EXPECT_EQ(run_time_of("v.x + 1"), "silent");
    EXPECT_EQ(run_time_of("v.x.quality"), "INT");
    EXPECT_EQ(run_time_of("tag(\"a.i\") + 1"), "INT");
    EXPECT_EQ(run_time_of("tag(\"a.none\")"),
              "CF-310 at 1: tag \"a.none\" does not exist");
    EXPECT_EQ(run_time_of("tag(\"a.*\")"),
              "CF-523 at 1: expression: tag names one tag, not the pattern "
              "\"a.*\"");
    EXPECT_EQ(run_time_of("NOT a.i"),
              "CF-523 at 1: expression: NOT needs BOOL, not INT");
}

// The functions that read tags (5.6).
TEST(RunTimeTypes, FunctionsOfTags) {
    EXPECT_EQ(run_time_of("quality(a.r)"), "INT");
    EXPECT_EQ(run_time_of("age(tag(\"a.r\"))"), "TIME");
    EXPECT_EQ(run_time_of("is_good(a.b)"), "BOOL");
    EXPECT_EQ(run_time_of("is_good(1)"),
              "CF-523 at 1: expression: is_good needs a tag reference");
    EXPECT_EQ(run_time_of("is_good(a.r.quality)"),
              "CF-523 at 1: expression: is_good needs a tag reference");
    EXPECT_EQ(run_time_of("sum(\"inv*.power\")"), "REAL");
    EXPECT_EQ(run_time_of("sum(a.i, inv2.power)"), "INT");
    EXPECT_EQ(run_time_of("min_of(a.i, a.r)"), "REAL");
    EXPECT_EQ(run_time_of("avg(a.i, inv2.power)"), "REAL");
    EXPECT_EQ(run_time_of("count_good(\"f?\")"), "INT");
    EXPECT_EQ(run_time_of("sum(\"f?\")"),
              "CF-523 at 1: expression: sum needs numbers, not BOOL");
    EXPECT_EQ(run_time_of("max_of(\"none.*\")"),
              "CF-311 at 8: pattern \"none.*\" matches no tag");
    EXPECT_EQ(run_time_of("count_good(\"a.*\")"),
              "CF-522 at 12: pattern \"a.*\" matches a string tag, which "
              "cannot be used in an expression");
    EXPECT_EQ(run_time_of("avg(\"v.*\")"), "silent");
    EXPECT_EQ(run_time_of("sum(a.i)"),
              "CF-523 at 1: expression: sum takes one pattern or two or more "
              "tag references");
    EXPECT_EQ(run_time_of("sum(a.i, 1)"),
              "CF-523 at 1: expression: sum takes one pattern or two or more "
              "tag references");
}

// A time function's window is 1 to 10,000 cycles of its task (CF-524).
TEST(RunTimeTypes, TheWindowOfATimeFunction) {
    EXPECT_EQ(run_time_of("moving_avg(a.r, T#5m)"), "REAL");
    EXPECT_EQ(run_time_of("rate(a.i, T#100ms)"), "REAL");
    EXPECT_EQ(run_time_of("totalise(a.r, T#1000s)"), "REAL");
    EXPECT_EQ(run_time_of("rate(a.r, T#99ms)"),
              "CF-524 at 11: rate: the window must be 1 to 10000 cycles of "
              "task \"normal\"");
    EXPECT_EQ(run_time_of("rate(a.r, T#1000001ms)"),
              "CF-524 at 11: rate: the window must be 1 to 10000 cycles of "
              "task \"normal\"");
    EXPECT_EQ(run_time_of("rate(a.b, T#1s)"),
              "CF-523 at 1: expression: rate needs a tag reference that is a "
              "number as argument 1");
    EXPECT_EQ(run_time_of("rate(a.r + 1, T#1s)"),
              "CF-523 at 1: expression: rate needs a tag reference that is a "
              "number as argument 1");
    EXPECT_EQ(run_time_of("rate(a.r, a.i)"),
              "CF-523 at 1: expression: rate needs a TIME literal as "
              "argument 2");
}

}  // namespace
}  // namespace scada::conf
