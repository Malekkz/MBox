#ifndef SCADA_CONF_YAML_H
#define SCADA_CONF_YAML_H

// The parser of the YAML subset of the configuration tree (configuration
// schemas contract, section 1.1; decision D-70, D-106).
//
// This is a subset parser, not a YAML parser. It accepts exactly the subset of
// section 1.1 and refuses everything else, each refusal with its code, line
// and column: anchors, aliases, merge keys, tags, complex keys, directives,
// document markers, several documents, null and empty values, a byte order
// mark, invalid UTF-8 and control characters. libscada-conf/README.md lists
// every refused construct.

#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace scada::conf {

// The parsing limits of section 1.3.
constexpr std::size_t MAX_FILE_NODES = 1000000;
constexpr std::size_t MAX_TREE_NODES = 4000000;
constexpr std::size_t MAX_DEPTH = 32;
constexpr std::size_t MAX_ENTRIES = 65536;
constexpr std::size_t MAX_SCALAR_BYTES = 65536;
constexpr std::size_t MAX_KEY_BYTES = 64;

// The parsing limits of one parse. Each is checked before the work it
// bounds. Tests make them smaller.
struct ParseLimits {
    // Nodes (scalars and collections) of the file.
    std::size_t max_nodes = MAX_FILE_NODES;
    // Collections nested in one another; the root collection is at depth 1.
    std::size_t max_depth = MAX_DEPTH;
    // Entries of one map or sequence.
    std::size_t max_entries = MAX_ENTRIES;
    // Bytes of a scalar's value.
    std::size_t max_scalar_bytes = MAX_SCALAR_BYTES;
    // Bytes of a map key.
    std::size_t max_key_bytes = MAX_KEY_BYTES;
};

// Nodes left for the whole tree, shared by the files of one validation.
// Each parse takes its nodes from it.
struct NodeBudget {
    std::size_t remaining = MAX_TREE_NODES;
    std::size_t limit = MAX_TREE_NODES;
};

// Parses `content`, the bytes of the file `path` (relative to the tree's
// root), into a Document whose only file is `path`.
//
// On the first construct outside the subset, or the first limit passed, adds
// that one error to `diagnostics` (CF-110 to CF-114) and returns nothing: a
// file is parsed whole or not at all. Keys that are equal as text are CF-111
// here; keys that become equal only once the schema types them (`16` and
// `0x10`) are found by stage 3. An empty file, or one of comments only, is an
// empty map.
//
// `budget`, if given, is the node budget of the tree and is reduced by the
// nodes of this file.
[[nodiscard]] std::optional<Document> parse_yaml(std::string_view content,
                                                 const std::string& path,
                                                 Diagnostics& diagnostics,
                                                 const ParseLimits& limits = {},
                                                 NodeBudget* budget = nullptr);

}  // namespace scada::conf

#endif  // SCADA_CONF_YAML_H
