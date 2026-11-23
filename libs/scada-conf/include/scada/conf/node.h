#ifndef SCADA_CONF_NODE_H
#define SCADA_CONF_NODE_H

// The tree of one parsed configuration file: maps, sequences and scalars,
// each with the file, line and column it came from (configuration schemas
// contract, sections 1.1, 1.2 and 1.4). The YAML subset parser builds it;
// the loader of the expanded file (section 13.3) is to build the same type.

#include "scada/conf/diagnostic.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scada::conf {

enum class NodeKind : std::uint8_t { MAP, SEQUENCE, SCALAR };

// How a scalar was written. Its type is decided later by the schema of its
// key from the text and this style (section 1.2): a quoted `"10"` is never an
// integer.
enum class ScalarStyle : std::uint8_t {
    PLAIN,
    SINGLE_QUOTED,
    DOUBLE_QUOTED,
    LITERAL,  // `|`
    FOLDED,   // `>`
};

// The index of a node in its Document.
using NodeId = std::uint32_t;

struct Node {
    NodeKind kind = NodeKind::SCALAR;
    ScalarStyle style = ScalarStyle::PLAIN;
    // Index into `Document::files()`.
    std::uint32_t file = 0;
    // Where the node starts: its first character, or for a collection its
    // first entry's indicator (`-`), key or bracket. 1-based, columns in
    // characters (D-106).
    std::uint32_t line = 1;
    std::uint32_t column = 1;
    // A scalar's value: the text as written for a plain scalar (lines folded
    // into one), the unquoted and unescaped text otherwise. Empty for a
    // collection.
    std::string text;
    // A sequence's items in order. A map's entries in order, as key, value,
    // key, value; every key is a scalar.
    std::vector<NodeId> children;
};

// Owns the nodes of one parsed file. Nodes refer to each other by NodeId and
// never point into another Document. A Document is not changed after it is
// built.
class Document {
public:
    // A document over the files named in `files`, without nodes yet.
    explicit Document(std::vector<std::string> files);

    // Adds `node` and returns its id. For the builders (the parser, the
    // loader) only.
    NodeId add(Node node);

    // Sets the root. For the builders only.
    void set_root(NodeId root) noexcept { root_ = root; }

    // Appends `child` to the children of `parent`. For the builders only.
    void append_child(NodeId parent, NodeId child);

    // The root node. A Document always has one once built.
    [[nodiscard]] NodeId root() const noexcept { return root_; }

    // The node `node_id`. An id that is not of this document ends the process
    // (a programmer error, never outside input).
    [[nodiscard]] const Node& node(NodeId node_id) const;

    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

    [[nodiscard]] const std::vector<std::string>& files() const noexcept {
        return files_;
    }

    // The location of node `node_id`, for a diagnostic.
    [[nodiscard]] Location location(NodeId node_id) const;

    // For a map: the number of entries, and the key and value of entry
    // `index` (index below `entry_count`).
    [[nodiscard]] std::size_t entry_count(NodeId map) const;
    [[nodiscard]] NodeId key(NodeId map, std::size_t index) const;
    [[nodiscard]] NodeId value(NodeId map, std::size_t index) const;

    // For a map: the value of the entry whose key text is `key_text`, if any.
    // Keys are unique by text once parsed (CF-111).
    [[nodiscard]] std::optional<NodeId> find(NodeId map,
                                             std::string_view key_text) const;

private:
    std::vector<std::string> files_;
    std::vector<Node> nodes_;
    NodeId root_ = 0;
};

}  // namespace scada::conf

#endif  // SCADA_CONF_NODE_H
