// Stage 5 on run-time expressions (decision D-117). Virtual tags are typed
// first, in the order their inputs need, so that every later expression
// reads their derived types; a cycle among them is reported once and its
// tags stay of unknown type, which ends the checks of what reads them
// without a second error.

#include "runtime_expressions.h"

#include "expression_site.h"
#include "expression_types.h"
#include "grouped_errors.h"
#include "name_pattern.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/expression.h"
#include "scada/conf/node.h"
#include "scada/conf/references.h"
#include "scada/conf/scalar.h"
#include "scada/conf/tree.h"
#include "scada/conf/validate.h"
#include "tag_names.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::conf {

namespace {

constexpr std::uint16_t CF_SIZE_LIMIT = 113;
constexpr std::uint16_t CF_NO_SUCH_NAME = 310;
constexpr std::uint16_t CF_TYPE_MISMATCH = 523;
constexpr std::uint16_t CF_CYCLE = 525;
constexpr std::uint16_t CF_DERIVED_TYPE = 527;

// The tasks that exist without `control/tasks.yaml` (9.1), in ms.
constexpr std::int64_t FAST_MS = 10;
constexpr std::int64_t NORMAL_MS = 100;
constexpr std::int64_t SLOW_MS = 1000;

// The task of a virtual tag or an interlock without `task` (5.1, 7.3).
constexpr std::string_view DEFAULT_TASK = "normal";

// The bytes of text the kept summaries and results hold at most (S-1 of the
// delta check of #163): the pattern texts and the names and messages they
// keep. Past it, a pattern is resolved, and a text checked, at each use.
constexpr std::size_t MAX_KEPT_BYTES = std::size_t{16} << 20U;

// How a tag of `value_type` (4.7) is read: UINT64 as INT (5.6); a virtual
// tag not typed yet has none.
TagRead read_of(std::string_view value_type) {
    if (value_type.empty()) {
        return TagRead::UNKNOWN;
    }
    if (value_type == "bool") {
        return TagRead::BOOL;
    }
    if (value_type == "string") {
        return TagRead::STRING;
    }
    if (value_type == "float64") {
        return TagRead::REAL;
    }
    return TagRead::INT;
}

// The value type a virtual tag derives from its expression (5.6): BOOL
// gives bool, INT int64, REAL float64, TIME int64 milliseconds.
// A constant, as TagNames holds a view of it (D-116).
std::string_view derived_value_type(ValueType type) {
    switch (type) {
        case ValueType::BOOL:
            return "bool";
        case ValueType::REAL:
            return "float64";
        default:
            return "int64";
    }
}

// The bytes of a name pattern before its first wildcard: every name it
// matches starts with them.
std::size_t prefix_of(std::string_view pattern) {
    return std::min(pattern.find_first_of("*?"), pattern.size());
}

// The scalar at `key` of `map` in `document`, or nothing.
std::optional<NodeId> scalar_at(const Document& document, NodeId map,
                                std::string_view key) {
    if (document.kind(map) != NodeKind::MAP) {
        return std::nullopt;
    }
    const std::optional<NodeId> node = document.find(map, key);
    if (!node || document.kind(*node) != NodeKind::SCALAR) {
        return std::nullopt;
    }
    return node;
}

std::string text_at(const Document& document, NodeId map,
                    std::string_view key) {
    const std::optional<NodeId> node = scalar_at(document, map, key);
    return node ? std::string(document.text(*node)) : std::string();
}

// As `text_at`, a view into `document`.
std::string_view view_at(const Document& document, NodeId map,
                         std::string_view key) {
    const std::optional<NodeId> node = scalar_at(document, map, key);
    return node ? document.text(*node) : std::string_view();
}

// The items of the sequence at `key` of `map`; none if it is not one.
std::span<const NodeId> items_at(const Document& document, NodeId map,
                                 std::string_view key) {
    if (document.kind(map) != NodeKind::MAP) {
        return {};
    }
    const std::optional<NodeId> node = document.find(map, key);
    if (!node || document.kind(*node) != NodeKind::SEQUENCE) {
        return {};
    }
    return document.items(*node);
}

// One run-time expression where it stands.
struct Site {
    const Document* document = nullptr;
    NodeId scalar = 0;
    // The task that runs it, empty where none does (an alarm): a view into
    // the document, or DEFAULT_TASK.
    std::string_view task;
    // Its result must be BOOL: an alarm's `expr`, `suppress_when`, an
    // interlock's `condition`.
    bool needs_bool = false;
    // In the expanded tree: the device that holds it, and the note's detail.
    bool expanded = false;
    std::string device;
    std::string detail;
};

// A virtual tag or an interlock: a vertex of the graph of what reads what
// (an interlock is evaluated "like a virtual tag", 7.3; R1 of the review
// of #163).
struct Named {
    std::string name;
    const CheckedFile* file = nullptr;
    NodeId name_node = 0;
    bool is_virtual = true;
    std::optional<NodeId> value_type;
    Site site;
    std::optional<Expression> tree;
    // The vertices it reads: a virtual tag or an interlock by index, a
    // pattern by its own vertex; sorted.
    std::vector<std::size_t> inputs;
};

// A distinct name pattern that a virtual tag or an interlock reads: a
// vertex whose inputs are the names it matches, walked when needed from
// the sorted names, never stored (M1 of the review of #163).
struct Pattern {
    std::string text;
    std::size_t prefix = 0;  // the bytes before its first wildcard
};

// Tarjan's strongly connected components, iteratively (no recursion: a
// tree may hold many virtual tags). Each component is complete before any
// that reads it, so the virtual tags are typed in the order their inputs
// need. `next(node, cursor)` gives a node's next input, from a cursor that
// starts at 0, or nothing past the last: inputs may be made as they are
// walked.
class Components {
public:
    using Next =
        std::function<std::optional<std::size_t>(std::size_t, std::size_t&)>;

