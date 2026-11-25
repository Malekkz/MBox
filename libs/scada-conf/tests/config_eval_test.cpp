// The evaluation of configuration expressions (contract 5.6, 4.5 step 7;
// decision D-114): values, the canonical text of computed values, and the
// failures that are CF-410.

#include "config_eval.h"

#include "expression_types.h"
#include "scada/conf/expression.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::conf {
namespace {

// `text` evaluated with i = 7, r = 2.5, b = TRUE, s = 'meter',
// o = {meter, battery}, p = {battery, meter, meter} and q = {grid}: the
// canonical text and type of its value, or
// "CF-<code> at <column>: <message>".
std::string eval(std::string_view text,
                 ExpressionKind kind = ExpressionKind::CONFIGURATION) {
    ExpressionError syntax;
    const std::optional<Expression> tree = parse_expression(text, kind, syntax);
    if (!tree) {
        return "syntax: " + syntax.message;
    }
    constexpr std::int64_t SEVEN = 7;
    constexpr double TWO_AND_A_HALF = 2.5;
    static const std::map<std::string, ConfigValue, std::less<>> NAMES = [] {
        std::map<std::string, ConfigValue, std::less<>> values;
        values.emplace("i", ConfigValue::of_int(SEVEN));
        values.emplace("r", ConfigValue::of_real(TWO_AND_A_HALF));
        values.emplace("b", ConfigValue::of_bool(true));
        values.emplace("s", ConfigValue::of_string("meter"));
        ConfigValue set;
        set.type = ValueType::SET;
        set.set = {"meter", "battery"};
        values.emplace("o", set);
        set.set = {"battery", "meter", "meter"};
        values.emplace("p", set);
        set.set = {"grid"};
        values.emplace("q", set);
        return values;
    }();
    const Bindings bindings = [](std::string_view name) -> const ConfigValue* {
        const auto found = NAMES.find(name);
        return found == NAMES.end() ? nullptr : &found->second;
    };
    EvalError error;
    const std::optional<ConfigValue> value =
        evaluate_configuration(*tree, bindings, error);
    if (!value) {
        return "CF-" + std::to_string(error.code) + " at " +
               std::to_string(error.column) + ": " + error.message;
    }
    return canonical_text(*value) + " " +
           std::string(value_type_name(value->type));
}

TEST(ConfigEval, Arithmetic) {
    EXPECT_EQ(eval("1000 + 2 * (i - 1)"), "1012 INT");
    EXPECT_EQ(eval("i / 2"), "3.5 REAL");
    EXPECT_EQ(eval("250 / 1000"), "0.25 REAL");
    EXPECT_EQ(eval("i DIV 2"), "3 INT");
    EXPECT_EQ(eval("-7 DIV 2"), "-3 INT");
    EXPECT_EQ(eval("-7 MOD 2"), "-1 INT");
    EXPECT_EQ(eval("2 ** 3"), "8.0 REAL");
    EXPECT_EQ(eval("r * 2"), "5.0 REAL");
    EXPECT_EQ(eval("0.1 + 0.2"), "0.30000000000000004 REAL");
    EXPECT_EQ(eval("-i"), "-7 INT");
    EXPECT_EQ(eval("T#1s + T#500ms"), "1500ms TIME");
    EXPECT_EQ(eval("T#1s * 2.5"), "2500ms TIME");
    EXPECT_EQ(eval("T#5ms * 0.5"), "3ms TIME");
    EXPECT_EQ(eval("T#1s / 3"), "333ms TIME");
    EXPECT_EQ(eval("T#1s / T#4s"), "0.25 REAL");
}

TEST(ConfigEval, LogicComparisonsAndSets) {
    EXPECT_EQ(eval("'meter' IN o"), "true BOOL");
    EXPECT_EQ(eval("'grid' IN o"), "false BOOL");
    EXPECT_EQ(eval("i IN 1..i"), "true BOOL");
    EXPECT_EQ(eval("i IN 8..20"), "false BOOL");
    EXPECT_EQ(eval("s = 'meter' AND NOT (i > 7)"), "true BOOL");
    EXPECT_EQ(eval("i = 7.0"), "true BOOL");
    EXPECT_EQ(eval("b XOR TRUE"), "false BOOL");
    EXPECT_EQ(eval("s < 'z'"),
              "CF-523 at 3: expression: a comparison needs numbers or two "
              "TIMEs, not STRING and STRING");
    EXPECT_EQ(eval("1..i", ExpressionKind::FOR_VALUE), "1..7 a range");
}

TEST(ConfigEval, Functions) {
    EXPECT_EQ(eval("if(i <= 24, 1000 + 2 * (i - 1), 1400 + 2 * (i - 25))"),
              "1012 INT");
    // A REAL in either branch makes the result REAL, whichever is chosen.
    EXPECT_EQ(eval("if(b, 1, r)"), "1.0 REAL");
    // Only the chosen branch is evaluated (5.6).
    EXPECT_EQ(eval("if(b, 1, 1 DIV 0)"), "1 INT");
    EXPECT_EQ(eval("limit(0, i, 5)"), "5 INT");
    EXPECT_EQ(eval("limit(0, r, 5)"), "2.5 REAL");
    EXPECT_EQ(eval("min(i, 3, 9)"), "3 INT");
    EXPECT_EQ(eval("max(i, r)"), "7.0 REAL");
    EXPECT_EQ(eval("abs(-3)"), "3 INT");
    EXPECT_EQ(eval("abs(-r)"), "2.5 REAL");
    EXPECT_EQ(eval("round(2.5)"), "3 INT");
    EXPECT_EQ(eval("round(-2.5)"), "-3 INT");
    EXPECT_EQ(eval("trunc(-2.7)"), "-2 INT");
    EXPECT_EQ(eval("floor(-2.5)"), "-3 INT");
    EXPECT_EQ(eval("ceil(2.1)"), "3 INT");
    EXPECT_EQ(eval("to_int(r)"), "2 INT");
    EXPECT_EQ(eval("to_real(i)"), "7.0 REAL");
    EXPECT_EQ(eval("sqrt(16)"), "4.0 REAL");
    EXPECT_EQ(eval("band(i, 3)"), "3 INT");
    EXPECT_EQ(eval("bor(i, 8)"), "15 INT");
    EXPECT_EQ(eval("bxor(i, 1)"), "6 INT");
    EXPECT_EQ(eval("bnot(0)"), "-1 INT");
    EXPECT_EQ(eval("shl(1, 4)"), "16 INT");
    EXPECT_EQ(eval("shr(-1, 60)"), "15 INT");
    EXPECT_EQ(eval("bit(i, 2)"), "true BOOL");
    EXPECT_EQ(eval("bit(i, 3)"), "false BOOL");
}

TEST(ConfigEval, FailuresAreCf410) {
    EXPECT_EQ(eval("1 DIV 0"), "CF-410 at 3: expression: division by zero");
    EXPECT_EQ(eval("i MOD 0"), "CF-410 at 3: expression: division by zero");
    EXPECT_EQ(eval("i / 0"), "CF-410 at 3: expression: division by zero");
    EXPECT_EQ(eval("9223372036854775807 + 1"),
              "CF-410 at 21: expression: the result does not fit INT");
    EXPECT_EQ(eval("4611686018427387904 * 2"),
              "CF-410 at 21: expression: the result does not fit INT");
    EXPECT_EQ(eval("-9223372036854775807 - 2"),
              "CF-410 at 22: expression: the result does not fit INT");
    EXPECT_EQ(eval("sqrt(-1)"),
              "CF-410 at 1: expression: sqrt of -1 is outside its domain");
    EXPECT_EQ(eval("ln(0)"),
              "CF-410 at 1: expression: ln of 0 is outside its domain");
    EXPECT_EQ(eval("exp(1000)"),
              "CF-410 at 1: expression: the result is not a finite number");
    EXPECT_EQ(eval("10.0 ** 400"),
              "CF-410 at 6: expression: the result is not a finite number");
    EXPECT_EQ(eval("round(1.0e300)"),
              "CF-410 at 1: expression: the result does not fit INT");
    EXPECT_EQ(eval("shl(1, 64)"),
              "CF-410 at 1: expression: a shift of 64 is not 0 to 63");
    EXPECT_EQ(eval("bit(1, -1)"),
              "CF-410 at 1: expression: bit number -1 is not 0 to 63");
    // A name that is not bound here.
    EXPECT_EQ(eval("n + 1"),
              "CF-410 at 1: expression: \"n\" is not a parameter and no "
              "variable bound to this point");
}

// The edges of each operator and function (T6 of the review of #146), one
// row each: the bounds of INT, each sign of an overflowing product, the
// ends of a range, bit and shift 63, the bounds of `limit`, the order of
// `atan2`, every REAL function, and SETs compared as sets (C3).
TEST(ConfigEval, Edges) {
    const std::vector<std::pair<std::string_view, std::string_view>> rows = {
        {"-9223372036854775807 - 1", "-9223372036854775808 INT"},
        {"-9223372036854775807 - 1 - 1",
         "CF-410 at 26: expression: the result does not fit INT"},
        {"-(-9223372036854775807 - 1)",
         "CF-410 at 1: expression: the result does not fit INT"},
        {"(-9223372036854775807 - 1) DIV -1",
         "CF-410 at 28: expression: the result does not fit INT"},
        {"-4611686018427387904 * 2", "-9223372036854775808 INT"},
        {"-4611686018427387904 * 3",
         "CF-410 at 22: expression: the result does not fit INT"},
        {"3 * -4611686018427387904",
         "CF-410 at 3: expression: the result does not fit INT"},
        {"-4611686018427387904 * -2",
         "CF-410 at 22: expression: the result does not fit INT"},
        {"round(9223372036854774784.0)", "9223372036854774784 INT"},
        {"round(9223372036854775808.0)",
         "CF-410 at 1: expression: the result does not fit INT"},
        {"i >= 7", "true BOOL"},
        {"i >= 8", "false BOOL"},
        {"i < 8", "true BOOL"},
        {"i < 7", "false BOOL"},
        {"1 IN 1..5", "true BOOL"},
        {"0 IN 1..5", "false BOOL"},
        {"5 IN 1..5", "true BOOL"},
        {"6 IN 1..5", "false BOOL"},
        {"bit(-1, 63)", "true BOOL"},
        {"bit(1, 63)", "false BOOL"},
        // `shl` drops the bits it shifts out, with no error (the lead's
        // ruling on #146).
        {"shl(1, 63)", "-9223372036854775808 INT"},
        {"shl(3, 63)", "-9223372036854775808 INT"},
        {"shl(1, -1)", "CF-410 at 1: expression: a shift of -1 is not 0 to 63"},
        {"limit(5, 1, 9)", "5 INT"},
        {"limit(5, 1, 5)", "5 INT"},
        {"limit(5, 9, 5)", "5 INT"},
        {"9223372036854775807 * 1", "9223372036854775807 INT"},
        {"(-9223372036854775807 - 1) * 1", "-9223372036854775808 INT"},
        {"9223372036854775807 + (-9223372036854775807 - 1)", "-1 INT"},
        {"limit(9, 1, 5)",
         "CF-410 at 1: expression: limit needs lo at most hi"},
        {"atan2(1, 0)", "1.5707963267948966 REAL"},
        {"atan2(0, 1)", "0.0 REAL"},
        {"sqrt(2)", "1.4142135623730951 REAL"},
        {"exp(0)", "1.0 REAL"},
        {"ln(1)", "0.0 REAL"},
        {"log10(100)", "2.0 REAL"},
        {"sin(0)", "0.0 REAL"},
        {"cos(0)", "1.0 REAL"},
        {"tan(0)", "0.0 REAL"},
        {"asin(1)", "1.5707963267948966 REAL"},
        {"acos(1)", "0.0 REAL"},
        {"atan(1)", "0.7853981633974483 REAL"},
        {"o = p", "true BOOL"},
        {"o = q", "false BOOL"},
        {"o <> q", "true BOOL"},
        {"o <> p", "false BOOL"},
    };
    for (const auto& [text, expected] : rows) {
        EXPECT_EQ(eval(text), expected) << text;
    }
}

}  // namespace
}  // namespace scada::conf
