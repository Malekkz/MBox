#ifndef SCADA_CONF_SRC_OUTPUT_RULES_H
#define SCADA_CONF_SRC_OUTPUT_RULES_H

// Stage 5 on areas, outputs and their safe states (configuration schemas
// contract 7.1 to 7.4 and 10.7; decision D-124, slice 7e-1 of #100).

#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/references.h"
#include "tag_names.h"

#include <span>

namespace scada::conf {

// Checks the outputs of `files` against their targets, and the areas.
// Adds to `diagnostics`:
// - CF-702 for a target that is not a point's own tag with `access: rw` or
//   `wo` and a value type other than STRING, at the `target`;
// - CF-703 for a target of a second output, at its `target`;
// - CF-705 for a `refresh`, or a `safe_state`, `on_shutdown`,
//   `on_control_loss` or `on_generation_change` other than `hold`, on an
//   output whose target is `dangerous`;
// - CF-706 for a `refresh` on an output whose target is `storage: nvm`;
// - CF-709 for an output with neither `feedback` nor `no_feedback`, and
//   CF-710 for one without `safe_state` whose target is not dangerous;
// - CF-711 for an output of a kind other than `setpoint` without
//   `interlocks`, and warning CW-712 for one whose interlock lists are
//   empty;
// - CF-714 for `refresh` on a kind other than `setpoint` or with `sbo`, and
//   for `release` in a fail-safe key without `limits.release`;
// - CF-718 for `local_blocks_auto` without `local_panel`, or a
//   `local_panel.input` that is not a BOOL tag a driver reads;
// - CF-719 for a `step`, `pulse` or `trigger` output whose `safe_state` or
//   fail-safe key is not `hold`; CF-720 for a `duration` on a `pulse`;
// - CF-722 for `safe` in a fail-safe key written with `safe_state: hold`;
//   CF-723 for `release` in a fail-safe key of a kind other than
//   `setpoint`;
// - warning CW-701 for an area no `area` key names;
// - warning CW-721 for a remote switching output (a `single` or `double`
//   output on a protocol driver, not dangerous, with `safe_state` `off` or
//   `on`) whose written `on_control_loss` holds.
// The checks that read a target are not made when `tags` is not complete
// (CF-1001).
void check_output_rules(const TagNames& tags, const ExpandedTree& tree,
                        std::span<const CheckedFile> files,
                        Diagnostics& diagnostics);

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_OUTPUT_RULES_H