    Components(std::size_t size, Next next)
        : next_input_(std::move(next)),
          index_(size, UNSEEN),
          low_(size, 0),
          on_stack_(size, false) {}

    std::vector<std::vector<std::size_t>> run() {
        for (std::size_t start = 0; start < index_.size(); ++start) {
            if (index_[start] == UNSEEN) {
                walk_from(start);
            }
        }
        return std::move(out_);
    }

private:
    static constexpr std::size_t UNSEEN =
        std::numeric_limits<std::size_t>::max();

    void walk_from(std::size_t start) {
        walk_.emplace_back(start, 0);
        while (!walk_.empty()) {
            auto& [node, cursor] = walk_.back();
            if (index_[node] == UNSEEN) {
                open(node);
            }
            if (const std::optional<std::size_t> next =
                    next_input_(node, cursor)) {
                const std::size_t input = *next;
                if (index_[input] == UNSEEN) {
                    walk_.emplace_back(input, 0);
                } else if (on_stack_[input]) {
                    low_[node] = std::min(low_[node], index_[input]);
                }
                continue;
            }
            close(node);
        }
    }

    void open(std::size_t node) {
        index_[node] = low_[node] = next_++;
        stack_.push_back(node);
        on_stack_[node] = true;
    }

    void close(std::size_t done) {
        walk_.pop_back();
        if (!walk_.empty()) {
            const std::size_t parent = walk_.back().first;
            low_[parent] = std::min(low_[parent], low_[done]);
        }
        if (low_[done] != index_[done]) {
            return;
        }
        std::vector<std::size_t> component;
        std::size_t member = UNSEEN;
        while (member != done) {
            member = stack_.back();
            stack_.pop_back();
            on_stack_[member] = false;
            component.push_back(member);
        }
        std::ranges::sort(component);
        out_.push_back(std::move(component));
    }

