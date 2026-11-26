// Stage 4: templates and instances expanded in the merge order of contract
// 4.5 (decision D-114). Values are copied out of the parsed files into a
// mutable tree, merged, evaluated, and written into one Document.

#include "scada/conf/expand.h"

#include "config_eval.h"
#include "expression_site.h"
#include "expression_types.h"
#include "grouped_errors.h"
#include "name_pattern.h"
#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expression.h"
#include "scada/conf/names.h"
#include "scada/conf/node.h"
#include "scada/conf/references.h"
#include "scada/conf/scalar.h"
#include "scada/conf/tree.h"
#include "scada/conf/types.h"

#include <algorithm>
#include <array>
#include <cmath>
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
#include <tuple>
#include <utility>
#include <vector>

namespace scada::conf {

namespace {

constexpr std::uint16_t CF_SIZE_LIMIT = 113;
constexpr std::uint16_t CF_MISSING_KEY = 202;
constexpr std::uint16_t CF_EXTENDS = 401;
constexpr std::uint16_t CF_TAG_NAME_LENGTH = 402;
constexpr std::uint16_t CF_GENERATED_NAME_LENGTH = 403;
constexpr std::uint16_t CF_BAD_NAME = 212;
constexpr std::uint16_t CF_EVALUATION = 410;
constexpr std::uint16_t CF_NO_VALUE = 411;
constexpr std::uint16_t CF_BAD_VALUE = 412;
constexpr std::uint16_t CF_NO_SUCH_POINT = 413;
constexpr std::uint16_t CF_TOO_MANY_POINTS = 414;
constexpr std::uint16_t CF_PARAMETER_TYPE = 415;
constexpr std::uint16_t CF_POINT_TWICE = 416;
constexpr std::uint16_t CF_NO_ADDRESS = 417;
constexpr std::uint16_t CF_WRONG_PROTOCOL = 420;
constexpr std::uint16_t CF_MIXED_TEMPLATES = 421;
constexpr std::uint16_t CF_WHEN_MERGED = 419;
constexpr std::uint16_t CF_BINDINGS = 422;
constexpr std::uint16_t CF_TYPE_MISMATCH = 523;

// Most parents of a template (4.3, D-71).
constexpr std::size_t MAX_PARENTS = 4;
// Most values of one `repeat` variable, of the pairs of two, and points of
// one instance (4.4, CF-414).
constexpr std::size_t MAX_REPEAT_VALUES = 65536;
constexpr std::size_t MAX_POINTS = 65536;
// `limits.max_tags` without a value (4.1 of the system file).
constexpr std::int64_t DEFAULT_MAX_TAGS = ExpandedTree::DEFAULT_MAX_TAGS;
constexpr std::uint16_t CF_TOO_MANY_TAGS = 1001;
// The nodes of parsed expressions kept for later evaluations, about 33 MB;
// past it the cache is emptied (E1 of the review of #146).
constexpr std::size_t MAX_COMPILED_NODES = 250'000;
// Most bytes of a tag name (2.1, #119).
constexpr std::size_t MAX_TAG_NAME_BYTES = 127;

// --- The mutable tree --------------------------------------------------------

// A value while it is merged: a copy of a parsed node, or a computed scalar.
struct Value;

// Member and Value hold each other: their copies recurse, bounded by the
// nesting limit of 32 (contract 1.3).
// NOLINTNEXTLINE(misc-no-recursion)
struct Member {
    std::string key;
    const CheckedFile* key_file = nullptr;
    NodeId key_node = 0;
    std::vector<Value> value;  // exactly one; a vector keeps Value incomplete
};

// A plain aggregate that the merge reads and writes field by field; its
// member functions are lookups, not an interface to keep it consistent.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
// NOLINTNEXTLINE(misc-no-recursion): see Member
struct Value {
    NodeKind kind = NodeKind::MAP;
    // Where it was written (for a computed scalar, its expression).
    const CheckedFile* file = nullptr;
    NodeId node = 0;
    // A computed scalar's text and type (13.3).
    bool computed = false;
    std::string text;
    ScalarType type = ScalarType::NONE;
    std::vector<Member> members;  // a map, in order
    std::vector<Value> items;     // a sequence

    // An empty map that takes the position of `node` of `file`.
    [[nodiscard]] static Value map_at(const CheckedFile* file, NodeId node) {
        Value value;
        value.kind = NodeKind::MAP;
        value.file = file;
        value.node = node;
        return value;
    }

    [[nodiscard]] Value* find(std::string_view key) {
        for (Member& member : members) {
            if (member.key == key) {
                return member.value.data();
            }
        }
        return nullptr;
    }

    [[nodiscard]] const Value* find(std::string_view key) const {
        for (const Member& member : members) {
            if (member.key == key) {
                return member.value.data();
            }
        }
        return nullptr;
    }

    void erase(std::string_view key) {
        std::erase_if(
            members, [key](const Member& member) { return member.key == key; });
    }

    [[nodiscard]] std::string_view scalar_text() const {
        if (kind != NodeKind::SCALAR) {
            return {};
        }
        return computed ? std::string_view(text) : file->document->text(node);
    }

    [[nodiscard]] ScalarType scalar_type() const {
        return computed ? type : file->types->of(node);
    }

    [[nodiscard]] Location location() const {
        return file->document->location(node);
    }
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

// A copy of `node` of `file`.
// NOLINTNEXTLINE(misc-no-recursion): bounded by the nesting limit of 32
Value copy_of(const CheckedFile& file, NodeId node) {
    const Document& document = *file.document;
    Value value = Value::map_at(&file, node);
    value.kind = document.kind(node);
    if (value.kind == NodeKind::MAP) {
        for (const Entry& entry : document.entries(node)) {
            value.members.push_back(
                Member{.key = std::string(document.text(entry.key)),
                       .key_file = &file,
                       .key_node = entry.key,
                       .value = {copy_of(file, entry.value)}});
        }
    } else if (value.kind == NodeKind::SEQUENCE) {
        for (const NodeId item : document.items(node)) {
            value.items.push_back(copy_of(file, item));
        }
    }
    return value;
}

// The keys that exclude each other within a map under `parent` (4.5,
// "Exclusive groups"); `on` and `off` of a write encoding count as one.
std::span<const std::vector<std::string_view>> exclusive_groups(
    std::string_view parent) {
    static const std::vector<std::vector<std::string_view>> SCALE = {
        {"gain", "divide_by"}};
    static const std::vector<std::vector<std::string_view>> ADDRESS = {
        {"bit", "field", "on", "off", "fault"}, {"register", "parts"}};
    static const std::vector<std::vector<std::string_view>> WRITE = {
        {"value", "values", "on", "mask"}, {"value", "values", "off", "mask"}};
    if (parent == "scale") {
        return SCALE;
    }
    if (parent == "address") {
        return ADDRESS;
    }
    if (parent == "write") {
        return WRITE;
    }
    return {};
}

bool is_replacing(const Value& value) {
    const Value* replace = value.find("replace");
    return replace != nullptr && replace->scalar_text() == "true";
}

// Removes every `replace` key below `value`: it only steers merging.
// NOLINTNEXTLINE(misc-no-recursion): bounded by the nesting limit of 32
void strip_replace(Value& value) {
    value.erase("replace");
    for (Member& member : value.members) {
        strip_replace(member.value.front());
    }
    for (Value& item : value.items) {
        strip_replace(item);
    }
}

// True if `member` has an integer key, as stage 3 typed it by the schema's
// key type (`bits`, enumerations, a quality rule's `map`, `states`, a BOOL
// point's `values`; 1.2).
bool integer_key(const Member& member) {
    return member.key_file != nullptr &&
           member.key_file->types->of(member.key_node) == ScalarType::INTEGER;
}

// True if two map keys are one key: the same text, or two integer keys of
// one value (`15` and `0xF`; B6 of the review of #146). Keys the schema
// types as strings (`write.values`) are compared as text.
bool same_key(const Member& left, const Member& right) {
    if (left.key == right.key) {
        return true;
    }
    if (!integer_key(left) || !integer_key(right)) {
        return false;
    }
    const std::optional<Integer> left_value = parse_integer(left.key);
    const std::optional<Integer> right_value = parse_integer(right.key);
    if (!left_value || !right_value ||
        left_value->magnitude != right_value->magnitude) {
        return false;
    }
    return left_value->magnitude == 0 ||
           left_value->negative == right_value->negative;
}

// The member of `map` that is the key of `member` by `same_key`.
Member* member_of_key(Value& map, const Member& member) {
    for (Member& candidate : map.members) {
        if (same_key(candidate, member)) {
            return &candidate;
        }
    }
    return nullptr;
}

// The text by which a key is one key (`same_key`): an integer key by its
// value, any other by its text.
std::string key_identity(const Member& member) {
    if (integer_key(member)) {
        if (const std::optional<Integer> value = parse_integer(member.key)) {
            return std::string(value->negative && value->magnitude != 0 ? "-"
                                                                        : "") +
                   std::to_string(value->magnitude);
        }
    }
    return "'" + member.key;
}

void merge(Value& into, const Value& over, std::string_view parent);

// `merge` of two maps without exclusive groups, by an index of the keys of
// `into`: a map of n keys over one of n keys is O(n log n), not O(n^2) (T1
// of the review of #146).
// NOLINTNEXTLINE(misc-no-recursion): bounded by the nesting limit of 32
void merge_by_index(Value& into, const Value& over) {
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < into.members.size(); ++i) {
        index.emplace(key_identity(into.members[i]), i);
    }
    for (const Member& member : over.members) {
        if (member.key == "replace") {
            continue;
        }
        const auto found = index.find(key_identity(member));
        if (found == index.end()) {
            index.emplace(key_identity(member), into.members.size());
            into.members.push_back(member);
            continue;
        }
        Member& existing = into.members[found->second];
        // The child's spelling of an integer key is kept with its value.
        existing.key = member.key;
        existing.key_file = member.key_file;
        existing.key_node = member.key_node;
        merge(existing.value.front(), member.value.front(), member.key);
    }
}

// Merges `over` into `into` (4.5, "How two values merge"): maps key by key,
// the keys of `over` winning; a map with `replace: true`, and any other
// value, replaces what was there whole. `parent` is the key of the maps.
// NOLINTNEXTLINE(misc-no-recursion): bounded by the nesting limit of 32
void merge(Value& into, const Value& over, std::string_view parent) {
    if (into.kind != NodeKind::MAP || over.kind != NodeKind::MAP ||
        is_replacing(over)) {
        into = over;
        return;
    }
    if (exclusive_groups(parent).empty()) {
        merge_by_index(into, over);
        return;
    }
    for (const Member& member : over.members) {
        if (member.key == "replace") {
            continue;
        }
        for (const std::vector<std::string_view>& group :
             exclusive_groups(parent)) {
            if (std::ranges::find(group, member.key) == group.end()) {
                continue;
            }
            std::erase_if(into.members, [&group, &member](const Member& old) {
                return old.key != member.key &&
                       std::ranges::find(group, old.key) != group.end() &&
                       !(member.key == "on" && old.key == "off") &&
                       !(member.key == "off" && old.key == "on");
            });
        }
        if (Member* existing = member_of_key(into, member)) {
            // The child's spelling of an integer key is kept with its value.
            existing->key = member.key;
            existing->key_file = member.key_file;
            existing->key_node = member.key_node;
            merge(existing->value.front(), member.value.front(), member.key);
        } else {
            into.members.push_back(member);
        }
    }
}

// --- Parameters --------------------------------------------------------------

struct Parameter {
    std::string type;  // int, number, bool, string, enum, set
    const CheckedFile* file = nullptr;
    NodeId declaration = 0;
    std::optional<Value> default_value;
    std::optional<Value> min;
    std::optional<Value> max;
    std::vector<std::string> values;  // of an enum or set
};

// A point while the levels are merged, with the variables `repeat` bound to
// it and the template that bound each.
struct Point {
    std::string name;
    Value value;
    std::map<std::string, std::pair<std::int64_t, std::string>, std::less<>>
        bindings;
    // The `when` of each `repeat` block that made the point, decided at
    // step 4 with the point's own (the ruling O-1 on #146).
    std::vector<Value> conditions;
};

class Expander {
public:
    Expander(std::span<const CheckedFile> files, Diagnostics& diagnostics,
             const ExpandLimits& limits, ExpandCounts* counts)
        : files_(files),
          diagnostics_(&diagnostics),
          limits_(limits),
          counts_(counts),
          grouped_(limits.kept_errors) {}

