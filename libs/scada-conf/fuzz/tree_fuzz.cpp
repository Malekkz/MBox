// Fuzz target: `validate_tree`, every stage over a small tree (configuration
// schemas contract 1.4): the tree scan, the parser, the schemas and the rules
// of stage 3 (rules.h) together. The input is a tree of up to 16 files,
// separated by NUL bytes; each file is its relative path, a line feed, and
// its content. A path that could leave the tree (absolute, `..`, an empty
// component, a character outside `[a-z0-9_.-]` and `/`) drops its file.
// The tree is written to a directory that `mkdtemp` makes under the
// temporary directory, emptied before each input and removed at exit.
// Properties: validation never fails on a readable tree; the count is at least
// the list; every error has a message, and a line given with its column.

#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/validate.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace {

constexpr std::size_t MAX_FILES = 16;
constexpr std::size_t MAX_PATH = 64;
constexpr std::size_t MAX_DEPTH = 3;

void check(bool condition) {
    if (!condition) {
        std::abort();
    }
}

// The directory of this process's trees: made once, with a name no other
// process has, and removed with everything in it when the process exits.
class TreeDirectory {
public:
    TreeDirectory() {
        std::string name =
            (std::filesystem::temp_directory_path() / "scada-tree-fuzz-XXXXXX")
                .string();
        check(mkdtemp(name.data()) != nullptr);
        path_ = name;
    }
    TreeDirectory(const TreeDirectory&) = delete;
    TreeDirectory& operator=(const TreeDirectory&) = delete;
    TreeDirectory(TreeDirectory&&) = delete;
    TreeDirectory& operator=(TreeDirectory&&) = delete;
    ~TreeDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    // Removes what the last input wrote, keeping the directory.
    void empty() const {
        std::error_code error;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(path_, error)) {
            std::filesystem::remove_all(entry.path(), error);
        }
    }

private:
    std::filesystem::path path_;
};

const TreeDirectory& tree_directory() {
    static const TreeDirectory DIRECTORY;
    return DIRECTORY;
}

bool safe_path(std::string_view path) {
    if (path.empty() || path.size() > MAX_PATH) {
        return false;
    }
    std::size_t depth = 0;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        const std::string_view part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == ".." ||
            ++depth > MAX_DEPTH) {
            return false;
        }
        for (const char chr : part) {
            const bool allowed = (chr >= 'a' && chr <= 'z') ||
                                 (chr >= '0' && chr <= '9') || chr == '_' ||
                                 chr == '.' || chr == '-';
            if (!allowed) {
                return false;
            }
        }
        start = end + 1;
    }
    return true;
}

void write_tree(std::string_view input, const std::filesystem::path& root) {
    std::size_t files = 0;
    while (!input.empty() && files < MAX_FILES) {
        const std::size_t end = input.find('\0');
        const std::string_view piece = input.substr(0, end);
        input = end == std::string_view::npos ? std::string_view{}
                                              : input.substr(end + 1);
        const std::size_t line_end = piece.find('\n');
        if (line_end == std::string_view::npos) {
            continue;
        }
        const std::string_view path = piece.substr(0, line_end);
        if (!safe_path(path)) {
            continue;
        }
        const std::filesystem::path file = root / std::string(path);
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        if (error) {
            continue;
        }
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        const std::string_view content = piece.substr(line_end + 1);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        ++files;
    }
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    const std::string input(bytes.begin(), bytes.end());
    const TreeDirectory& directory = tree_directory();
    directory.empty();
    const std::filesystem::path& root = directory.path();
    write_tree(input, root);

    const scada::Result<scada::conf::Report> report =
        scada::conf::validate_tree(root);
    check(report.ok());
    const scada::conf::Report& found = report.value();
    check(found.error_count >= found.errors.size());
    for (const scada::conf::Diagnostic& diagnostic : found.errors) {
        check(!diagnostic.message.empty());
        check(diagnostic.location.line == 0 || diagnostic.location.column > 0);
    }
    return 0;
}
