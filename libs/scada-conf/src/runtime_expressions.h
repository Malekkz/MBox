#ifndef SCADA_CONF_SRC_RUNTIME_EXPRESSIONS_H
#define SCADA_CONF_SRC_RUNTIME_EXPRESSIONS_H

// Stage 5 on run-time expressions (configuration schemas contract 5.6;
// decision D-117, slice 7b of #100): virtual tags, alarm expressions,
// `suppress_when` and interlock conditions, checked against the tags of the
// expanded tree.

#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/references.h"
#include "tag_names.h"

#include <span>

namespace scada::conf {

// Types every virtual tag from its expression, in the order its inputs
// need, and gives `tags` its derived value type; then checks every other
// run-time expression. Adds to `diagnostics`:
// - CF-525 for virtual tags that form a cycle, also through a pattern;
// - CF-527 for a virtual tag whose given `value_type` is not the one its
//   expression gives;
// - for any run-time expression, the errors of `run_time_type`
//   (CF-310, CF-311, CF-521 to CF-524), and CF-523 when an alarm's `expr`,
//   its `suppress_when` or an interlock's `condition` is not BOOL.
// An expression of a template's point is placed at its instance, with the
// template note, and one cause at many instances is one error (D-114).
void check_run_time_expressions(TagNames& tags, const ExpandedTree& tree,
                                std::span<const CheckedFile> files,
                                Diagnostics& diagnostics);

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_RUNTIME_EXPRESSIONS_H
