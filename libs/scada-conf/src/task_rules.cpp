// The control tasks against their programs and the I/O scan (configuration
// schemas contract 9.1 and 10.9; decision D-125, slice 7e-2 of #100).

#include "task_rules.h"
#include "runtime_expressions.h"

#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/node.h"
#include "scada/conf/references.h"
#include "scada/conf/scalar.h"
#include "scada/conf/tree.h"
#include "tag_names.h"
#include "tree_reader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::conf {

namespace {

using detail::Reader;
using detail::Reporter;

constexpr std::uint16_t CF_NAME_TWICE = 303;
constexpr std::uint16_t CF_PROGRAM_TASKS = 901;
constexpr std::uint16_t CF_PROGRAM_TASK = 902;
constexpr std::uint16_t CW_SLOWER_INPUT = 903;
constexpr std::uint16_t CF_RT_INPUT = 904;
constexpr std::uint16_t CF_WRITTEN_TWICE = 905;
constexpr std::uint16_t CF_PRIORITY = 906;

// 9.1, 4.1: the default task priority and the default I/O scan priority.
constexpr std::int64_t DEFAULT_PRIORITY = 50;
constexpr std::int64_t DEFAULT_IO_PRIORITY = 95;

// 9.1: the tasks of a tree without control/tasks.yaml, and their periods.
constexpr std::array<std::string_view, 3> DEFAULT_TASKS = {"fast", "normal",
                                                           "slow"};
constexpr std::array<std::pair<std::string_view, std::string_view>, 3>
    DEFAULT_PERIODS = {{{"fast", "10ms"}, {"normal", "100ms"}, {"slow", "1s"}}};

// 4.6: the poll groups of a channel that gives none.
constexpr std::array<std::pair<std::string_view, std::string_view>, 3>
    DEFAULT_GROUPS = {{{"fast", "200ms"}, {"normal", "1s"}, {"slow", "10s"}}};
constexpr std::string_view DEFAULT_GROUP = "normal";

// A task: its period, as written, and whether it is real-time only.
struct Task {
    std::uint64_t period_ms = 0;
    std::string_view period;
    bool rt_required = false;
};

// What one reader of a task reads (a program, a virtual tag, an
// interlock), and where each finding about it is placed.
struct Reads {
    std::string slowest;
    InputClass slowest_class;
    NodeId slowest_at = 0;
    std::string driver_tag;
    std::string driver;
    NodeId driver_at = 0;
    // The virtual tags it reads, each with where.
    std::vector<std::pair<std::string, NodeId>> virtuals;
};

// An integer of `text`, nothing for one the schema refused.
[[nodiscard]] std::optional<std::int64_t> integer_of(std::string_view text) {
    const std::optional<Integer> value = parse_integer(text);
    return value ? as_int64(*value) : std::nullopt;
}

// Where a program is listed: its task and the list item.
struct Listing {
    std::string_view task;
    NodeId item = 0;
};

class Checker {
public:
    Checker(const TagNames& tags, const ExpandedTree& tree,
            std::span<const CheckedFile> files, const TaskInputs& inputs,
            Diagnostics& diagnostics)
        : tags_(&tags),
          inputs_(&inputs),
          files_(files),
          reporter_(tree, diagnostics) {}

    // The readers point into the tree: not copied, not moved.
    Checker(const Checker&) = delete;
    Checker& operator=(const Checker&) = delete;
    Checker(Checker&&) = delete;
    Checker& operator=(Checker&&) = delete;
    ~Checker() = default;

    void run() {
        readers_.reserve(files_.size());
        for (const CheckedFile& file : files_) {
            readers_.emplace_back(*file.document);
        }
        read_io_priority();
        const Reader* tasks = nullptr;
        for (std::size_t i = 0; i < files_.size(); ++i) {
            if (files_[i].source->path_class.type == FileType::TASKS) {
                tasks = &readers_[i];
            }
        }
        if (tasks != nullptr) {
            check_tasks(*tasks);
        } else {
            check_default_tasks();
        }
        collect_internal_tags();
        for (const ExpressionInputs& expression : inputs_->expressions) {
            if (expression.is_virtual) {
                virtuals_.try_emplace(expression.name, &expression);
            }
        }
        for (std::size_t i = 0; i < files_.size(); ++i) {
            if (files_[i].source->path_class.type == FileType::PROGRAM) {
                check_program(readers_[i]);
            }
        }
        if (inputs_->whole && tags_->complete()) {
            for (const ExpressionInputs& expression : inputs_->expressions) {
                check_expression(expression);
            }
        }
        reporter_.flush();
    }

private:
    void error(const Reader& reader, NodeId node, std::uint16_t code,
               std::string message) {
        reporter_.error(reader.document(), node, code, std::move(message));
    }

