#include "scada/conf/node.h"

#include "scada/common/panic.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/conf/diagnostic.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::conf {

namespace {

// The size of a node record, which node.h and the README state.
constexpr std::size_t RECORD_BYTES = 20;

constexpr std::size_t MAX_INDEX = std::numeric_limits<std::uint32_t>::max();

}  // namespace

// --- Document ------------------------------------------------------------

const Document::Record& Document::record(NodeId node) const noexcept {
    // The memory figure of node.h and the README.
    static_assert(sizeof(Record) == RECORD_BYTES, "a node is 20 bytes");
    if (node >= nodes_.size()) {
        panic("Document: a node id of another document");
    }
    return nodes_[node];
}

NodeKind Document::kind(NodeId node) const noexcept {
    return record(node).kind;
}

ScalarStyle Document::style(NodeId node) const noexcept {
    return record(node).style;
}

std::string_view Document::text(NodeId node) const noexcept {
    const Record& found = record(node);
    if (found.kind != NodeKind::SCALAR) {
        return {};
    }
    return std::string_view(text_).substr(found.first, found.count);
}

SourcePosition Document::position(NodeId node) const noexcept {
    const Record& found = record(node);
    return SourcePosition{
        .file = found.file, .line = found.line, .column = found.column};
}

bool Document::verbatim(NodeId node) const noexcept {
    static_cast<void>(record(node));
    return verbatim_[node];
}

Location Document::location(NodeId node) const {
    const Record& found = record(node);
    std::string file;
    if (found.file < files_.size()) {
        file = files_[found.file];
    }
    return Location{
        .file = std::move(file), .line = found.line, .column = found.column};
}

std::size_t Document::entry_count(NodeId node) const noexcept {
    const Record& found = record(node);
    return found.kind == NodeKind::SCALAR ? 0 : found.count;
}

std::span<const Entry> Document::entries(NodeId node) const noexcept {
    const Record& found = record(node);
    if (found.kind != NodeKind::MAP) {
        return {};
    }
    return std::span<const Entry>(entries_).subspan(found.first, found.count);
}

std::span<const NodeId> Document::items(NodeId node) const noexcept {
    const Record& found = record(node);
    if (found.kind != NodeKind::SEQUENCE) {
        return {};
    }
    return std::span<const NodeId>(items_).subspan(found.first, found.count);
}

std::optional<NodeId> Document::find(NodeId map,
                                     std::string_view key) const noexcept {
    const Record& found = record(map);
    if (found.kind != NodeKind::MAP) {
        return std::nullopt;
    }
    const std::span<const Entry> all = entries(map);
    const std::span<const std::uint32_t> order =
        std::span<const std::uint32_t>(sorted_).subspan(found.first,
                                                        found.count);
    const auto position = std::ranges::lower_bound(
        order, key, {},
        [this, all](std::uint32_t index) { return text(all[index].key); });
    if (position == order.end() || text(all[*position].key) != key) {
        return std::nullopt;
    }
    return all[*position].value;
}

// --- DocumentBuilder -----------------------------------------------------

Status DocumentBuilder::fail(Status status) noexcept {
    if (failure_.ok()) {
        failure_ = status;
    }
    return failure_;
}

Result<FileIndex> DocumentBuilder::add_file(std::string name) {
    if (!failure_.ok()) {
        return failure_;
    }
    if (document_.files_.size() >= MAX_DOCUMENT_FILES) {
        return fail(Status(StatusCode::OUT_OF_RANGE,
                           "DocumentBuilder: too many files"));
    }
    document_.files_.push_back(std::move(name));
    return static_cast<FileIndex>(document_.files_.size() - 1);
}

// Places a finished node where the builder stands: the root, a sequence
// item or a map's value.
Status DocumentBuilder::place_value(NodeId node) noexcept {
    if (open_.empty()) {
        if (has_root_) {
            return fail(Status(StatusCode::FAILED_PRECONDITION,
                               "DocumentBuilder: a second root"));
        }
        has_root_ = true;
        document_.root_ = node;
        return {};
    }
    Frame& frame = open_.back();
    if (frame.kind == NodeKind::MAP) {
        if (frame.expect_key) {
            return fail(Status(StatusCode::FAILED_PRECONDITION,
                               "DocumentBuilder: a value where a key is due"));
        }
        frame.expect_key = true;
    }
    pending_.push_back(node);
    return {};
}

Result<NodeId> DocumentBuilder::add(NodeKind kind, ScalarStyle style,
                                    SourcePosition where,
                                    std::string_view text) {
    if (!failure_.ok()) {
        return failure_;
    }
    if (document_.nodes_.size() >= MAX_INDEX ||
        document_.text_.size() + text.size() > MAX_INDEX) {
        return fail(Status(StatusCode::RESOURCE_EXHAUSTED,
                           "DocumentBuilder: the document is too large"));
    }
    if (where.file >= document_.files_.size()) {
        return fail(Status(StatusCode::INVALID_ARGUMENT,
                           "DocumentBuilder: an unknown file index"));
    }
    const auto node = static_cast<NodeId>(document_.nodes_.size());
    Document::Record record{.first = 0,
                            .count = 0,
                            .line = where.line,
                            .column = where.column,
                            .file = where.file,
                            .kind = kind,
                            .style = style};
    if (kind == NodeKind::SCALAR) {
        record.first = static_cast<std::uint32_t>(document_.text_.size());
        record.count = static_cast<std::uint32_t>(text.size());
        document_.text_.append(text);
    }
    document_.nodes_.push_back(record);
    document_.verbatim_.push_back(false);
    return node;
}