    std::optional<ExpandedTree> run() {
        for (const CheckedFile& file : files_) {
            if (file.source->path_class.type == FileType::TEMPLATE) {
                templates_.emplace(file.source->path_class.stem, &file);
            }
            file_index_.emplace(&file, static_cast<FileIndex>(paths_.size()));
            paths_.push_back(file.source->path);
        }
        max_tags_ = max_tags_of();
        writer_.begin();
        // A template's own faults once, at the template, whether or not an
        // instance uses it (the ruling O-6 on #146).
        for (const CheckedFile& file : files_) {
            if (file.source->path_class.type == FileType::TEMPLATE) {
                check_template(file);
            }
        }
        for (const CheckedFile& file : files_) {
            if (file.source->path_class.type == FileType::CHANNEL &&
                !stopped_) {
                expand_channel(file);
            }
        }
        flush_diagnostics();
        if (failed_) {
            return std::nullopt;
        }
        std::optional<ExpandedTree> tree = writer_.finish();
        if (tree) {
            tree->max_tags = max_tags_;
        }
        return tree;
    }

private:
    // --- Errors ----------------------------------------------------------

    // An error at `site`: in the instance's own file it is there; in a
    // template it is at the instance, with the template's line as a note.
    void error(std::uint16_t code, const Value& site, std::string message,
               std::string detail = {}) {
        error_at(code, *site.file, site.node, std::move(message),
                 std::move(detail));
    }

    void error_at(std::uint16_t code, const CheckedFile& file, NodeId node,
                  std::string message, std::string detail = {}) {
        error_in(code, file, file.document->location(node), std::move(message),
                 std::move(detail));
    }

    void error_in(std::uint16_t code, const CheckedFile& file,
                  Location location, std::string message,
                  std::string detail = {}) {
        failed_ = true;
        device_failed_ = true;
        Diagnostic diagnostic{.severity = Severity::ERROR,
                              .code = code,
                              .location = std::move(location),
                              .message = std::move(message),
                              .notes = {}};
        if (channel_ != nullptr && &file != channel_ && instance_) {
            diagnostic.notes.push_back(
                Diagnostic::Note{.kind = "template",
                                 .location = std::move(diagnostic.location),
                                 .detail = std::move(detail)});
            diagnostic.location = *instance_;
        }
        // One cause in a template, met by many instances, is one error at
        // the first, with their count (the ruling O-6 on #146); past the
        // kept limit an error is counted, not kept (C2 of the review of
        // #146, D2 of the delta review).
        const std::string key = diagnostic.message;
        grouped_.add(std::move(diagnostic), key, device_name_, *diagnostics_);
    }

    // Gives the buffered diagnostics, in the order they arose.
    void flush_diagnostics() {
        if (counts_ != nullptr) {
            counts_->kept_texts = grouped_.kept_texts();
            counts_->causes = grouped_.causes();
            counts_->parses = parses_;
            counts_->typings = typings_;
            counts_->written_text = writer_.text_bytes();
        }
        grouped_.flush(*diagnostics_);
    }

    // --- Channels and devices -----------------------------------------------

    struct Device {
        Value keys;                 // the device object as it will be written
        std::vector<Point> points;  // in order
    };

    void expand_channel(const CheckedFile& file) {
        channel_ = &file;
        writer_.begin_channel(file);
        const Document& document = *file.document;
        const NodeId root = document.root();
        if (const std::optional<NodeId> devices =
                document.kind(root) == NodeKind::MAP
                    ? document.find(root, "devices")
                    : std::nullopt) {
            for (const NodeId device : document.items(*devices)) {
                if (document.kind(device) != NodeKind::MAP) {
                    continue;
                }
                if (stopped_) {
                    break;
                }
                std::optional<Device> expanded = expand_device(file, device);
                // A device is written as soon as it is finished and then
                // dropped; after an error nothing more is written, as no
                // tree is given (C2 of the review of #146).
                if (expanded && instance_ && !failed_) {
                    writer_.device(*expanded, *instance_);
                    tags_ += tags_of(*expanded);
                    check_totals(*instance_);
                }
            }
        }
        writer_.end_channel();
        channel_ = nullptr;
        instance_.reset();
    }

    // The running totals of the expanded tree after a device, so before the
    // next is expanded: its nodes and text (CF-113) and its tags (CF-1001).
    // Passing one stops the expansion (1.3).
    void check_totals(const Location& instance) {
        std::string message;
        std::uint16_t code = CF_SIZE_LIMIT;
        if (writer_.nodes() > limits_.nodes) {
            message = nodes_message();
        } else if (writer_.text_bytes() > limits_.text_bytes) {
            message = "the expanded tree exceeds its limit of " +
                      bytes_text(limits_.text_bytes) + " of text";
        } else if (tags_ > static_cast<std::uint64_t>(max_tags_)) {
            code = CF_TOO_MANY_TAGS;
            message = std::to_string(tags_) + " tags, the limit is " +
                      std::to_string(max_tags_);
        } else {
            return;
        }
        error_in(code, *channel_, instance, std::move(message));
        stopped_ = true;
    }

    // The tags of a device: a point's, unless `tag: false`, and each named
    // bit's (12.5).
    static std::uint64_t tags_of(const Device& device) {
        std::uint64_t count = 0;
        for (const Point& point : device.points) {
            const Value* tag = point.value.find("tag");
            if (tag == nullptr || tag->scalar_text() != "false") {
                ++count;
            }
            if (const Value* bits = point.value.find("bits")) {
                count += bits->members.size();
            }
        }
        return count;
    }

    // `limits.max_tags` of system.yaml, or its default.
    [[nodiscard]] std::int64_t max_tags_of() const {
        for (const CheckedFile& file : files_) {
            if (file.source->path_class.type != FileType::SYSTEM) {
                continue;
            }
            const Document& document = *file.document;
            const std::optional<NodeId> limits =
                document.kind(document.root()) == NodeKind::MAP
                    ? document.find(document.root(), "limits")
                    : std::nullopt;
            const std::optional<NodeId> max_tags =
                limits && document.kind(*limits) == NodeKind::MAP
                    ? document.find(*limits, "max_tags")
                    : std::nullopt;
            if (max_tags) {
                if (const std::optional<Integer> value =
                        parse_integer(document.text(*max_tags))) {
                    if (const std::optional<std::int64_t> limit =
                            as_int64(*value)) {
                        return *limit;
                    }
                }
            }
        }
        return DEFAULT_MAX_TAGS;
    }

    // The nodes of `value` as the builder will hold them, a key counted as
    // one.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by the nesting limit of 32
    static std::size_t node_count(const Value& value) {
        std::size_t count = 1;
        for (const Member& member : value.members) {
            count += 1 + node_count(member.value.front());
        }
        for (const Value& item : value.items) {
            count += node_count(item);
        }
        return count;
    }

    std::optional<Device> expand_device(const CheckedFile& file, NodeId node) {
        const Value written = copy_of(file, node);
        const Value* name = written.find("name");
        if (name == nullptr) {
            return std::nullopt;
        }
        instance_ = name->location();
        device_name_ = std::string(name->scalar_text());
        device_failed_ = false;
        made_nodes_ = 0;
        params_.clear();
        param_weights_.clear();
        Device device{.keys = written, .points = {}};
        // `identify` is the template's with the instance's appended (step
        // 6), written with the device keys.
        for (const std::string_view key :
             {"template", "params", "defaults", "overrides", "disable", "only",
              "points", "identify"}) {
            device.keys.erase(key);
        }
        // Both take the instance's position: the device keys are written
        // at the device, and the defaults are under every point.
        Value device_keys = Value::map_at(written.file, written.node);
        Value defaults = Value::map_at(written.file, written.node);
        refuse_when(written.find("defaults"), "defaults");
        refuse_when(written.find("overrides"), "overrides");
        const Value* reference = written.find("template");
        if (reference != nullptr) {
            // A template with a fault of its own is reported at it; its
            // instances are not expanded.
            if (const auto found = templates_.find(reference->scalar_text());
                found != templates_.end() &&
                faulty_templates_.contains(found->second)) {
                return std::nullopt;
            }
            std::optional<std::vector<const CheckedFile*>> chain =
                chain_of(*reference);
            if (!chain ||
                std::ranges::any_of(*chain, [this](const auto* level) {
                    return faulty_templates_.contains(level);
                })) {
                return std::nullopt;
            }
            if (!expand_templates(*chain, written, device_keys, defaults,
                                  device.points)) {
                return device;
            }
        } else {
            // Without a template, steps 1 to 3 are empty (4.5): the
            // instance's points enter step 4 (B2 of the review of #146).
            add_instance_points(written, device);
        }
        keep_present(written, device.points);
        apply_defaults(defaults, written, device.points);
        // Past a limit at step 5 the device's later steps are not done.
        if (stopped_) {
            return device;
        }
        apply_overrides(written, device_keys, device.points);
        if (reference != nullptr) {
            // With a template, after step 6: no default reaches them.
            add_instance_points(written, device, true);
        }
        // After step 6, which may change a raw type (B5 of the review of
        // #146).
        for (Point& point : device.points) {
            pick_invalid_values(point.value,
                                channel_->source->path_class.protocol);
        }
        // The template's device keys go under the instance's.
        Value keys = device_keys;
        merge(keys, device.keys, "");
        device.keys = std::move(keys);
        if (device_failed_) {
            return device;
        }
        finish_points(written, device);
        return device;
    }

    // The root of a template, copied once for the run, not twice per
    // instance (T2 of the review of #146); levels only read it.
    const Value& root_of(const CheckedFile& file) {
        auto found = roots_.find(&file);
        if (found == roots_.end()) {
            found = roots_.emplace(&file, copy_of(file, file.document->root()))
                        .first;
        }
        return found->second;
    }

    // --- The templates alone ----------------------------------------------

