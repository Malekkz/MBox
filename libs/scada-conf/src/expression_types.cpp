#include "expression_types.h"

#include "scada/conf/diagnostic.h"
#include "scada/conf/expression.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::conf {

namespace {

constexpr std::uint16_t CF_NO_SUCH_NAME = 310;
constexpr std::uint16_t CF_TYPE_MISMATCH = 523;

bool is_number(ValueType type) noexcept {
    return type == ValueType::INT || type == ValueType::REAL;
}

// REAL if either is REAL, else INT; both must be numbers.
ValueType wider(ValueType lhs, ValueType rhs) noexcept {
    return lhs == ValueType::REAL || rhs == ValueType::REAL ? ValueType::REAL
                                                            : ValueType::INT;
}

// The functions of 5.6 that read tags, which a configuration expression has
// none of.
constexpr std::array<std::string_view, 12> TAG_FUNCTIONS = {
    "sum",  "avg",      "min_of",  "max_of", "count_good", "moving_avg",
    "rate", "totalise", "quality", "age",    "is_good",    "tag"};

// The functions of one REAL or INT argument that give REAL.
constexpr std::array<std::string_view, 10> REAL_FUNCTIONS = {
    "sqrt", "exp", "ln", "log10", "sin", "cos", "tan", "asin", "acos", "atan"};

// The functions of one REAL argument that give INT.
constexpr std::array<std::string_view, 4> ROUNDING_FUNCTIONS = {
    "round", "trunc", "floor", "ceil"};

bool listed(std::span<const std::string_view> names,
            std::string_view name) noexcept {
    return std::ranges::find(names, name) != names.end();
}

class Checker {
public:
    Checker(const Expression& tree, const NameTypes& names, TypeError& error)
        : tree_(&tree), names_(&names), error_(&error) {}

    std::optional<ValueType> run() {
        types_.resize(tree_->nodes.size());
        for (std::size_t i = 0; i < tree_->nodes.size(); ++i) {
            types_[i] = type_of(tree_->nodes[i]);
            if (!types_[i]) {
                return std::nullopt;
            }
        }
        return types_.at(tree_->root);
    }

    // The type of every node, once `run` succeeded.
    [[nodiscard]] std::vector<ValueType> types() const {
        std::vector<ValueType> out;
        out.reserve(types_.size());
        for (const std::optional<ValueType>& type : types_) {
            out.push_back(type.value_or(ValueType::INT));
        }
        return out;
    }

private:
    std::optional<ValueType> type_of(const ExprNode& node) {
        switch (node.op) {
            case ExprOp::INTEGER:
            case ExprOp::QUALITY:
                return ValueType::INT;
            case ExprOp::REAL:
                return ValueType::REAL;
            case ExprOp::TIME:
                return ValueType::TIME;
            case ExprOp::STRING:
                return ValueType::STRING;
            case ExprOp::BOOLEAN:
                return ValueType::BOOL;
            case ExprOp::REFERENCE:
                return reference(node);
            case ExprOp::PATTERN:
                // Only an argument of a function that reads tags, which the
                // call refuses.
                return ValueType::STRING;
            case ExprOp::CALL:
                return call(node);
            default:
                return operation(node);
        }
    }

    std::optional<ValueType> reference(const ExprNode& node) {
        const std::optional<ValueType> type = (*names_)(node.text);
        if (!type) {
            return fail(CF_NO_SUCH_NAME, node,
                        "parameter or variable " + quote_text(node.text) +
                            " does not exist");
        }
        return type;
    }

    [[nodiscard]] ValueType child(const ExprNode& node,
                                  std::size_t index) const {
        // Children come first in the arena, so each has its type here.
        return types_.at(node.children.at(index)).value_or(ValueType::INT);
    }

