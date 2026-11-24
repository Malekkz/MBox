#ifndef SCADA_CONF_NODE_H
#define SCADA_CONF_NODE_H

// The tree of parsed configuration files: maps, sequences and scalars, each
// with the file, line and column it came from (configuration schemas
// contract, sections 1.1, 1.2 and 1.4). The YAML subset parser builds it; the
// loader of the expanded file (section 13.3) is to build the same type.
//
// A Document is immutable once built: only a DocumentBuilder makes one.
// Reading a const Document from several threads at once is safe; no accessor
// changes anything, and there are no lazy caches.

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/conf/diagnostic.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
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

// The index of a file name in `Document::files()`.
using FileIndex = std::uint16_t;

// The most file names one Document can hold. A tree holds at most 20,000
// files (section 1.3).
constexpr std::size_t MAX_DOCUMENT_FILES = 65535;

// Where a node starts: its first character, or for a collection its first
// entry's indicator (`-`), key or bracket. Line and column are 1-based;
// columns count characters (D-106).
struct SourcePosition {
    FileIndex file = 0;
    std::uint32_t line = 1;
    std::uint32_t column = 1;
};

// One entry of a map. The key is always a scalar.
struct Entry {
    NodeId key = 0;
    NodeId value = 0;
};

// Owns the nodes of one or more parsed files. Nodes refer to each other by
// NodeId and never point into another Document.
//
// Memory: 20 bytes per node, 12 bytes per map entry, 4 bytes per sequence
// item, plus the bytes of every scalar's text and the file names.
//
// Every accessor taking a NodeId expects an id of this Document (below
// `size()`); any other id ends the process, as a programmer error that
// outside input cannot cause. No accessor throws. A Document that was moved
// from holds no nodes and must not be read: `kind(root())` ends the process.
//
// The type a schema gives a scalar (stage 3) is not stored here: the
// Document stays immutable, and stage 3 keeps its types beside it, by NodeId.
class Document {
public:
    Document(const Document&) = default;
    Document(Document&&) noexcept = default;
    Document& operator=(const Document&) = default;
    Document& operator=(Document&&) noexcept = default;
    ~Document() = default;

    // The root node. A built Document always has one.
    [[nodiscard]] NodeId root() const noexcept { return root_; }

    // The number of nodes. Ids run from 0 to `size() - 1`.
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

    [[nodiscard]] NodeKind kind(NodeId node) const noexcept;

    // How a scalar was written; PLAIN for a collection.
    [[nodiscard]] ScalarStyle style(NodeId node) const noexcept;

    // A scalar's value: the text as written for a plain scalar (lines folded
    // into one), the unquoted and unescaped text otherwise. Empty for a
    // collection. Valid as long as the Document.
    [[nodiscard]] std::string_view text(NodeId node) const noexcept;

    [[nodiscard]] SourcePosition position(NodeId node) const noexcept;

    // True if a scalar's text is its source characters, on the line of its
    // position: a plain or quoted scalar on one line with no escape, folded
    // break or doubled quote. The text then starts at the position's column,
    // or one column later for a quoted scalar, so a character of the text
    // has a column in the file (D-109). False for anything else.
    [[nodiscard]] bool verbatim(NodeId node) const noexcept;

    // The position of `node` with its file's name, for a diagnostic.
    [[nodiscard]] Location location(NodeId node) const;

    // The names of the files the nodes came from, by FileIndex.
    [[nodiscard]] std::span<const std::string> files() const noexcept {
        return files_;
    }

    // The number of entries of a map or items of a sequence; 0 for a scalar.
    [[nodiscard]] std::size_t entry_count(NodeId node) const noexcept;

    // A map's entries in the order of the file; empty for anything else.
    [[nodiscard]] std::span<const Entry> entries(NodeId node) const noexcept;

    // A sequence's items in order; empty for anything else.
    [[nodiscard]] std::span<const NodeId> items(NodeId node) const noexcept;

    // For a map: the value of the entry whose key text is `key`, if any.
    // Keys of one map are unique by text. O(log n) in the map's entries;
    // nothing for anything but a map.
    [[nodiscard]] std::optional<NodeId> find(
        NodeId map, std::string_view key) const noexcept;

private:
    friend class DocumentBuilder;