    // `realtime.io_priority` of system.yaml, its default without one.
    void read_io_priority() {
        for (std::size_t i = 0; i < files_.size(); ++i) {
            if (files_[i].source->path_class.type != FileType::SYSTEM) {
                continue;
            }
            const Reader& reader = readers_[i];
            const std::optional<NodeId> realtime =
                reader.child(reader.document().root(), "realtime");
            const std::optional<NodeId> priority =
                realtime ? reader.scalar(*realtime, "io_priority")
                         : std::nullopt;
            if (priority) {
                io_priority_ = integer_of(reader.document().text(*priority));
                io_reader_ = &reader;
                io_node_ = *priority;
            }
        }
    }

    // CF-906 for a task of priority `priority` placed at `node`.
    void check_priority(const Reader& reader, NodeId node,
                        std::string_view name, std::int64_t priority) {
        if (io_priority_ && priority >= *io_priority_) {
            error(reader, node, CF_PRIORITY,
                  "task " + quote_text(name) + ": priority " +
                      std::to_string(priority) + " must be below " +
                      std::to_string(*io_priority_));
        }
    }

    void check_tasks(const Reader& reader) {
        const Document& document = reader.document();
        for (const Entry& task : reader.entries(document.root(), "tasks")) {
            const std::string_view name = document.text(task.key);
            tasks_.insert(name);
            const std::string_view period =
                reader.text_of(task.value, "period");
            task_info_.try_emplace(
                name,
                Task{.period_ms = parse_duration_ms(period).value_or(0),
                     .period = period,
                     .rt_required =
                         reader.text_of(task.value, "rt_required") == "true"});
            const std::optional<NodeId> priority =
                reader.scalar(task.value, "priority");
            if (const std::optional<std::int64_t> value =
                    priority ? integer_of(document.text(*priority))
                             : std::optional<std::int64_t>(DEFAULT_PRIORITY)) {
                check_priority(reader, priority.value_or(task.key), name,
                               *value);
            }
            for (const NodeId program : reader.items(task.value, "programs")) {
                if (document.kind(program) == NodeKind::SCALAR) {
                    list_program(reader, name, program);
                }
            }
        }
    }

    // A program listed by `task`. Named twice in one task's list it is
    // CF-303, a program name used twice (the review of #189), and counts
    // once for CF-901.
    void list_program(const Reader& reader, std::string_view task,
                      NodeId program) {
        const std::string_view name = reader.document().text(program);
        std::vector<Listing>& listings = listings_[std::string(name)];
        for (const Listing& listing : listings) {
            if (listing.task == task) {
                const Location first =
                    reporter_.place_of(reader.document(), listing.item);
                error(reader, program, CF_NAME_TWICE,
                      "program " + quote_text(name) +
                          " is defined twice; first at " + first.file + ":" +
                          std::to_string(first.line));
                return;
            }
        }
        listings.push_back(Listing{.task = task, .item = program});
    }

    // The default tasks have the default priority and list no program: a
    // CF-906 for them is at `io_priority`, which made it.
    void check_default_tasks() {
        tasks_.insert(DEFAULT_TASKS.begin(), DEFAULT_TASKS.end());
        for (const auto& [name, period] : DEFAULT_PERIODS) {
            task_info_.try_emplace(
                name, Task{.period_ms = parse_duration_ms(period).value_or(0),
                           .period = period,
                           .rt_required = false});
        }
        if (io_reader_ == nullptr) {
            return;
        }
        for (const std::string_view name : DEFAULT_TASKS) {
            check_priority(*io_reader_, io_node_, name, DEFAULT_PRIORITY);
        }
    }