    std::optional<ValueType> operation(const ExprNode& node) {
        switch (node.op) {
            case ExprOp::NEGATE:
                return negate(node);
            case ExprOp::NOT:
                return logical(node, "NOT");
            case ExprOp::AND:
                return logical(node, "AND");
            case ExprOp::OR:
                return logical(node, "OR");
            case ExprOp::XOR:
                return logical(node, "XOR");
            case ExprOp::ADD:
            case ExprOp::SUBTRACT:
                return sum(node);
            case ExprOp::MULTIPLY:
                return product(node);
            case ExprOp::DIVIDE:
                return quotient(node);
            case ExprOp::INT_DIVIDE:
            case ExprOp::MODULO:
                return integers(node,
                                node.op == ExprOp::MODULO ? "MOD" : "DIV");
            case ExprOp::POWER:
                return numbers(node, "**", ValueType::REAL);
            case ExprOp::EQUAL:
            case ExprOp::NOT_EQUAL:
                return equality(node);
            case ExprOp::IN_VALUE:
                return in_value(node);
            case ExprOp::IN_RANGE:
                return in_range(node);
            case ExprOp::RANGE:
                return range(node);
            default:
                return order(node);
        }
    }

    std::optional<ValueType> negate(const ExprNode& node) {
        const ValueType operand = child(node, 0);
        if (is_number(operand) || operand == ValueType::TIME) {
            return operand;
        }
        return mismatch(node, "- needs INT, REAL or TIME, not " +
                                  std::string(value_type_name(operand)));
    }

    std::optional<ValueType> logical(const ExprNode& node,
                                     std::string_view name) {
        for (const std::size_t operand : node.children) {
            const ValueType type = types_.at(operand).value_or(ValueType::BOOL);
            if (type != ValueType::BOOL) {
                return mismatch(node, std::string(name) + " needs BOOL, not " +
                                          std::string(value_type_name(type)));
            }
        }
        return ValueType::BOOL;
    }

    std::optional<ValueType> sum(const ExprNode& node) {
        const ValueType lhs = child(node, 0);
        const ValueType rhs = child(node, 1);
        if (is_number(lhs) && is_number(rhs)) {
            return wider(lhs, rhs);
        }
        if (lhs == ValueType::TIME && rhs == ValueType::TIME) {
            return ValueType::TIME;
        }
        return pair_mismatch(node, node.op == ExprOp::ADD ? "+" : "-",
                             "numbers or two TIMEs");
    }

    std::optional<ValueType> product(const ExprNode& node) {
        const ValueType lhs = child(node, 0);
        const ValueType rhs = child(node, 1);
        if (is_number(lhs) && is_number(rhs)) {
            return wider(lhs, rhs);
        }
        if ((lhs == ValueType::TIME && is_number(rhs)) ||
            (rhs == ValueType::TIME && is_number(lhs))) {
            return ValueType::TIME;
        }
        return pair_mismatch(node, "*", "numbers, or a TIME and a number");
    }

    std::optional<ValueType> quotient(const ExprNode& node) {
        const ValueType lhs = child(node, 0);
        const ValueType rhs = child(node, 1);
        if (is_number(lhs) && is_number(rhs)) {
            return ValueType::REAL;
        }
        if (lhs == ValueType::TIME && rhs == ValueType::TIME) {
            return ValueType::REAL;
        }
        if (lhs == ValueType::TIME && is_number(rhs)) {
            return ValueType::TIME;
        }
        return pair_mismatch(node, "/",
                             "numbers, two TIMEs, or a TIME and a "
                             "number");
    }

    std::optional<ValueType> integers(const ExprNode& node,
                                      std::string_view name) {
        if (child(node, 0) == ValueType::INT &&
            child(node, 1) == ValueType::INT) {
            return ValueType::INT;
        }
        return pair_mismatch(node, name, "two INTs");
    }

    std::optional<ValueType> numbers(const ExprNode& node,
                                     std::string_view name, ValueType result) {
        if (is_number(child(node, 0)) && is_number(child(node, 1))) {
            return result;
        }
        return pair_mismatch(node, name, "numbers");
    }

