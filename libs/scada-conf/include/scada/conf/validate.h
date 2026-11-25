#ifndef SCADA_CONF_VALIDATE_H
#define SCADA_CONF_VALIDATE_H

// Validation of a whole configuration tree (configuration schemas contract,
// section 1.4). `scada-conf` validates a tree, never a single file.

#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/tree.h"

#include <filesystem>
#include <optional>

namespace scada::conf {

// Validates the tree below `root` and returns what it found, in the order of
// section 1.4: the errors of the first stage that has any, otherwise the
// warnings. A stage runs only when the stages before it found no error.
//
// Implemented so far:
// - stage 1: paths, file kinds and sizes;
// - stage 2: the YAML subset of every YAML file; every `.st` file is CF-920,
//   as structured text is not available in this version (contract 9.3);
// - stage 3: the schema of every file type, a channel and a template with
//   their protocol's (Annexes A and B); CF-102, CF-111 once typed, CF-211,
//   `time.sync.servers`, the keys an object requires or refuses by the
//   value of another (rules.h) and CF-517 within one point object;
// - stage 4, when stages 1 to 3 found no error: the expansion of templates
//   and instances (expand.h);
// - stage 5, when stage 4 found no error: the names, duplicates, channel
//   rules and expression types that need no point tags (references.h), and
//   the checks of each merged point: CF-202, CF-504, CF-516, CF-517, and
//   each computed value against its key's rules (CF-204 and others).
// The rest of stage 5 comes with slice 7 of #100.
//
// Fails when the tree cannot be read (see `scan_tree`), and with INTERNAL
// when a computed value's key has no rule in its schema, a defect of this
// software; a tree that is wrong is a report with errors, not a failure.
[[nodiscard]] Result<Report> validate_tree(const std::filesystem::path& root,
                                           const ScanLimits& limits = {});

// What `validate_and_expand` gives: the report, and the expanded tree of
// stage 4 when the report has no error.
struct Validation {
    Report report;
    std::optional<ExpandedTree> expanded;
    // What stage 4 held to report its errors (expand.h).
    ExpandCounts expansion;
};

// As `validate_tree`, and keeps the expanded tree, which the expanded files
// of contract 13.3 are written from (#102).
// `expand_limits` are the limits of the expanded tree, the contract's unless
// a test gives others.
[[nodiscard]] Result<Validation> validate_and_expand(
    const std::filesystem::path& root, const ScanLimits& limits = {},
    const ExpandLimits& expand_limits = {});

}  // namespace scada::conf

#endif  // SCADA_CONF_VALIDATE_H
