#ifndef SCADA_CONF_TESTS_EXPANDED_TEXT_H
#define SCADA_CONF_TESTS_EXPANDED_TEXT_H

// The expanded points of a tree as text, one line per point, for the unit
// tests and the corpus (contract 14, test 6).

#include "scada/conf/expand.h"
#include "scada/conf/node.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::conf {

// A value of the expanded tree in flow style, as one line.
// NOLINTNEXTLINE(misc-no-recursion): bounded by the nesting limit of 32
inline std::string flow(const Document& document, NodeId node) {
    switch (document.kind(node)) {
        case NodeKind::SCALAR:
            return std::string(document.text(node));
        case NodeKind::SEQUENCE: {
            std::string out = "[";
            for (const NodeId item : document.items(node)) {
                out += (out.size() > 1 ? ", " : "") + flow(document, item);
            }
            return out + "]";
        }
        case NodeKind::MAP: {
            std::string out = "{";
            for (const Entry& entry : document.entries(node)) {
                out += (out.size() > 1 ? ", " : "") +
                       std::string(document.text(entry.key)) + ": " +
                       flow(document, entry.value);
            }
            return out + "}";
        }
    }
    return {};
}

// The value of `key` in the map `node`, which the expanded tree always has.
inline NodeId child(const Document& document, NodeId node,
                    std::string_view key) {
    return document.find(node, key).value_or(node);
}

// `{<key>: <value>, ...}` of a map, without the keys in `skipped`.
inline std::string keys_of(const Document& document, NodeId map,
                           std::string_view skipped,
                           std::string_view also_skipped = {}) {
    std::string keys = "{";
    for (const Entry& entry : document.entries(map)) {
        const std::string_view key = document.text(entry.key);
        if (key == skipped || key == also_skipped) {
            continue;
        }
        if (keys.size() > 1) {
            keys += ", ";
        }
        keys += key;
        keys += ": ";
        keys += flow(document, entry.value);
    }
    return keys + "}";
}

// Each expanded point as `<device>.<point>: {<keys but name>}`; with
// `with_devices`, each device first as `<device>: {<its keys but name and
// points>}`.
inline std::vector<std::string> points_of(const ExpandedTree& tree,
                                          bool with_devices = false) {
    const Document& document = tree.document;
    std::vector<std::string> lines;
    const NodeId channels = child(document, document.root(), "channels");
    for (const NodeId channel : document.items(channels)) {
        for (const NodeId device :
             document.items(child(document, channel, "devices"))) {
            const std::string prefix(
                document.text(child(document, device, "name")));
            if (with_devices) {
                lines.push_back(prefix + ": " +
                                keys_of(document, device, "name", "points"));
            }
            for (const NodeId point :
                 document.items(child(document, device, "points"))) {
                std::string line = prefix + ".";
                line += document.text(child(document, point, "name"));
                line += ": ";
                line += keys_of(document, point, "name");
                lines.push_back(std::move(line));
            }
        }
    }
    return lines;
}

}  // namespace scada::conf

#endif  // SCADA_CONF_TESTS_EXPANDED_TEXT_H