    // What a template gets wrong whatever its instance: its chain (CF-401,
    // CF-421), its parameters along it (CF-415, CF-202), two written points
    // of one name in it (CF-416), a base template's `address`, `read` or
    // `identify` (CF-421, 4.1) and `when` in its `defaults` (CF-419).
    void check_template(const CheckedFile& file) {
        channel_ = nullptr;
        instance_.reset();
        device_failed_ = false;
        const Document& document = *file.document;
        if (document.kind(document.root()) != NodeKind::MAP) {
            return;
        }
        const Value& root = root_of(file);
        const std::string& name = file.source->path_class.stem;
        Value reference;
        reference.kind = NodeKind::SCALAR;
        reference.file = &file;
        reference.node = document.root();
        reference.computed = true;
        reference.text = name;
        reference.type = ScalarType::STRING;
        if (std::optional<std::vector<const CheckedFile*>> chain =
                chain_of(reference)) {
            collect_parameters(*chain);
        }
        std::set<std::string, std::less<>> written;
        const auto written_points = [&](const Value* list) {
            if (list == nullptr) {
                return;
            }
            for (const Value& item : list->items) {
                const Value* point_name = item.find("name");
                if (point_name == nullptr) {
                    continue;
                }
                const std::string text(point_name->scalar_text());
                if (text.find('{') == std::string::npos &&
                    !written.insert(text).second) {
                    error(CF_POINT_TWICE, *point_name,
                          "point " + quote_text(text) + " is defined twice");
                }
            }
        };
        written_points(root.find("points"));
        if (protocol_of(file).empty()) {
            check_base_template(root, name);
        }
        refuse_when(root.find("defaults"), "defaults");
        if (device_failed_) {
            faulty_templates_.insert(&file);
        }
    }

    // 4.1: a base template has common parts only.
    void check_base_template(const Value& root, const std::string& name) {
        for (const std::string_view key : {"read", "identify"}) {
            if (const Value* own = root.find(key)) {
                error(CF_MIXED_TEMPLATES, *own,
                      "template " + quote_text(name) +
                          ": a base template has "
                          "no " +
                          std::string(key));
            }
        }
        const auto addresses = [&](const Value* list) {
            if (list == nullptr) {
                return;
            }
            for (const Value& item : list->items) {
                if (const Value* address = item.find("address")) {
                    error(CF_MIXED_TEMPLATES, *address,
                          "template " + quote_text(name) +
                              ": a base template has no address");
                }
            }
        };
        addresses(root.find("points"));
        if (const Value* blocks = root.find("repeat")) {
            for (const Value& block : blocks->items) {
                addresses(block.find("points"));
            }
        }
        if (const Value* defaults = root.find("defaults")) {
            if (const Value* address = defaults->find("address")) {
                error(CF_MIXED_TEMPLATES, *address,
                      "template " + quote_text(name) +
                          ": a base template has no address");
            }
        }
    }

    // CF-419: presence is decided before `defaults` and `overrides` are
    // merged (4.4). `owner` is a `defaults` map, or with `key` "overrides"
    // the map of the points' overrides.
    void refuse_when(const Value* owner, std::string_view key) {
        if (owner == nullptr || owner->kind != NodeKind::MAP) {
            return;
        }
        if (key != "overrides") {
            refuse_when_in(*owner, key);
            return;
        }
        for (const Member& member : owner->members) {
            refuse_when_in(member.value.front(), key);
        }
    }

    void refuse_when_in(const Value& map, std::string_view key) {
        if (const Value* when = map.find("when")) {
            error(CF_WHEN_MERGED, *when,
                  "when cannot be given in " + std::string(key));
        }
    }

    // --- Step 1: the chain -------------------------------------------------

    // The chain from the root template down to the instance's, or nothing
    // when a template of it is missing (stage 5's CF-310) or it is wrong.
    std::optional<std::vector<const CheckedFile*>> chain_of(
        const Value& reference) {
        std::vector<const CheckedFile*> chain;
        std::string name(reference.scalar_text());
        Value named_by = reference;
        Value first_extends = reference;
        while (true) {
            const auto found = templates_.find(name);
            if (found == templates_.end()) {
                return std::nullopt;
            }
            const CheckedFile* file = found->second;
            // A cycle at the `extends` that closes it, of the template
            // whose line it is; a chain too deep at the first template's
            // `extends` (once, at the template: the ruling O-6 on #146).
            if (std::ranges::find(chain, file) != chain.end()) {
                error(CF_EXTENDS, named_by,
                      "template " +
                          quote_text(chain.back()->source->path_class.stem) +
                          ": extends forms a cycle through " +
                          quote_text(name));
                return std::nullopt;
            }
            if (chain.size() > MAX_PARENTS) {
                error(CF_EXTENDS, first_extends,
                      "template " +
                          quote_text(chain.front()->source->path_class.stem) +
                          ": extends is deeper than 4 parents");
                return std::nullopt;
            }
            chain.push_back(file);
            const Document& document = *file->document;
            const std::optional<NodeId> parent =
                document.kind(document.root()) == NodeKind::MAP
                    ? document.find(document.root(), "extends")
                    : std::nullopt;
            if (!parent) {
                break;
            }
            named_by = copy_of(*file, *parent);
            if (chain.size() == 1) {
                first_extends = named_by;
            }
            name = std::string(document.text(*parent));
        }
        std::ranges::reverse(chain);
        if (!chain_fits(chain, reference)) {
            return std::nullopt;
        }
        return chain;
    }

    static std::string protocol_of(const CheckedFile& file) {
        const Document& document = *file.document;
        if (document.kind(document.root()) != NodeKind::MAP) {
            return {};
        }
        const std::optional<NodeId> protocol =
            document.find(document.root(), "protocol");
        return protocol ? std::string(document.text(*protocol)) : std::string();
    }

    // 4.3: a base template extends only a base template; a protocol
    // template extends a base template or one of its protocol. The
    // instance's template is of its channel's protocol (CF-420), and a
    // protocol this version does not know is stage 5's CF-310.
    bool chain_fits(const std::vector<const CheckedFile*>& chain,
                    const Value& reference) {
        for (std::size_t i = 1; i < chain.size(); ++i) {
            const std::string parent = protocol_of(*chain[i - 1]);
            const std::string child = protocol_of(*chain[i]);
            if (!parent.empty() && parent != child) {
                const Document& document = *chain[i]->document;
                const NodeId key = document.find(document.root(), "extends")
                                       .value_or(document.root());
                error_at(CF_MIXED_TEMPLATES, *chain[i], key,
                         "template " +
                             quote_text(chain[i]->source->path_class.stem) +
                             ": " +
                             (child.empty() ? "a base template cannot extend a "
                                              "protocol template"
                                            : "a template cannot extend one of "
                                              "another protocol"));
                return false;
            }
        }
        const std::string protocol = protocol_of(*chain.back());
        if (!protocol.empty() && !is_known_protocol(protocol)) {
            return false;
        }
        if (channel_ == nullptr) {
            return true;
        }
        const std::string& wanted = channel_->source->path_class.protocol;
        if (protocol != wanted) {
            error(protocol.empty() ? CF_MIXED_TEMPLATES : CF_WRONG_PROTOCOL,
                  reference,
                  protocol.empty()
                      ? "template " + quote_text(reference.scalar_text()) +
                            ": a base template has no protocol; a device "
                            "uses a template of its channel's"
                      : "template " + quote_text(reference.scalar_text()) +
                            " is for " + quote_text(protocol) +
                            ", the channel is " + quote_text(wanted));
            return false;
        }
        return true;
    }

    // --- Steps 2 to 6 --------------------------------------------------------

    // Steps 2 and 3: the parameters, then the levels of the chain merged
    // into `points`, with their device keys and defaults.
    bool expand_templates(const std::vector<const CheckedFile*>& chain,
                          const Value& instance, Value& device_keys,
                          Value& defaults, std::vector<Point>& points) {
        collect_parameters(chain);
        if (!bind_parameters(instance)) {
            return false;
        }
        for (const CheckedFile* level : chain) {
            const Value& root = root_of(*level);
            merge_level_keys(root, device_keys, defaults);
            expand_level(*level, root, points);
        }
        return !device_failed_;
    }

    // Step 2: the parameter declarations of the chain, root first. A
    // declaration may leave its type to a parent (a child that changes a
    // default); one that no level types is CF-202.
    void collect_parameters(const std::vector<const CheckedFile*>& chain) {
        declarations_.clear();
        for (const CheckedFile* level : chain) {
            const Value& root = root_of(*level);
            const Value* params = root.find("params");
            if (params == nullptr) {
                continue;
            }
            for (const Member& member : params->members) {
                declare(member);
            }
        }
        for (const auto& [name, declared] : declarations_) {
            if (declared.type.empty()) {
                error_at(CF_MISSING_KEY, *declared.file, declared.declaration,
                         "parameter " + quote_text(name) + " needs type",
                         "parameter " + name);
            }
        }
    }

    void declare(const Member& member) {
        const Value& spec = member.value.front();
        const Value* type = spec.find("type");
        Parameter declared;
        declared.type =
            type != nullptr ? std::string(type->scalar_text()) : std::string();
        declared.file = member.key_file;
        declared.declaration = member.key_node;
        const auto read =
            [&spec](std::string_view key) -> std::optional<Value> {
            const Value* value = spec.find(key);
            return value != nullptr ? std::optional<Value>(*value)
                                    : std::nullopt;
        };
        declared.default_value = read("default");
        declared.min = read("min");
        declared.max = read("max");
        if (const Value* values = spec.find("values")) {
            for (const Value& item : values->items) {
                declared.values.emplace_back(item.scalar_text());
            }
        }
        const auto found = declarations_.find(member.key);
        if (found == declarations_.end()) {
            declarations_.emplace(member.key, std::move(declared));
            return;
        }
        // A child may change the default, min and max, never the type.
        Parameter& inherited = found->second;
        if (!declared.type.empty() && declared.type != inherited.type) {
            error(CF_PARAMETER_TYPE, *type,
                  "parameter " + quote_text(member.key) +
                      " cannot change its type");
            return;
        }
        for (auto [own, kept] :
             {std::pair{&declared.default_value, &inherited.default_value},
              std::pair{&declared.min, &inherited.min},
              std::pair{&declared.max, &inherited.max}}) {
            if (*own) {
                *kept = *own;
            }
        }
        if (!declared.values.empty()) {
            inherited.values = declared.values;
        }
    }

    bool bind_parameters(const Value& instance) {
        const Value* given = instance.find("params");
        // By name, not a search per declaration (T1 of the review of #146).
        std::map<std::string_view, const Value*> values;
        if (given != nullptr) {
            for (const Member& member : given->members) {
                values.emplace(member.key, member.value.data());
                if (!declarations_.contains(member.key)) {
                    error_at(CF_BAD_VALUE, *member.key_file, member.key_node,
                             "parameter " + quote_text(member.key) +
                                 ": the template has no parameter of this "
                                 "name");
                }
            }
        }
        for (const auto& [name, declared] : declarations_) {
            const auto found = values.find(name);
            const Value* value =
                found != values.end() ? found->second : nullptr;
            if (value == nullptr && declared.default_value) {
                value = &*declared.default_value;
            }
            if (value == nullptr) {
                error_at(CF_NO_VALUE, *declared.file, declared.declaration,
                         "parameter " + quote_text(name) + " needs a value",
                         "parameter " + name);
                continue;
            }
            std::optional<ConfigValue> bound =
                parameter_value(name, declared, *value);
            if (bound) {
                param_weights_.emplace(name, weight_of(*bound));
                params_.emplace(name, std::move(*bound));
            }
        }
        return !device_failed_;
    }