    // CF-901 and CF-902 (9.1).
    void check_program(const Reader& reader) {
        const Document& document = reader.document();
        const std::optional<NodeId> name =
            reader.scalar(document.root(), "program");
        if (!name) {
            return;  // CF-202 already
        }
        const std::string_view program = document.text(*name);
        const auto found = listings_.find(program);
        const std::size_t count =
            found == listings_.end() ? 0 : found->second.size();
        if (count != 1) {
            error(reader, *name, CF_PROGRAM_TASKS,
                  "program " + quote_text(program) +
                      " must be in exactly one task");
            return;
        }
        // A task that names no task is CF-310 of the references, and not
        // CF-902 as well.
        if (const std::optional<NodeId> task =
                reader.scalar(document.root(), "task");
            task && tasks_.contains(document.text(*task)) &&
            document.text(*task) != found->second.front().task) {
            error(reader, *task, CF_PROGRAM_TASK,
                  "program " + quote_text(program) + ": task " +
                      quote_text(document.text(*task)) +
                      " disagrees with tasks.yaml (" +
                      quote_text(found->second.front().task) + ")");
        }
        check_blocks(reader, found->second.front().task);
    }

    // --- What a task reads and writes (rulings T1 to T3 of D-125) --------

    // The internal tags of the tag files, and the task each names.
    void collect_internal_tags() {
        for (std::size_t i = 0; i < files_.size(); ++i) {
            if (files_[i].source->path_class.type != FileType::TAGS) {
                continue;
            }
            const Reader& reader = readers_[i];
            for (const NodeId tag :
                 reader.items(reader.document().root(), "tags")) {
                if (reader.text_of(tag, "type") == "internal") {
                    internal_.try_emplace(reader.text_of(tag, "name"),
                                          reader.text_of(tag, "task"));
                }
            }
        }
    }

    // Adds tag `name`, read at `node`, to `reads`. A program's source takes
    // no `.quality` or `.age` (CF-310 of the references).
    void note(Reads& reads, std::string_view name, NodeId node) const {
        const std::optional<TagInfo> info = tags_->find(name);
        if (!info || !inputs_->classify) {
            return;
        }
        InputClass read = inputs_->classify(name, *info);
        if (read.poll_ms > reads.slowest_class.poll_ms) {
            reads.slowest = std::string(name);
            reads.slowest_class = read;
            reads.slowest_at = node;
        }
        if (!read.driver.empty() && reads.driver_tag.empty()) {
            reads.driver_tag = std::string(name);
            reads.driver = std::move(read.driver);
            reads.driver_at = node;
        }
        if (virtuals_.contains(name)) {
            reads.virtuals.emplace_back(name, node);
        }
    }

    // The sources and the `out` of a program's blocks (9.2), for the task
    // that lists it. A source `a.b` whose `a` is a block of the program is
    // a pin, not a tag.
    void check_blocks(const Reader& reader, std::string_view task) {
        const Document& document = reader.document();
        const std::span<const NodeId> blocks =
            reader.items(document.root(), "blocks");
        std::set<std::string_view> ids;
        for (const NodeId block : blocks) {
            ids.insert(reader.text_of(block, "id"));
        }
        Reads reads;
        for (const NodeId block : blocks) {
            for (const Entry& input : reader.entries(block, "in")) {
                const std::optional<NodeId> source =
                    document.kind(input.value) == NodeKind::SCALAR
                        ? std::optional<NodeId>(input.value)
                        : reader.scalar(input.value, "tag");
                if (!source) {
                    continue;
                }
                const std::string_view text = document.text(*source);
                if (const std::size_t dot = text.find('.');
                    dot != std::string_view::npos &&
                    ids.contains(text.substr(0, dot))) {
                    continue;
                }
                note(reads, text, *source);
            }
            for (const Entry& out : reader.entries(block, "out")) {
                if (document.kind(out.value) == NodeKind::SCALAR) {
                    check_write(reader, out.value, task);
                }
            }
        }
        if (inputs_->whole && tags_->complete()) {
            check_reads(reader, reads, task);
        }
    }

    // CF-905 (ruling T3): an internal tag has one writing task, its own.
    void check_write(const Reader& reader, NodeId node, std::string_view task) {
        const std::string_view tag = reader.document().text(node);
        const auto internal = internal_.find(tag);
        if (internal == internal_.end()) {
            return;  // not an internal tag: phase 6's (the block library)
        }
        if (!internal->second.empty() && internal->second != task) {
            error(reader, node, CF_WRITTEN_TWICE,
                  "tag " + quote_text(tag) + " is written by " +
                      quote_text(task) + " and " +
                      quote_text(internal->second));
            return;
        }
        const auto [first, added] =
            writers_.try_emplace(std::string(tag), task);
        if (!added && first->second != task) {
            error(reader, node, CF_WRITTEN_TWICE,
                  "tag " + quote_text(tag) + " is written by " +
                      quote_text(first->second) + " and " + quote_text(task));
        }
    }

