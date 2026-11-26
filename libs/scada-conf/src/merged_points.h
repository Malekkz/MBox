#ifndef SCADA_CONF_SRC_MERGED_POINTS_H
#define SCADA_CONF_SRC_MERGED_POINTS_H

// Stage 5 on each merged point of the expanded tree (configuration schemas
// contract 4.5, 4.7, Annexes A and B; decisions D-108, D-134, D-115): what
// only the whole point, after every level, default and override, can show.

#include "scada/common/status.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/references.h"
#include "scada/conf/schema.h"

#include <span>

namespace scada::conf {

// Adds to `diagnostics`, for every point of `tree`:
// - CF-202 for a key that a merged map still lacks: a Modbus address's
//   `table`, `register` and the keys its raw type needs; an `io` address's
//   keys for its channel's backend; `exponent_from`'s `point` and
//   `direction`; a bit's `name`; `on_invalid`'s `substitute`; `tz` of a
//   local `device_time`;
// - CF-504 for an integer value type or a counter on a floating raw type
//   (an enumeration and a double point read a code and may);
// - CF-516 for an integer scaling that can pass its value type over the raw
//   type's whole range;
// - CF-517 for an integer value type with a scaling that is not integer,
//   where merging brought the keys together or the type is derived;
// - CF-310 for an enumeration that a template's point names and neither the
//   instance's chain nor `enums.yaml` of `files` defines (B5 of the review
//   of #144; an enumeration the instance's own file names is stage 5's
//   reference check);
// - for each value computed from a configuration expression, the errors
//   stage 3 gives that value written at its key: CF-204 for `register`,
//   `bit` or `divide_by` out of range, and every other rule of the key in
//   the protocol's schema (`SchemaSet::check_value`).
// The rules of a computed value are found in `schemas`, the built-in set
// unless a test gives another.
// Fails with INTERNAL, a defect of this software, when a computed value's
// key has no rule in its protocol's schema; otherwise OK, the errors in
// `diagnostics`.
[[nodiscard]] Status check_merged_points(
    const ExpandedTree& tree, std::span<const CheckedFile> files,
    Diagnostics& diagnostics, const SchemaSet& schemas = SchemaSet::builtin());

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_MERGED_POINTS_H
