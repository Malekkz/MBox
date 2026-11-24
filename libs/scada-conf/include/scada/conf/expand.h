#ifndef SCADA_CONF_EXPAND_H
#define SCADA_CONF_EXPAND_H

// Stage 4 of validation: the expansion of templates and device instances
// (configuration schemas contract 4.4 and 4.5; decision D-114). Each
// instance becomes its final list of points, in the merge order of 4.5,
// with every value keeping the file and line it came from.

#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/conf/references.h"
#include "scada/conf/types.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace scada::conf {

// The expanded channels of a tree, as one Document of the one tree type
// (D-111):
//
//   channels:
//     - protocol: <protocol>        (the channel file's directory)
//       file: <path of the channel file>
//       channel: <the channel object as written>
//       devices:
//         - <the instance's own keys, the template's device keys (`read`,
//           `enums`, `bit_order`, `functions`, `device_time`, `keepalive`,
//           `idle_timeout`, `on_mismatch`, `identify`, `health`,
//           `restart_indication`) under them>
//           points: [ <each point as merged, `when` removed> ]
//
// The keys that drive expansion (`params`, `defaults`, `overrides`,
// `disable`, `only`) and every `replace` are gone. A scalar keeps its source
// text, style, position and stage 3 type; a value a configuration
// expression computed has its canonical text (13.3), its type, and the
// position of the expression. Positions index `document.files()`, which
// holds the paths of the tree's files.
struct ExpandedTree {
    Document document;
    ScalarTypes types;
    // For each node, the index in `instances` of the device it belongs to,
    // or NO_INSTANCE for a node outside a device.
    std::vector<std::uint32_t> instance_of;
    // The place of each device instance: its `name` key.
    std::vector<Location> instances;

    static constexpr std::uint32_t NO_INSTANCE = 0xFFFFFFFF;
};

// A diagnostic on `node` of `tree` (contract 1.4): at the node when it was
// written in its instance's own file or belongs to no instance; otherwise at
// the instance, with a note "from template <file>:<line>:<column>
// (<detail>)", the part in brackets only with a detail.
[[nodiscard]] Diagnostic expanded_diagnostic(const ExpandedTree& tree,
                                             std::uint16_t code, NodeId node,
                                             std::string message,
                                             std::string detail = {});

// Expands every device instance of `files`, which passed stage 3. Errors go
// to `diagnostics`; one that a template's line causes is located at the
// instance, with a note "from template <file>:<line>:<column>" (1.4). A
// device whose template the tree does not have, or whose chain names one,
// is left out without an error: the missing name is CF-310 of stage 5.
// Nothing when an error was found.
[[nodiscard]] std::optional<ExpandedTree> expand_tree(
    std::span<const CheckedFile> files, Diagnostics& diagnostics);

}  // namespace scada::conf

#endif  // SCADA_CONF_EXPAND_H