    // The value of a parameter, checked against its declaration (CF-412).
    std::optional<ConfigValue> parameter_value(const std::string& name,
                                               const Parameter& declared,
                                               const Value& value) {
        const auto refuse = [&](const std::string& reason) {
            error(CF_BAD_VALUE, value,
                  "parameter " + quote_text(name) + ": " + reason,
                  "parameter " + name);
            return std::optional<ConfigValue>();
        };
        const std::string& type = declared.type;
        if (type == "set") {
            if (value.kind != NodeKind::SEQUENCE) {
                return refuse("a set is a list of its values");
            }
            ConfigValue set;
            set.type = ValueType::SET;
            for (const Value& item : value.items) {
                const std::string text(item.scalar_text());
                if (item.kind != NodeKind::SCALAR ||
                    std::ranges::find(declared.values, text) ==
                        declared.values.end()) {
                    return refuse(quote_text(text) +
                                  " is not one of its values");
                }
                set.set.push_back(text);
            }
            return set;
        }
        if (value.kind != NodeKind::SCALAR) {
            return refuse("must be one value of type " + type);
        }
        const std::string text(value.scalar_text());
        const bool plain =
            value.computed ||
            value.file->document->style(value.node) == ScalarStyle::PLAIN;
        if (type == "bool") {
            if (!plain || (text != "true" && text != "false")) {
                return refuse("must be true or false");
            }
            return ConfigValue::of_bool(text == "true");
        }
        if (type == "string" || type == "enum") {
            if (type == "enum" && std::ranges::find(declared.values, text) ==
                                      declared.values.end()) {
                return refuse(quote_text(text) + " is not one of its values");
            }
            return ConfigValue::of_string(text);
        }
        return number_value(type, text, plain, declared, refuse);
    }

    template <typename Refuse>
    std::optional<ConfigValue> number_value(const std::string& type,
                                            const std::string& text, bool plain,
                                            const Parameter& declared,
                                            const Refuse& refuse) {
        std::optional<ConfigValue> number;
        if (plain) {
            if (const std::optional<std::int64_t> integer = int64_of(text)) {
                number = ConfigValue::of_int(*integer);
            } else if (type == "number") {
                if (const std::optional<double> real = parse_decimal(text)) {
                    number = ConfigValue::of_real(*real);
                }
            }
        }
        if (!number) {
            return refuse(type == "int" ? "must be an integer"
                                        : "must be a number");
        }
        if (const std::optional<std::string> outside =
                outside_bounds(declared, *number)) {
            return refuse(text + *outside);
        }
        // A `number` keeps its written form, as everywhere in the
        // validator: written as an integer it is an INT (the ruling O-9 on
        // #146).
        return number;
    }

    // " is above its max M" or " is below its min M" when `value` is
    // outside the declared bounds.
    // An INT and an integer bound are compared as integers, so that no
    // binary64 rounding lets a value past its bound (T3 of the review of
    // #146); anything else as binary64.
    static std::optional<std::string> outside_bounds(const Parameter& declared,
                                                     const ConfigValue& value) {
        for (const auto& [bound, above] : {std::pair{&declared.min, false},
                                           std::pair{&declared.max, true}}) {
            if (!*bound) {
                continue;
            }
            const std::string limit_text((*bound)->scalar_text());
            const std::optional<int> order = order_against(value, limit_text);
            if (order && (above ? *order > 0 : *order < 0)) {
                return std::string(above ? " is above its max "
                                         : " is below its min ") +
                       limit_text;
            }
        }
        return std::nullopt;
    }

    // The order of `value` and the bound written as `limit_text` (-1, 0,
    // 1), or nothing for a bound that is no number. An INT against an
    // integer bound as integers (T3), against a decimal bound exactly, the
    // INT not converted (D7 of the delta review of #146).
    static std::optional<int> order_against(const ConfigValue& value,
                                            const std::string& limit_text) {
        const std::optional<std::int64_t> integer_limit = int64_of(limit_text);
        if (value.type == ValueType::INT && integer_limit) {
            if (value.integer < *integer_limit) {
                return -1;
            }
            return value.integer > *integer_limit ? 1 : 0;
        }
        const std::optional<double> limit = nearest_binary64(limit_text);
        if (!limit) {
            return std::nullopt;
        }
        return value.type == ValueType::REAL
                   ? compare_reals(value.real, *limit)
                   : compare_exactly(value.integer, *limit);
    }

    static int compare_reals(double left, double right) {
        if (left < right) {
            return -1;
        }
        return left > right ? 1 : 0;
    }

    // The order of `integer` and `real` (-1, 0, 1), exact: `real` is
    // finite, as a bound read by `nearest_binary64` is.
    static int compare_exactly(std::int64_t integer, double real) {
        // 2^63, exact in binary64.
        constexpr double TWO_TO_63 = 9223372036854775808.0;
        if (real >= TWO_TO_63) {
            return -1;
        }
        if (real < -TWO_TO_63) {
            return 1;
        }
        const double whole = std::floor(real);
        // `whole` is in [-2^63, 2^63), so it converts exactly.
        const auto floor_integer = static_cast<std::int64_t>(whole);
        if (integer != floor_integer) {
            return integer < floor_integer ? -1 : 1;
        }
        return whole == real ? 0 : -1;
    }

    static std::optional<std::int64_t> int64_of(const std::string& text) {
        const std::optional<Integer> integer = parse_integer(text);
        if (!integer) {
            return std::nullopt;
        }
        return as_int64(*integer);
    }

    // An integer or a decimal (contract 1.2), as a binary64.
    static std::optional<double> parse_decimal(const std::string& text) {
        if (const std::optional<std::int64_t> integer = int64_of(text)) {
            return static_cast<double>(*integer);
        }
        return scada::conf::parse_decimal(text);
    }

    // Step 3.1: the device keys of a level, and its `defaults`.
    static void merge_level_keys(const Value& level, Value& device_keys,
                                 Value& defaults) {
        for (const std::string_view key :
             {"read", "bit_order", "functions", "device_time", "keepalive",
              "idle_timeout", "on_mismatch"}) {
            merge_member(level, key, device_keys);
        }
        // An enumeration table of a level replaces the inherited one of its
        // name whole.
        if (const Value* enums = level.find("enums")) {
            Value* kept = device_keys.find("enums");
            if (kept == nullptr) {
                device_keys.members.push_back(
                    Member{.key = "enums",
                           .key_file = nullptr,
                           .key_node = 0,
                           .value = {Value::map_at(enums->file, enums->node)}});
                kept = device_keys.find("enums");
            }
            // One pass, not one erase per table (T1 of the review of #146).
            std::set<std::string_view> replaced;
            for (const Member& table : enums->members) {
                replaced.insert(table.key);
            }
            std::erase_if(kept->members, [&replaced](const Member& old) {
                return replaced.contains(old.key);
            });
            for (const Member& table : enums->members) {
                kept->members.push_back(table);
            }
        }
        // Step 3.5: a level that gives these replaces the inherited list.
        for (const std::string_view key :
             {"identify", "health", "restart_indication"}) {
            if (const Value* list = level.find(key)) {
                device_keys.erase(key);
                device_keys.members.push_back(member_of(level, key, *list));
            }
        }
        if (const Value* own = level.find("defaults")) {
            merge(defaults, *own, "defaults");
        }
    }

    static Member member_of(const Value& map, std::string_view key,
                            const Value& value) {
        for (const Member& member : map.members) {
            if (member.key == key) {
                return Member{.key = member.key,
                              .key_file = member.key_file,
                              .key_node = member.key_node,
                              .value = {value}};
            }
        }
        return Member{.key = std::string(key), .value = {value}};
    }

    static void merge_member(const Value& from, std::string_view key,
                             Value& into) {
        const Value* value = from.find(key);
        if (value == nullptr) {
            return;
        }
        if (Value* existing = into.find(key)) {
            merge(*existing, *value, key);
        } else {
            into.members.push_back(member_of(from, key, *value));
        }
    }

    // Steps 3.2 to 3.4 for one template of the chain.
    void expand_level(const CheckedFile& level, const Value& root,
                      std::vector<Point>& points) {
        const std::string& template_name = level.source->path_class.stem;
        std::vector<Point> own;
        std::set<std::string, std::less<>> names;
        const auto add = [&](Point point) {
            // What an instance makes before step 4 counts toward the node
            // limit of the tree with what is written, so that no instance
            // holds more than the tree may (C2 of the review of #146).
            // CF-414 counts the points that exist after step 4.
            if (device_failed_) {
                return;
            }
            const Value& name = *point.value.find("name");
            if (!count_work(name) ||
                !count_made(name, node_count(point.value))) {
                return;
            }
            if (!names.insert(point.name).second) {
                error(CF_POINT_TWICE, *point.value.find("name"),
                      "point " + quote_text(point.name) + " is defined twice",
                      "point " + point.name);
                return;
            }
            own.push_back(std::move(point));
        };
        if (const Value* written = root.find("points")) {
            for (const Value& item : written->items) {
                if (std::optional<Point> point =
                        point_of(item, {}, template_name)) {
                    add(std::move(*point));
                }
            }
        }
        if (const Value* blocks = root.find("repeat")) {
            for (const Value& block : blocks->items) {
                expand_repeat(block, template_name, add);
            }
        }
        // An index by name, so that joining is O(n log n) in the points.
        std::map<std::string, std::size_t, std::less<>> index =
            index_of(points);
        for (Point& point : own) {
            join(point, template_name, points, index);
        }
        select(root, template_name, points);
    }

    // A point of a list, with the variables bound to it.
    static std::optional<Point> point_of(
        const Value& item,
        const std::map<std::string, std::int64_t, std::less<>>& variables,
        const std::string& template_name) {
        const Value* name = item.find("name");
        if (item.kind != NodeKind::MAP || name == nullptr) {
            return std::nullopt;
        }
        Point point{.name = std::string(name->scalar_text()),
                    .value = item,
                    .bindings = {},
                    .conditions = {}};
        for (const auto& [variable, value] : variables) {
            point.bindings.emplace(variable, std::pair{value, template_name});
        }
        if (!variables.empty()) {
            point.name = substituted(point.name, variables);
            Value* own = point.value.find("name");
            own->computed = true;
            own->text = point.name;
            own->type = ScalarType::STRING;
        }
        return point;
    }

    // `{n}` in a point name replaced by the value of `n` (4.4).
    static std::string substituted(
        const std::string& name,
        const std::map<std::string, std::int64_t, std::less<>>& variables) {
        std::string out;
        for (std::size_t i = 0; i < name.size(); ++i) {
            if (name[i] == '{') {
                const std::size_t close = name.find('}', i);
                if (close != std::string::npos) {
                    const auto found =
                        variables.find(name.substr(i + 1, close - i - 1));
                    if (found != variables.end()) {
                        out += std::to_string(found->second);
                        i = close;
                        continue;
                    }
                }
            }
            out.push_back(name[i]);
        }
        return out;
    }