Result<NodeId> DocumentBuilder::scalar(std::string_view text, ScalarStyle style,
                                       SourcePosition where, bool verbatim) {
    const Result<NodeId> node = add(NodeKind::SCALAR, style, where, text);
    if (!node.ok()) {
        return node;
    }
    document_.verbatim_[node.value()] = verbatim;
    if (const Status placed = place_value(node.value()); !placed.ok()) {
        return placed;
    }
    return node;
}

Result<NodeId> DocumentBuilder::key(std::string_view text, ScalarStyle style,
                                    SourcePosition where) {
    if (!failure_.ok()) {
        return failure_;
    }
    if (open_.empty() || open_.back().kind != NodeKind::MAP ||
        !open_.back().expect_key) {
        return fail(Status(StatusCode::FAILED_PRECONDITION,
                           "DocumentBuilder: a key where none is due"));
    }
    const Result<NodeId> node = add(NodeKind::SCALAR, style, where, text);
    if (!node.ok()) {
        return node;
    }
    Frame& frame = open_.back();
    if (!frame.keys.insert(node.value()).second) {
        return fail(Status(StatusCode::ALREADY_EXISTS,
                           "DocumentBuilder: a duplicate key"));
    }
    frame.expect_key = false;
    pending_.push_back(node.value());
    return node;
}

Result<NodeId> DocumentBuilder::begin_map(SourcePosition where) {
    const Result<NodeId> node =
        add(NodeKind::MAP, ScalarStyle::PLAIN, where, {});
    if (!node.ok()) {
        return node;
    }
    if (const Status placed = place_value(node.value()); !placed.ok()) {
        return placed;
    }
    open_.push_back(
        Frame{.node = node.value(),
              .kind = NodeKind::MAP,
              .first_pending = pending_.size(),
              .expect_key = true,
              .keys = std::set<NodeId, KeyLess>(KeyLess(&document_))});
    return node;
}

Result<NodeId> DocumentBuilder::begin_sequence(SourcePosition where) {
    const Result<NodeId> node =
        add(NodeKind::SEQUENCE, ScalarStyle::PLAIN, where, {});
    if (!node.ok()) {
        return node;
    }
    if (const Status placed = place_value(node.value()); !placed.ok()) {
        return placed;
    }
    open_.push_back(
        Frame{.node = node.value(),
              .kind = NodeKind::SEQUENCE,
              .first_pending = pending_.size(),
              .expect_key = false,
              .keys = std::set<NodeId, KeyLess>(KeyLess(&document_))});
    return node;
}

Status DocumentBuilder::end(NodeKind kind) {
    if (!failure_.ok()) {
        return failure_;
    }
    if (open_.empty() || open_.back().kind != kind ||
        (kind == NodeKind::MAP && !open_.back().expect_key)) {
        return fail(Status(StatusCode::FAILED_PRECONDITION,
                           "DocumentBuilder: no such collection to end"));
    }
    const Frame& frame = open_.back();
    const std::span<const NodeId> children =
        std::span<const NodeId>(pending_).subspan(frame.first_pending);
    Document::Record& record = document_.nodes_[frame.node];
    if (kind == NodeKind::SEQUENCE) {
        if (document_.items_.size() + children.size() > MAX_INDEX) {
            return fail(Status(StatusCode::RESOURCE_EXHAUSTED,
                               "DocumentBuilder: the document is too large"));
        }
        record.first = static_cast<std::uint32_t>(document_.items_.size());
        record.count = static_cast<std::uint32_t>(children.size());
        document_.items_.insert(document_.items_.end(), children.begin(),
                                children.end());
    } else {
        const std::size_t count = children.size() / 2;
        if (document_.entries_.size() + count > MAX_INDEX) {
            return fail(Status(StatusCode::RESOURCE_EXHAUSTED,
                               "DocumentBuilder: the document is too large"));
        }
        const std::size_t first = document_.entries_.size();
        record.first = static_cast<std::uint32_t>(first);
        record.count = static_cast<std::uint32_t>(count);
        for (std::size_t i = 0; i < count; ++i) {
            document_.entries_.push_back(
                Entry{.key = children[2 * i], .value = children[(2 * i) + 1]});
            document_.sorted_.push_back(static_cast<std::uint32_t>(i));
        }
        const std::span<const Entry> entries =
            std::span<const Entry>(document_.entries_).subspan(first, count);
        std::ranges::sort(
            std::span<std::uint32_t>(document_.sorted_).subspan(first, count),
            {}, [this, entries](std::uint32_t index) {
                return document_.text(entries[index].key);
            });
    }
    pending_.resize(frame.first_pending);
    open_.pop_back();
    return {};
}

Status DocumentBuilder::end_map() {
    return end(NodeKind::MAP);
}

Status DocumentBuilder::end_sequence() {
    return end(NodeKind::SEQUENCE);
}

Result<Document> DocumentBuilder::finish() {
    if (!failure_.ok()) {
        return failure_;
    }
    if (!open_.empty() || !has_root_) {
        return fail(Status(StatusCode::FAILED_PRECONDITION,
                           "DocumentBuilder: the document is not complete"));
    }
    Document done = std::move(document_);
    document_ = Document();
    has_root_ = false;
    pending_.clear();
    return done;
}

}  // namespace scada::conf
