#include "config_eval.h"

#include "expression_types.h"
#include "scada/conf/expression.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::conf {

namespace {

constexpr std::uint16_t CF_EVALUATION = 410;

constexpr std::int64_t LARGEST_INT = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t SMALLEST_INT = std::numeric_limits<std::int64_t>::min();
constexpr int MAX_SHIFT = 63;

// Room for the shortest text of any double.
constexpr std::size_t REAL_TEXT = 32;

// True and the sum in `out` unless it overflows.
bool add(std::int64_t lhs, std::int64_t rhs, std::int64_t& out) noexcept {
    if ((rhs > 0 && lhs > LARGEST_INT - rhs) ||
        (rhs < 0 && lhs < SMALLEST_INT - rhs)) {
        return false;
    }
    out = lhs + rhs;
    return true;
}

bool subtract(std::int64_t lhs, std::int64_t rhs, std::int64_t& out) noexcept {
    if ((rhs < 0 && lhs > LARGEST_INT + rhs) ||
        (rhs > 0 && lhs < SMALLEST_INT + rhs)) {
        return false;
    }
    out = lhs - rhs;
    return true;
}

bool multiply(std::int64_t lhs, std::int64_t rhs, std::int64_t& out) noexcept {
    if (lhs == 0 || rhs == 0) {
        out = 0;
        return true;
    }
    if ((lhs == -1 && rhs == SMALLEST_INT) ||
        (rhs == -1 && lhs == SMALLEST_INT)) {
        return false;
    }
    bool overflows = false;
    if (lhs > 0) {
        overflows =
            rhs > 0 ? lhs > LARGEST_INT / rhs : rhs < SMALLEST_INT / lhs;
    } else {
        overflows =
            rhs > 0 ? lhs < SMALLEST_INT / rhs : lhs < LARGEST_INT / rhs;
    }
    if (overflows) {
        return false;
    }
    out = lhs * rhs;
    return true;
}

// A REAL as an INT when it is within INT, after `round` (half away from
// zero) or truncation.
std::optional<std::int64_t> to_int64(double value) noexcept {
    // 2^63 is exact in binary64; every double below it fits.
    constexpr double LIMIT = 9223372036854775808.0;
    if (!std::isfinite(value) || value >= LIMIT || value < -LIMIT) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(value);
}

// -1, 0 or 1 as `lhs` is below, equal to or above `rhs`.
template <typename T>
int sign(T lhs, T rhs) noexcept {
    return static_cast<int>(lhs > rhs) - static_cast<int>(lhs < rhs);
}

double as_real(const ConfigValue& value) noexcept {
    return value.type == ValueType::REAL ? value.real
                                         : static_cast<double>(value.integer);
}

// A function of one REAL argument and its domain (5.6).
struct RealFunction {
    std::string_view name;
    double (*apply)(double);
    bool (*in_domain)(double);
};

bool everywhere(double /*value*/) {
    return true;
}
bool not_negative(double value) {
    return value >= 0.0;
}
bool positive(double value) {
    return value > 0.0;
}
bool unit_interval(double value) {
    return value >= -1.0 && value <= 1.0;
}

const RealFunction* real_function(std::string_view name) {
    static const std::array<RealFunction, 10> FUNCTIONS = {{
        {"sqrt", [](double value) { return std::sqrt(value); }, not_negative},
        {"exp", [](double value) { return std::exp(value); }, everywhere},
        {"ln", [](double value) { return std::log(value); }, positive},
        {"log10", [](double value) { return std::log10(value); }, positive},
        {"sin", [](double value) { return std::sin(value); }, everywhere},
        {"cos", [](double value) { return std::cos(value); }, everywhere},
        {"tan", [](double value) { return std::tan(value); }, everywhere},
        {"asin", [](double value) { return std::asin(value); }, unit_interval},
        {"acos", [](double value) { return std::acos(value); }, unit_interval},
        {"atan", [](double value) { return std::atan(value); }, everywhere},
    }};
    for (const RealFunction& function : FUNCTIONS) {
        if (function.name == name) {
            return &function;
        }
    }
    return nullptr;
}

class Evaluator {
public:
    Evaluator(const Expression& tree, const Bindings& bindings,
              EvalError& error)
        : tree_(&tree), bindings_(&bindings), error_(&error) {}

