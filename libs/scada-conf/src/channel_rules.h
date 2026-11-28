#ifndef SCADA_CONF_SRC_CHANNEL_RULES_H
#define SCADA_CONF_SRC_CHANNEL_RULES_H

// Stage 5 on the channels of the expanded tree, each against the limits of
// system.yaml and against the other channels (configuration schemas
// contract 4.1, 10.6 and Annex A.1; decision D-123, slice 7d-3 of #100).

#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/references.h"

#include <span>

namespace scada::conf {

// Checks the channels of `tree`. Adds to `diagnostics`:
// - CF-605 for a channel with more devices than
//   `limits.max_devices_per_channel` of system.yaml (default 247), at its
//   `name`;
// - CF-620 for a Modbus `rtu` channel whose `device` path is written as
//   that of an earlier channel, at its `device`;
// - warning CW-621 for a Modbus `tcp` or `rtu_over_tcp` channel whose host
//   and port (default 502) are those of an earlier channel, the host
//   compared as an address, at its `host`.
// A channel is paired with the first earlier channel of its path or
// endpoint only, so n channels on one give n - 1 diagnostics. Channels are
// in the order of the expanded tree. Linear in the channels and devices, up
// to logarithms. `files` are the files of the tree, for system.yaml.
void check_channel_rules(const ExpandedTree& tree,
                         std::span<const CheckedFile> files,
                         Diagnostics& diagnostics);

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_CHANNEL_RULES_H