    Next next_input_;
    std::vector<std::size_t> index_;
    std::vector<std::size_t> low_;
    std::vector<bool> on_stack_;
    std::vector<std::size_t> stack_;
    // The walk: a node and the cursor of its next input.
    std::vector<std::pair<std::size_t, std::size_t>> walk_;
    std::vector<std::vector<std::size_t>> out_;
    std::size_t next_ = 0;
};

class Checker {
public:
    Checker(TagNames& tags, const ExpandedTree& tree,
            std::span<const CheckedFile> files, Diagnostics& diagnostics,
            std::size_t kept, std::uint64_t pattern_work, RunTimeCounts* counts)
        : tags_(&tags),
          tree_(&tree),
          files_(files),
          diagnostics_(&diagnostics),
          kept_(kept),
          pattern_budget_(pattern_work),
          counts_out_(counts),
          grouped_(MAX_KEPT_ERRORS) {}

    void run() {
        // Past `limits.max_tags` the tags are not whole (CF-1001): nothing
        // is looked up, as for the references of D-116 (M4 of the review).
        if (!tags_->complete()) {
            return;
        }
        read_tasks();
        collect_named();
        type_virtuals();
        for (const CheckedFile& file : files_) {
            check_file(file);
        }
        check_expanded();
        grouped_.flush(*diagnostics_);
        if (counts_out_ != nullptr) {
            *counts_out_ = counts_;
        }
    }

private:
    // --- Tasks --------------------------------------------------------------

    void read_tasks() {
        periods_ = {
            {"fast", FAST_MS}, {"normal", NORMAL_MS}, {"slow", SLOW_MS}};
        for (const CheckedFile& file : files_) {
            if (file.source->path_class.type != FileType::TASKS) {
                continue;
            }
            // A tasks file replaces the default tasks (9.1).
            periods_.clear();
            tasks_given_ = true;
            const Document& document = *file.document;
            const std::optional<NodeId> tasks =
                document.find(document.root(), "tasks");
            if (!tasks || document.kind(*tasks) != NodeKind::MAP) {
                continue;
            }
            for (const Entry& entry : document.entries(*tasks)) {
                task_names_.emplace(document.text(entry.key));
                const std::optional<std::uint64_t> period =
                    parse_duration_ms(text_at(document, entry.value, "period"));
                if (period &&
                    *period <= static_cast<std::uint64_t>(
                                   std::numeric_limits<std::int64_t>::max())) {
                    periods_.emplace(std::string(document.text(entry.key)),
                                     static_cast<std::int64_t>(*period));
                }
            }
        }
    }

    // --- What a run-time expression is checked against --------------------

    [[nodiscard]] RunTimeContext context_of(std::string_view task) {
        RunTimeContext context;
        context.tag = [this](std::string_view name) -> std::optional<TagRead> {
            const std::optional<TagInfo> info = tags_->find(name);
            if (!info) {
                return std::nullopt;
            }
            return read_of(info->value_type);
        };
        context.matching = [this](std::string_view pattern) {
            return matching(pattern);
        };
        if (const auto found = periods_.find(task); found != periods_.end()) {
            context.task = std::string(task);
            context.period_ms = found->second;
        }
        return context;
    }

    // How the tags `pattern` matches are read (PatternReads), found once
    // and kept: at most `kept_` patterns (65,536 by default), each a few
    // names in size, whatever
    // it matches (M2 of the review of #163). Every virtual tag a pattern
    // matches is an input of what reads it, so it is typed before the
    // pattern is first resolved, and the summary does not change.
    PatternReads matching(std::string_view pattern) {
        if (const auto kept = matches_.find(pattern); kept != matches_.end()) {
            return kept->second;
        }
        PatternReads reads;
        tags_->each_with_prefix(
            pattern.substr(0, prefix_of(pattern)),
            [this, &reads, pattern](std::string_view name,
                                    const TagInfo& info) {
                if (over_budget_) {
                    return;
                }
                ++counts_.comparisons;
                std::uint64_t steps = 0;
                const bool matched = matches_name_pattern(pattern, name, steps);
                if (spend(RUN_TIME_NAME_WORK + steps) && matched) {
                    add_read(reads, name, read_of(info.value_type));
                }
            });
        if (over_budget_) {
            return reads;  // not read: what checks it reports nothing
        }
        const std::size_t bytes =
            pattern.size() + reads.first_odd.size() + reads.first_bool.size();
        if (matches_.size() < kept_ && keep_bytes(bytes)) {
            matches_.emplace(std::string(pattern), reads);
            counts_.kept_patterns = matches_.size();
        }
        return reads;
    }

