#ifndef SCADA_CONF_TESTS_EXPANDED_TEXT_H
#define SCADA_CONF_TESTS_EXPANDED_TEXT_H

// The expanded points of a tree as text, one line per point, for the unit
// tests and the corpus (contract 14, test 6).

#include "scada/conf/expand.h"
#include "scada/conf/node.h"

#include <string>
#include <string_view>
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

// Each expanded point as `<device>.<point>: {<keys but name>}`; with
// `with_devices`, each device first as `<device>: {<its keys but name and
// points>}`.
inline std::vector<std::string> points_of(const ExpandedTree& tree,
                                          bool with_devices = false) {
    const Document& document = tree.document;
    std::vector<std::string> lines;
    const NodeId channels = *document.find(document.root(), "channels");
    for (const NodeId channel : document.items(channels)) {
        const NodeId devices = *document.find(channel, "devices");
        for (const NodeId device : document.items(devices)) {
            const std::string prefix(
                document.text(*document.find(device, "name")));
            if (with_devices) {
                std::string keys;
                for (const Entry& entry : document.entries(device)) {
                    const std::string_view key = document.text(entry.key);
                    if (key == "name" || key == "points") {
                        continue;
                    }
                    keys += (keys.empty() ? "" : ", ") + std::string(key) +
                            ": " + flow(document, entry.value);
                }
                lines.push_back(prefix + ": {" + keys + "}");
            }
            for (const NodeId point :
                 document.items(*document.find(device, "points"))) {
                std::string keys;
                for (const Entry& entry : document.entries(point)) {
                    if (document.text(entry.key) == "name") {
                        continue;
                    }
                    keys += (keys.empty() ? "" : ", ") +
                            std::string(document.text(entry.key)) + ": " +
                            flow(document, entry.value);
                }
                lines.push_back(
                    prefix + "." +
                    std::string(document.text(*document.find(point, "name"))) +
                    ": {" + keys + "}");
            }
        }
    }
    return lines;
}

}  // namespace scada::conf

#endif  // SCADA_CONF_TESTS_EXPANDED_TEXT_H
