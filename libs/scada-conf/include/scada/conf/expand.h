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

#include <cstddef>
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
    // The scalars computed from a configuration expression (13.3), in the
    // order written, for the checks of their values at stage 5 (D-115).
    std::vector<NodeId> computed;
    // `limits.max_tags` of system.yaml, or its default: the tags of the tag
    // set, generated tags included (4.1, CF-1001), which stage 5 counts
    // (D-116).
    std::int64_t max_tags = DEFAULT_MAX_TAGS;

    static constexpr std::uint32_t NO_INSTANCE = 0xFFFFFFFF;
    static constexpr std::int64_t DEFAULT_MAX_TAGS = 50'000;
};

// True if `node` of `tree`, or a key or value below it, was written in its
// instance's own file (an override, an instance point). An error placed
// there is that instance's own, never one cause with a template's (D-114,
// D-115).
[[nodiscard]] bool written_in_instance(const ExpandedTree& tree, NodeId node);

// A diagnostic on `node` of `tree` (contract 1.4): at the node when it was
// written in its instance's own file or belongs to no instance; otherwise at
// the instance, with a note "from template <file>:<line>:<column>
// (<detail>)", the part in brackets only with a detail.
[[nodiscard]] Diagnostic expanded_diagnostic(const ExpandedTree& tree,
                                             std::uint16_t code, NodeId node,
                                             std::string message,
                                             std::string detail = {});

// The limits of the expanded tree (contract 1.3; decision D-114), each on
// a running total; passing one stops the expansion (CF-113). The defaults
// are the contract's; a test may give smaller ones to reach them cheaply.
// The contract's values (1.3): the nodes and text of a tree, and the
// diagnostics kept, twice the reported 1,000.
inline constexpr std::size_t MAX_EXPANDED_NODES = 4'000'000;
inline constexpr std::size_t MAX_EXPANDED_TEXT = std::size_t{256} << 20U;
inline constexpr std::size_t MAX_KEPT_ERRORS = 2 * Diagnostics::REPORTED_LIMIT;
inline constexpr std::size_t MAX_KEPT_RUN_TIME = 65536;
// The expression budget is 32 times the node limit (G1 of the review of
// #146): an expression node takes up to about 96 ns, so the worst accepted
// tree evaluates for about 13 s.
inline constexpr std::size_t EXPRESSION_WORK_FACTOR = 32;
inline constexpr std::size_t MAX_EXPRESSION_WORK =
    EXPRESSION_WORK_FACTOR * MAX_EXPANDED_NODES;

// The work of matching the patterns of run-time expressions at stage 5
// (D-117, item 1 of the delta check of #163): 32 units per name visited and
// 1 per step of the matcher, at most 2e10 units, about 13 s as the
// expression budget of stage 4.
inline constexpr std::uint64_t RUN_TIME_NAME_WORK = 32;
inline constexpr std::uint64_t MAX_RUN_TIME_PATTERN_WORK = 20'000'000'000;

struct ExpandLimits {
    std::size_t nodes = MAX_EXPANDED_NODES;
    std::size_t text_bytes = MAX_EXPANDED_TEXT;
    // Points made, written or generated, kept or not, and `repeat` values
    // evaluated.
    std::size_t work = MAX_EXPANDED_NODES;
    // Expression nodes evaluated, and the nodes of every parse and typing:
    // 32 times the node limit (G1 of the review of #146).
    std::size_t expression_work = MAX_EXPRESSION_WORK;
    // Diagnostics kept; past it an error is counted only.
    std::size_t kept_errors = MAX_KEPT_ERRORS;
    // At stage 5, the summaries of run-time patterns and the results of
    // run-time expression texts kept, each; past it a pattern is resolved,
    // and a text checked, at each use (D-117).
    std::size_t kept_run_time = MAX_KEPT_RUN_TIME;
    // At stage 5, the work of matching run-time patterns (CF-113).
    std::uint64_t run_time_pattern_work = MAX_RUN_TIME_PATTERN_WORK;
};

// What the expansion held to report its errors, at its end: the texts that
// suppress a repeated diagnostic and the causes met at several instances.
// Both are at most `ExpandLimits::kept_errors`; for tests of that bound.
struct ExpandCounts {
    std::size_t kept_texts = 0;
    std::size_t causes = 0;
    // The parses and typings of configuration expressions: one parse per
    // expression of the source while the cache of parsed expressions holds
    // them, a typing again only when a name changes type.
    std::size_t parses = 0;
    std::size_t typings = 0;
    // The bytes of text written into the document when the expansion
    // ended: past the text limit, at most one point more than the limit.
    std::size_t written_text = 0;
};

// A computed value (13.3) is a scalar of style PLAIN with its canonical
// text; its type is in `types`, so a computed STRING such as `12` or `true`
// must be written by its type, quoted, never by its style (T5 of the
// review of #146): the writer of the expanded files (#102) goes by `types`.
//
// Expands every device instance of `files`, which passed stage 3. Errors go
// to `diagnostics`. A template's own faults (its chain, its parameters'
// types, two written points of one name, a base template's addresses,
// `when` in its defaults) are at the template, once, whether or not an
// instance uses it, and its instances are not expanded. An error that a
// template's line causes in an instance is located at the instance, with a
// note "from template <file>:<line>:<column> (<detail>)" (1.4); one cause
// met by several instances is one error at the first, "(and N more
// instances)". A device whose template the tree does not have, or whose
// chain names one, is left out without an error: the missing name is CF-310
// of stage 5. Nothing when an error was found.
[[nodiscard]] std::optional<ExpandedTree> expand_tree(
    std::span<const CheckedFile> files, Diagnostics& diagnostics,
    const ExpandLimits& limits = {}, ExpandCounts* counts = nullptr);

}  // namespace scada::conf

#endif  // SCADA_CONF_EXPAND_H