    // True if `bytes` more of text may be kept (MAX_KEPT_BYTES), and counts
    // them.
    bool keep_bytes(std::size_t bytes) {
        if (bytes > MAX_KEPT_BYTES - counts_.kept_bytes) {
            return false;
        }
        counts_.kept_bytes += bytes;
        return true;
    }

    // Counts `units` of the work of matching patterns; false past the
    // budget, which is CF-113 once, at the place being checked (item 1 of
    // the delta check of #163).
    bool spend(std::uint64_t units) {
        if (over_budget_) {
            return false;
        }
        counts_.pattern_work =
            units > std::numeric_limits<std::uint64_t>::max() -
                        counts_.pattern_work
                ? std::numeric_limits<std::uint64_t>::max()
                : counts_.pattern_work + units;
        if (counts_.pattern_work <= pattern_budget_) {
            return true;
        }
        over_budget_ = true;
        diagnostics_->error(
            CF_SIZE_LIMIT,
            place_document_ == nullptr ? Location{}
                                       : place_document_->location(place_node_),
            "the pattern matching of run-time expressions exceeds its limit "
            "of " +
                std::to_string(pattern_budget_) + " units");
        return false;
    }

    // Where CF-113 goes: what is being checked.
    void place_at(const Document& document, NodeId node) {
        place_document_ = &document;
        place_node_ = node;
    }

    // --- Virtual tags and interlocks ---------------------------------------

    void collect_named() {
        for (const CheckedFile& file : files_) {
            const FileType type = file.source->path_class.type;
            if (type == FileType::TAGS) {
                collect_virtuals(file);
            } else if (type == FileType::OUTPUTS) {
                collect_interlocks(file);
            }
        }
        // The names in order, for the inputs of a pattern; the views stay
        // valid, as `named_` no longer grows.
        names_.reserve(by_name_.size());
        for (const auto& [name, index] : by_name_) {
            names_.emplace_back(named_[index].name, index);
        }
        for (Named& vertex : named_) {
            vertex.inputs = inputs_of(vertex);
        }
    }

    void collect_virtuals(const CheckedFile& file) {
        const Document& document = *file.document;
        for (const NodeId tag : items_at(document, document.root(), "tags")) {
            const std::optional<NodeId> name = scalar_at(document, tag, "name");
            const std::optional<NodeId> expr = scalar_at(document, tag, "expr");
            if (text_at(document, tag, "type") != "virtual" || !name || !expr) {
                continue;
            }
            add_named(
                Named{.name = std::string(document.text(*name)),
                      .file = &file,
                      .name_node = *name,
                      .is_virtual = true,
                      .value_type = scalar_at(document, tag, "value_type"),
                      .site = Site{.document = &document,
                                   .scalar = *expr,
                                   .task = task_of(document, tag, *name),
                                   .needs_bool = false,
                                   .expanded = false,
                                   .device = {},
                                   .detail = {}},
                      .tree = parse(document, *expr),
                      .inputs = {}});
        }
    }

