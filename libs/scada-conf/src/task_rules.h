#ifndef SCADA_CONF_SRC_TASK_RULES_H
#define SCADA_CONF_SRC_TASK_RULES_H

// Stage 5 on the control tasks (configuration schemas contract 9.1 and
// 10.9; decision D-125, slice 7e-2 of #100).

#include "runtime_expressions.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/references.h"
#include "tag_names.h"

#include <span>

namespace scada::conf {

// Checks the tasks of control/tasks.yaml, or the default tasks, against
// the programs of `files` and system.yaml. Adds to `diagnostics`:
// - CF-901 for a program that no task lists, or that two task entries
//   list, at its `program`;
// - CF-902 for a program whose `task` is not the task that lists it, at
//   its `task`;
// - CF-906 for a task whose priority, given or the default 50, is at or
//   above `realtime.io_priority` (default 95), at its `priority`, else at
//   the task's name, else, for the default tasks, at `io_priority`;
// - warning CW-903 for a reader of a task (one of its programs' sources,
//   one of its virtual tags or interlocks) whose input polled most slowly
//   has a poll period longer than the task's, at the reader;
// - CF-904 for a reader of an `rt_required` task that reads a tag a
//   protocol driver owns, directly or through virtual tags, naming the
//   first such tag (ruling T2);
// - CF-905 for an internal tag that programs of two tasks write, or that a
//   task other than its own writes, at the later `out` (ruling T3).
// The input rules are not checked when `inputs` is not whole (CF-113) or
// `tags` is not complete (CF-1001).
void check_task_rules(const TagNames& tags, const ExpandedTree& tree,
                      std::span<const CheckedFile> files,
                      const TaskInputs& inputs, Diagnostics& diagnostics);

// The class of each input tag of `tree` (D-125): a read point's or bit's
// poll group period, and `drv-<protocol>` for a point, a bit, a device's or
// a channel's tag on a channel that is not `io`.
[[nodiscard]] InputClassifier input_classifier(const ExpandedTree& tree);

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_TASK_RULES_H
