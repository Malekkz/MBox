#ifndef SCADA_CONF_TREE_H
#define SCADA_CONF_TREE_H

// The configuration tree and stage 1 of validation: paths, file kinds and
// sizes (configuration schemas contract, sections 1, 1.3 and 1.4).

#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scada::conf {

// The schema of a file, from its path (the table of section 1).
enum class FileType : std::uint8_t {
    SYSTEM,           // system.yaml
    AREAS,            // areas.yaml
    ENUMS,            // enums.yaml
    LOGGING,          // logging.yaml
    TEMPLATE,         // templates/<name>.yaml
    CHANNEL,          // drivers/<protocol>/<channel>.yaml
    TAGS,             // tags/*.yaml
    OUTPUTS,          // outputs/*.yaml
    ALARMS,           // alarms/*.yaml
    TASKS,            // control/tasks.yaml
    PROGRAM,          // control/*.yaml other than tasks.yaml
    STRUCTURED_TEXT,  // control/*.st
};

// What a path of the tree is.
struct PathClass {
    enum class Kind : std::uint8_t {
        FILE,       // a file of the type below
        DIRECTORY,  // a directory the tree may hold
    };
    Kind kind = Kind::FILE;
    FileType type = FileType::SYSTEM;
    // For a CHANNEL file and for a `drivers/<protocol>` directory: the
    // protocol name. Empty otherwise.
    std::string protocol;
    // For a file: the part of its name before the extension, which some files
    // must repeat in their name key (CF-102).
    std::string stem;
};

// The protocols of phase 2 (section 1: Annexes A and B).
[[nodiscard]] bool is_known_protocol(std::string_view name) noexcept;

// Classifies `path`, relative to the root with `/` as separator, as a
// directory (`is_directory`) or a file the tree may hold. Nothing if the
// path is not part of a configuration tree: outside the path grammar, a
// reserved file, an unknown file or directory (CF-101). Pure: it looks at the
// text only.
[[nodiscard]] std::optional<PathClass> classify_path(std::string_view path,
                                                     bool is_directory);

// The size limit of a file of `type` in bytes (section 1.3): 8 MiB, and
// 1 MiB for a `.st` file.
[[nodiscard]] std::size_t file_size_limit(FileType type) noexcept;

// One file of a scanned tree, with its bytes.
struct TreeFile {
    std::string path;  // relative to the root, `/` as separator
    PathClass path_class;
    std::string content;
};

// The files of one configuration version, sorted by path (bytes).
struct Tree {
    std::vector<TreeFile> files;
};

// The limits of section 1.3 that stage 1 checks.
constexpr std::size_t MIB = std::size_t{1024} * 1024;
constexpr std::size_t MAX_TREE_FILES = 20000;
constexpr std::size_t MAX_TREE_BYTES = 256 * MIB;
constexpr std::size_t MAX_FILE_BYTES = 8 * MIB;
constexpr std::size_t MAX_STRUCTURED_TEXT_BYTES = 1 * MIB;

// Limits of stage 1 (section 1.3). Tests make them smaller.
struct ScanLimits {
    // Files of the tree. Every entry counts, refused ones too, except the
    // directories the tree may hold (D-105).
    std::size_t max_entries = MAX_TREE_FILES;
    std::size_t max_tree_bytes = MAX_TREE_BYTES;
    // Per file; nothing for the limits of `file_size_limit`.
    std::optional<std::size_t> max_file_bytes;
};

// Stage 1: reads the tree below the directory `root`.
//
// Every entry is classified with `classify_path`. Problems of the tree are
// added to `diagnostics`: an entry that is not a file of the tree, also a
// symbolic link, a FIFO, a socket or a device node (CF-101, nothing below it
// is read), a file over its size limit or a tree over its limits (CF-113;
// scanning stops at a tree limit, before the next entry is read) and a
// missing `system.yaml` (CF-103). Links are never followed and nothing
// outside `root` is opened; a file is read through the descriptor that was
// checked, so a file replaced while the scan runs is not read in its place.
// The bytes of files that passed are returned in the tree, files with an
// error are left out.
//
// Fails only when the tree cannot be read: `root` cannot be opened, or a
// system call fails for another reason than the tree's content (IO_ERROR,
// NOT_FOUND, PERMISSION_DENIED). What was added to `diagnostics` is then
// incomplete, and the caller reports the failure instead.
[[nodiscard]] Result<Tree> scan_tree(const std::filesystem::path& root,
                                     Diagnostics& diagnostics,
                                     const ScanLimits& limits = {});

}  // namespace scada::conf

#endif  // SCADA_CONF_TREE_H
