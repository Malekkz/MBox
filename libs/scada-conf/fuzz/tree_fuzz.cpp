// Fuzz target: `validate_tree`, every stage over a small tree (configuration
// schemas contract 1.4): the tree scan, the parser, the schemas and the rules
// of stage 3 (rules.h) together. The input is a tree of up to 16 files,
// separated by NUL bytes; each file is its relative path, a line feed, and
// its content. A path that could leave the tree (absolute, `..`, an empty
// component, a character outside `[a-z0-9_.-]` and `/`) drops its file.
// The tree is written to a directory that `mkdtemp` makes under the
// temporary directory, emptied before each input and removed at exit.
// Properties: validation never fails on a readable tree; the count is at least
// the list; every error has a message, and a line given with its column; the
// expanded tree (stage 4) exists exactly when the tree is valid, and each of
// its nodes names a file of the tree and the device it belongs to. In the
// expanded tree no key that steers expansion is left, no expression outside
// `alarms`, every point name is valid and unique in its device, and the tree
// is within the node limit (T7 of the review of #146).

#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/names.h"
#include "scada/conf/node.h"
#include "scada/conf/validate.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <set>
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

namespace {

using scada::conf::Document;
using scada::conf::NodeId;
using scada::conf::NodeKind;

// No key that steers expansion is left below `node` (T7 of the review of
// #146), and no expression outside `alarms`, which hold run-time ones.
// NOLINTNEXTLINE(misc-no-recursion): bounded by the nesting limit of 32
void check_expanded_value(const scada::conf::ExpandedTree& tree, NodeId node,
                          bool in_alarms) {
    const Document& document = tree.document;
    switch (document.kind(node)) {
        case NodeKind::SCALAR: {
            const scada::conf::ScalarType type = tree.types.of(node);
            check(in_alarms ||
                  (type != scada::conf::ScalarType::EXPRESSION &&
                   type != scada::conf::ScalarType::INTEGER_EXPRESSION));
            return;
        }
        case NodeKind::MAP:
            for (const scada::conf::Entry& entry : document.entries(node)) {
                const std::string_view key = document.text(entry.key);
                for (const std::string_view steering :
                     {"when", "repeat", "for", "defaults", "overrides",
                      "disable", "only", "replace", "template", "params"}) {
                    check(key != steering);
                }
                check_expanded_value(tree, entry.value,
                                     in_alarms || key == "alarms");
            }
            return;
        case NodeKind::SEQUENCE:
            for (const NodeId item : document.items(node)) {
                check_expanded_value(tree, item, in_alarms);
            }
            return;
    }
}

// Each device's points: a valid name (2.1) with no `{...}` left, unique in
// its device; the keys below them as `check_expanded_value`.
void check_devices(const scada::conf::ExpandedTree& tree) {
    const Document& document = tree.document;
    const std::optional<NodeId> channels =
        document.find(document.root(), "channels");
    check(channels.has_value());
    if (!channels) {
        return;
    }
    for (const NodeId channel : document.items(*channels)) {
        const std::optional<NodeId> devices = document.find(channel, "devices");
        check(devices.has_value());
        if (!devices) {
            continue;
        }
        for (const NodeId device : document.items(*devices)) {
            std::set<std::string_view> names;
            for (const scada::conf::Entry& entry : document.entries(device)) {
                if (document.text(entry.key) != "points") {
                    check_expanded_value(tree, entry.value, false);
                    continue;
                }
                for (const NodeId point : document.items(entry.value)) {
                    const std::optional<NodeId> name =
                        document.find(point, "name");
                    check(name.has_value());
                    if (!name) {
                        continue;
                    }
                    const std::string_view text = document.text(*name);
                    check(scada::conf::is_tag_name(text));
                    check(names.insert(text).second);
                    check_expanded_value(tree, point, false);
                }
            }
        }
    }
}

}  // namespace

// Every node of `tree` lies in a file of the tree, has a type table entry,
// and belongs to no device or to one the tree lists; the tree is within the
// node limit of 1.3, and its devices hold what expansion makes (T7).
void check_expanded(const scada::conf::ExpandedTree& tree) {
    constexpr std::size_t MAX_TREE_NODES = 4'000'000;
    const scada::conf::Document& document = tree.document;
    check(document.size() <= MAX_TREE_NODES);
    check(tree.types.size() == document.size());
    check(tree.instance_of.size() == document.size());
    for (scada::conf::NodeId node = 0; node < document.size(); ++node) {
        check(document.position(node).file < document.files().size());
        const std::uint32_t instance = tree.instance_of[node];
        check(instance == scada::conf::ExpandedTree::NO_INSTANCE ||
              instance < tree.instances.size());
    }
    check_devices(tree);
}

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    const std::string input(bytes.begin(), bytes.end());
    const TreeDirectory& directory = tree_directory();
    directory.empty();
    const std::filesystem::path& root = directory.path();
    write_tree(input, root);

    const scada::Result<scada::conf::Validation> validation =
        scada::conf::validate_and_expand(root);
    check(validation.ok());
    const scada::conf::Report& found = validation.value().report;
    check(found.error_count >= found.errors.size());
    for (const scada::conf::Diagnostic& diagnostic : found.errors) {
        check(!diagnostic.message.empty());
        check(diagnostic.location.line == 0 || diagnostic.location.column > 0);
    }
    // The expanded tree exists exactly when the tree is valid, and every node
    // of it names a file of the tree and a device it belongs to.
    const std::optional<scada::conf::ExpandedTree>& expanded =
        validation.value().expanded;
    check(expanded.has_value() == scada::conf::passed(found));
    if (expanded) {
        check_expanded(*expanded);
    }
    return 0;
}