    std::optional<ValueType> equality(const ExprNode& node) {
        const ValueType lhs = child(node, 0);
        const ValueType rhs = child(node, 1);
        if (lhs == rhs || (is_number(lhs) && is_number(rhs))) {
            // Two SETs are two values of one type (5.6); a range is not a
            // value, only a part of `IN` and of a `for` value (D-113).
            if (lhs == ValueType::RANGE) {
                return pair_mismatch(node,
                                     node.op == ExprOp::EQUAL ? "=" : "<>",
                                     "two values of one type");
            }
            return ValueType::BOOL;
        }
        return pair_mismatch(node, node.op == ExprOp::EQUAL ? "=" : "<>",
                             "two values of one type");
    }

    std::optional<ValueType> order(const ExprNode& node) {
        const ValueType lhs = child(node, 0);
        const ValueType rhs = child(node, 1);
        if ((is_number(lhs) && is_number(rhs)) ||
            (lhs == ValueType::TIME && rhs == ValueType::TIME)) {
            return ValueType::BOOL;
        }
        return pair_mismatch(node, "a comparison", "numbers or two TIMEs");
    }

    std::optional<ValueType> in_value(const ExprNode& node) {
        if (child(node, 0) == ValueType::STRING &&
            child(node, 1) == ValueType::SET) {
            return ValueType::BOOL;
        }
        return pair_mismatch(node, "IN", "a STRING and a SET");
    }

    std::optional<ValueType> in_range(const ExprNode& node) {
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            if (child(node, i) != ValueType::INT) {
                return mismatch(
                    node, "IN with a range needs INTs, not " +
                              std::string(value_type_name(child(node, i))));
            }
        }
        return ValueType::BOOL;
    }

    std::optional<ValueType> range(const ExprNode& node) {
        if (child(node, 0) == ValueType::INT &&
            child(node, 1) == ValueType::INT) {
            return ValueType::RANGE;
        }
        return pair_mismatch(node, "a range", "two INTs");
    }

    std::optional<ValueType> call(const ExprNode& node) {
        const std::string& name = node.text;
        if (listed(TAG_FUNCTIONS, name)) {
            return mismatch(node, name +
                                      " reads tags, and a configuration "
                                      "expression has none");
        }
        if (name == "if") {
            return conditional(node);
        }
        if (name == "limit" || name == "min" || name == "max") {
            return numeric_call(node, name == "limit" ? 3 : 2, name != "limit");
        }
        if (name == "abs") {
            return abs_call(node);
        }
        if (listed(REAL_FUNCTIONS, name)) {
            return typed_call(node, 1, {}, ValueType::REAL);
        }
        if (name == "atan2") {
            return typed_call(node, 2, {}, ValueType::REAL);
        }
        if (listed(ROUNDING_FUNCTIONS, name)) {
            return typed_call(node, 1, ValueType::REAL, ValueType::INT);
        }
        if (name == "to_real") {
            return typed_call(node, 1, {}, ValueType::REAL);
        }
        if (name == "to_int") {
            return typed_call(node, 1, {}, ValueType::INT);
        }
        if (name == "bit") {
            return typed_call(node, 2, ValueType::INT, ValueType::BOOL);
        }
        // band, bor, bxor, shl, shr take two INTs; bnot one.
        return typed_call(node, name == "bnot" ? 1 : 2, ValueType::INT,
                          ValueType::INT);
    }

    bool count_is(const ExprNode& node, std::size_t count, bool or_more) {
        const std::size_t given = node.children.size();
        if (given == count || (or_more && given > count)) {
            return true;
        }
        const std::string counted =
            std::to_string(count) + (or_more ? " or more" : "") +
            (count == 1 && !or_more ? " argument" : " arguments");
        fail(CF_TYPE_MISMATCH, node,
             "expression: " + node.text + " takes " + counted + ", not " +
                 std::to_string(given));
        return false;
    }