    // Step 3.2: the points of a `repeat` block, for every value (or pair of
    // values, the first variable outermost) whose `when` holds.
    template <typename Add>
    void expand_repeat(const Value& block, const std::string& template_name,
                       const Add& add) {
        const Value* range = block.find("for");
        if (range == nullptr) {
            return;
        }
        const std::optional<Axes> axes = axes_of(*range);
        if (!axes || axes->empty()) {
            return;
        }
        const Value* points = block.find("points");
        const Value* when = block.find("when");
        std::map<std::string, std::int64_t, std::less<>> variables;
        const std::function<void(std::size_t)> walk = [&](std::size_t axis) {
            if (device_failed_) {
                return;
            }
            if (axis == axes->size()) {
                if (count_repeat_value(block) && points != nullptr) {
                    make_points(*points, when, variables, template_name, add);
                }
                return;
            }
            for (const std::int64_t value : (*axes)[axis].second) {
                variables[(*axes)[axis].first] = value;
                walk(axis + 1);
            }
        };
        walk(0);
    }

    // The points of one value (or pair) of a block, each with the block's
    // `when` to decide at step 4.
    template <typename Add>
    static void make_points(
        const Value& points, const Value* when,
        const std::map<std::string, std::int64_t, std::less<>>& variables,
        const std::string& template_name, const Add& add) {
        for (const Value& item : points.items) {
            if (std::optional<Point> point =
                    point_of(item, variables, template_name)) {
                if (when != nullptr) {
                    point->conditions.push_back(*when);
                }
                add(std::move(*point));
            }
        }
    }

    // Each value counts toward the work of the tree, whether or not its
    // `when` keeps the point (the lead's ruling S1 on #146); false past
    // the budget.
    bool count_repeat_value(const Value& block) { return count_work(block); }

    // The work of the expansion: every point made, written or generated,
    // kept or not, and every `repeat` value evaluated counts one (the
    // ruling S1 on #146, D3 of the delta review); false past the limit.
    bool count_work(const Value& site) {
        if (stopped_) {
            return false;
        }
        if (++work_ <= limits_.work) {
            return true;
        }
        error(CF_SIZE_LIMIT, site,
              "the points and repeat values made for the tree exceed their "
              "limit of " +
                  std::to_string(limits_.work));
        stopped_ = true;
        return false;
    }

    // The work of the expressions, a budget of its own (G1, the lead's
    // ruling on #146): the nodes of every evaluation, parse and typing;
    // false past the limit.
    bool count_expression_work(const Value& site, std::size_t nodes) {
        if (stopped_) {
            return false;
        }
        expression_work_ = nodes > SIZE_MAX - expression_work_
                               ? SIZE_MAX
                               : expression_work_ + nodes;
        if (expression_work_ <= limits_.expression_work) {
            return true;
        }
        error(CF_SIZE_LIMIT, site,
              "the expression nodes evaluated for the tree exceed their "
              "limit of " +
                  std::to_string(limits_.expression_work));
        stopped_ = true;
        return false;
    }

    // `nodes` more made for the instance being expanded, which count
    // toward the node limit with what is written; false past it.
    bool count_made(const Value& site, std::size_t nodes) {
        made_nodes_ =
            nodes > SIZE_MAX - made_nodes_ ? SIZE_MAX : made_nodes_ + nodes;
        if (made_nodes_ <= limits_.nodes &&
            writer_.nodes() <= limits_.nodes - made_nodes_) {
            return true;
        }
        error(CF_SIZE_LIMIT, site, nodes_message());
        stopped_ = true;
        return false;
    }

    [[nodiscard]] std::string nodes_message() const {
        return "the expanded tree exceeds its limit of " +
               std::to_string(limits_.nodes) + " nodes";
    }

    // `256 MiB`, or `1000 bytes` for a limit that is no whole MiB.
    static std::string bytes_text(std::size_t bytes) {
        constexpr std::size_t MIB = std::size_t{1024} * 1024;
        return bytes % MIB == 0 ? std::to_string(bytes / MIB) + " MiB"
                                : std::to_string(bytes) + " bytes";
    }

    using Axes = std::vector<std::pair<std::string, std::vector<std::int64_t>>>;

    // The variables of a `for` and their values; nothing on an error. Two
    // variables give at most 65,536 pairs (CF-113).
    std::optional<Axes> axes_of(const Value& range) {
        Axes axes;
        for (const Member& member : range.members) {
            // A variable would hide the parameter, and change a parent's
            // expression that meant it (the ruling O-3 on #146).
            if (declarations_.contains(member.key)) {
                error_at(CF_BAD_VALUE, *member.key_file, member.key_node,
                         "parameter " + quote_text(member.key) +
                             ": a repeat variable cannot have its name",
                         "parameter " + member.key);
                return std::nullopt;
            }
            std::optional<std::vector<std::int64_t>> values =
                values_of(member.value.front(), member.key);
            if (!values) {
                return std::nullopt;
            }
            axes.emplace_back(member.key, std::move(*values));
        }
        std::size_t pairs = 1;
        for (const auto& axis : axes) {
            pairs *= axis.second.size();
        }
        if (pairs > MAX_REPEAT_VALUES) {
            error(CF_SIZE_LIMIT, range,
                  "the values of a repeat block exceed its limit of 65536");
            return std::nullopt;
        }
        return axes;
    }

    // The number of values of `runs`, or nothing above the limit. Each
    // run's span is at most 2^64 - 1 in uint64, so it is refused before it
    // is added: `high - low + 1` of the whole INT span wraps to 0 (B1 of
    // the review of #146).
    static std::optional<std::uint64_t> count_of(
        const std::vector<std::pair<std::int64_t, std::int64_t>>& runs) {
        std::uint64_t count = 0;
        for (const auto& [low, high] : runs) {
            if (high < low) {
                continue;
            }
            const std::uint64_t span = static_cast<std::uint64_t>(high) -
                                       static_cast<std::uint64_t>(low);
            if (span >= MAX_REPEAT_VALUES) {
                return std::nullopt;
            }
            count += span + 1;
            if (count > MAX_REPEAT_VALUES) {
                return std::nullopt;
            }
        }
        return count;
    }

    // The values of a `for` range (4.4), checked from its bounds before any
    // is produced (CF-113).
    std::optional<std::vector<std::int64_t>> values_of(
        const Value& range, const std::string& name) {
        std::vector<std::pair<std::int64_t, std::int64_t>> runs;
        context_ = "variable " + name;
        const auto add_run = [&](const Value& part) {
            std::optional<ConfigValue> value =
                evaluate(part, {}, ExpressionKind::FOR_VALUE);
            if (!value) {
                return false;
            }
            if (value->type == ValueType::RANGE) {
                runs.emplace_back(value->low, value->high);
            } else if (value->type == ValueType::INT) {
                runs.emplace_back(value->integer, value->integer);
            } else {
                error(CF_TYPE_MISMATCH, part,
                      "expression: the value must be a range or an INT, not " +
                          std::string(value_type_name(value->type)));
                return false;
            }
            return true;
        };
        if (range.kind == NodeKind::SEQUENCE) {
            for (const Value& item : range.items) {
                if (!add_run(item)) {
                    return std::nullopt;
                }
            }
        } else if (!add_run(range)) {
            return std::nullopt;
        }
        const std::optional<std::uint64_t> count = count_of(runs);
        if (!count) {
            error(CF_SIZE_LIMIT, range,
                  "the range of " + quote_text(name) +
                      " exceeds its limit of 65536");
            return std::nullopt;
        }
        std::vector<std::int64_t> values;
        values.reserve(static_cast<std::size_t>(*count));
        for (const auto& [low, high] : runs) {
            for (std::int64_t value = low; value <= high; ++value) {
                values.push_back(value);
                if (value == high) {
                    break;
                }
            }
        }
        return values;
    }

    // Step 3.3: a point of this level merged into the inherited one of its
    // name, or added; the variables bound to both are joined (CF-422).
    void join(Point& point, const std::string& template_name,
              std::vector<Point>& points,
              std::map<std::string, std::size_t, std::less<>>& index) {
        const auto position = index.find(point.name);
        if (position == index.end()) {
            index.emplace(point.name, points.size());
            points.push_back(std::move(point));
            return;
        }
        Point* const found = &points[position->second];
        for (const auto& [variable, bound] : point.bindings) {
            const auto inherited = found->bindings.find(variable);
            if (inherited != found->bindings.end() &&
                inherited->second.first != bound.first) {
                error(CF_BINDINGS, *point.value.find("name"),
                      bindings_message(point.name, variable, inherited->second,
                                       bound.first, template_name),
                      "point " + point.name);
            } else {
                found->bindings.insert_or_assign(variable, bound);
            }
        }
        merge(found->value, point.value, "point");
        for (Value& condition : point.conditions) {
            found->conditions.push_back(std::move(condition));
        }
    }

    static std::string bindings_message(
        const std::string& point, const std::string& variable,
        const std::pair<std::int64_t, std::string>& inherited,
        std::int64_t value, const std::string& template_name) {
        std::string message = "point " + quote_text(point) + ": variable ";
        message += quote_text(variable);
        message += " is " + std::to_string(inherited.first);
        message += " in " + quote_text(inherited.second);
        message += " and " + std::to_string(value);
        message += " in " + quote_text(template_name);
        return message;
    }

    // Step 3.4 (and 4 for an instance): `disable` removes the points it
    // matches, `only` keeps those it matches; an entry that matches none is
    // CF-413.
    void select(const Value& owner, const std::string& template_name,
                std::vector<Point>& points) {
        for (const bool keep : {false, true}) {
            const Value* list = owner.find(keep ? "only" : "disable");
            if (list == nullptr) {
                continue;
            }
            std::vector<bool> matched(points.size(), false);
            // A plain name is looked up, a pattern matched against every
            // point: n names over n points is O(n log n), not O(n^2) (T1 of
            // the review of #146).
            const std::map<std::string, std::size_t, std::less<>> by_name =
                index_of(points);
            for (const Value& entry : list->items) {
                const std::string_view pattern = entry.scalar_text();
                if (!mark_matches(pattern, points, by_name, matched)) {
                    error(CF_NO_SUCH_POINT, entry,
                          no_such_point(pattern, template_name));
                }
            }
            std::size_t index = 0;
            std::erase_if(
                points, [&](const Point&) { return matched[index++] != keep; });
        }
    }

    // Marks the points `pattern` matches; false if none. A plain name is
    // looked up, a pattern matched against every point.
    static bool mark_matches(
        std::string_view pattern, const std::vector<Point>& points,
        const std::map<std::string, std::size_t, std::less<>>& by_name,
        std::vector<bool>& matched) {
        if (pattern.find_first_of("*?") == std::string_view::npos) {
            const auto found = by_name.find(pattern);
            if (found == by_name.end()) {
                return false;
            }
            matched[found->second] = true;
            return true;
        }
        bool any = false;
        for (std::size_t i = 0; i < points.size(); ++i) {
            if (matches_name_pattern(pattern, points[i].name)) {
                matched[i] = true;
                any = true;
            }
        }
        return any;
    }

    // CF-413: `point {name} does not exist in template {template}`, and
    // for a device without a template `... : the device has no template`
    // (S10 of the delta review of #146).
    static std::string no_such_point(std::string_view name,
                                     const std::string& template_name) {
        if (template_name.empty()) {
            return "point " + quote_text(name) +
                   " does not exist: the device has no template";
        }
        return "point " + quote_text(name) + " does not exist in template " +
               quote_text(template_name);
    }