    // An interlock is a vertex only for its cycles: its condition is
    // checked with the other expressions.
    void collect_interlocks(const CheckedFile& file) {
        const Document& document = *file.document;
        for (const NodeId interlock :
             items_at(document, document.root(), "interlocks")) {
            const std::optional<NodeId> name =
                scalar_at(document, interlock, "name");
            const std::optional<NodeId> condition =
                scalar_at(document, interlock, "condition");
            if (!name || !condition) {
                continue;
            }
            add_named(Named{.name = std::string(document.text(*name)),
                            .file = &file,
                            .name_node = *name,
                            .is_virtual = false,
                            .value_type = std::nullopt,
                            .site = Site{},
                            .tree = parse(document, *condition),
                            .inputs = {}});
            static_cast<void>(task_of(document, interlock, *name));
        }
    }

    // A vertex for the tag that defines its name: one of two of one name is
    // CF-302 (D-116), and the other is not read as this one (S3 of the
    // review of #163).
    void add_named(Named vertex) {
        const std::optional<TagInfo> info = tags_->find(vertex.name);
        if (!info || info->document != vertex.file->document ||
            info->node != vertex.name_node) {
            return;
        }
        if (by_name_.try_emplace(vertex.name, named_.size()).second) {
            named_.push_back(std::move(vertex));
        }
    }

    // The task of a virtual tag or an interlock: its `task`, else `normal`
    // (5.1, 7.3), which a tasks file without it does not have: CF-310 at
    // the name (S4 of the review of #163).
    std::string_view task_of(const Document& document, NodeId map,
                             NodeId name) {
        const std::string_view task = view_at(document, map, "task");
        if (!task.empty()) {
            return task;
        }
        if (tasks_given_ && !task_names_.contains(DEFAULT_TASK)) {
            diagnostics_->error(
                CF_NO_SUCH_NAME, document.location(name),
                "task " + quote_text(DEFAULT_TASK) + " does not exist");
        }
        return DEFAULT_TASK;
    }

    static std::optional<Expression> parse(const Document& document,
                                           NodeId scalar) {
        ExpressionError unused;
        return parse_expression(document.text(scalar), ExpressionKind::RUN_TIME,
                                unused);
    }

    // The vertices that `vertex`'s expression reads: virtual tags and
    // interlocks by name, with `.quality` or `.age`, or through
    // `tag("...")`, and the vertex of each distinct pattern (5.6: a cycle
    // through a pattern that matches the tag itself is one).
    [[nodiscard]] std::vector<std::size_t> inputs_of(const Named& vertex) {
        std::vector<std::size_t> inputs;
        if (!vertex.tree) {
            return inputs;
        }
        const auto add_named = [this, &inputs](std::string_view name) {
            if (const auto found = by_name_.find(name);
                found != by_name_.end()) {
                inputs.push_back(found->second);
            }
        };
        for (const ExprNode& node : vertex.tree->nodes) {
            if (node.op == ExprOp::REFERENCE) {
                add_named(node.text);
                for (const std::string_view suffix : {".quality", ".age"}) {
                    if (node.text.ends_with(suffix)) {
                        add_named(std::string_view(node.text).substr(
                            0, node.text.size() - suffix.size()));
                    }
                }
            } else if (node.op == ExprOp::PATTERN) {
                if (!is_name_pattern(node.text)) {
                    add_named(node.text);
                    continue;
                }
                const auto [entry, added] =
                    pattern_index_.try_emplace(node.text, patterns_.size());
                if (added) {
                    patterns_.push_back(Pattern{
                        .text = node.text, .prefix = prefix_of(node.text)});
                }
                inputs.push_back(named_.size() + entry->second);
            }
        }
        std::ranges::sort(inputs);
        const auto [first, last] = std::ranges::unique(inputs);
        inputs.erase(first, last);
        return inputs;
    }

