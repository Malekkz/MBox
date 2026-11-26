#ifndef SCADA_CONF_SRC_EXPRESSION_TYPES_H
#define SCADA_CONF_SRC_EXPRESSION_TYPES_H

// The types of a configuration expression (configuration schemas contract
// 5.6, the operator and function tables; decision D-113). Run-time
// expressions read tags and are checked once the expanded tree has them.

#include "scada/conf/expression.h"

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

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_EXPRESSION_TYPES_H