    std::optional<ConfigValue> run() {
        const NameTypes names =
            [this](std::string_view name) -> std::optional<ValueType> {
            const ConfigValue* value = (*bindings_)(name);
            if (value == nullptr) {
                return std::nullopt;
            }
            return value->type;
        };
        TypeError type_error;
        if (!configuration_type(*tree_, names, type_error, &types_)) {
            // A name nothing binds here is the expander's CF-410: whether it
            // exists at all was checked against the chain.
            *error_ = EvalError{.code = type_error.code == CF_TYPE_MISMATCH
                                            ? CF_TYPE_MISMATCH
                                            : CF_EVALUATION,
                                .column = type_error.column,
                                .message = type_error.code == CF_TYPE_MISMATCH
                                               ? type_error.message
                                               : unbound(type_error.message)};
            return std::nullopt;
        }
        return value_of(tree_->root);
    }

private:
    static constexpr std::uint16_t CF_TYPE_MISMATCH = 523;

    static std::string unbound(const std::string& message) {
        // "parameter or variable "n" does not exist" from the type check.
        const std::size_t quote = message.find('"');
        const std::string name =
            quote == std::string::npos ? message : message.substr(quote);
        const std::size_t end = name.find(" does not exist");
        return "expression: " + name.substr(0, end) +
               " is not a parameter and no variable bound to this point";
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by the parser's depth, 32
    std::optional<ConfigValue> value_of(std::size_t index) {
        const ExprNode& node = tree_->nodes.at(index);
        switch (node.op) {
            case ExprOp::INTEGER:
            case ExprOp::QUALITY:
                return ConfigValue::of_int(node.integer);
            case ExprOp::REAL:
                return ConfigValue::of_real(node.real);
            case ExprOp::TIME: {
                ConfigValue value = ConfigValue::of_int(node.integer);
                value.type = ValueType::TIME;
                return value;
            }
            case ExprOp::STRING:
                return ConfigValue::of_string(node.text);
            case ExprOp::BOOLEAN:
                return ConfigValue::of_bool(node.integer != 0);
            case ExprOp::REFERENCE:
                return *(*bindings_)(node.text);
            case ExprOp::CALL:
                return call(index);
            default:
                return operation(node);
        }
    }

    // The values of a node's children, in order; nothing on an error.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by the parser's depth, 32
    std::optional<std::vector<ConfigValue>> operands(const ExprNode& node) {
        std::vector<ConfigValue> values;
        values.reserve(node.children.size());
        for (const std::size_t child : node.children) {
            std::optional<ConfigValue> value = value_of(child);
            if (!value) {
                return std::nullopt;
            }
            values.push_back(std::move(*value));
        }
        return values;
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by the parser's depth, 32
    std::optional<ConfigValue> operation(const ExprNode& node) {
        if (node.op == ExprOp::AND || node.op == ExprOp::OR) {
            return logical(node);
        }
        std::optional<std::vector<ConfigValue>> values = operands(node);
        if (!values) {
            return std::nullopt;
        }
        const std::vector<ConfigValue>& args = *values;
        switch (node.op) {
            case ExprOp::NEGATE:
                return negate(node, args[0]);
            case ExprOp::NOT:
                return ConfigValue::of_bool(args[0].integer == 0);
            case ExprOp::XOR:
                return ConfigValue::of_bool((args[0].integer != 0) !=
                                            (args[1].integer != 0));
            case ExprOp::ADD:
            case ExprOp::SUBTRACT:
            case ExprOp::MULTIPLY:
                return arithmetic(node, args[0], args[1]);
            case ExprOp::DIVIDE:
                return divide(node, args[0], args[1]);
            case ExprOp::INT_DIVIDE:
            case ExprOp::MODULO:
                return int_divide(node, args[0], args[1]);
            case ExprOp::POWER:
                return finite(node,
                              std::pow(as_real(args[0]), as_real(args[1])));
            case ExprOp::IN_VALUE:
                return ConfigValue::of_bool(
                    std::ranges::find(args[1].set, args[0].text) !=
                    args[1].set.end());
            case ExprOp::IN_RANGE:
                return ConfigValue::of_bool(args[0].integer >=
                                                args[1].integer &&
                                            args[0].integer <= args[2].integer);
            case ExprOp::RANGE: {
                ConfigValue range;
                range.type = ValueType::RANGE;
                range.low = args[0].integer;
                range.high = args[1].integer;
                return range;
            }
            default:
                return compare(node, args[0], args[1]);
        }
    }

    // AND and OR evaluate both sides: an expression has no side effects,
    // and every name was checked.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by the parser's depth, 32
    std::optional<ConfigValue> logical(const ExprNode& node) {
        std::optional<std::vector<ConfigValue>> values = operands(node);
        if (!values) {
            return std::nullopt;
        }
        const bool lhs = (*values)[0].integer != 0;
        const bool rhs = (*values)[1].integer != 0;
        return ConfigValue::of_bool(node.op == ExprOp::AND ? lhs && rhs
                                                           : lhs || rhs);
    }

    std::optional<ConfigValue> negate(const ExprNode& node,
                                      const ConfigValue& value) {
        if (value.type == ValueType::REAL) {
            return ConfigValue::of_real(-value.real);
        }
        if (value.integer == SMALLEST_INT) {
            return fail(node, "the result does not fit INT");
        }
        ConfigValue result = value;
        result.integer = -value.integer;
        return result;
    }

    std::optional<ConfigValue> arithmetic(const ExprNode& node,
                                          const ConfigValue& lhs,
                                          const ConfigValue& rhs) {
        const bool time =
            lhs.type == ValueType::TIME || rhs.type == ValueType::TIME;
        if (node.op == ExprOp::MULTIPLY && time &&
            (lhs.type == ValueType::REAL || rhs.type == ValueType::REAL)) {
            return scaled_time(
                node, lhs.type == ValueType::TIME ? lhs : rhs,
                lhs.type == ValueType::TIME ? as_real(rhs) : as_real(lhs),
                false);
        }
        if (lhs.type == ValueType::REAL || rhs.type == ValueType::REAL) {
            const double left = as_real(lhs);
            const double right = as_real(rhs);
            switch (node.op) {
                case ExprOp::ADD:
                    return finite(node, left + right);
                case ExprOp::SUBTRACT:
                    return finite(node, left - right);
                default:
                    return finite(node, left * right);
            }
        }
        std::int64_t out = 0;
        bool fits = false;
        switch (node.op) {
            case ExprOp::ADD:
                fits = add(lhs.integer, rhs.integer, out);
                break;
            case ExprOp::SUBTRACT:
                fits = subtract(lhs.integer, rhs.integer, out);
                break;
            default:
                fits = multiply(lhs.integer, rhs.integer, out);
                break;
        }
        if (!fits) {
            return fail(node, "the result does not fit INT");
        }
        ConfigValue result = ConfigValue::of_int(out);
        if (time) {
            result.type = ValueType::TIME;
        }
        return result;
    }

    // A TIME times or divided by a number, rounded half away from zero to
    // whole milliseconds (5.6).
    std::optional<ConfigValue> scaled_time(const ExprNode& node,
                                           const ConfigValue& time,
                                           double factor, bool divide) {
        if (divide && factor == 0.0) {
            return fail(node, "division by zero");
        }
        const double scaled = divide
                                  ? static_cast<double>(time.integer) / factor
                                  : static_cast<double>(time.integer) * factor;
        const std::optional<std::int64_t> milliseconds =
            to_int64(std::round(scaled));
        if (!milliseconds) {
            return fail(node, "the result does not fit TIME");
        }
        ConfigValue result = ConfigValue::of_int(*milliseconds);
        result.type = ValueType::TIME;
        return result;
    }

    std::optional<ConfigValue> divide(const ExprNode& node,
                                      const ConfigValue& lhs,
                                      const ConfigValue& rhs) {
        if (lhs.type == ValueType::TIME && rhs.type != ValueType::TIME) {
            return scaled_time(node, lhs, as_real(rhs), true);
        }
        const double right = as_real(rhs);
        if (right == 0.0) {
            return fail(node, "division by zero");
        }
        return finite(node, as_real(lhs) / right);
    }

    std::optional<ConfigValue> int_divide(const ExprNode& node,
                                          const ConfigValue& lhs,
                                          const ConfigValue& rhs) {
        if (rhs.integer == 0) {
            return fail(node, "division by zero");
        }
        if (lhs.integer == SMALLEST_INT && rhs.integer == -1) {
            return fail(node, "the result does not fit INT");
        }
        // C++ truncates toward zero, and the remainder has the sign of the
        // dividend, as 5.6 says.
        return ConfigValue::of_int(node.op == ExprOp::INT_DIVIDE
                                       ? lhs.integer / rhs.integer
                                       : lhs.integer % rhs.integer);
    }

    static std::optional<ConfigValue> compare(const ExprNode& node,
                                              const ConfigValue& lhs,
                                              const ConfigValue& rhs) {
        int order = 0;
        if (lhs.type == ValueType::SET) {
            // Two SETs are equal when they hold the same strings, whatever
            // their order or repeats (C3 of the review of #146); only `=`
            // and `<>` take a SET (5.6).
            const std::set<std::string, std::less<>> left(lhs.set.begin(),
                                                          lhs.set.end());
            const std::set<std::string, std::less<>> right(rhs.set.begin(),
                                                           rhs.set.end());
            order = left == right ? 0 : 1;
        } else if (lhs.type == ValueType::STRING) {
            order = lhs.text.compare(rhs.text);
        } else if (lhs.type == ValueType::REAL || rhs.type == ValueType::REAL) {
            const double left = as_real(lhs);
            const double right = as_real(rhs);
            order = sign(left, right);
        } else {
            order = sign(lhs.integer, rhs.integer);
        }
        switch (node.op) {
            case ExprOp::EQUAL:
                return ConfigValue::of_bool(order == 0);
            case ExprOp::NOT_EQUAL:
                return ConfigValue::of_bool(order != 0);
            case ExprOp::LESS:
                return ConfigValue::of_bool(order < 0);
            case ExprOp::LESS_EQUAL:
                return ConfigValue::of_bool(order <= 0);
            case ExprOp::GREATER:
                return ConfigValue::of_bool(order > 0);
            default:
                return ConfigValue::of_bool(order >= 0);
        }
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by the parser's depth, 32
    std::optional<ConfigValue> call(std::size_t index) {
        const ExprNode& node = tree_->nodes.at(index);
        if (node.text == "if") {
            const std::optional<ConfigValue> condition =
                value_of(node.children.at(0));
            if (!condition) {
                return std::nullopt;
            }
            std::optional<ConfigValue> chosen =
                value_of(node.children.at(condition->integer != 0 ? 1 : 2));
            if (!chosen) {
                return std::nullopt;
            }
            // INT and REAL branches give REAL (5.6): the type of the call.
            if (chosen->type == ValueType::INT &&
                types_.at(index) == ValueType::REAL) {
                return ConfigValue::of_real(
                    static_cast<double>(chosen->integer));
            }
            return chosen;
        }
        std::optional<std::vector<ConfigValue>> values = operands(node);
        if (!values) {
            return std::nullopt;
        }
        return function(node, *values);
    }

    std::optional<ConfigValue> function(const ExprNode& node,
                                        const std::vector<ConfigValue>& args) {
        const std::string& name = node.text;
        if (name == "limit" || name == "min" || name == "max") {
            return extreme(node, args);
        }
        if (name == "abs") {
            if (args[0].type == ValueType::REAL) {
                return ConfigValue::of_real(std::fabs(args[0].real));
            }
            return negate_if(node, args[0]);
        }
        if (name == "round" || name == "trunc" || name == "floor" ||
            name == "ceil" || name == "to_int") {
            return rounded(node, args[0]);
        }
        if (name == "to_real") {
            return ConfigValue::of_real(as_real(args[0]));
        }
        if (name == "bit") {
            if (args[1].integer < 0 || args[1].integer > MAX_SHIFT) {
                return fail(node, "bit number " +
                                      std::to_string(args[1].integer) +
                                      " is not 0 to 63");
            }
            return ConfigValue::of_bool(
                ((static_cast<std::uint64_t>(args[0].integer) >>
                  static_cast<unsigned>(args[1].integer)) &
                 1U) != 0);
        }
        if (name.front() == 'b' || name == "shl" || name == "shr") {
            return bitwise(node, args);
        }
        return transcendental(node, args);
    }

    std::optional<ConfigValue> negate_if(const ExprNode& node,
                                         const ConfigValue& value) {
        if (value.integer >= 0) {
            return value;
        }
        return negate(node, value);
    }

    std::optional<ConfigValue> extreme(const ExprNode& node,
                                       const std::vector<ConfigValue>& args) {
        const bool real = std::ranges::any_of(args, [](const ConfigValue& arg) {
            return arg.type == ValueType::REAL;
        });
        const auto less = [real](const ConfigValue& lhs,
                                 const ConfigValue& rhs) {
            return real ? as_real(lhs) < as_real(rhs)
                        : lhs.integer < rhs.integer;
        };
        ConfigValue result;
        if (node.text == "limit") {
            // limit(lo, x, hi); lo above hi is a mistake of the
            // configuration, not a silent `hi` (the lead's ruling on #146).
            if (less(args[2], args[0])) {
                return fail(node, "limit needs lo at most hi");
            }
            result = args[1];
            if (less(result, args[0])) {
                result = args[0];
            }
            if (less(args[2], result)) {
                result = args[2];
            }
        } else {
            result = args[0];
            for (const ConfigValue& arg : args) {
                if (node.text == "min" ? less(arg, result)
                                       : less(result, arg)) {
                    result = arg;
                }
            }
        }
        if (real) {
            return ConfigValue::of_real(as_real(result));
        }
        return result;
    }

    std::optional<ConfigValue> rounded(const ExprNode& node,
                                       const ConfigValue& value) {
        if (value.type == ValueType::INT) {
            return value;
        }
        const std::string& name = node.text;
        double whole = std::trunc(value.real);
        if (name == "round") {
            whole = std::round(value.real);
        } else if (name == "floor") {
            whole = std::floor(value.real);
        } else if (name == "ceil") {
            whole = std::ceil(value.real);
        }
        const std::optional<std::int64_t> integer = to_int64(whole);
        if (!integer) {
            return fail(node, "the result does not fit INT");
        }
        return ConfigValue::of_int(*integer);
    }

    std::optional<ConfigValue> bitwise(const ExprNode& node,
                                       const std::vector<ConfigValue>& args) {
        const auto bits = [](const ConfigValue& value) {
            return static_cast<std::uint64_t>(value.integer);
        };
        const std::string& name = node.text;
        std::uint64_t out = 0;
        if (name == "band") {
            out = bits(args[0]) & bits(args[1]);
        } else if (name == "bor") {
            out = bits(args[0]) | bits(args[1]);
        } else if (name == "bxor") {
            out = bits(args[0]) ^ bits(args[1]);
        } else if (name == "bnot") {
            out = ~bits(args[0]);
        } else {
            if (args[1].integer < 0 || args[1].integer > MAX_SHIFT) {
                return fail(node, "a shift of " +
                                      std::to_string(args[1].integer) +
                                      " is not 0 to 63");
            }
            const auto shift = static_cast<unsigned>(args[1].integer);
            out =
                name == "shl" ? bits(args[0]) << shift : bits(args[0]) >> shift;
        }
        return ConfigValue::of_int(static_cast<std::int64_t>(out));
    }

    std::optional<ConfigValue> transcendental(
        const ExprNode& node, const std::vector<ConfigValue>& args) {
        const std::string& name = node.text;
        const double value = as_real(args[0]);
        if (name == "atan2") {
            return finite(node, std::atan2(value, as_real(args[1])));
        }
        const RealFunction* function = real_function(name);
        if (function == nullptr) {
            return fail(node, name + " is not a function");
        }
        if (!function->in_domain(value)) {
            return fail(node, name + " of " + canonical_text(args[0]) +
                                  " is outside its domain");
        }
        return finite(node, function->apply(value));
    }

    std::optional<ConfigValue> finite(const ExprNode& node, double value) {
        if (!std::isfinite(value)) {
            return fail(node, "the result is not a finite number");
        }
        return ConfigValue::of_real(value);
    }

    std::optional<ConfigValue> fail(const ExprNode& node,
                                    const std::string& reason) {
        *error_ = EvalError{.code = CF_EVALUATION,
                            .column = node.column,
                            .message = "expression: " + reason};
        return std::nullopt;
    }

    const Expression* tree_;
    const Bindings* bindings_;
    EvalError* error_;
    // The type of every node, from the type check.
    std::vector<ValueType> types_;
};

}  // namespace

ConfigValue ConfigValue::of_bool(bool value) {
    ConfigValue result;
    result.type = ValueType::BOOL;
    result.integer = value ? 1 : 0;
    return result;
}

ConfigValue ConfigValue::of_int(std::int64_t value) {
    ConfigValue result;
    result.type = ValueType::INT;
    result.integer = value;
    return result;
}

ConfigValue ConfigValue::of_real(double value) {
    ConfigValue result;
    result.type = ValueType::REAL;
    result.real = value;
    return result;
}

ConfigValue ConfigValue::of_string(std::string value) {
    ConfigValue result;
    result.type = ValueType::STRING;
    result.text = std::move(value);
    return result;
}

std::optional<ConfigValue> evaluate_configuration(const Expression& tree,
                                                  const Bindings& bindings,
                                                  EvalError& error) {
    return Evaluator(tree, bindings, error).run();
}

std::string canonical_text(const ConfigValue& value) {
    switch (value.type) {
        case ValueType::BOOL:
            return value.integer != 0 ? "true" : "false";
        case ValueType::INT:
            return std::to_string(value.integer);
        case ValueType::TIME:
            return std::to_string(value.integer) + "ms";
        case ValueType::REAL: {
            std::array<char, REAL_TEXT> buffer{};
            const std::to_chars_result written = std::to_chars(
                buffer.data(), buffer.data() + buffer.size(), value.real);
            std::string text(buffer.data(), written.ptr);
            // A decimal keeps a point, so that it reads back as a decimal
            // (contract 1.2), not as an integer.
            if (text.find_first_of(".en") == std::string::npos) {
                text += ".0";
            }
            return text;
        }
        case ValueType::STRING:
            return value.text;
        case ValueType::SET: {
            std::string text;
            for (const std::string& item : value.set) {
                text += (text.empty() ? "" : ",") + item;
            }
            return text;
        }
        case ValueType::RANGE:
            return std::to_string(value.low) + ".." +
                   std::to_string(value.high);
    }
    return {};
}

}  // namespace scada::conf