    // The next input of `vertex` from `cursor`: of a virtual tag or an
    // interlock, the next it reads; of a pattern, the next name it
    // matches, from the sorted names that start with its prefix. A pattern
    // is walked once, however many read it.
    std::optional<std::size_t> next_input(std::size_t vertex,
                                          std::size_t& cursor) {
        if (vertex < named_.size()) {
            place_at(*named_[vertex].file->document, named_[vertex].name_node);
            const std::vector<std::size_t>& inputs = named_[vertex].inputs;
            if (cursor < inputs.size()) {
                return inputs[cursor++];
            }
            return std::nullopt;
        }
        const Pattern& pattern = patterns_[vertex - named_.size()];
        const std::string_view prefix =
            std::string_view(pattern.text).substr(0, pattern.prefix);
        if (cursor == 0) {
            // The cursor is one past the position of the next name.
            cursor =
                1 + static_cast<std::size_t>(
                        std::ranges::lower_bound(
                            names_, prefix, {},
                            &std::pair<std::string_view, std::size_t>::first) -
                        names_.begin());
        }
        while (!over_budget_ && cursor - 1 < names_.size() &&
               names_[cursor - 1].first.starts_with(prefix)) {
            const auto [name, index] = names_[cursor - 1];
            ++cursor;
            ++counts_.comparisons;
            std::uint64_t steps = 0;
            const bool matched =
                matches_name_pattern(pattern.text, name, steps);
            if (spend(RUN_TIME_NAME_WORK + steps) && matched) {
                return index;
            }
        }
        cursor = names_.size() + 1;
        return std::nullopt;
    }

    // Each component of the graph, complete before any that reads it (a
    // cycle is one component).
    [[nodiscard]] std::vector<std::vector<std::size_t>> components() {
        return Components(named_.size() + patterns_.size(),
                          [this](std::size_t vertex, std::size_t& cursor) {
                              return next_input(vertex, cursor);
                          })
            .run();
    }

    void type_virtuals() {
        const std::vector<std::vector<std::size_t>> walked = components();
        for (const std::vector<std::size_t>& component : walked) {
            // Past the budget the graph is not whole: no cycle and no type
            // is told from it.
            if (over_budget_) {
                return;
            }
            // A component's virtual tags and interlocks, without patterns:
            // a component is sorted, so they come first.
            const auto end = std::ranges::lower_bound(component, named_.size());
            const std::span<const std::size_t> members(component.begin(), end);
            if (members.empty()) {
                continue;
            }
            const Named& first = named_[members.front()];
            const bool reads_itself =
                std::ranges::binary_search(first.inputs, members.front());
            if (component.size() > 1 || reads_itself) {
                report_cycle(members);
                continue;
            }
            if (first.is_virtual) {
                type_virtual(first);
            }
        }
    }

    // CF-525 once for a cycle, at its first tag in the order written, also
    // for a cycle through interlocks (R1 of the review of #163); its
    // virtual tags stay of unknown type.
    void report_cycle(std::span<const std::size_t> members) {
        // The message is cut at MAX_DIAGNOSTIC_TEXT_BYTES: no name past the
        // cut is written.
        std::string names;
        for (const std::size_t member : members) {
            if (names.size() > MAX_DIAGNOSTIC_TEXT_BYTES) {
                break;
            }
            names += (names.empty() ? "" : ", ") + named_[member].name;
        }
        const Named& first = named_[members.front()];
        diagnostics_->error(CF_CYCLE,
                            first.file->document->location(first.name_node),
                            "virtual tags form a cycle: " + names);
    }

    void type_virtual(const Named& tag) {
        if (!tag.tree) {
            return;
        }
        TypeError error;
        place_at(*tag.site.document, tag.site.scalar);
        const std::optional<ValueType> type =
            run_time_type(*tag.tree, context_of(tag.site.task), error);
        if (over_budget_) {
            return;
        }
        if (!type) {
            report(tag.site, error);
            return;
        }
        const std::string_view derived = derived_value_type(*type);
        if (tag.value_type) {
            // A view into the tag file, which outlives the TagNames.
            const std::string_view given =
                tag.file->document->text(*tag.value_type);
            if (given != derived) {
                diagnostics_->error(
                    CF_DERIVED_TYPE,
                    tag.file->document->location(*tag.value_type),
                    quote_text(tag.name) + ": the expression gives " +
                        std::string(derived));
            }
            // The given type is the one its readers expect.
            tags_->set_value_type(tag.name, given);
            return;
        }
        tags_->set_value_type(tag.name, derived);
    }

