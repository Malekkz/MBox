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
#include "tag_names.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::conf {

namespace {

constexpr std::uint16_t CF_TYPE_MISMATCH = 523;
constexpr std::uint16_t CF_CYCLE = 525;
constexpr std::uint16_t CF_DERIVED_TYPE = 527;

// The tasks that exist without `control/tasks.yaml` (9.1), in ms.
constexpr std::int64_t FAST_MS = 10;
constexpr std::int64_t NORMAL_MS = 100;
constexpr std::int64_t SLOW_MS = 1000;

// The task of a virtual tag or an interlock without `task` (5.1, 7.3).
constexpr std::string_view DEFAULT_TASK = "normal";

// The results of an expression kept for the next instance that holds the
// same text (one template line at every instance), at most this many.
constexpr std::size_t MAX_KEPT_RESULTS = 65536;

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
std::string derived_value_type(ValueType type) {
    switch (type) {
        case ValueType::BOOL:
            return "bool";
        case ValueType::REAL:
            return "float64";
        default:
            return "int64";
    }
}

// The part of a name pattern before its first wildcard: every name it
// matches starts with it.
std::string_view pattern_prefix(std::string_view pattern) {
    return pattern.substr(
        0, std::min(pattern.find_first_of("*?"), pattern.size()));
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
    // The task that runs it, empty where none does (an alarm).
    std::string task;
    // Its result must be BOOL: an alarm's `expr`, `suppress_when`, an
    // interlock's `condition`.
    bool needs_bool = false;
    // In the expanded tree: the device that holds it, and the note's detail.
    bool expanded = false;
    std::string device;
    std::string detail;
};

// A virtual tag and what its expression reads.
struct Virtual {
    std::string name;
    const CheckedFile* file = nullptr;
    NodeId name_node = 0;
    std::optional<NodeId> value_type;
    Site site;
    std::optional<Expression> tree;
    // The virtual tags it reads, by index.
    std::vector<std::size_t> inputs;
};

// Tarjan's strongly connected components, iteratively (no recursion: a
// tree may hold many virtual tags). Each component is complete before any
// that reads it, so the virtual tags are typed in the order their inputs
// need.
class Components {
public:
    explicit Components(std::span<const std::vector<std::size_t>* const> inputs)
        : inputs_(inputs),
          index_(inputs.size(), UNSEEN),
          low_(inputs.size(), 0),
          on_stack_(inputs.size(), false) {}

