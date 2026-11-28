#ifndef SCADA_CONF_SRC_RUNTIME_EXPRESSIONS_H
#define SCADA_CONF_SRC_RUNTIME_EXPRESSIONS_H

// Stage 5 on run-time expressions (configuration schemas contract 5.6;
// decision D-117, slice 7b of #100): virtual tags, alarm expressions,
// `suppress_when` and interlock conditions, checked against the tags of the
// expanded tree.

#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/references.h"
#include "scada/conf/validate.h"
#include "tag_names.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scada::conf {

// How a task reads an input tag (D-125): its poll period, and the process
// of the protocol driver that owns it.
struct InputClass {
    std::uint64_t poll_ms = 0;  // 0: no poll period
    std::string poll_text;      // the period as written, or the default's
    std::string driver;         // `drv-<protocol>`; empty for none
};

// The class of tag `name`, given its TagInfo.
using InputClassifier =
    std::function<InputClass(std::string_view name, const TagInfo& info)>;

// What one virtual tag or interlock reads (D-125, ruling T1): its own
// inputs, patterns expanded within the budget of the run-time patterns.
struct ExpressionInputs {
    std::string name;
    bool is_virtual = true;
    std::string task;  // its task, given or `normal`
    const Document* document = nullptr;
    NodeId at = 0;  // its `expr` or `condition`
    // The input polled most slowly, and its class.
    std::string slowest;
    InputClass slowest_class;
    // The first tag a protocol driver owns that it reads, and that driver:
    // its own inputs first, in the expression's order and a pattern's name
    // order, else through the virtual tags it reads, in the order they are
    // defined (ruling T2 of D-125). Found once per tag over the graph of
    // CF-525, so a pattern that many tags read costs one walk.
    std::string driver_tag;
    std::string driver;
};

// What the virtual tags and interlocks of a tree read, for the task rules.
struct TaskInputs {
    InputClassifier classify;
    std::vector<ExpressionInputs> expressions;
    // False when the pattern budget ran out (CF-113): the inputs are not
    // whole, and nothing is checked from them.
    bool whole = true;
};

// Types every virtual tag from its expression, in the order its inputs
// need, and gives `tags` its derived value type; then checks every other
// run-time expression. Adds to `diagnostics`:
// - CF-525 for virtual tags and interlocks that form a cycle, also through
//   a pattern;
// - CF-310 for the default task `normal` of a virtual tag or an interlock
//   that a tasks file does not have;
// - CF-527 for a virtual tag whose given `value_type` is not the one its
//   expression gives;
// - for any run-time expression, the errors of `run_time_type`
//   (CF-310, CF-311, CF-521 to CF-524), and CF-523 when an alarm's `expr`,
//   its `suppress_when` or an interlock's `condition` is not BOOL.
// An expression of a template's point is placed at its instance, with the
// template note, and one cause at many instances is one error (D-114).
// Nothing when `tags` is not complete (CF-1001). `counts`, when given,
// receives the work done, for tests of its bound.
// `task_inputs`, when given, receives what each virtual tag and interlock
// reads, classed by its `classify` (D-125).
// `kept` bounds the pattern summaries and the expression results kept.
// `pattern_work` bounds the work of matching patterns: past it, CF-113, and
// nothing more is checked, nor anything reported of the expression being
// checked.
void check_run_time_expressions(
    TagNames& tags, const ExpandedTree& tree,
    std::span<const CheckedFile> files, Diagnostics& diagnostics,
    std::size_t kept = MAX_KEPT_RUN_TIME,
    std::uint64_t pattern_work = MAX_RUN_TIME_PATTERN_WORK,
    RunTimeCounts* counts = nullptr, TaskInputs* task_inputs = nullptr);

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_RUNTIME_EXPRESSIONS_H
