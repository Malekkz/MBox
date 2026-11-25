// Stage 4: templates and instances expanded in the merge order of contract
// 4.5 (decision D-114). Values are copied out of the parsed files into a
// mutable tree, merged, evaluated, and written into one Document.

#include "scada/conf/expand.h"

#include "config_eval.h"
#include "expression_site.h"
#include "expression_types.h"
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

// True if two map keys are one key: the same text, or two integers of one
// value (`15` and `0xF` of `bits`, an inline `enum` or `states`; B6 of the
// review of #146). No key the schema types as a name has the integer form.
bool same_key(std::string_view left, std::string_view right) {
    if (left == right) {
        return true;
    }
    const std::optional<Integer> left_value = parse_integer(left);
    const std::optional<Integer> right_value = parse_integer(right);
    if (!left_value || !right_value ||
        left_value->magnitude != right_value->magnitude) {
        return false;
    }
    return left_value->magnitude == 0 ||
           left_value->negative == right_value->negative;
}

// The member of `map` whose key is `key` by `same_key`.
Member* member_of_key(Value& map, std::string_view key) {
    for (Member& member : map.members) {
        if (same_key(member.key, key)) {
            return &member;
        }
    }
    return nullptr;
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
        if (Member* existing = member_of_key(into, member.key)) {
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

// True if `name` matches `pattern` (2.5): `*` any characters within one
// segment, `?` one character of a segment.
bool matches(std::string_view pattern, std::string_view name) {
    std::size_t cursor = 0;
    std::size_t pos = 0;
    std::size_t star = std::string_view::npos;
    std::size_t resume = 0;
    while (pos < name.size()) {
        if (cursor < pattern.size() &&
            (pattern[cursor] == name[pos] ||
             (pattern[cursor] == '?' && name[pos] != '.'))) {
            ++cursor;
            ++pos;
        } else if (cursor < pattern.size() && pattern[cursor] == '*') {
            star = cursor++;
            resume = pos;
        } else if (star != std::string_view::npos && name[resume] != '.') {
            cursor = star + 1;
            pos = ++resume;
        } else {
            return false;
        }
    }
    while (cursor < pattern.size() && pattern[cursor] == '*') {
        ++cursor;
    }
    return cursor == pattern.size();
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
    Expander(std::span<const CheckedFile> files, Diagnostics& diagnostics)
        : files_(files), diagnostics_(&diagnostics) {}

    std::optional<ExpandedTree> run() {
        for (const CheckedFile& file : files_) {
            if (file.source->path_class.type == FileType::TEMPLATE) {
                templates_.emplace(file.source->path_class.stem, &file);
            }
            file_index_.emplace(&file, static_cast<FileIndex>(paths_.size()));
            paths_.push_back(file.source->path);
        }
        // A template's own faults once, at the template, whether or not an
        // instance uses it (the ruling O-6 on #146).
        for (const CheckedFile& file : files_) {
            if (file.source->path_class.type == FileType::TEMPLATE) {
                check_template(file);
            }
        }
        for (const CheckedFile& file : files_) {
            if (file.source->path_class.type == FileType::CHANNEL) {
                expand_channel(file);
            }
        }
        flush_diagnostics();
        if (failed_) {
            return std::nullopt;
        }
        return write();
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
        const std::string key = format(diagnostic);
        if (!reported_.insert(key).second) {
            return;
        }
        // One cause in a template, met by many instances, is one error at
        // the first, with their count (the ruling O-6 on #146).
        if (!diagnostic.notes.empty()) {
            const Location& site = diagnostic.notes.front().location;
            std::string cause = std::to_string(code);
            cause += '|';
            cause += site.file;
            cause += ':' + std::to_string(site.line) + ':' +
                     std::to_string(site.column) + '|';
            cause += diagnostic.message;
            const auto [group, added] =
                causes_.try_emplace(std::move(cause), pending_.size(), 0);
            if (!added) {
                ++group->second.second;
                return;
            }
        }
        pending_.push_back(std::move(diagnostic));
    }

    // Gives the buffered diagnostics, in the order they arose.
    void flush_diagnostics() {
        for (const auto& [cause, group] : causes_) {
            if (group.second > 0) {
                pending_[group.first].message +=
                    " (and " + std::to_string(group.second) +
                    (group.second == 1 ? " more instance)"
                                       : " more instances)");
            }
        }
        for (Diagnostic& diagnostic : pending_) {
            diagnostics_->add(std::move(diagnostic));
        }
        pending_.clear();
        causes_.clear();
    }

    // --- Channels and devices -----------------------------------------------

    struct Device {
        Value keys;                 // the device object as it will be written
        std::vector<Point> points;  // in order
    };

    struct Channel {
        const CheckedFile* file = nullptr;
        std::vector<Device> devices;
        std::vector<Location> instances;
    };

    void expand_channel(const CheckedFile& file) {
        channel_ = &file;
        Channel channel{.file = &file, .devices = {}, .instances = {}};
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
                std::optional<Device> expanded = expand_device(file, device);
                if (expanded && instance_) {
                    channel.devices.push_back(std::move(*expanded));
                    channel.instances.push_back(*instance_);
                }
            }
        }
        channels_.push_back(std::move(channel));
        channel_ = nullptr;
        instance_.reset();
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
        params_.clear();
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
        apply_overrides(written, device_keys, device.points);
        if (reference != nullptr) {
            // With a template, after step 6: no default reaches them.
            const std::size_t first = device.points.size();
            add_instance_points(written, device);
            keep_present_from(first, device.points);
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
        const Value root = copy_of(file, document.root());
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
                            " is for " + protocol + ", the channel is " +
                            wanted);
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
            const Value root = copy_of(*level, level->document->root());
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
            const Value root = copy_of(*level, level->document->root());
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
        if (given != nullptr) {
            for (const Member& member : given->members) {
                if (!declarations_.contains(member.key)) {
                    error_at(CF_BAD_VALUE, *member.key_file, member.key_node,
                             "parameter " + quote_text(member.key) +
                                 ": the template has no parameter of this "
                                 "name");
                }
            }
        }
        for (const auto& [name, declared] : declarations_) {
            const Value* value = given != nullptr ? given->find(name) : nullptr;
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
        const double as_real = number->type == ValueType::REAL
                                   ? number->real
                                   : static_cast<double>(number->integer);
        if (const std::optional<std::string> outside =
                outside_bounds(declared, as_real)) {
            return refuse(text + *outside);
        }
        // A `number` keeps its written form, as everywhere in the
        // validator: written as an integer it is an INT (the ruling O-9 on
        // #146).
        return number;
    }

    // " is above its max M" or " is below its min M" when `value` is
    // outside the declared bounds.
    static std::optional<std::string> outside_bounds(const Parameter& declared,
                                                     double value) {
        for (const auto& [bound, above] : {std::pair{&declared.min, false},
                                           std::pair{&declared.max, true}}) {
            if (!*bound) {
                continue;
            }
            const std::string limit_text((*bound)->scalar_text());
            const std::optional<double> limit = parse_decimal(limit_text);
            if (limit && (above ? value > *limit : value < *limit)) {
                return std::string(above ? " is above its max "
                                         : " is below its min ") +
                       limit_text;
            }
        }
        return std::nullopt;
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
            for (const Member& table : enums->members) {
                kept->erase(table.key);
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
            // The point limit bounds production too: a `repeat` of 65,536
            // pairs over many points stops at the limit (CF-414). A level's
            // own points have distinct names, so more of them than the
            // limit is more points than the limit after merging.
            if (own.size() >= MAX_POINTS) {
                if (!device_failed_) {
                    error(CF_TOO_MANY_POINTS, *point.value.find("name"),
                          "template " + quote_text(template_name) +
                              " expands to more than 65536 points, the "
                              "limit is 65536");
                }
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
                if (points == nullptr) {
                    return;
                }
                for (const Value& item : points->items) {
                    if (std::optional<Point> point =
                            point_of(item, variables, template_name)) {
                        if (when != nullptr) {
                            point->conditions.push_back(*when);
                        }
                        add(std::move(*point));
                    }
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
                  "the range of " + name + " exceeds its limit of 65536");
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
        message += variable;
        message += " is " + std::to_string(inherited.first);
        message += " in " + inherited.second;
        message += " and " + std::to_string(value);
        message += " in " + template_name;
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
            for (const Value& entry : list->items) {
                const std::string_view pattern = entry.scalar_text();
                bool any = false;
                for (std::size_t i = 0; i < points.size(); ++i) {
                    if (matches(pattern, points[i].name)) {
                        matched[i] = true;
                        any = true;
                    }
                }
                if (!any) {
                    error(CF_NO_SUCH_POINT, entry,
                          "point " + quote_text(pattern) +
                              " does not exist in template " +
                              quote_text(template_name));
                }
            }
            std::size_t index = 0;
            std::erase_if(
                points, [&](const Point&) { return matched[index++] != keep; });
        }
    }

    // Step 4: a point whose `when`, or the `when` of a block that made it,
    // is false does not exist; then the instance's `disable` or `only`.
    void keep_present(const Value& instance, std::vector<Point>& points) {
        keep_present_from(0, points);
        select(instance, template_of(instance), points);
    }

    // The `when` part of step 4 for the points from `first` on.
    void keep_present_from(std::size_t first, std::vector<Point>& points) {
        std::size_t position = 0;
        std::erase_if(points, [this, first, &position](Point& point) {
            if (position++ < first) {
                return false;
            }
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
    static void apply_defaults(const Value& template_defaults,
                               const Value& instance,
                               std::vector<Point>& points) {
        Value defaults = template_defaults;
        if (const Value* own = instance.find("defaults")) {
            merge(defaults, *own, "defaults");
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
                             "point " + quote_text(member.key) +
                                 " does not exist in template " +
                                 quote_text(template_of(instance)));
                    continue;
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
    void add_instance_points(const Value& instance, Device& device) {
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
        // The identity checks' expressions (4.8, 2.4) have the parameters
        // and no variable.
        if (Value* identify = device.keys.find("identify")) {
            context_ = "identify";
            evaluate_below(*identify, {});
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
            if (!names.insert(text).second) {
                error(CF_POINT_TWICE, *name,
                      "point " + quote_text(text) + " is defined twice",
                      "point " + point.name);
                continue;
            }
            if (check_name_grammar(text, *name, point.name)) {
                check_tag_name(CF_GENERATED_NAME_LENGTH, text, *name,
                               point.name);
            }
        }
    }

    // Step 8: a point or bit name made by expansion follows 2.1 (CF-212;
    // 1.4, stage 4): `s{n}` outside a block, `x.{n}` with n = -1. Its
    // length is CF-402 or CF-403. A `{...}` left in the name names no
    // variable bound to the point, which the message says (B7 of the
    // review of #146).
    bool check_name_grammar(const std::string& name, const Value& site,
                            const std::string& point) {
        if (is_tag_name(name, std::numeric_limits<std::size_t>::max())) {
            return true;
        }
        std::string message = quote_text(name) + " is not a valid point name";
        const std::size_t open = name.find('{');
        const std::size_t close = open == std::string::npos
                                      ? std::string::npos
                                      : name.find('}', open);
        if (close != std::string::npos) {
            message += ": " + name.substr(open, close - open + 1) +
                       " names no variable bound to this point";
        }
        error(CF_BAD_NAME, site, message, "point " + point);
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

    std::optional<ConfigValue> evaluate(
        const Value& value,
        const std::map<std::string, std::int64_t, std::less<>>& variables,
        ExpressionKind kind, Wanted wanted = Wanted::ANY) {
        ExpressionError syntax;
        const std::string text(value.scalar_text());
        const std::optional<Expression> tree =
            parse_expression(text, kind, syntax);
        if (!tree) {
            // A plain number under `number_or_expression` is no expression.
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
        EvalError problem;
        std::optional<ConfigValue> result =
            evaluate_configuration(*tree, bindings, problem);
        if (!result) {
            // Every failure of a configuration expression, a type that
            // does not fit included, is CF-410 in the form of 2.4 and 10.4
            // (B6 of the review of #144), at the offending character.
            std::string reason = problem.message;
            constexpr std::string_view PREFIX = "expression: ";
            if (reason.starts_with(PREFIX)) {
                reason = reason.substr(PREFIX.size());
            }
            report_evaluation(value, problem.column, reason,
                              detail_of(*tree, bindings));
            return result;
        }
        if (!fits(result->type, wanted)) {
            report_evaluation(
                value, tree->nodes.at(tree->root).column,
                std::string(wanted == Wanted::INT
                                ? "the value must be an INT, not "
                                : "the value must be a number, not ") +
                    std::string(value_type_name(result->type)),
                detail_of(*tree, bindings));
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

    std::optional<ExpandedTree> write() {
        Writer writer(*this);
        return writer.run();
    }

    class Writer {
    public:
        explicit Writer(const Expander& expander) : expander_(&expander) {}

        std::optional<ExpandedTree> run() {
            for (const std::string& path : expander_->paths_) {
                if (!builder_.add_file(path).ok()) {
                    return std::nullopt;
                }
            }
            const SourcePosition top{};
            ok_ = builder_.begin_map(top).ok() && key("channels", top) &&
                  builder_.begin_sequence(top).ok();
            for (const Channel& channel : expander_->channels_) {
                write_channel(channel);
            }
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
                              .instances = std::move(instances_)};
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
            const Result<NodeId> node =
                builder_.key(text, ScalarStyle::PLAIN, where);
            if (node.ok()) {
                note(node.value());
            }
            return node.ok();
        }

        void scalar(std::string_view text, ScalarType type,
                    SourcePosition where) {
            const Result<NodeId> node =
                builder_.scalar(text, ScalarStyle::PLAIN, where);
            ok_ = ok_ && node.ok();
            if (node.ok()) {
                note(node.value());
                types_.emplace_back(node.value(), type);
            }
        }

        void write_channel(const Channel& channel) {
            const CheckedFile& file = *channel.file;
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
            for (std::size_t i = 0; i < channel.devices.size(); ++i) {
                instance_ = static_cast<std::uint32_t>(instances_.size());
                instances_.push_back(channel.instances[i]);
                write_device(channel.devices[i]);
                instance_ = ExpandedTree::NO_INSTANCE;
            }
            ok_ =
                ok_ && builder_.end_sequence().ok() && builder_.end_map().ok();
        }

        void write_device(const Device& device) {
            const SourcePosition top =
                position(*device.keys.file, device.keys.node);
            ok_ = ok_ && builder_.begin_map(top).ok();
            write_members(device.keys);
            ok_ =
                ok_ && key("points", top) && builder_.begin_sequence(top).ok();
            for (const Point& point : device.points) {
                write_value(point.value);
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
                        scalar(value.text, value.type, where);
                        return;
                    }
                    const Document& document = *value.file->document;
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
        std::vector<Location> instances_;
        std::vector<std::pair<NodeId, ScalarType>> types_;
    };

    std::span<const CheckedFile> files_;
    Diagnostics* diagnostics_;
    std::map<std::string, const CheckedFile*, std::less<>> templates_;
    std::set<const CheckedFile*> faulty_templates_;
    // What is being evaluated, for the detail of a CF-410.
    std::string context_;
    // The diagnostics until the end of the run, and the first of each cause
    // with the number of other instances it arose at.
    std::vector<Diagnostic> pending_;
    std::map<std::string, std::pair<std::size_t, std::size_t>> causes_;
    std::map<const CheckedFile*, FileIndex> file_index_;
    std::vector<std::string> paths_;
    std::vector<Channel> channels_;
    std::set<std::string> reported_;
    bool failed_ = false;
    // An error in the instance being expanded, which ends its steps.
    bool device_failed_ = false;

    // The instance being expanded.
    const CheckedFile* channel_ = nullptr;
    std::optional<Location> instance_;
    std::string device_name_;
    std::map<std::string, Parameter, std::less<>> declarations_;
    std::map<std::string, ConfigValue, std::less<>> params_;
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

std::optional<ExpandedTree> expand_tree(std::span<const CheckedFile> files,
                                        Diagnostics& diagnostics) {
    return Expander(files, diagnostics).run();
}

}  // namespace scada::conf