    // --- The other expressions ----------------------------------------------

    void check_file(const CheckedFile& file) {
        const Document& document = *file.document;
        const NodeId root = document.root();
        switch (file.source->path_class.type) {
            case FileType::TAGS:
                for (const NodeId tag : items_at(document, root, "tags")) {
                    for (const NodeId alarm :
                         items_at(document, tag, "alarms")) {
                        check_alarm(document, alarm, {}, {});
                    }
                }
                break;
            case FileType::ALARMS:
                for (const NodeId alarm : items_at(document, root, "alarms")) {
                    check_alarm(document, alarm, {}, {});
                }
                break;
            case FileType::OUTPUTS:
                for (const NodeId interlock :
                     items_at(document, root, "interlocks")) {
                    const std::optional<NodeId> condition =
                        scalar_at(document, interlock, "condition");
                    if (!condition) {
                        continue;
                    }
                    const std::string_view task =
                        view_at(document, interlock, "task");
                    check(Site{.document = &document,
                               .scalar = *condition,
                               .task = task.empty() ? DEFAULT_TASK : task,
                               .needs_bool = true,
                               .expanded = false,
                               .device = {},
                               .detail = {}});
                }
                break;
            default:
                break;
        }
    }

    // An alarm's `expr` (of an expression alarm) and `suppress_when`: BOOL,
    // with no task (D-117: an alarm runs in no control task).
    void check_alarm(const Document& document, NodeId alarm,
                     const std::string& device, const std::string& point) {
        for (const std::string_view key : {"expr", "suppress_when"}) {
            const std::optional<NodeId> scalar =
                scalar_at(document, alarm, key);
            if (!scalar) {
                continue;
            }
            check(Site{
                .document = &document,
                .scalar = *scalar,
                .task = {},
                .needs_bool = true,
                .expanded = &document == &tree_->document,
                .device = device,
                .detail = point.empty() ? std::string() : "point " + point});
        }
    }

    // The alarms of every point and bit of the expanded tree.
    void check_expanded() {
        const Document& document = tree_->document;
        for (const NodeId channel :
             items_at(document, document.root(), "channels")) {
            for (const NodeId device : items_at(document, channel, "devices")) {
                const std::string device_name =
                    text_at(document, device, "name");
                for (const NodeId point :
                     items_at(document, device, "points")) {
                    const std::string point_name =
                        text_at(document, point, "name");
                    for (const NodeId alarm :
                         items_at(document, point, "alarms")) {
                        check_alarm(document, alarm, device_name, point_name);
                    }
                    const std::optional<NodeId> bits =
                        document.find(point, "bits");
                    if (!bits || document.kind(*bits) != NodeKind::MAP) {
                        continue;
                    }
                    for (const Entry& entry : document.entries(*bits)) {
                        for (const NodeId alarm :
                             items_at(document, entry.value, "alarms")) {
                            check_alarm(document, alarm, device_name,
                                        point_name);
                        }
                    }
                }
            }
        }
    }

    void check(const Site& site) {
        if (over_budget_) {
            return;
        }
        const std::string_view text = site.document->text(site.scalar);
        // One template line comes back at every instance with the same
        // text, and every tag has its type: the result is kept.
        // Views into the documents, which outlive the check: a kept result
        // copies no text but its error's message.
        const std::pair<std::string_view, std::string_view> key(site.task,
                                                                text);
        if (const auto kept = results_.find(key); kept != results_.end()) {
            const std::optional<TypeError>& kept_error = kept->second;
            if (kept_error.has_value()) {
                report(site, kept_error.value());
            }
            return;
        }
        place_at(*site.document, site.scalar);
        std::optional<TypeError> failed = type_of(site);
        if (over_budget_) {
            return;
        }
        if (results_.size() < kept_ &&
            keep_bytes(failed ? failed->message.size() : 0)) {
            results_.emplace(key, failed);
            counts_.kept_results = results_.size();
        }
        if (failed) {
            report(site, *failed);
        }
    }