    // Step 4: a point whose `when`, or the `when` of a block that made it,
    // is false does not exist; then the instance's `disable` or `only`.
    void keep_present(const Value& instance, std::vector<Point>& points) {
        std::erase_if(points, [this](Point& point) {
            context_ = "point " + point.name;
            const std::map<std::string, std::int64_t, std::less<>> variables =
                bound_to(point);
            bool present = true;
            for (const Value& condition : point.conditions) {
                present = present && holds(condition, variables);
            }
            const Value* when = point.value.find("when");
            present = present && (when == nullptr || holds(*when, variables));
            point.value.erase("when");
            point.conditions.clear();
            return !present;
        });
        select(instance, template_of(instance), points);
    }

    static std::string template_of(const Value& instance) {
        const Value* name = instance.find("template");
        return name != nullptr ? std::string(name->scalar_text())
                               : std::string();
    }

    static std::map<std::string, std::int64_t, std::less<>> bound_to(
        const Point& point) {
        std::map<std::string, std::int64_t, std::less<>> variables;
        for (const auto& [name, bound] : point.bindings) {
            variables.emplace(name, bound.first);
        }
        return variables;
    }

    // Step 5: the template's `defaults` merged with the instance's, merged
    // under every point.
    // The defaults are merged under every point, so their nodes times the
    // points count toward the node limit before any merge (D1 of the delta
    // review of #146).
    void apply_defaults(const Value& template_defaults, const Value& instance,
                        std::vector<Point>& points) {
        Value defaults = template_defaults;
        if (const Value* own = instance.find("defaults")) {
            merge(defaults, *own, "defaults");
        }
        const std::size_t each = node_count(defaults) - 1;
        const std::size_t total = each != 0 && points.size() > SIZE_MAX / each
                                      ? SIZE_MAX
                                      : each * points.size();
        if (!count_made(*instance.find("name"), total)) {
            return;
        }
        for (Point& point : points) {
            Value merged = defaults;
            merged.file = point.value.file;
            merged.node = point.value.node;
            merge(merged, point.value, "point");
            point.value = std::move(merged);
        }
    }

    // The raw type of a Modbus address (Annex A.2): its `type`, else `bool`
    // for one bit, coils and discrete inputs, `uint16` otherwise. Empty for
    // another protocol, whose raw type its address does not say.
    static std::string raw_type_of(const Value* address,
                                   std::string_view protocol) {
        if (address == nullptr) {
            return {};
        }
        if (const Value* raw = address->find("type")) {
            return std::string(raw->scalar_text());
        }
        if (protocol != "modbus") {
            return {};
        }
        const Value* table = address->find("table");
        const std::string_view name =
            table != nullptr ? table->scalar_text() : std::string_view();
        const bool one_bit = address->find("bit") != nullptr ||
                             name == "coil" || name == "discrete";
        return one_bit ? "bool" : "uint16";
    }

    // `invalid_values` of `defaults` is a map from raw type to a list
    // (4.7); a point that gives no list of its own takes the one of its raw
    // type, written or the protocol's default, or none.
    static void pick_invalid_values(Value& point, std::string_view protocol) {
        Value* invalid = point.find("invalid_values");
        if (invalid == nullptr || invalid->kind != NodeKind::MAP) {
            return;
        }
        const std::string raw = raw_type_of(point.find("address"), protocol);
        const Value* list = raw.empty() ? nullptr : invalid->find(raw);
        if (list == nullptr) {
            point.erase("invalid_values");
            return;
        }
        const Value chosen = *list;
        *invalid = chosen;
    }

    // The position of each point by its name.
    static std::map<std::string, std::size_t, std::less<>> index_of(
        const std::vector<Point>& points) {
        std::map<std::string, std::size_t, std::less<>> index;
        for (std::size_t i = 0; i < points.size(); ++i) {
            index.emplace(points[i].name, i);
        }
        return index;
    }

    // Step 6: the instance's overrides, and its `identify` appended.
    void apply_overrides(const Value& instance, Value& device_keys,
                         std::vector<Point>& points) {
        if (const Value* overrides = instance.find("overrides")) {
            const std::map<std::string, std::size_t, std::less<>> index =
                index_of(points);
            for (const Member& member : overrides->members) {
                const auto found = index.find(member.key);
                if (found == index.end()) {
                    error_at(CF_NO_SUCH_POINT, *member.key_file,
                             member.key_node,
                             no_such_point(member.key, template_of(instance)));
                    continue;
                }
                if (!count_made(member.value.front(),
                                node_count(member.value.front()))) {
                    return;
                }
                merge(points[found->second].value, member.value.front(),
                      "point");
            }
        }
        if (const Value* own = instance.find("identify")) {
            Value combined = *own;
            if (const Value* inherited = device_keys.find("identify")) {
                combined = *inherited;
                for (const Value& check : own->items) {
                    combined.items.push_back(check);
                }
            }
            device_keys.erase("identify");
            device_keys.members.push_back(
                member_of(instance, "identify", combined));
        }
    }

    // An instance's own points: after step 6, a name that exists is CF-416.
    // With `decide_when` (an instance of a template), a point's `when` is
    // decided first, with the parameters: a point that does not exist does
    // not collide with a template's point of its name (S10 of the delta
    // review of #146). Without a template, the points enter step 4.
    void add_instance_points(const Value& instance, Device& device,
                             bool decide_when = false) {
        const Value* own = instance.find("points");
        if (own == nullptr) {
            return;
        }
        std::map<std::string, std::size_t, std::less<>> index =
            index_of(device.points);
        for (const Value& item : own->items) {
            std::optional<Point> point = point_of(item, {}, {});
            if (!point) {
                continue;
            }
            if (decide_when) {
                context_ = "point " + point->name;
                const Value* when = point->value.find("when");
                const bool present = when == nullptr || holds(*when, {});
                point->value.erase("when");
                if (!present) {
                    continue;
                }
            }
            if (!count_work(*point->value.find("name")) ||
                !count_made(*point->value.find("name"),
                            node_count(point->value))) {
                return;
            }
            if (!index.emplace(point->name, device.points.size()).second) {
                error(CF_POINT_TWICE, *point->value.find("name"),
                      "point " + quote_text(point->name) + " is defined twice");
                continue;
            }
            device.points.push_back(std::move(*point));
        }
    }

    // Steps 7 and 8, and the checks of the expanded instance.
    void finish_points(const Value& instance, Device& device) {
        const std::string template_name = template_of(instance);
        // The device's own expressions (2.4) have the parameters and no
        // variable: the identity checks (4.8), the read ranges' ends (A.4)
        // and a health rule's bit field (4.9; M7 of the review of #149).
        for (const std::string_view key : {"identify", "read", "health"}) {
            if (Value* own = device.keys.find(key)) {
                context_ = std::string(key);
                evaluate_below(*own, {});
            }
        }
        if (device.points.size() > MAX_POINTS) {
            error(CF_TOO_MANY_POINTS, *instance.find("name"),
                  "template " + quote_text(template_name) + " expands to " +
                      std::to_string(device.points.size()) +
                      " points, the limit is 65536");
            return;
        }
        const bool protocol_template =
            !template_name.empty() &&
            !protocol_of(*templates_.at(template_name)).empty();
        std::set<std::string, std::less<>> names;
        for (const Point& point : device.points) {
            names.insert(point.name);
        }
        for (Point& point : device.points) {
            evaluate_point(point);
            strip_replace(point.value);
            const Value& name = *point.value.find("name");
            if (protocol_template && point.value.find("address") == nullptr) {
                error(CF_NO_ADDRESS, name,
                      "point " + quote_text(point.name) + " has no address",
                      "point " + point.name);
            }
            if (check_name_grammar(point.name, name, point.name)) {
                check_tag_name(CF_TAG_NAME_LENGTH, point.name, name,
                               point.name);
            }
            check_bits(point, names);
        }
    }

    void check_tag_name(std::uint16_t code, const std::string& name,
                        const Value& site, const std::string& point) {
        const std::string tag = device_name_ + "." + name;
        if (tag.size() > MAX_TAG_NAME_BYTES) {
            error(code, site,
                  std::string(code == CF_TAG_NAME_LENGTH ? "tag name "
                                                         : "generated tag ") +
                      quote_text(tag) + " is " + std::to_string(tag.size()) +
                      " bytes, the limit is 127",
                  "point " + point);
        }
    }

    // A bit's tag is `<device>.<name>` (5.4): its name is unique among the
    // device's points and bits.
    void check_bits(const Point& point,
                    std::set<std::string, std::less<>>& names) {
        const Value* bits = point.value.find("bits");
        if (bits == nullptr) {
            return;
        }
        for (const Member& member : bits->members) {
            const Value& definition = member.value.front();
            const Value* name = definition.kind == NodeKind::SCALAR
                                    ? &definition
                                    : definition.find("name");
            if (name == nullptr) {
                continue;
            }
            const std::string text(name->scalar_text());
            // An invalid name is one error, not also one per value of a
            // block that repeats it.
            if (!check_name_grammar(text, *name, point.name, true)) {
                continue;
            }
            if (!names.insert(text).second) {
                error(CF_POINT_TWICE, *name,
                      "point " + quote_text(text) + " is defined twice",
                      "point " + point.name);
                continue;
            }
            check_tag_name(CF_GENERATED_NAME_LENGTH, text, *name, point.name);
        }
    }

    // Step 8: a point or bit name made by expansion follows 2.1 (CF-212;
    // 1.4, stage 4): `s{n}` outside a block, `x.{n}` with n = -1. Its
    // length is CF-402 or CF-403 (B7 of the review of #146).
    // - A `{...}` left in a point name names no variable bound to the
    //   point. The message gives the name as written, so that a block's
    //   values make one error per definition, not one per value (S10 of
    //   the delta review).
    // - A bit name takes no variables (4.4 replaces them in point names
    //   only; the schema types a bit's name as `point_name`, S9).
    bool check_name_grammar(const std::string& name, const Value& site,
                            const std::string& point, bool bit = false) {
        if (is_tag_name(name, std::numeric_limits<std::size_t>::max())) {
            return true;
        }
        const std::size_t open = name.find('{');
        const std::size_t close = open == std::string::npos
                                      ? std::string::npos
                                      : name.find('}', open);
        std::string message;
        std::string detail = "point " + point;
        if (close == std::string::npos) {
            message = quote_text(name) + " is not a valid point name";
        } else if (bit) {
            message = quote_text(name) +
                      " is not a valid point name: a bit name takes no "
                      "variables";
        } else {
            const std::string written(site.file->document->text(site.node));
            message = quote_text(written) + " is not a valid point name: " +
                      name.substr(open, close - open + 1) +
                      " names no variable bound to this point";
            detail.clear();
        }
        error(CF_BAD_NAME, site, message, std::move(detail));
        return false;
    }

    // --- Step 7: evaluation ------------------------------------------------