    // For a scalar, `first` and `count` are its text in `text_`; for a map,
    // its entries in `entries_` and `sorted_`; for a sequence, its items in
    // `items_`.
    struct Record {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
        std::uint32_t line = 1;
        std::uint32_t column = 1;
        FileIndex file = 0;
        NodeKind kind = NodeKind::SCALAR;
        ScalarStyle style = ScalarStyle::PLAIN;
    };

    Document() = default;

    [[nodiscard]] const Record& record(NodeId node) const noexcept;

    std::vector<std::string> files_;
    std::vector<Record> nodes_;
    std::string text_;
    std::vector<Entry> entries_;
    // For each map's entries, the indexes of its entries (from 0 within the
    // map) in byte order of their keys.
    std::vector<std::uint32_t> sorted_;
    std::vector<NodeId> items_;
    // One bit per node: `verbatim`.
    std::vector<bool> verbatim_;
    NodeId root_ = 0;
};

// Builds a Document. Nodes are added depth first: `begin_map` or
// `begin_sequence`, the children, then `end_map` or `end_sequence`. Inside a
// map, `key` and a value (a scalar or a collection) alternate. The first
// collection or scalar outside any collection is the root.
//
// The first misuse or limit makes every later call fail with the same status,
// and `finish` returns it. A builder is neither copied nor moved, and is used
// from one thread.
class DocumentBuilder {
public:
    DocumentBuilder() = default;
    DocumentBuilder(const DocumentBuilder&) = delete;
    DocumentBuilder(DocumentBuilder&&) = delete;
    DocumentBuilder& operator=(const DocumentBuilder&) = delete;
    DocumentBuilder& operator=(DocumentBuilder&&) = delete;
    ~DocumentBuilder() = default;

    // Adds a file name for SourcePosition::file. OUT_OF_RANGE past
    // MAX_DOCUMENT_FILES names.
    Result<FileIndex> add_file(std::string name);

    // Adds a scalar as a sequence item, a map's value or the root.
    // `verbatim`: its text is its source characters on one line (see
    // Document::verbatim).
    Result<NodeId> scalar(std::string_view text, ScalarStyle style,
                          SourcePosition where, bool verbatim = false);

    // Adds the key of the next entry of the innermost map. ALREADY_EXISTS if
    // the map has a key of the same text; the builder stays usable for no
    // other call then.
    Result<NodeId> key(std::string_view text, ScalarStyle style,
                       SourcePosition where);

    // Starts a map or a sequence, as a scalar would stand.
    Result<NodeId> begin_map(SourcePosition where);
    Result<NodeId> begin_sequence(SourcePosition where);

    // Ends the innermost collection, which must be of that kind; a map must
    // not wait for the value of its last key.
    Status end_map();
    Status end_sequence();

    // The number of nodes added so far.
    [[nodiscard]] std::size_t size() const noexcept {
        return document_.nodes_.size();
    }

    // The Document, once every collection is ended and a root exists.
    // The builder is empty afterwards.
    Result<Document> finish();

private:
    // Orders key nodes by their text, for the duplicate check.
    class KeyLess {
    public:
        explicit KeyLess(const Document* document) noexcept
            : document_(document) {}
        bool operator()(NodeId lhs, NodeId rhs) const noexcept {
            return document_->text(lhs) < document_->text(rhs);
        }

    private:
        const Document* document_;
    };

    // A collection being built.
    struct Frame {
        NodeId node = 0;
        NodeKind kind = NodeKind::MAP;
        // Where its children start in `pending_`.
        std::size_t first_pending = 0;
        bool expect_key = true;
        std::set<NodeId, KeyLess> keys{KeyLess(nullptr)};
    };

    Status fail(Status status) noexcept;
    Result<NodeId> add(NodeKind kind, ScalarStyle style, SourcePosition where,
                       std::string_view text);
    Status place_value(NodeId node) noexcept;
    Status end(NodeKind kind);

    Document document_;
    std::vector<Frame> open_;
    std::vector<NodeId> pending_;
    bool has_root_ = false;
    Status failure_;
};

}  // namespace scada::conf

#endif  // SCADA_CONF_NODE_H
