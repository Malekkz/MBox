#ifndef SCADA_CONF_SRC_VALIDATE_INTERNAL_H
#define SCADA_CONF_SRC_VALIDATE_INTERNAL_H

// `validate_and_expand` with the schema set that stage 5 reads a computed
// value's rules from, so that a test can reach the INTERNAL failure that the
// built-in schemas never give (item 5 of the delta review of #149).

#include "scada/common/result.h"
#include "scada/conf/expand.h"
#include "scada/conf/schema.h"
#include "scada/conf/tree.h"
#include "scada/conf/validate.h"

#include <filesystem>

namespace scada::conf {

// As `validate_and_expand`; the checks of a merged point find the rules of
// a computed value in `stage5_schemas` (stage 3 always reads the built-in
// set).
[[nodiscard]] Result<Validation> validate_and_expand_with(
    const std::filesystem::path& root, const ScanLimits& limits,
    const ExpandLimits& expand_limits, const SchemaSet& stage5_schemas);

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_VALIDATE_INTERNAL_H
