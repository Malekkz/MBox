#include "expression_types.h"

#include "name_pattern.h"

#include "scada/conf/diagnostic.h"
#include "scada/conf/expression.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::conf {

namespace {

constexpr std::uint16_t CF_NO_SUCH_NAME = 310;
constexpr std::uint16_t CF_NO_MATCH = 311;
constexpr std::uint16_t CF_AMBIGUOUS = 521;
constexpr std::uint16_t CF_STRING_TAG = 522;
// The largest shift count of `shl` and `shr` (5.6).
constexpr std::int64_t MAX_SHIFT = 63;
constexpr std::uint16_t CF_TYPE_MISMATCH = 523;
constexpr std::uint16_t CF_WINDOW = 524;

// The window of a time function, in task cycles (5.6).
constexpr std::int64_t MAX_WINDOW_CYCLES = 10'000;

// The aggregates of 5.6: one pattern, or two or more tag references.
constexpr std::array<std::string_view, 5> AGGREGATES = {"sum", "avg", "min_of",
                                                        "max_of", "count_good"};

// The time functions of 5.6: a tag reference and a TIME literal.
constexpr std::array<std::string_view, 3> TIME_FUNCTIONS = {"moving_avg",
                                                            "rate", "totalise"};

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

    Checker(const Expression& tree, const RunTimeContext& run_time,
            TypeError& error)
        : tree_(&tree), run_time_(&run_time), error_(&error) {}

