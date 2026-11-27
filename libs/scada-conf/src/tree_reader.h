#ifndef SCADA_CONF_SRC_TREE_READER_H
#define SCADA_CONF_SRC_TREE_READER_H

// Reading the expanded tree and the files of a tree, and placing the errors
// of stage 5 on them (D-114, D-116), for the checks that look tags up.

#include "grouped_errors.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/node.h"

#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace scada::conf {

// Reads one Document: the expanded tree or a file of the tree.
class Reader {
public:
    explicit Reader(const Document& document) : document_(&document) {}

    [[nodiscard]] const Document& document() const { return *document_; }

    [[nodiscard]] std::optional<NodeId> child(NodeId map,
                                              std::string_view key) const {
        if (document_->kind(map) != NodeKind::MAP) {
            return std::nullopt;
        }
        return document_->find(map, key);
    }

    // The scalar at `key` of `map`, or nothing.
    [[nodiscard]] std::optional<NodeId> scalar(NodeId map,
                                               std::string_view key) const {
        const std::optional<NodeId> node = child(map, key);
        if (!node || document_->kind(*node) != NodeKind::SCALAR) {
            return std::nullopt;
        }
        return node;
    }

    // The text of the scalar at `key` of `map`, a view into the Document;
    // empty if there is none.
    [[nodiscard]] std::string_view text_of(NodeId map,
                                           std::string_view key) const {
        const std::optional<NodeId> node = scalar(map, key);
        return node ? document_->text(*node) : std::string_view();
    }

    // The items of the sequence at `key` of `map`; none if it is not one.
    [[nodiscard]] std::span<const NodeId> items(NodeId map,
                                                std::string_view key) const {
        const std::optional<NodeId> node = child(map, key);
        if (!node || document_->kind(*node) != NodeKind::SEQUENCE) {
            return {};
        }
        return document_->items(*node);
    }

    // The entries of the map at `key` of `map`; none if it is not one.
    [[nodiscard]] std::span<const Entry> entries(NodeId map,
                                                 std::string_view key) const {
        const std::optional<NodeId> node = child(map, key);
        if (!node || document_->kind(*node) != NodeKind::MAP) {
            return {};
        }
        return document_->entries(*node);
    }

private:
    const Document* document_;
};

// The errors of stage 5 at the nodes of one tree (D-114, D-116): an error
// on the expanded tree is at its instance with the template note, one
// cause at many instances is one error, and an error that the instance's
// own file contributes to is never grouped. An error of another file goes
// to the Diagnostics at once, which keep the first of the sorted order
// (contract 1.4) however many the expanded tree has.
class Reporter {
public:
    Reporter(const ExpandedTree& tree, Diagnostics& diagnostics)
        : tree_(&tree), diagnostics_(&diagnostics), grouped_(MAX_KEPT_ERRORS) {}

    // An error at `node` of `document`. On the expanded tree, `detail` is
    // the note's detail ("point flow") and `device` the instance's name.
    // `key`, when given, names the cause in place of the message.
    void error(const Document& document, NodeId node, std::uint16_t code,
               std::string message, std::string_view detail = {},
               std::string_view device = {}, std::string_view key = {}) {
        if (&document != &tree_->document) {
            diagnostics_->error(code, document.location(node),
                                std::move(message));
            return;
        }
        Diagnostic diagnostic = expanded_diagnostic(
            *tree_, code, node, std::move(message), std::string(detail));
        std::string cause = key.empty()
                                ? instance_free_key(diagnostic.message, device)
                                : std::string(key);
        // "first at <the instance's own place>" names the instance too.
        if (const std::string own = own_place(node);
            !own.empty() && cause.ends_with(own)) {
            cause.replace(cause.size() - own.size(), own.size(), "<instance>");
        }
        grouped_.add(std::move(diagnostic), cause, device, *diagnostics_,
                     !written_in_instance(*tree_, node));
    }

    // A warning at `node` of `document`. On the expanded tree it is placed
    // like an error, and a template's line gives it once, at the first
    // instance met (D-119): warnings are not counted with errors.
    void warning(const Document& document, NodeId node, std::uint16_t code,
                 std::string message, std::string_view detail = {}) {
        if (&document != &tree_->document) {
            diagnostics_->warning(code, document.location(node),
                                  std::move(message));
            return;
        }
        Diagnostic diagnostic = expanded_diagnostic(
            *tree_, code, node, std::move(message), std::string(detail));
        diagnostic.severity = Severity::WARNING;
        if (!written_in_instance(*tree_, node) && !diagnostic.notes.empty() &&
            diagnostic.notes.front().kind == "template") {
            const Location& site = diagnostic.notes.front().location;
            if (!warned_
                     .insert(std::to_string(code) + '|' + site.file + ':' +
                             std::to_string(site.line) + ':' +
                             std::to_string(site.column))
                     .second) {
                return;
            }
        }
        diagnostics_->add(std::move(diagnostic));
    }

    // An error with no node: CF-1001 past the process tags of a tree
    // without system.yaml.
    void error_at(Location location, std::uint16_t code, std::string message) {
        diagnostics_->error(code, std::move(location), std::move(message));
    }

    // Where an error at `node` of `document` is reported, for "first at":
    // on the expanded tree, the instance's place.
    [[nodiscard]] Location place_of(const Document& document,
                                    NodeId node) const {
        if (&document == &tree_->document) {
            return expanded_diagnostic(*tree_, 0, node, {}).location;
        }
        return document.location(node);
    }

    void flush() { grouped_.flush(*diagnostics_); }

private:
    // "<file>:<line>" of the instance `node` belongs to, or nothing.
    [[nodiscard]] std::string own_place(NodeId node) const {
        if (node >= tree_->instance_of.size() ||
            tree_->instance_of[node] >= tree_->instances.size()) {
            return {};
        }
        const Location& instance = tree_->instances[tree_->instance_of[node]];
        return instance.file + ":" + std::to_string(instance.line);
    }

    const ExpandedTree* tree_;
    Diagnostics* diagnostics_;
    GroupedErrors grouped_;
    std::set<std::string> warned_;  // the template sites warned at
};

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_TREE_READER_H