    void evaluate_point(Point& point) {
        context_ = "point " + point.name;
        const std::map<std::string, std::int64_t, std::less<>> variables =
            bound_to(point);
        for (Member& member : point.value.members) {
            // Alarms hold run-time expressions, which are checked with the
            // tags (stage 5).
            if (member.key == "alarms" || member.key == "name") {
                continue;
            }
            evaluate_below(member.value.front(), variables);
        }
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by the nesting limit of 32
    void evaluate_below(
        Value& value,
        const std::map<std::string, std::int64_t, std::less<>>& variables) {
        switch (value.kind) {
            case NodeKind::SCALAR: {
                const ScalarType type = value.scalar_type();
                if (value.computed ||
                    (type != ScalarType::EXPRESSION &&
                     type != ScalarType::INTEGER_EXPRESSION)) {
                    return;
                }
                std::optional<ConfigValue> result = evaluate(
                    value, variables, ExpressionKind::CONFIGURATION,
                    type == ScalarType::INTEGER_EXPRESSION ? Wanted::INT
                                                           : Wanted::NUMBER);
                if (!result) {
                    return;
                }
                value.computed = true;
                value.text = canonical_text(*result);
                value.type = scalar_type_of(result->type);
                return;
            }
            case NodeKind::MAP:
                for (Member& member : value.members) {
                    // An alarm's expressions read tags, also on a bit (B2 of
                    // the review of #144).
                    if (member.key != "alarms") {
                        evaluate_below(member.value.front(), variables);
                    }
                }
                return;
            case NodeKind::SEQUENCE:
                for (Value& item : value.items) {
                    evaluate_below(item, variables);
                }
                return;
        }
    }

    static ScalarType scalar_type_of(ValueType type) {
        switch (type) {
            case ValueType::BOOL:
                return ScalarType::BOOLEAN;
            case ValueType::INT:
                return ScalarType::INTEGER;
            case ValueType::REAL:
                return ScalarType::DECIMAL;
            case ValueType::TIME:
                return ScalarType::DURATION;
            default:
                return ScalarType::STRING;
        }
    }

    // True if `when` (a boolean or a configuration expression) holds for
    // the parameters and `variables`.
    bool holds(
        const Value& when,
        const std::map<std::string, std::int64_t, std::less<>>& variables) {
        if (when.scalar_type() == ScalarType::BOOLEAN) {
            return when.scalar_text() == "true";
        }
        std::optional<ConfigValue> value =
            evaluate(when, variables, ExpressionKind::CONFIGURATION);
        if (!value) {
            return false;
        }
        if (value->type != ValueType::BOOL) {
            error(CF_EVALUATION, when,
                  "expression " + quote_text(when.scalar_text()) +
                      ": the value must be BOOL, not " +
                      std::string(value_type_name(value->type)));
            return false;
        }
        return value->integer != 0;
    }

    // What a value must be: anything (a `when` is checked by `holds`), a
    // number, as every expression of a point's keys and of `identify` is
    // (2.4; stage 5 types a template's the same way, D-113), or an INT, for
    // a key the schema types as an integer (B9 of the review of #146).
    enum class Wanted : std::uint8_t { ANY, NUMBER, INT };

    // An expression of the source, parsed once for all its evaluations
    // (E1 of the review of #146), with its types for the last types of the
    // names they read.
    struct Compiled {
        std::optional<Expression> tree;
        // Each name the expression reads and how many times it does.
        std::map<std::string, std::size_t, std::less<>> references;
        bool typed = false;
        bool type_ok = false;
        // The names the typing read and their types then; the types hold
        // while each name keeps its type.
        std::vector<std::pair<std::string, std::optional<ValueType>>> read;
        std::vector<ValueType> types;
        EvalError type_error;
    };
    using SiteKey = std::tuple<const Document*, NodeId, ExpressionKind>;

    // The parse of `value`'s expression; nothing past the work budget. A
    // parse counts its nodes toward the work, as an evaluation does, so
    // that the cache may be cleared at its bound without unbounded work.
    Compiled* compiled(const Value& value, ExpressionKind kind) {
        const SiteKey key{value.file->document, value.node, kind};
        if (const auto found = compiled_.find(key); found != compiled_.end()) {
            return &found->second;
        }
        Compiled site;
        ExpressionError syntax;
        site.tree = parse_expression(value.scalar_text(), kind, syntax);
        ++parses_;
        const std::size_t nodes = site.tree ? site.tree->nodes.size() : 1;
        if (site.tree) {
            for (const ExprNode& node : site.tree->nodes) {
                if (node.op == ExprOp::REFERENCE) {
                    ++site.references[node.text];
                }
            }
        }
        if (!count_expression_work(value, std::max<std::size_t>(1, nodes))) {
            return nullptr;
        }
        if (compiled_nodes_ + nodes > MAX_COMPILED_NODES) {
            compiled_.clear();
            compiled_nodes_ = 0;
        }
        compiled_nodes_ += nodes;
        return &compiled_.emplace(key, std::move(site)).first->second;
    }

    // What an evaluation of `site` costs toward the work (E1 of the review
    // of #146): one per node, and for each reference to a parameter the
    // weight of the value it copies, so that a reference to a set of 65,536
    // strings is not one unit.
    [[nodiscard]] std::size_t evaluation_cost(const Compiled& site,
                                              const Expression& tree) const {
        std::size_t cost = std::max<std::size_t>(1, tree.nodes.size());
        for (const auto& [name, count] : site.references) {
            const auto found = param_weights_.find(name);
            if (found == param_weights_.end()) {
                continue;
            }
            const std::size_t weight = found->second > SIZE_MAX / count
                                           ? SIZE_MAX
                                           : found->second * count;
            cost = weight > SIZE_MAX - cost ? SIZE_MAX : cost + weight;
        }
        return cost;
    }

    // The weight of a parameter's value: its set's items and a unit per 64
    // bytes of its text, what a reference copies.
    static std::size_t weight_of(const ConfigValue& value) {
        constexpr std::size_t BYTES_PER_UNIT = 64;
        std::size_t bytes = value.text.size();
        for (const std::string& item : value.set) {
            bytes += item.size();
        }
        return value.set.size() + bytes / BYTES_PER_UNIT;
    }

    // Types `site` for `bindings` unless the names it read keep their
    // types; a typing counts its nodes toward the work.
    bool type_site(Compiled& site, const Expression& tree, const Value& value,
                   const Bindings& bindings) {
        if (site.typed && std::ranges::all_of(site.read, [&](const auto& name) {
                const ConfigValue* bound = bindings(name.first);
                return (bound == nullptr
                            ? std::nullopt
                            : std::optional(bound->type)) == name.second;
            })) {
            return true;
        }
        if (!count_expression_work(
                value, std::max<std::size_t>(1, tree.nodes.size()))) {
            return false;
        }
        site.read.clear();
        const Bindings recording =
            [&site, &bindings](std::string_view name) -> const ConfigValue* {
            const ConfigValue* bound = bindings(name);
            site.read.emplace_back(std::string(name),
                                   bound == nullptr
                                       ? std::nullopt
                                       : std::optional<ValueType>(bound->type));
            return bound;
        };
        site.types.clear();
        site.type_ok =
            type_for_evaluation(tree, recording, site.types, site.type_error);
        site.typed = true;
        ++typings_;
        return true;
    }

    std::optional<ConfigValue> evaluate(
        const Value& value,
        const std::map<std::string, std::int64_t, std::less<>>& variables,
        ExpressionKind kind, Wanted wanted = Wanted::ANY) {
        const std::string text(value.scalar_text());
        Compiled* site = compiled(value, kind);
        if (site == nullptr) {
            return std::nullopt;
        }
        if (!site->tree) {
            // Stage 3 parsed every scalar it typed as an expression, so this
            // is a fault of the validator, never passed in silence (T5 of
            // the review of #146).
            error(CF_EVALUATION, value,
                  "expression " + quote_text(text) +
                      ": its text does not parse at expansion, which is an "
                      "internal error of the validator");
            return std::nullopt;
        }
        const Expression& tree = *site->tree;
        if (!count_expression_work(value, evaluation_cost(*site, tree))) {
            return std::nullopt;
        }
        std::map<std::string, ConfigValue, std::less<>> local;
        for (const auto& [name, number] : variables) {
            local.emplace(name, ConfigValue::of_int(number));
        }
        const Bindings bindings =
            [this, &local](std::string_view name) -> const ConfigValue* {
            if (const auto found = local.find(name); found != local.end()) {
                return &found->second;
            }
            if (const auto found = params_.find(name); found != params_.end()) {
                return &found->second;
            }
            return nullptr;
        };
        if (!type_site(*site, tree, value, bindings)) {
            return std::nullopt;
        }
        EvalError problem = site->type_error;
        std::optional<ConfigValue> result =
            site->type_ok ? evaluate_typed(tree, site->types, bindings, problem)
                          : std::nullopt;
        if (!result) {
            // Every failure of a configuration expression, a type that
            // does not fit included, is CF-410 in the form of 2.4 and 10.4
            // (B6 of the review of #144), at the offending character.
            std::string reason = problem.message;
            constexpr std::string_view PREFIX = "expression: ";
            if (reason.starts_with(PREFIX)) {
                reason = reason.substr(PREFIX.size());
            }
            // A device's own key (`identify`, `read`, `health`) has the
            // parameters only: the name is not one of the device's (item 7
            // of the delta review of #149).
            constexpr std::string_view UNBOUND =
                " is not a parameter and no variable bound to this point";
            if (local.empty() && !context_.starts_with("point ") &&
                reason.ends_with(UNBOUND)) {
                reason.replace(reason.size() - UNBOUND.size(), UNBOUND.size(),
                               " is not a parameter of this device");
            }
            report_evaluation(value, problem.column, reason,
                              detail_of(tree, bindings));
            return result;
        }
        if (!fits(result->type, wanted)) {
            report_evaluation(
                value, tree.nodes.at(tree.root).column,
                std::string(wanted == Wanted::INT
                                ? "the value must be an INT, not "
                                : "the value must be a number, not ") +
                    std::string(value_type_name(result->type)),
                detail_of(tree, bindings));
            return std::nullopt;
        }
        return result;
    }

    static bool fits(ValueType type, Wanted wanted) noexcept {
        switch (wanted) {
            case Wanted::ANY:
                return true;
            case Wanted::NUMBER:
                return type == ValueType::INT || type == ValueType::REAL;
            case Wanted::INT:
                return type == ValueType::INT;
        }
        return false;
    }

    // CF-410 at `column` of the expression of `value` (D-109); in a block
    // scalar or an escaped string, the place is given within the text, as
    // for CF-520.
    void report_evaluation(const Value& value, std::uint32_t column,
                           std::string reason, std::string detail) {
        ExpressionSite site =
            expression_site(*value.file->document, value.node, column);
        if (!value.file->document->verbatim(value.node)) {
            reason += site.where;
        }
        error_in(
            CF_EVALUATION, *value.file, std::move(site.location),
            "expression " + quote_text(value.scalar_text()) + ": " + reason,
            std::move(detail));
    }

    // The point, and each name the expression used with its value (S2 of
    // the review of #146): `point s3; n = 3`.
    [[nodiscard]] std::string detail_of(const Expression& tree,
                                        const Bindings& bindings) const {
        std::string out = context_;
        std::set<std::string, std::less<>> seen;
        std::string values;
        for (const ExprNode& node : tree.nodes) {
            if (node.op != ExprOp::REFERENCE ||
                !seen.insert(node.text).second) {
                continue;
            }
            if (const ConfigValue* bound = bindings(node.text)) {
                values += (values.empty() ? "" : ", ") + node.text + " = " +
                          canonical_text(*bound);
            }
        }
        if (!values.empty()) {
            out += (out.empty() ? "" : "; ") + values;
        }
        return out;
    }

    // --- The Document ------------------------------------------------------

    class Writer {
    public:
        explicit Writer(const Expander& expander) : expander_(&expander) {}

        // Opens the document: `channels`, written one device at a time.
        void begin() {
            for (const std::string& path : expander_->paths_) {
                ok_ = ok_ && builder_.add_file(path).ok();
            }
            const SourcePosition top{};
            ok_ = ok_ && builder_.begin_map(top).ok() && key("channels", top) &&
                  builder_.begin_sequence(top).ok();
        }

        // The nodes and the bytes of text written so far.
        [[nodiscard]] std::size_t nodes() const noexcept {
            return instance_of_.size();
        }
        [[nodiscard]] std::size_t text_bytes() const noexcept {
            return text_bytes_;
        }

        void begin_channel(const CheckedFile& file) {
            const SourcePosition top = position(file, file.document->root());
            ok_ = ok_ && builder_.begin_map(top).ok() && key("protocol", top);
            scalar(file.source->path_class.protocol, ScalarType::STRING, top);
            ok_ = ok_ && key("file", top);
            scalar(file.source->path, ScalarType::STRING, top);
            const Document& document = *file.document;
            if (const std::optional<NodeId> written =
                    document.find(document.root(), "channel")) {
                ok_ = ok_ && key("channel", top);
                write_value(copy_of(file, *written));
            }
            ok_ =
                ok_ && key("devices", top) && builder_.begin_sequence(top).ok();
        }

        void device(const Device& device, const Location& instance) {
            instance_ = static_cast<std::uint32_t>(instances_.size());
            instances_.push_back(instance);
            write_device(device);
            instance_ = ExpandedTree::NO_INSTANCE;
        }

        void end_channel() {
            ok_ =
                ok_ && builder_.end_sequence().ok() && builder_.end_map().ok();
        }

        std::optional<ExpandedTree> finish() {
            ok_ =
                ok_ && builder_.end_sequence().ok() && builder_.end_map().ok();
            if (!ok_) {
                return std::nullopt;
            }
            Result<Document> document = builder_.finish();
            if (!document.ok()) {
                return std::nullopt;
            }
            ExpandedTree tree{.document = std::move(document).value(),
                              .types = {},
                              .instance_of = std::move(instance_of_),
                              .instances = std::move(instances_),
                              .computed = std::move(computed_)};
            tree.types = ScalarTypes(tree.document);
            for (const auto& [node, type] : types_) {
                tree.types.set(node, type);
            }
            tree.instance_of.resize(tree.document.size(),
                                    ExpandedTree::NO_INSTANCE);
            return tree;
        }

    private:
        [[nodiscard]] SourcePosition position(const CheckedFile& file,
                                              NodeId node) const {
            SourcePosition where = file.document->position(node);
            where.file = expander_->file_index_.at(&file);
            return where;
        }

        void note(NodeId node) {
            if (instance_of_.size() <= node) {
                instance_of_.resize(node + 1, ExpandedTree::NO_INSTANCE);
            }
            instance_of_[node] = instance_;
        }

        bool key(std::string_view text, SourcePosition where) {
            text_bytes_ += text.size();
            const Result<NodeId> node =
                builder_.key(text, ScalarStyle::PLAIN, where);
            if (node.ok()) {
                note(node.value());
            }
            return node.ok();
        }

        std::optional<NodeId> scalar(std::string_view text, ScalarType type,
                                     SourcePosition where) {
            text_bytes_ += text.size();
            const Result<NodeId> node =
                builder_.scalar(text, ScalarStyle::PLAIN, where);
            ok_ = ok_ && node.ok();
            if (!node.ok()) {
                return std::nullopt;
            }
            note(node.value());
            types_.emplace_back(node.value(), type);
            return node.value();
        }

        void write_device(const Device& device) {
            const SourcePosition top =
                position(*device.keys.file, device.keys.node);
            ok_ = ok_ && builder_.begin_map(top).ok();
            write_members(device.keys);
            ok_ =
                ok_ && key("points", top) && builder_.begin_sequence(top).ok();
            // The text is checked after each point, not only after the
            // device (D4 of the delta review of #146): past the limit the
            // device stops here and the expansion with it.
            for (const Point& point : device.points) {
                write_value(point.value);
                if (text_bytes_ > expander_->limits_.text_bytes) {
                    break;
                }
            }
            ok_ =
                ok_ && builder_.end_sequence().ok() && builder_.end_map().ok();
        }

        // NOLINTNEXTLINE(misc-no-recursion): bounded by the nesting limit
        void write_members(const Value& map) {
            for (const Member& member : map.members) {
                const Value& value = member.value.front();
                const SourcePosition where =
                    member.key_file != nullptr
                        ? position(*member.key_file, member.key_node)
                        : position(*value.file, value.node);
                ok_ = ok_ && key(member.key, where);
                write_value(value);
            }
        }

        // NOLINTNEXTLINE(misc-no-recursion): bounded by the nesting limit
        void write_value(const Value& value) {
            const SourcePosition where = position(*value.file, value.node);
            switch (value.kind) {
                case NodeKind::SCALAR: {
                    if (value.computed) {
                        if (const std::optional<NodeId> node =
                                scalar(value.text, value.type, where)) {
                            computed_.push_back(*node);
                        }
                        return;
                    }
                    const Document& document = *value.file->document;
                    text_bytes_ += document.text(value.node).size();
                    const Result<NodeId> node = builder_.scalar(
                        document.text(value.node), document.style(value.node),
                        where, document.verbatim(value.node));
                    ok_ = ok_ && node.ok();
                    if (node.ok()) {
                        note(node.value());
                        types_.emplace_back(node.value(),
                                            value.file->types->of(value.node));
                    }
                    return;
                }
                case NodeKind::MAP: {
                    const Result<NodeId> node = builder_.begin_map(where);
                    ok_ = ok_ && node.ok();
                    if (node.ok()) {
                        note(node.value());
                    }
                    write_members(value);
                    ok_ = ok_ && builder_.end_map().ok();
                    return;
                }
                case NodeKind::SEQUENCE: {
                    const Result<NodeId> node = builder_.begin_sequence(where);
                    ok_ = ok_ && node.ok();
                    if (node.ok()) {
                        note(node.value());
                    }
                    for (const Value& item : value.items) {
                        write_value(item);
                    }
                    ok_ = ok_ && builder_.end_sequence().ok();
                    return;
                }
            }
        }

        const Expander* expander_;
        DocumentBuilder builder_;
        bool ok_ = true;
        std::uint32_t instance_ = ExpandedTree::NO_INSTANCE;
        std::vector<std::uint32_t> instance_of_;
        std::vector<NodeId> computed_;
        std::vector<Location> instances_;
        std::vector<std::pair<NodeId, ScalarType>> types_;
        std::size_t text_bytes_ = 0;
    };

    // The expanded tree, written as devices are finished.
    Writer writer_{*this};

    std::span<const CheckedFile> files_;
    Diagnostics* diagnostics_;
    ExpandLimits limits_;
    ExpandCounts* counts_;
    // The diagnostics until the end of the run, one per cause (D-114).
    GroupedErrors grouped_;
    std::map<std::string, const CheckedFile*, std::less<>> templates_;
    std::set<const CheckedFile*> faulty_templates_;
    std::map<const CheckedFile*, Value> roots_;
    // What is being evaluated, for the detail of a CF-410.
    std::string context_;
    std::map<const CheckedFile*, FileIndex> file_index_;
    std::vector<std::string> paths_;
    bool failed_ = false;
    // An error in the instance being expanded, which ends its steps.
    bool device_failed_ = false;
    // A limit of the tree was passed: expansion stops (1.3).
    bool stopped_ = false;
    // The running totals the limits are checked on (C2 of the review of
    // #146): the tags written, the nodes the instance being expanded has
    // made, and the `repeat` values evaluated in the tree.
    std::int64_t max_tags_ = DEFAULT_MAX_TAGS;
    std::uint64_t tags_ = 0;
    std::size_t made_nodes_ = 0;
    std::size_t work_ = 0;
    std::size_t expression_work_ = 0;
    // The expressions parsed, at most MAX_COMPILED_NODES nodes of them, and
    // the parses and typings done (for tests of E1 of the review of #146).
    std::map<SiteKey, Compiled> compiled_;
    std::size_t compiled_nodes_ = 0;
    std::size_t parses_ = 0;
    std::size_t typings_ = 0;

    // The instance being expanded.
    const CheckedFile* channel_ = nullptr;
    std::optional<Location> instance_;
    std::string device_name_;
    std::map<std::string, Parameter, std::less<>> declarations_;
    std::map<std::string, ConfigValue, std::less<>> params_;
    std::map<std::string, std::size_t, std::less<>> param_weights_;
};

}  // namespace

Diagnostic expanded_diagnostic(const ExpandedTree& tree, std::uint16_t code,
                               NodeId node, std::string message,
                               std::string detail) {
    Diagnostic diagnostic{.severity = Severity::ERROR,
                          .code = code,
                          .location = tree.document.location(node),
                          .message = std::move(message),
                          .notes = {}};
    const std::uint32_t instance = node < tree.instance_of.size()
                                       ? tree.instance_of[node]
                                       : ExpandedTree::NO_INSTANCE;
    if (instance == ExpandedTree::NO_INSTANCE ||
        instance >= tree.instances.size() ||
        tree.instances[instance].file == diagnostic.location.file) {
        return diagnostic;
    }
    diagnostic.notes.push_back(
        Diagnostic::Note{.kind = "template",
                         .location = std::move(diagnostic.location),
                         .detail = std::move(detail)});
    diagnostic.location = tree.instances[instance];
    return diagnostic;
}

bool written_in_instance(const ExpandedTree& tree, NodeId node) {
    // A work list rather than recursion: the depth of a tree is the
    // parser's to bound, not this check's.
    const Document& document = tree.document;
    std::vector<NodeId> pending{node};
    while (!pending.empty()) {
        const NodeId next = pending.back();
        pending.pop_back();
        const std::uint32_t instance = next < tree.instance_of.size()
                                           ? tree.instance_of[next]
                                           : ExpandedTree::NO_INSTANCE;
        if (instance < tree.instances.size() &&
            document.location(next).file == tree.instances[instance].file) {
            return true;
        }
        if (document.kind(next) == NodeKind::MAP) {
            for (const Entry& entry : document.entries(next)) {
                pending.push_back(entry.key);
                pending.push_back(entry.value);
            }
        } else if (document.kind(next) == NodeKind::SEQUENCE) {
            const auto items = document.items(next);
            pending.insert(pending.end(), items.begin(), items.end());
        }
    }
    return false;
}

std::optional<ExpandedTree> expand_tree(std::span<const CheckedFile> files,
                                        Diagnostics& diagnostics,
                                        const ExpandLimits& limits,
                                        ExpandCounts* counts) {
    return Expander(files, diagnostics, limits, counts).run();
}

}  // namespace scada::conf
