#ifndef SCADA_CONF_SRC_POINT_RULES_H
#define SCADA_CONF_SRC_POINT_RULES_H

// Stage 5 on the keys of the points of the expanded tree that need no
// protocol's address rules (configuration schemas contract 4.6 to 4.9 and
// 5.3; decision D-120, slice 7d-1 of #100).

#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/references.h"

#include <span>

namespace scada::conf {

// Checks every point of every device of `tree` (a device given twice,
// CF-303, once). Adds to `diagnostics`:
// - CF-611 for a `max_age`, given or 3 times its poll group's period,
//   below the point's `update_period`;
// - CF-612 for a companion (`exponent_from`, `quality_from`,
//   `source_time_from`) or a health point that is not in the poll group of
//   the point that uses it, or is not read (`wo`);
// - CF-613 for a Modbus double point that is read, whose address is a
//   field or an integer, and that gives no `states`;
// - CF-614 for a `wo` point with `poll_group`, `alarms` or `logging`;
// - CF-616 for an identity check on a point that is not read;
// - CF-618 for a device whose points use a bit, a field or `bits` and that
//   has no `bit_order`;
// - CF-619 for an `rw` or `wo` point without `storage`;
// - CF-640 for `keep_last` (in `on_invalid` or an invalid value's
//   `action`) on a point that gives no `max_age`.
// An error of a template's point is at its instance, with the template
// note, and one cause at many instances is one error (D-114). `files` are
// the files of the tree, for the names of templates.
void check_point_rules(const ExpandedTree& tree,
                       std::span<const CheckedFile> files,
                       Diagnostics& diagnostics);

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_POINT_RULES_H