    // CW-903 and CF-904 for one reader of `task`.
    // CW-903 is for programs' inputs and interlocks' conditions, not for
    // virtual tags (the lead's ruling R1 on #189); CF-904 is for every
    // reader. `slow_warning` says which.
    void check_reads(const Reader& reader, const Reads& reads,
                     std::string_view task_name, bool slow_warning = true) {
        const auto task = task_info_.find(task_name);
        if (task == task_info_.end()) {
            return;  // CF-310 of the references
        }
        if (slow_warning && task->second.period_ms != 0 &&
            reads.slowest_class.poll_ms > task->second.period_ms) {
            reporter_.warning(
                reader.document(), reads.slowest_at, CW_SLOWER_INPUT,
                "task " + quote_text(task_name) + " runs every " +
                    std::string(task->second.period) + " but reads " +
                    quote_text(reads.slowest) + ", polled every " +
                    reads.slowest_class.poll_text);
        }
        if (!task->second.rt_required) {
            return;
        }
        std::string tag = reads.driver_tag;
        std::string driver = reads.driver;
        NodeId place = reads.driver_at;
        // A virtual tag read gives what it reaches (ruling T2), found once
        // for every tag by the run-time expressions.
        for (const auto& [name, node] : reads.virtuals) {
            if (!tag.empty()) {
                break;
            }
            const auto found = virtuals_.find(name);
            if (found != virtuals_.end() &&
                !found->second->driver_tag.empty()) {
                tag = found->second->driver_tag;
                driver = found->second->driver;
                place = node;
            }
        }
        if (!tag.empty()) {
            error(reader, place, CF_RT_INPUT,
                  "task " + quote_text(task_name) +
                      " is rt_required and reads " + quote_text(tag) +
                      " from " + driver);
        }
    }

    void check_expression(const ExpressionInputs& expression) {
        if (expression.document == nullptr) {
            return;
        }
        const Reads reads{.slowest = expression.slowest,
                          .slowest_class = expression.slowest_class,
                          .slowest_at = expression.at,
                          .driver_tag = expression.driver_tag,
                          .driver = expression.driver,
                          .driver_at = expression.at,
                          .virtuals = {}};
        const Reader reader(*expression.document);
        check_reads(reader, reads, expression.task, !expression.is_virtual);
    }

    const TagNames* tags_;
    const TaskInputs* inputs_;
    std::span<const CheckedFile> files_;
    Reporter reporter_;
    std::map<std::string_view, Task, std::less<>> task_info_;
    // The internal tags and the task each names (empty for none).
    std::map<std::string_view, std::string_view, std::less<>> internal_;
    // The task that first writes each internal tag.
    std::map<std::string, std::string_view, std::less<>> writers_;
    // The virtual tags, by name: what each reads.
    std::map<std::string_view, const ExpressionInputs*, std::less<>> virtuals_;
    std::vector<Reader> readers_;
    std::optional<std::int64_t> io_priority_ = DEFAULT_IO_PRIORITY;
    const Reader* io_reader_ = nullptr;
    NodeId io_node_ = 0;
    // The tasks of the tree: views into tasks.yaml, or constants.
    std::set<std::string_view, std::less<>> tasks_;
    // Each program named in tasks.yaml, and where.
    std::map<std::string, std::vector<Listing>, std::less<>> listings_;
};

}  // namespace

void check_task_rules(const TagNames& tags, const ExpandedTree& tree,
                      std::span<const CheckedFile> files,
                      const TaskInputs& inputs, Diagnostics& diagnostics) {
    Checker(tags, tree, files, inputs, diagnostics).run();
}

namespace {

// The class of each input tag (D-125): what `input_classifier` returns
// reads it, shared by the copies of the function.
class InputClasses {
public:
    explicit InputClasses(const ExpandedTree& tree) {
        const Reader expanded(tree.document);
        for (const NodeId channel :
             expanded.items(tree.document.root(), "channels")) {
            read_channel(expanded, channel);
        }
    }

