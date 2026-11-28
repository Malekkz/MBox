#ifndef SCADA_CONF_SRC_MODBUS_RULES_H
#define SCADA_CONF_SRC_MODBUS_RULES_H

// Stage 5 on the Modbus addresses of the expanded tree (configuration
// schemas contract Annex A.1 to A.5 and 10.6; decision D-122, slice 7d-2
// of #100).

#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"

namespace scada::conf {

// Checks the devices of every Modbus channel of `tree` (a device given
// twice, CF-303, once). Adds to `diagnostics`:
// - CF-603 for an address that is not valid for Modbus: a register outside
//   0 to 65,535 after `address_base`, a value or a write `quantity` that
//   runs past 65,535, a bit key or a type other than `bool` on a coil or
//   discrete input, a field wider than the raw type, and a read range
//   outside 0 to 65,535;
// - CF-602 for two points of one device that read the same register of one
//   table as whole values (a bit, a field or a byte is a part, which
//   shares it), or that write a bit in common (A.3, A.5), at the point
//   written later;
// - CF-601 for two devices of one channel whose registers meet (A.3);
// - CF-610 for a `side_effect` or `exclusive` range whose points are in
//   different poll groups, that an identity check reads or a point writes,
//   and for any range that overlaps a range of another kind (A.4);
// - CF-615 for a function a point needs that the device's `functions`
//   excludes, with no substitution (A.1, A.5);
// - warning CW-607 for a device with an `idle_timeout` shorter than its
//   slowest poll period or its `backoff_max`, and no `keepalive` (4.6).
// Linear in the points and ranges, up to logarithms. An error of a
// template's point is at its instance, with the template note, and one cause
// at many instances is one error (D-114).
void check_modbus_rules(const ExpandedTree& tree, Diagnostics& diagnostics);

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_MODBUS_RULES_H
