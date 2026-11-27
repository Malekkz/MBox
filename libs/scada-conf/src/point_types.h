#ifndef SCADA_CONF_SRC_POINT_TYPES_H
#define SCADA_CONF_SRC_POINT_TYPES_H

// The raw and value types of a point of the expanded tree (configuration
// schemas contract 4.7, Annexes A.2 and B.2), shared by the checks of a
// merged point (D-115) and the tag names of stage 5 (D-116).

#include "scada/conf/expand.h"
#include "scada/conf/node.h"

#include <string>
#include <string_view>

namespace scada::conf {

// True for a raw type that reads an integer: `bool`, the integer types and
// `mod10k`.
[[nodiscard]] bool is_integer_raw(std::string_view raw) noexcept;

// The raw type of a Modbus `address` map: `bool` for a bit, else its
// `type`, else `bool` for coils and discrete inputs and `uint16` otherwise.
[[nodiscard]] std::string modbus_raw_type(const Document& document,
                                          NodeId address);

// The value type of a Modbus point of `tree` whose raw type is `raw`: its
// `value_type`, else derived (4.7): `int64` for an enumeration or a double
// point, `bool` for a `bool` raw type, `string` for `string` and `bytes`,
// `float64` for another raw type that is not an integer, for
// `exponent_from`, a `divide_by` above 1 or a `gain` or `offset` that is
// not an integer, else `uint64` for a `uint64` raw type and `int64`.
[[nodiscard]] std::string modbus_value_type(const ExpandedTree& tree,
                                            NodeId point, std::string_view raw);

// The value type of an `io` point (B.2): its `value_type`, else `bool` on
// a `gpio` channel and `float64` on another backend.
[[nodiscard]] std::string io_value_type(const Document& document, NodeId point,
                                        std::string_view backend);

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_POINT_TYPES_H
