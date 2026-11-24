#ifndef SCADA_CONF_VALIDATE_H
#define SCADA_CONF_VALIDATE_H

// Validation of a whole configuration tree (configuration schemas contract,
// section 1.4). `scada-conf` validates a tree, never a single file.

#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/tree.h"

#include <filesystem>

namespace scada::conf {

// Validates the tree below `root` and returns what it found, in the order of
// section 1.4: the errors of the first stage that has any, otherwise the
// warnings. A stage runs only when the stages before it found no error.
//
// Implemented so far: stage 1 (paths, file kinds, sizes) and stage 2 (the
// YAML subset of every YAML file; every `.st` file is CF-920, as structured
// text is not available in this version, contract 9.3). Later stages are
// added by P2-03 (#100) slice by slice.
//
// Fails only when the tree cannot be read (see `scan_tree`); a tree that is
// wrong is a report with errors, not a failure.
[[nodiscard]] Result<Report> validate_tree(const std::filesystem::path& root,
                                           const ScanLimits& limits = {});

}  // namespace scada::conf

#endif  // SCADA_CONF_VALIDATE_H