    [[nodiscard]] InputClass classify(std::string_view name,
                                      const TagInfo& info) const {
        InputClass read;
        std::string protocol;
        if (info.origin == TagOrigin::POINT || info.origin == TagOrigin::BIT) {
            protocol = info.protocol;
            if (const auto poll = polls_.find(info.point);
                poll != polls_.end()) {
                read.poll_ms = poll->second.first;
                read.poll_text = poll->second.second;
            }
        } else if (name.starts_with(CHANNEL_PREFIX)) {
            const std::string_view rest = name.substr(CHANNEL_PREFIX.size());
            const auto found =
                channel_protocols_.find(rest.substr(0, rest.find('.')));
            if (found != channel_protocols_.end()) {
                protocol = found->second;
            }
        } else if (name.starts_with(DEVICE_PREFIX)) {
            protocol = device_protocol(name.substr(DEVICE_PREFIX.size()));
        }
        if (!protocol.empty() && protocol != "io") {
            read.driver = "drv-" + protocol;
        }
        return read;
    }

private:
    static constexpr std::string_view CHANNEL_PREFIX = "sys.drv.";
    static constexpr std::string_view DEVICE_PREFIX = "sys.dev.";

    void read_channel(const Reader& expanded, NodeId channel) {
        const std::string protocol(expanded.text_of(channel, "protocol"));
        const std::optional<NodeId> keys = expanded.child(channel, "channel");
        if (keys) {
            channel_protocols_.try_emplace(
                std::string(expanded.text_of(*keys, "name")), protocol);
        }
        std::map<std::string_view, std::string_view> groups;
        if (keys && expanded.child(*keys, "poll_groups")) {
            for (const Entry& group : expanded.entries(*keys, "poll_groups")) {
                groups.emplace(expanded.document().text(group.key),
                               expanded.text_of(group.value, "period"));
            }
        } else {
            groups.insert(DEFAULT_GROUPS.begin(), DEFAULT_GROUPS.end());
        }
        for (const NodeId device : expanded.items(channel, "devices")) {
            device_protocols_.try_emplace(
                std::string(expanded.text_of(device, "name")), protocol);
            // An `io` line is read every scan cycle and has no poll group
            // (the lead's interim ruling, D-125): no CW-903.
            if (protocol == "io") {
                continue;
            }
            for (const NodeId point : expanded.items(device, "points")) {
                read_point(expanded, point, groups);
            }
        }
    }

    // A read point's poll period: its group's, `normal` when it names
    // none; a `wo` point is not read (4.7), an unknown group is CF-604.
    void read_point(
        const Reader& expanded, NodeId point,
        const std::map<std::string_view, std::string_view>& groups) {
        if (expanded.text_of(point, "access") == "wo") {
            return;
        }
        std::string_view group = expanded.text_of(point, "poll_group");
        group = group.empty() ? DEFAULT_GROUP : group;
        const auto period = groups.find(group);
        if (period == groups.end()) {
            return;
        }
        if (const std::optional<std::uint64_t> poll_ms =
                parse_duration_ms(period->second)) {
            polls_.try_emplace(point, *poll_ms, std::string(period->second));
        }
    }

    // The protocol of the device a `sys.dev.` tag names: the longest device
    // name the rest starts with, as names hold dots.
    [[nodiscard]] std::string device_protocol(std::string_view rest) const {
        std::string protocol;
        for (std::size_t dot = rest.find('.'); dot != std::string_view::npos;
             dot = rest.find('.', dot + 1)) {
            if (const auto found = device_protocols_.find(rest.substr(0, dot));
                found != device_protocols_.end()) {
                protocol = found->second;
            }
        }
        return protocol;
    }

    std::map<NodeId, std::pair<std::uint64_t, std::string>> polls_;
    std::map<std::string, std::string, std::less<>> device_protocols_;
    std::map<std::string, std::string, std::less<>> channel_protocols_;
};

}  // namespace

InputClassifier input_classifier(const ExpandedTree& tree) {
    auto classes = std::make_shared<const InputClasses>(tree);
    return [classes](std::string_view name, const TagInfo& info) {
        return classes->classify(name, info);
    };
}

}  // namespace scada::conf
