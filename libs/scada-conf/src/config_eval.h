#ifndef SCADA_CONF_SRC_CONFIG_EVAL_H
#define SCADA_CONF_SRC_CONFIG_EVAL_H

// The evaluation of configuration expressions during expansion
// (configuration schemas contract 4.5, step 7, and 5.6; decision D-114).

#include "expression_types.h"
#include "scada/conf/expression.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scada::conf {

// A value of a configuration expression. `integer` holds an INT, a TIME in
// milliseconds and a BOOL (0 or 1); `low` and `high` a RANGE.
struct ConfigValue {
    ValueType type = ValueType::INT;
    std::int64_t integer = 0;
    double real = 0.0;
    std::string text;
    std::vector<std::string> set;
    std::int64_t low = 0;
    std::int64_t high = 0;

    [[nodiscard]] static ConfigValue of_bool(bool value);
    [[nodiscard]] static ConfigValue of_int(std::int64_t value);
    [[nodiscard]] static ConfigValue of_real(double value);
    [[nodiscard]] static ConfigValue of_string(std::string value);
};

// The value of a name: a parameter or a variable bound to the point, or
// nothing.
using Bindings = std::function<const ConfigValue*(std::string_view)>;

// Why an evaluation failed: CF-410 (overflow, division by zero, a domain, a
// result that is not finite, a name not bound here) or CF-523 (a type that
// does not fit), at a byte column of the text.
struct EvalError {
    std::uint16_t code = 0;
    std::uint32_t column = 1;
    std::string message;
};

// The value of `tree`. Only the chosen branch of `if` is evaluated (5.6).
[[nodiscard]] std::optional<ConfigValue> evaluate_configuration(
    const Expression& tree, const Bindings& bindings, EvalError& error);

// The canonical text of a computed value (13.3): an INT in decimal, a REAL
// as the shortest decimal that reads back to the same binary64, a BOOL as
// `true` or `false`, a STRING as it is, a TIME as its milliseconds and
// `ms`.
[[nodiscard]] std::string canonical_text(const ConfigValue& value);

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_CONFIG_EVAL_H
