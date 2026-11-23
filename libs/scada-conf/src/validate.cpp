#include "scada/conf/validate.h"

#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/conf/tree.h"
#include "scada/conf/yaml.h"

#include <filesystem>
#include <optional>

namespace scada::conf {

Result<Report> validate_tree(const std::filesystem::path& root,
                             const ScanLimits& limits) {
    Report report;
    Diagnostics stage1;
    const Result<Tree> tree = scan_tree(root, stage1, limits);
    if (!tree.ok()) {
        return tree.status();
    }
    const bool stage1_failed = stage1.error_count() > 0;
    stage1.move_into(report);
    if (stage1_failed) {
        return report;
    }

    // Stage 2: every YAML file. A `.st` file is not parsed in phase 2; it is
    // held to the limits of stage 1 only (D-106).
    Diagnostics stage2;
    NodeBudget budget;
    for (const TreeFile& file : tree.value().files) {
        if (file.path_class.type == FileType::STRUCTURED_TEXT) {
            continue;
        }
        const std::optional<Document> document =
            parse_yaml(file.content, file.path, stage2, {}, &budget);
        static_cast<void>(document);  // stage 3 is added by the next slice
    }
    stage2.move_into(report);
    return report;
}

}  // namespace scada::conf
