#include "scada/conf/node.h"

#include "scada/common/panic.h"
#include "scada/conf/diagnostic.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::conf {

Document::Document(std::vector<std::string> files) : files_(std::move(files)) {}

NodeId Document::add(Node node) {
    const auto node_id = static_cast<NodeId>(nodes_.size());
    if (nodes_.size() != node_id) {
        panic("Document::add: too many nodes");
    }
    nodes_.push_back(std::move(node));
    return node_id;
}

void Document::append_child(NodeId parent, NodeId child) {
    if (parent >= nodes_.size() || child >= nodes_.size()) {
        panic("Document::append_child: no such node");
    }
    nodes_[parent].children.push_back(child);
}

const Node& Document::node(NodeId node_id) const {
    if (node_id >= nodes_.size()) {
        panic("Document::node: no such node");
    }
    return nodes_[node_id];
}

Location Document::location(NodeId node_id) const {
    const Node& found = node(node_id);
    std::string file;
    if (found.file < files_.size()) {
        file = files_[found.file];
    }
    return Location{
        .file = std::move(file), .line = found.line, .column = found.column};
}

std::size_t Document::entry_count(NodeId map) const {
    return node(map).children.size() / 2;
}

NodeId Document::key(NodeId map, std::size_t index) const {
    return node(map).children.at(2 * index);
}

NodeId Document::value(NodeId map, std::size_t index) const {
    return node(map).children.at((2 * index) + 1);
}

std::optional<NodeId> Document::find(NodeId map,
                                     std::string_view key_text) const {
    const std::size_t count = entry_count(map);
    for (std::size_t i = 0; i < count; ++i) {
        if (node(key(map, i)).text == key_text) {
            return value(map, i);
        }
    }
    return std::nullopt;
}

}  // namespace scada::conf
