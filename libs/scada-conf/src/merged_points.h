#ifndef SCADA_CONF_SRC_MERGED_POINTS_H
#define SCADA_CONF_SRC_MERGED_POINTS_H

// Stage 5 on each merged point of the expanded tree (configuration schemas
// contract 4.5, 4.7, Annexes A and B; decisions D-108, D-134, D-115): what
// only the whole point, after every level, default and override, can show.

#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"

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
//   where merging brought the keys together or the type is derived.
void check_merged_points(const ExpandedTree& tree, Diagnostics& diagnostics);

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_MERGED_POINTS_H