    std::optional<ValueType> conditional(const ExprNode& node) {
        if (!count_is(node, 3, false)) {
            return std::nullopt;
        }
        if (child(node, 0) != ValueType::BOOL) {
            return mismatch(node,
                            "if needs a BOOL condition, not " +
                                std::string(value_type_name(child(node, 0))));
        }
        const ValueType then = child(node, 1);
        const ValueType otherwise = child(node, 2);
        if (then == otherwise) {
            return then;
        }
        if (is_number(then) && is_number(otherwise)) {
            return ValueType::REAL;
        }
        return mismatch(node, "if needs two values of one type, not " +
                                  std::string(value_type_name(then)) + " and " +
                                  std::string(value_type_name(otherwise)));
    }

    std::optional<ValueType> numeric_call(const ExprNode& node,
                                          std::size_t count, bool or_more) {
        if (!count_is(node, count, or_more)) {
            return std::nullopt;
        }
        ValueType result = ValueType::INT;
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            if (!is_number(child(node, i))) {
                return argument_mismatch(node, i, "a number");
            }
            result = wider(result, child(node, i));
        }
        return result;
    }

    std::optional<ValueType> abs_call(const ExprNode& node) {
        if (!count_is(node, 1, false)) {
            return std::nullopt;
        }
        const ValueType operand = child(node, 0);
        if (is_number(operand) || operand == ValueType::TIME) {
            return operand;
        }
        return argument_mismatch(node, 0, "INT, REAL or TIME");
    }

    // A call of `count` arguments, each of type `argument` or, when it is
    // not given, a number.
    std::optional<ValueType> typed_call(const ExprNode& node, std::size_t count,
                                        std::optional<ValueType> argument,
                                        ValueType result) {
        if (!count_is(node, count, false)) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const ValueType given = child(node, i);
            if (argument ? given != *argument : !is_number(given)) {
                return argument_mismatch(
                    node, i,
                    argument ? std::string(value_type_name(*argument))
                             : std::string("a number"));
            }
        }
        return result;
    }

    std::optional<ValueType> argument_mismatch(const ExprNode& node,
                                               std::size_t index,
                                               const std::string& wanted) {
        return mismatch(node,
                        node.text + " needs " + wanted + " as argument " +
                            std::to_string(index + 1) + ", not " +
                            std::string(value_type_name(child(node, index))));
    }

    std::optional<ValueType> pair_mismatch(const ExprNode& node,
                                           std::string_view name,
                                           std::string_view wanted) {
        return mismatch(
            node, std::string(name) + " needs " + std::string(wanted) +
                      ", not " + std::string(value_type_name(child(node, 0))) +
                      " and " + std::string(value_type_name(child(node, 1))));
    }

    std::optional<ValueType> mismatch(const ExprNode& node,
                                      const std::string& reason) {
        return fail(CF_TYPE_MISMATCH, node, "expression: " + reason);
    }

    std::optional<ValueType> fail(std::uint16_t code, const ExprNode& node,
                                  std::string message) {
        *error_ = TypeError{
            .code = code, .column = node.column, .message = std::move(message)};
        return std::nullopt;
    }

    const Expression* tree_;
    const NameTypes* names_;
    TypeError* error_;
    std::vector<std::optional<ValueType>> types_;
};

}  // namespace

std::string_view value_type_name(ValueType type) noexcept {
    switch (type) {
        case ValueType::BOOL:
            return "BOOL";
        case ValueType::INT:
            return "INT";
        case ValueType::REAL:
            return "REAL";
        case ValueType::TIME:
            return "TIME";
        case ValueType::STRING:
            return "STRING";
        case ValueType::SET:
            return "SET";
        case ValueType::RANGE:
            return "a range";
    }
    return "?";
}

std::optional<ValueType> configuration_type(
    const Expression& tree, const NameTypes& names, TypeError& error,
    std::vector<ValueType>* node_types) {
    Checker checker(tree, names, error);
    std::optional<ValueType> type = checker.run();
    if (type && node_types != nullptr) {
        *node_types = checker.types();
    }
    return type;
}

}  // namespace scada::conf