    // The error of the expression at `site`, or nothing if it fits.
    std::optional<TypeError> type_of(const Site& site) {
        const std::optional<Expression> tree =
            parse(*site.document, site.scalar);
        if (!tree) {
            // Stage 3 refused it already.
            return std::nullopt;
        }
        TypeError error;
        const std::optional<ValueType> type =
            run_time_type(*tree, context_of(site.task), error);
        if (!type) {
            return error;
        }
        if (site.needs_bool && *type != ValueType::BOOL) {
            return TypeError{
                .code = CF_TYPE_MISMATCH,
                .column = tree->nodes.at(tree->root).column,
                .message = "expression: the result must be BOOL, not " +
                           std::string(value_type_name(*type))};
        }
        return std::nullopt;
    }

    // `error` at its character of the expression at `site` (D-109); in the
    // expanded tree at the instance, with the template note, grouped.
    void report(const Site& site, const TypeError& error) {
        if (error.code == 0) {
            return;
        }
        ExpressionSite place =
            expression_site(*site.document, site.scalar, error.column);
        std::string message = error.message;
        if (!site.document->verbatim(site.scalar)) {
            message += place.where;
        }
        Diagnostic diagnostic{.severity = Severity::ERROR,
                              .code = error.code,
                              .location = std::move(place.location),
                              .message = message,
                              .notes = {}};
        if (site.expanded) {
            const std::uint32_t instance =
                site.scalar < tree_->instance_of.size()
                    ? tree_->instance_of[site.scalar]
                    : ExpandedTree::NO_INSTANCE;
            if (instance < tree_->instances.size() &&
                tree_->instances[instance].file != diagnostic.location.file) {
                diagnostic.notes.push_back(
                    Diagnostic::Note{.kind = "template",
                                     .location = std::move(diagnostic.location),
                                     .detail = site.detail});
                diagnostic.location = tree_->instances[instance];
            }
        }
        grouped_.add(std::move(diagnostic), message, site.device,
                     *diagnostics_);
    }

    TagNames* tags_;
    const ExpandedTree* tree_;
    std::span<const CheckedFile> files_;
    Diagnostics* diagnostics_;
    // The summaries of patterns and the results of texts kept, at most.
    std::size_t kept_;
    // The work of matching patterns allowed, and whether it is spent.
    std::uint64_t pattern_budget_;
    bool over_budget_ = false;
    const Document* place_document_ = nullptr;
    NodeId place_node_ = 0;
    RunTimeCounts* counts_out_;
    GroupedErrors grouped_;
    std::map<std::string, std::int64_t, std::less<>> periods_;
    std::set<std::string, std::less<>> task_names_;
    bool tasks_given_ = false;
    std::vector<Named> named_;
    std::map<std::string, std::size_t, std::less<>> by_name_;
    std::vector<std::pair<std::string_view, std::size_t>> names_;
    std::vector<Pattern> patterns_;
    std::map<std::string, std::size_t, std::less<>> pattern_index_;
    // What the tags of each pattern are read as.
    std::map<std::string, PatternReads, std::less<>> matches_;
    std::map<std::pair<std::string_view, std::string_view>,
             std::optional<TypeError>>
        results_;
    RunTimeCounts counts_;
};

}  // namespace

void check_run_time_expressions(TagNames& tags, const ExpandedTree& tree,
                                std::span<const CheckedFile> files,
                                Diagnostics& diagnostics, std::size_t kept,
                                std::uint64_t pattern_work,
                                RunTimeCounts* counts) {
    Checker(tags, tree, files, diagnostics, kept, pattern_work, counts).run();
}

}  // namespace scada::conf