    std::vector<std::vector<std::size_t>> run() {
        for (std::size_t start = 0; start < inputs_.size(); ++start) {
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
            auto& [node, edge] = walk_.back();
            if (edge == 0 && index_[node] == UNSEEN) {
                open(node);
            }
            const std::vector<std::size_t>& inputs = *inputs_[node];
            if (edge < inputs.size()) {
                const std::size_t input = inputs[edge++];
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

    std::span<const std::vector<std::size_t>* const> inputs_;
    std::vector<std::size_t> index_;
    std::vector<std::size_t> low_;
    std::vector<bool> on_stack_;
    std::vector<std::size_t> stack_;
    // The walk: a node and the next of its inputs to visit.
    std::vector<std::pair<std::size_t, std::size_t>> walk_;
    std::vector<std::vector<std::size_t>> out_;
    std::size_t next_ = 0;
};

class Checker {
public:
    Checker(TagNames& tags, const ExpandedTree& tree,
            std::span<const CheckedFile> files, Diagnostics& diagnostics)
        : tags_(&tags),
          tree_(&tree),
          files_(files),
          diagnostics_(&diagnostics),
          grouped_(MAX_KEPT_ERRORS) {}

    void run() {
        read_tasks();
        collect_virtuals();
        type_virtuals();
        // Every tag has its type now: patterns and results may be kept.
        settled_ = true;
        for (const CheckedFile& file : files_) {
            check_file(file);
        }
        check_expanded();
        grouped_.flush(*diagnostics_);
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
            const Document& document = *file.document;
            const std::optional<NodeId> tasks =
                document.find(document.root(), "tasks");
            if (!tasks || document.kind(*tasks) != NodeKind::MAP) {
                continue;
            }
            for (const Entry& entry : document.entries(*tasks)) {
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

    [[nodiscard]] RunTimeContext context_of(const std::string& task) {
        RunTimeContext context;
        context.tag = [this](std::string_view name) -> std::optional<TagRead> {
            const TagInfo* info = tags_->find(name);
            if (info == nullptr) {
                return std::nullopt;
            }
            return read_of(info->value_type);
        };
        context.matching = [this](std::string_view pattern) {
            return matching(pattern);
        };
        if (const auto found = periods_.find(task); found != periods_.end()) {
            context.task = task;
            context.period_ms = found->second;
        }
        return context;
    }

    std::vector<TagRead> matching(std::string_view pattern) {
        if (settled_) {
            if (const auto kept = matches_.find(pattern);
                kept != matches_.end()) {
                return kept->second;
            }
        }
        std::vector<TagRead> found;
        tags_->each_with_prefix(
            pattern_prefix(pattern),
            [&found, pattern](std::string_view name, const TagInfo& info) {
                if (matches_name_pattern(pattern, name)) {
                    found.push_back(read_of(info.value_type));
                }
            });
        if (settled_ && matches_.size() < MAX_KEPT_RESULTS) {
            matches_.emplace(std::string(pattern), found);
        }
        return found;
    }

    // --- Virtual tags --------------------------------------------------------

    void collect_virtuals() {
        for (const CheckedFile& file : files_) {
            if (file.source->path_class.type != FileType::TAGS) {
                continue;
            }
            const Document& document = *file.document;
            for (const NodeId tag :
                 items_at(document, document.root(), "tags")) {
                const std::optional<NodeId> name =
                    scalar_at(document, tag, "name");
                const std::optional<NodeId> expr =
                    scalar_at(document, tag, "expr");
                if (text_at(document, tag, "type") != "virtual" || !name ||
                    !expr) {
                    continue;
                }
                std::string task = text_at(document, tag, "task");
                Virtual made{
                    .name = std::string(document.text(*name)),
                    .file = &file,
                    .name_node = *name,
                    .value_type = scalar_at(document, tag, "value_type"),
                    .site =
                        Site{.document = &document,
                             .scalar = *expr,
                             .task = task.empty() ? std::string(DEFAULT_TASK)
                                                  : std::move(task),
                             .needs_bool = false,
                             .expanded = false,
                             .device = {},
                             .detail = {}},
                    .tree = parse(document, *expr),
                    .inputs = {}};
                // Two tags of one name are CF-302 (D-116): the first counts.
                if (by_name_.try_emplace(made.name, virtuals_.size()).second) {
                    virtuals_.push_back(std::move(made));
                }
            }
        }
        for (Virtual& tag : virtuals_) {
            tag.inputs = inputs_of(tag);
        }
    }

    static std::optional<Expression> parse(const Document& document,
                                           NodeId scalar) {
        ExpressionError unused;
        return parse_expression(document.text(scalar), ExpressionKind::RUN_TIME,
                                unused);
    }

    // The virtual tags that `tag`'s expression reads: by name, with
    // `.quality` or `.age`, through `tag("...")` or a pattern (5.6: a cycle
    // through a pattern that matches the tag itself is one).
    [[nodiscard]] std::vector<std::size_t> inputs_of(const Virtual& tag) const {
        std::vector<std::size_t> inputs;
        if (!tag.tree) {
            return inputs;
        }
        const auto add_named = [this, &inputs](std::string_view name) {
            if (const auto found = by_name_.find(name);
                found != by_name_.end()) {
                inputs.push_back(found->second);
            }
        };
        for (const ExprNode& node : tag.tree->nodes) {
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
                for (std::size_t i = 0; i < virtuals_.size(); ++i) {
                    if (matches_name_pattern(node.text, virtuals_[i].name)) {
                        inputs.push_back(i);
                    }
                }
            }
        }
        std::ranges::sort(inputs);
        const auto [first, last] = std::ranges::unique(inputs);
        inputs.erase(first, last);
        return inputs;
    }

    // Each component of the graph of virtual tags, complete before any
    // that reads it (a cycle is one component).
    [[nodiscard]] std::vector<std::vector<std::size_t>> components() const {
        std::vector<const std::vector<std::size_t>*> inputs;
        inputs.reserve(virtuals_.size());
        for (const Virtual& tag : virtuals_) {
            inputs.push_back(&tag.inputs);
        }
        return Components(inputs).run();
    }

    void type_virtuals() {
        for (const std::vector<std::size_t>& component : components()) {
            const Virtual& first = virtuals_[component.front()];
            const bool reads_itself =
                std::ranges::binary_search(first.inputs, component.front());
            if (component.size() > 1 || reads_itself) {
                report_cycle(component);
                continue;
            }
            type_virtual(virtuals_[component.front()]);
        }
    }

    // CF-525 once for a cycle, at its first tag in the order written; its
    // tags stay of unknown type.
    void report_cycle(const std::vector<std::size_t>& component) {
        std::string names;
        for (const std::size_t member : component) {
            names += (names.empty() ? "" : ", ") + virtuals_[member].name;
        }
        const Virtual& first = virtuals_[component.front()];
        diagnostics_->error(CF_CYCLE,
                            first.file->document->location(first.name_node),
                            "virtual tags form a cycle: " + names);
    }

    void type_virtual(const Virtual& tag) {
        if (!tag.tree) {
            return;
        }
        TypeError error;
        const std::optional<ValueType> type =
            run_time_type(*tag.tree, context_of(tag.site.task), error);
        if (!type) {
            report(tag.site, error);
            return;
        }
        const std::string derived = derived_value_type(*type);
        if (tag.value_type) {
            const std::string given(tag.file->document->text(*tag.value_type));
            if (given != derived) {
                diagnostics_->error(
                    CF_DERIVED_TYPE,
                    tag.file->document->location(*tag.value_type),
                    quote_text(tag.name) + ": the expression gives " + derived);
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
                    std::string task = text_at(document, interlock, "task");
                    check(Site{.document = &document,
                               .scalar = *condition,
                               .task = task.empty() ? std::string(DEFAULT_TASK)
                                                    : std::move(task),
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
        const std::string_view text = site.document->text(site.scalar);
        // One template line comes back at every instance with the same
        // text, and every tag has its type: the result is kept.
        const std::string key = site.task + '\0' + std::string(text);
        if (const auto kept = results_.find(key); kept != results_.end()) {
            const std::optional<TypeError>& kept_error = kept->second;
            if (kept_error.has_value()) {
                report(site, kept_error.value());
            }
            return;
        }
        std::optional<TypeError> failed = type_of(site);
        if (results_.size() < MAX_KEPT_RESULTS) {
            results_.emplace(key, failed);
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
    GroupedErrors grouped_;
    std::map<std::string, std::int64_t, std::less<>> periods_;
    std::vector<Virtual> virtuals_;
    std::map<std::string, std::size_t, std::less<>> by_name_;
    bool settled_ = false;
    std::map<std::string, std::vector<TagRead>, std::less<>> matches_;
    std::map<std::string, std::optional<TypeError>, std::less<>> results_;
};

}  // namespace

void check_run_time_expressions(TagNames& tags, const ExpandedTree& tree,
                                std::span<const CheckedFile> files,
                                Diagnostics& diagnostics) {
    Checker(tags, tree, files, diagnostics).run();
}

}  // namespace scada::conf
