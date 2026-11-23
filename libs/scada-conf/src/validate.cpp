#include "scada/conf/validate.h"

#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/tree.h"

#include <filesystem>

namespace scada::conf {

Result<Report> validate_tree(const std::filesystem::path& root,
                             const ScanLimits& limits) {
    Report report;
    Diagnostics stage1;
    const Result<Tree> tree = scan_tree(root, stage1, limits);
    if (!tree.ok()) {
        return tree.status();
    }
    stage1.move_into(report);
    return report;
}

}  // namespace scada::conf