    std::optional<ValueType> run() {
        types_.resize(tree_->nodes.size());
        tag_reference_.assign(tree_->nodes.size(), false);
        for (std::size_t i = 0; i < tree_->nodes.size(); ++i) {
            current_ = i;
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
        if (run_time_ != nullptr) {
            return tag_reference(node);
        }
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

    // --- Run-time expressions: the tags they read (D-117) -----------------

    // A reference (5.6): a tag, or a tag followed by `.quality` or `.age`.
    std::optional<ValueType> tag_reference(const ExprNode& node) {
        const std::string& name = node.text;
        std::optional<ValueType> attribute;
        std::string_view base;
        for (const auto& [suffix, type] :
             {std::pair{std::string_view(".quality"), ValueType::INT},
              std::pair{std::string_view(".age"), ValueType::TIME}}) {
            if (name.size() > suffix.size() && name.ends_with(suffix)) {
                attribute = type;
                base = std::string_view(name).substr(
                    0, name.size() - suffix.size());
            }
        }
        const std::optional<TagRead> read = run_time_->tag(name);
        const std::optional<TagRead> base_read =
            attribute ? run_time_->tag(base) : std::nullopt;
        if (read && base_read) {
            return fail(CF_AMBIGUOUS, node,
                        quote_text(name) +
                            " is both a tag and an attribute; use quality() "
                            "or age()");
        }
        if (read) {
            tag_reference_[current_] = true;
            return read_as(*read, name, node);
        }
        if (base_read) {
            if (*base_read == TagRead::STRING) {
                return string_tag(base, node);
            }
            return attribute;
        }
        return fail(CF_NO_SUCH_NAME, node,
                    "tag " + quote_text(name) + " does not exist");
    }

    // The type a tag is read as (5.6): a UINT64 tag reads as INT.
    std::optional<ValueType> read_as(TagRead read, std::string_view name,
                                     const ExprNode& node) {
        switch (read) {
            case TagRead::BOOL:
                return ValueType::BOOL;
            case TagRead::INT:
                return ValueType::INT;
            case TagRead::REAL:
                return ValueType::REAL;
            case TagRead::STRING:
                return string_tag(name, node);
            case TagRead::UNKNOWN:
                break;
        }
        // Its own expression is reported where it stands.
        return fail(0, node, {});
    }

    std::optional<ValueType> string_tag(std::string_view name,
                                        const ExprNode& node) {
        return fail(CF_STRING_TAG, node,
                    "string tag " + quote_text(name) +
                        " cannot be used in an expression");
    }

    [[nodiscard]] bool is_tag(std::size_t child_index,
                              const ExprNode& node) const {
        return tag_reference_.at(node.children.at(child_index));
    }

    // `tag("name")`: one tag, named in full.
    std::optional<ValueType> named_tag(const ExprNode& node) {
        if (node.children.size() != 1 ||
            tree_->nodes.at(node.children[0]).op != ExprOp::PATTERN) {
            return mismatch(node, "tag takes one tag name in double quotes");
        }
        const std::string& name = tree_->nodes.at(node.children[0]).text;
        if (is_name_pattern(name)) {
            return mismatch(
                node, "tag names one tag, not the pattern " + quote_text(name));
        }
        const std::optional<TagRead> read = run_time_->tag(name);
        if (!read) {
            return fail(CF_NO_SUCH_NAME, node,
                        "tag " + quote_text(name) + " does not exist");
        }
        tag_reference_[current_] = true;
        return read_as(*read, name, node);
    }

    // `quality(x)`, `age(x)`, `is_good(x)`: one tag reference.
    std::optional<ValueType> tag_attribute(const ExprNode& node,
                                           ValueType result) {
        if (!count_is(node, 1, false)) {
            return std::nullopt;
        }
        if (!is_tag(0, node)) {
            return mismatch(node, node.text + " needs a tag reference");
        }
        return result;
    }

    // `moving_avg`, `rate`, `totalise`: a tag reference that is a number
    // and a TIME literal of 1 to 10,000 cycles of the task (CF-524).
    std::optional<ValueType> time_function(const ExprNode& node) {
        if (!count_is(node, 2, false)) {
            return std::nullopt;
        }
        if (!is_tag(0, node) || !is_number(child(node, 0))) {
            return mismatch(node, node.text +
                                      " needs a tag reference that is a "
                                      "number as argument 1");
        }
        const ExprNode& window = tree_->nodes.at(node.children[1]);
        if (window.op != ExprOp::TIME) {
            return mismatch(node,
                            node.text + " needs a TIME literal as argument 2");
        }
        const std::int64_t period = run_time_->period_ms;
        if (!run_time_->task.empty() && period > 0 &&
            (window.integer < period ||
             window.integer / MAX_WINDOW_CYCLES > period ||
             (window.integer / MAX_WINDOW_CYCLES == period &&
              window.integer % MAX_WINDOW_CYCLES != 0))) {
            return fail(CF_WINDOW, window,
                        node.text +
                            ": the window must be 1 to 10000 cycles "
                            "of task " +
                            quote_text(run_time_->task));
        }
        return ValueType::REAL;
    }

    // An aggregate over one pattern: what the tags it matches are read as
    // (PatternReads), the same work whatever their number (D-117).
    std::optional<ValueType> pattern_aggregate(const ExprNode& node,
                                               const ExprNode& pattern) {
        const PatternReads reads = run_time_->matching(pattern.text);
        if (reads.count == 0) {
            return fail(
                CF_NO_MATCH, pattern,
                "pattern " + quote_text(pattern.text) + " matches no tag");
        }
        if (!reads.first_odd.empty()) {
            // A tag not typed yet is reported where it stands.
            return reads.odd == TagRead::STRING
                       ? string_tag(reads.first_odd, pattern)
                       : fail(0, pattern, {});
        }
        if (node.text == "count_good") {
            return ValueType::INT;
        }
        if (!reads.first_bool.empty()) {
            return mismatch(node, node.text + " needs numbers, not BOOL: tag " +
                                      quote_text(reads.first_bool) +
                                      " of pattern " +
                                      quote_text(pattern.text));
        }
        if (node.text == "avg" || reads.any_real) {
            return ValueType::REAL;
        }
        return ValueType::INT;
    }

    // `sum`, `avg`, `min_of`, `max_of`, `count_good`: one pattern, resolved
    // now (2.5), or two or more tag references.
    std::optional<ValueType> aggregate(const ExprNode& node) {
        if (node.children.size() == 1 &&
            tree_->nodes.at(node.children[0]).op == ExprOp::PATTERN) {
            return pattern_aggregate(node, tree_->nodes.at(node.children[0]));
        }
        if (node.children.size() < 2) {
            return mismatch(node, node.text +
                                      " takes one pattern or two or more "
                                      "tag references");
        }
        std::vector<ValueType> inputs;
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            if (!is_tag(i, node)) {
                return mismatch(node, node.text +
                                          " takes one pattern or two or more "
                                          "tag references");
            }
            inputs.push_back(child(node, i));
        }
        if (node.text == "count_good") {
            return ValueType::INT;
        }
        ValueType result = ValueType::INT;
        for (const ValueType input : inputs) {
            if (!is_number(input)) {
                return mismatch(node, node.text + " needs numbers, not " +
                                          std::string(value_type_name(input)));
            }
            result = wider(result, input);
        }
        return node.text == "avg" ? ValueType::REAL : result;
    }

    // A function that reads tags, in a run-time expression.
    std::optional<ValueType> tag_function(const ExprNode& node) {
        const std::string& name = node.text;
        if (name == "tag") {
            return named_tag(node);
        }
        if (name == "quality") {
            return tag_attribute(node, ValueType::INT);
        }
        if (name == "age") {
            return tag_attribute(node, ValueType::TIME);
        }
        if (name == "is_good") {
            return tag_attribute(node, ValueType::BOOL);
        }
        if (listed(TIME_FUNCTIONS, name)) {
            return time_function(node);
        }
        if (listed(AGGREGATES, name)) {
            return aggregate(node);
        }
        return mismatch(node, name + " is not a function of tags");
    }

    std::optional<ValueType> call(const ExprNode& node) {
        const std::string& name = node.text;
        if (listed(TAG_FUNCTIONS, name) && run_time_ != nullptr) {
            return tag_function(node);
        }
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
        // band, bor, bxor, shl, shr take two INTs; bnot one. A shift count
        // written as a number is 0 to 63 (5.6); a configuration expression
        // evaluates it, and gives CF-410 there.
        if (run_time_ != nullptr && (name == "shl" || name == "shr") &&
            node.children.size() == 2) {
            const ExprNode& count = tree_->nodes.at(node.children[1]);
            const std::optional<std::int64_t> constant = constant_count(count);
            if (constant && (*constant < 0 || *constant > MAX_SHIFT)) {
                return fail(
                    CF_TYPE_MISMATCH, count,
                    "expression: " + name + ": the count must be from 0 to 63");
            }
        }
        return typed_call(node, name == "bnot" ? 1 : 2, ValueType::INT,
                          ValueType::INT);
    }

    // The value of a shift count written as a number: a literal, or the
    // negation of one (`-1` parses as NEGATE of 1); nothing for any other
    // operand, which is checked at run time.
    [[nodiscard]] std::optional<std::int64_t> constant_count(
        const ExprNode& count) const {
        if (count.op == ExprOp::INTEGER) {
            return count.integer;
        }
        if (count.op == ExprOp::NEGATE && count.children.size() == 1) {
            const ExprNode& operand = tree_->nodes.at(count.children[0]);
            if (operand.op == ExprOp::INTEGER) {
                return operand.integer ==
                               std::numeric_limits<std::int64_t>::min()
                           ? operand.integer
                           : -operand.integer;
            }
        }
        return std::nullopt;
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
    const NameTypes* names_ = nullptr;
    const RunTimeContext* run_time_ = nullptr;
    TypeError* error_;
    std::vector<std::optional<ValueType>> types_;
    // For each node, whether it is a tag reference: a reference to a tag
    // (not its `.quality` or `.age`) or `tag("...")`.
    std::vector<bool> tag_reference_;
    std::size_t current_ = 0;
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

std::optional<ValueType> run_time_type(const Expression& tree,
                                       const RunTimeContext& context,
                                       TypeError& error) {
    Checker checker(tree, context, error);
    return checker.run();
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

void add_read(PatternReads& reads, std::string_view name, TagRead read) {
    ++reads.count;
    if (!reads.first_odd.empty()) {
        return;
    }
    switch (read) {
        case TagRead::STRING:
        case TagRead::UNKNOWN:
            reads.first_odd = std::string(name);
            reads.odd = read;
            break;
        case TagRead::BOOL:
            if (reads.first_bool.empty()) {
                reads.first_bool = std::string(name);
            }
            break;
        case TagRead::REAL:
            reads.any_real = true;
            break;
        case TagRead::INT:
            break;
    }
}

}  // namespace scada::conf
