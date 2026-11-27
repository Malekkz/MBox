#ifndef SCADA_CONF_SRC_EXPRESSION_TYPES_H
#define SCADA_CONF_SRC_EXPRESSION_TYPES_H

// The types of an expression (configuration schemas contract 5.6, the
// operator and function tables): a configuration expression's over
// parameters and variables (D-113), a run-time expression's over the tags of
// the expanded tree (D-117).

#include "scada/conf/expression.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scada::conf {

// The value types of 5.6, and RANGE for `a..b`.
enum class ValueType : std::uint8_t {
    BOOL,
    INT,
    REAL,
    TIME,
    STRING,
    SET,
    RANGE
};

// The name of a type as messages write it: "BOOL", "INT".
[[nodiscard]] std::string_view value_type_name(ValueType type) noexcept;

// One error: its code (CF-310 for a name nothing defines, CF-523 for a type
// that does not fit), its byte column in the expression's text and its
// message.
struct TypeError {
    std::uint16_t code = 0;
    std::uint32_t column = 1;
    std::string message;
};

// The type of a name: a parameter or a `repeat` variable; nothing if none of
// that name exists.
using NameTypes = std::function<std::optional<ValueType>(std::string_view)>;

// The type of `tree`, a configuration expression that parsed, or nothing
// with the first error in `error`. Functions that read tags (aggregates,
// `quality`, `tag`) are CF-523: a configuration expression has no tags.
// With `node_types`, the type of every node, by index, once it succeeds.
[[nodiscard]] std::optional<ValueType> configuration_type(
    const Expression& tree, const NameTypes& names, TypeError& error,
    std::vector<ValueType>* node_types = nullptr);

// How a run-time expression reads a tag (5.6): a BOOL, INT or REAL value;
// a STRING tag, which no expression may use (CF-522); or a virtual tag whose
// type is not known, because its own expression is refused or is in a cycle
// (D-117), which is reported there and not again.
enum class TagRead : std::uint8_t { BOOL, INT, REAL, STRING, UNKNOWN };

// What the tags a name pattern matches are read as, in the order of their
// names: all an aggregate needs (D-117), in a size that does not grow with
// the tags, so that one use of a pattern costs the same whatever it matches.
struct PatternReads {
    std::size_t count = 0;
    // The first tag read as STRING or not typed yet (UNKNOWN), if any.
    std::string first_odd;
    TagRead odd = TagRead::INT;
    // The first BOOL tag, if any.
    std::string first_bool;
    bool any_real = false;
};

// Adds to `reads` a tag of `name`, read as `read`, after those added
// before.
void add_read(PatternReads& reads, std::string_view name, TagRead read);

// What a run-time expression is checked against (D-117).
struct RunTimeContext {
    // How tag `name` is read, or nothing if no tag has that name.
    std::function<std::optional<TagRead>(std::string_view)> tag;
    // How the tags that name pattern `pattern` (2.5) matches are read; a
    // count of 0 if none.
    std::function<PatternReads(std::string_view)> matching;
    // The task that runs the expression and its period, for the window of
    // a time function (CF-524); an empty name where no task runs it.
    std::string task;
    std::int64_t period_ms = 0;
};

// The type of `tree`, a run-time expression that parsed, or nothing with
// the first error in `error`: CF-310 for a name that is no tag, CF-311 for a
// pattern that matches none, CF-521 for an ambiguous `.quality` or `.age`,
// CF-522 for a STRING tag, CF-523 for a type that does not fit, CF-524 for
// a time function's window out of range. An error of code 0 is a tag of
// unknown type: the expression is not checked further, and nothing is
// reported.
[[nodiscard]] std::optional<ValueType> run_time_type(
    const Expression& tree, const RunTimeContext& context, TypeError& error);

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_EXPRESSION_TYPES_H
