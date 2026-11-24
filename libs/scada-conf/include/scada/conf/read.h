#ifndef SCADA_CONF_READ_H
#define SCADA_CONF_READ_H

// Typed reads of a scalar node, for the stages after stage 3 and for the
// drivers that read their configuration (#104). Each reads the text of one
// scalar by the rules of configuration schemas contract 1.2 and 2.2.
//
// Errors: INVALID_ARGUMENT for a node that is not a scalar or whose text has
// no form of the type (a quoted scalar is not a number or a boolean, 1.2);
// OUT_OF_RANGE for a value of the right form that does not fit.

#include "scada/common/result.h"
#include "scada/conf/node.h"

#include <cstdint>

namespace scada::conf {

// A plain integer (`-?(0|[1-9][0-9]*)` or `0x...`) that fits int64.
[[nodiscard]] Result<std::int64_t> to_int64(const Document& document,
                                            NodeId node);

// A plain integer that fits uint64.
[[nodiscard]] Result<std::uint64_t> to_uint64(const Document& document,
                                              NodeId node);

// A plain number, integer or decimal, as the nearest binary64.
[[nodiscard]] Result<double> to_decimal(const Document& document, NodeId node);

// Plain `true` or `false`.
[[nodiscard]] Result<bool> to_bool(const Document& document, NodeId node);

// A duration of section 2.2 (`100ms`, `5s`), plain or quoted, in
// nanoseconds.
[[nodiscard]] Result<std::int64_t> to_duration_ns(const Document& document,
                                                  NodeId node);

// A raw pattern (section 2.3) of a register `width_bits` wide (1 to 64): a
// plain integer from 0 to 2^width - 1, or a negative one from -2^(width - 1),
// given as its two's complement in `width_bits` bits. A width outside 1 to 64
// is INVALID_ARGUMENT.
[[nodiscard]] Result<std::uint64_t> to_raw_pattern(const Document& document,
                                                   NodeId node,
                                                   unsigned width_bits);

// True if `node` is a plain scalar written as an integer (section 1.2), of
// any size: `10`
// and `0x10`, not `10.0` or `'10'`. For CF-517 (an integer scaling).
[[nodiscard]] bool is_integer_form(const Document& document, NodeId node);

}  // namespace scada::conf

#endif  // SCADA_CONF_READ_H
