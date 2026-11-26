// The tags of the expanded tree, and the references to them outside
// expressions (decision D-116; slice 7a of #100).

#include "tag_names.h"

#include "point_types.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/node.h"
#include "scada/conf/references.h"
#include "scada/conf/tree.h"
#include "scada/conf/types.h"

#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
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

constexpr std::uint16_t CF_SYSTEM_TAG = 301;
constexpr std::uint16_t CF_TAG_TWICE = 302;
constexpr std::uint16_t CF_NO_SUCH = 310;
constexpr std::uint16_t CF_WRONG_TAG_TYPE = 312;
constexpr std::uint16_t CF_NO_POLL_GROUP = 604;

// 12.2: the processes always in the tag set.
constexpr std::array<std::string_view, 7> SERVICES = {
    "tagd", "control", "cmd", "alarm", "hist", "api", "audit"};

// A tag's name and value type.
struct Signal {
    std::string_view name;
    std::string_view type;
};

// Driver interface contract 11.1: per driver process, channel and device.
constexpr std::array<Signal, 7> PROCESS_SIGNALS = {{
    {"realtime", "bool"},
    {"config_id", "uint64"},
    {"config_state", "int64"},
    {"stale_outcomes", "uint64"},
    {"answers_dropped", "uint64"},
    {"safe_unjournalled", "uint64"},
    {"log_dropped", "uint64"},
}};
constexpr std::array<Signal, 5> CHANNEL_SIGNALS = {{
    {"state", "int64"},
    {"connects", "uint64"},
    {"refused_connects", "uint64"},
    {"secured", "bool"},
    {"requests_per_s", "float64"},
}};
constexpr std::array<Signal, 18> DEVICE_SIGNALS = {{
    {"state", "int64"},
    {"last_ok_time", "int64"},
    {"requests", "uint64"},
    {"errors", "uint64"},
    {"timeouts", "uint64"},
    {"response_ms", "float64"},
    {"requests_per_scan", "uint64"},
    {"pacing_limited", "bool"},
    {"last_status", "int64"},
    {"exceptions", "uint64"},
    {"refusals", "uint64"},
    {"sentinels", "uint64"},
    {"invalid", "uint64"},
    {"points_absent", "uint64"},
    {"restarts", "uint64"},
    {"selections_lost", "uint64"},
    {"last_command_stage", "int64"},
    {"refresh_failures", "uint64"},
}};
constexpr std::array<Signal, 2> IDENTITY_SIGNALS = {{
    {"identity", "string"},
    {"identity_ok", "bool"},
}};

// 4.6: the poll groups of a channel that gives none.
constexpr std::array<std::string_view, 3> DEFAULT_POLL_GROUPS = {
    "fast", "normal", "slow"};

// The segments of a tag name joined with dots.
std::string dotted(std::initializer_list<std::string_view> segments) {
    std::string name;
    for (const std::string_view segment : segments) {
        if (!name.empty()) {
            name += '.';
        }
        name += segment;
    }
    return name;
}

// The value types that are numbers (4.7).
bool is_number_type(std::string_view type) {
    return type != "bool" && type != "string";
}

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

    [[nodiscard]] std::string text_of(NodeId map, std::string_view key) const {
        const std::optional<NodeId> node = scalar(map, key);
        return node ? std::string(document_->text(*node)) : std::string();
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

private:
    const Document* document_;
};

class Collector {
public:
    Collector(const ExpandedTree& tree, std::span<const CheckedFile> files,
              Diagnostics& diagnostics)
        : tree_(&tree),
          expanded_(tree.document),
          files_(files),
          diagnostics_(&diagnostics) {}

    TagNames run() {
        for (const NodeId channel :
             expanded_.items(expanded_.document().root(), "channels")) {
            collect_channel(channel);
        }
        for (const CheckedFile& file : files_) {
            collect_file(file);
        }
        // The tags of an output need its target, which may be a tag of any
        // file: they are made once every other tag is known.
        for (const CheckedFile& file : files_) {
            if (file.source->path_class.type == FileType::OUTPUTS) {
                const Reader reader(*file.document);
                for (const NodeId output :
                     reader.items(file.document->root(), "outputs")) {
                    collect_output(reader, output);
                }
            }
        }
        collect_processes();
        return std::move(tags_);
    }

private:
    // --- Adding a tag ------------------------------------------------------

    // A tag that a user wrote, at `name` of the expanded tree (a point or
    // a bit, placed at its instance by `expanded_diagnostic`).
    void add_expanded(const std::string& tag, NodeId name, TagInfo info,
                      const std::string& point) {
        // The tag is the instance's: "first at" names the instance, not
        // the template line it came from.
        info.where = expanded_diagnostic(*tree_, 0, name, {}).location;
        if (is_system(tag)) {
            diagnostics_->add(expanded_diagnostic(
                *tree_, CF_SYSTEM_TAG, name,
                "tag names starting with sys. are reserved", point));
            return;
        }
        if (const TagInfo* first = tags_.add(tag, std::move(info))) {
            diagnostics_->add(expanded_diagnostic(
                *tree_, CF_TAG_TWICE, name, twice(tag, first->where), point));
        }
    }

    // A tag that a user wrote in `file`, at its node `name`.
    void add_written(const CheckedFile& file, NodeId name, TagInfo info) {
        const Document& document = *file.document;
        const std::string tag(document.text(name));
        info.where = document.location(name);
        if (is_system(tag)) {
            error(document, CF_SYSTEM_TAG, name,
                  "tag names starting with sys. are reserved");
            return;
        }
        if (const TagInfo* first = tags_.add(tag, std::move(info))) {
            error(document, CF_TAG_TWICE, name, twice(tag, first->where));
        }
    }

    // A generated tag (12.5), made for what is at `where`. A user tag of
    // the same name is CF-302 at the definition that came second.
    void add_generated(const std::string& tag, std::string_view type,
                       const Location& where) {
        TagInfo info{.value_type = std::string(type),
                     .origin = TagOrigin::GENERATED,
                     .where = where,
                     .point = 0,
                     .protocol = {}};
        // Two generated tags of one name come from a cause reported
        // elsewhere (a device or an output given twice, CF-303; two alarms
        // of one id on a tag, CF-812): only a user's tag is CF-302 here.
        if (const TagInfo* first = tags_.add(tag, std::move(info));
            first != nullptr && first->origin != TagOrigin::GENERATED) {
            diagnostics_->error(CF_TAG_TWICE, where, twice(tag, first->where));
        }
    }

    static bool is_system(std::string_view tag) {
        // 2.1: a user tag name "must not start with the segment `sys`".
        return tag == "sys" || tag.starts_with("sys.");
    }

    static std::string twice(std::string_view tag, const Location& first) {
        return "tag " + quote_text(tag) + " is defined twice; first at " +
               first.file + ":" + std::to_string(first.line);
    }

    void error(const Document& document, std::uint16_t code, NodeId node,
               std::string message) {
        diagnostics_->error(code, document.location(node), std::move(message));
    }

    // --- Channels, devices and points of the expanded tree ----------------

    void collect_channel(NodeId channel) {
        const std::string protocol = expanded_.text_of(channel, "protocol");
        std::string backend;
        std::string channel_name;
        std::vector<std::string> groups;
        Location channel_at = expanded_.document().location(channel);
        if (const std::optional<NodeId> written =
                expanded_.child(channel, "channel")) {
            backend = expanded_.text_of(*written, "backend");
            if (const std::optional<NodeId> name =
                    expanded_.scalar(*written, "name")) {
                channel_name = std::string(expanded_.document().text(*name));
                channel_at = expanded_.document().location(*name);
            }
            groups = poll_groups(*written);
        }
        protocols_.insert(protocol);
        if (!channel_name.empty()) {
            for (const Signal& signal : CHANNEL_SIGNALS) {
                add_generated(dotted({"sys.drv", channel_name, signal.name}),
                              signal.type, channel_at);
            }
            for (const std::string& group : groups) {
                add_generated(
                    dotted({"sys.drv", channel_name, "period", group}),
                    "float64", channel_at);
            }
        }
        for (const NodeId device : expanded_.items(channel, "devices")) {
            collect_device(device, protocol, backend);
        }
    }

    // The poll groups of a written channel (4.6): its own, or the defaults.
    [[nodiscard]] std::vector<std::string> poll_groups(NodeId channel) const {
        std::vector<std::string> groups;
        if (const std::optional<NodeId> given =
                expanded_.child(channel, "poll_groups");
            given && expanded_.document().kind(*given) == NodeKind::MAP) {
            for (const Entry& entry : expanded_.document().entries(*given)) {
                groups.emplace_back(expanded_.document().text(entry.key));
            }
            return groups;
        }
        for (const std::string_view group : DEFAULT_POLL_GROUPS) {
            groups.emplace_back(group);
        }
        return groups;
    }

    void collect_device(NodeId device, const std::string& protocol,
                        const std::string& backend) {
        const std::optional<NodeId> name = expanded_.scalar(device, "name");
        if (!name) {
            return;
        }
        const std::string device_name(expanded_.document().text(*name));
        const Location device_at = expanded_.document().location(*name);
        for (const NodeId point : expanded_.items(device, "points")) {
            collect_point(device_name, point, protocol, backend);
        }
        for (const Signal& signal : DEVICE_SIGNALS) {
            add_generated(dotted({"sys.dev", device_name, signal.name}),
                          signal.type, device_at);
        }
        if (!expanded_.items(device, "identify").empty()) {
            for (const Signal& signal : IDENTITY_SIGNALS) {
                add_generated(dotted({"sys.dev", device_name, signal.name}),
                              signal.type, device_at);
            }
        }
    }

    void collect_point(const std::string& device, NodeId point,
                       const std::string& protocol,
                       const std::string& backend) {
        const std::optional<NodeId> name = expanded_.scalar(point, "name");
        if (!name) {
            return;
        }
        const std::string point_name(expanded_.document().text(*name));
        const std::string tag = device + "." + point_name;
        if (expanded_.text_of(point, "tag") != "false") {
            add_expanded(
                tag, *name,
                TagInfo{.value_type = value_type(point, protocol, backend),
                        .origin = TagOrigin::POINT,
                        .where = {},
                        .point = point,
                        .protocol = protocol},
                "point " + point_name);
            collect_alarms(expanded_, point, tag,
                           expanded_.document().location(*name));
        }
        const std::optional<NodeId> bits = expanded_.child(point, "bits");
        if (!bits || expanded_.document().kind(*bits) != NodeKind::MAP) {
            return;
        }
        for (const Entry& entry : expanded_.document().entries(*bits)) {
            collect_bit(device, point, entry.value, protocol);
        }
    }

    void collect_bit(const std::string& device, NodeId point, NodeId bit,
                     const std::string& protocol) {
        const NodeId name = expanded_.document().kind(bit) == NodeKind::SCALAR
                                ? bit
                                : expanded_.scalar(bit, "name").value_or(0);
        if (name == 0) {
            return;
        }
        const std::string bit_name(expanded_.document().text(name));
        const std::string tag = device + "." + bit_name;
        add_expanded(tag, name,
                     TagInfo{.value_type = "bool",
                             .origin = TagOrigin::BIT,
                             .where = {},
                             .point = point,
                             .protocol = protocol},
                     "point " + bit_name);
        if (expanded_.document().kind(bit) == NodeKind::MAP) {
            collect_alarms(expanded_, bit, tag,
                           expanded_.document().location(name));
        }
    }

    [[nodiscard]] std::string value_type(NodeId point,
                                         const std::string& protocol,
                                         const std::string& backend) const {
        if (protocol == "io") {
            return io_value_type(expanded_.document(), point, backend);
        }
        const std::optional<NodeId> address = expanded_.child(point, "address");
        const std::string raw =
            address && expanded_.document().kind(*address) == NodeKind::MAP
                ? modbus_raw_type(expanded_.document(), *address)
                : std::string("uint16");
        return modbus_value_type(*tree_, point, raw);
    }

    // --- Alarms (12.5: `<tag>.alarm.<id>` and its `shelved_until`) --------

    // The alarms listed at `owner` (a point, a bit, a tag of a tag file),
    // whose tag is `tag` unless an alarm names its own.
    void collect_alarms(const Reader& reader, NodeId owner,
                        const std::string& tag, const Location& where) {
        for (const NodeId alarm : reader.items(owner, "alarms")) {
            add_alarm(reader, alarm, tag, where);
        }
    }

    void add_alarm(const Reader& reader, NodeId alarm,
                   const std::string& owner_tag, const Location& where) {
        const std::string type = reader.text_of(alarm, "type");
        std::string base = owner_tag;
        if (type == "expression") {
            base = reader.text_of(alarm, "name");
        } else if (const std::string given = reader.text_of(alarm, "tag");
                   !given.empty()) {
            base = given;
        }
        if (base.empty()) {
            return;
        }
        std::string alarm_id = reader.text_of(alarm, "id");
        if (alarm_id.empty()) {
            alarm_id = type;
        }
        const std::string state = dotted({base, "alarm", alarm_id});
        add_generated(state, "int64", where);
        add_generated(state + ".shelved_until", "int64", where);
    }

    // --- The other files --------------------------------------------------

    void collect_file(const CheckedFile& file) {
        const Reader reader(*file.document);
        const NodeId root = file.document->root();
        switch (file.source->path_class.type) {
            case FileType::TAGS:
                for (const NodeId tag : reader.items(root, "tags")) {
                    collect_tag(file, reader, tag);
                }
                break;
            case FileType::OUTPUTS:
                for (const NodeId interlock :
                     reader.items(root, "interlocks")) {
                    if (const std::optional<NodeId> name =
                            reader.scalar(interlock, "name")) {
                        add_written(file, *name,
                                    TagInfo{.value_type = "bool",
                                            .origin = TagOrigin::INTERLOCK,
                                            .where = {},
                                            .point = 0,
                                            .protocol = {}});
                    }
                }
                break;
            case FileType::AREAS:
                collect_areas(reader, root);
                break;
            case FileType::ALARMS:
                for (const NodeId alarm : reader.items(root, "alarms")) {
                    add_alarm(reader, alarm, {},
                              file.document->location(alarm));
                }
                break;
            default:
                break;
        }
    }

    void collect_tag(const CheckedFile& file, const Reader& reader,
                     NodeId tag) {
        const std::optional<NodeId> name = reader.scalar(tag, "name");
        if (!name) {
            return;
        }
        // A virtual tag's type comes from its expression (slice 7b).
        add_written(file, *name,
                    TagInfo{.value_type = reader.text_of(tag, "value_type"),
                            .origin = TagOrigin::TAG_FILE,
                            .where = {},
                            .point = 0,
                            .protocol = {}});
        collect_alarms(reader, tag, std::string(file.document->text(*name)),
                       file.document->location(*name));
    }

    void collect_areas(const Reader& reader, NodeId root) {
        const std::optional<NodeId> areas = reader.child(root, "areas");
        if (!areas || reader.document().kind(*areas) != NodeKind::MAP) {
            return;
        }
        for (const Entry& entry : reader.document().entries(*areas)) {
            add_generated(dotted({"sys.area", reader.document().text(entry.key),
                                  "authority"}),
                          "int64", reader.document().location(entry.key));
        }
    }

    // 12.5: the tags of an output. Those of the target's type need the
    // target: an output whose target is not a tag has none (CF-310).
    void collect_output(const Reader& reader, NodeId output) {
        const std::optional<NodeId> name = reader.scalar(output, "name");
        if (!name) {
            return;
        }
        const std::string base(reader.document().text(*name));
        const Location where = reader.document().location(*name);
        add_generated(base + ".mode", "int64", where);
        add_generated(base + ".status", "int64", where);
        const TagInfo* target = tags_.find(reader.text_of(output, "target"));
        if (target != nullptr) {
            add_generated(base + ".req_manual", target->value_type, where);
            add_generated(base + ".req_auto", target->value_type, where);
        }
        if (reader.scalar(output, "refresh")) {
            add_generated(base + ".refresh", "int64", where);
        }
        if (has_failsafe_action(reader, output, target)) {
            add_generated(base + ".failsafe", "int64", where);
        }
    }

    // True if a fail-safe key of `output`, given or by its default (7.2,
    // 7.4), is `safe` or `release`.
    [[nodiscard]] bool has_failsafe_action(const Reader& reader, NodeId output,
                                           const TagInfo* target) const {
        const auto acts = [](std::string_view action) {
            return action == "safe" || action == "release";
        };
        const bool local = target != nullptr && target->protocol == "io";
        if (const std::optional<NodeId> given =
                reader.scalar(output, "on_shutdown")) {
            if (acts(reader.document().text(*given))) {
                return true;
            }
        } else if (local) {
            return true;
        }
        if (acts(reader.text_of(output, "on_generation_change"))) {
            return true;
        }
        if (const std::optional<NodeId> loss =
                reader.child(output, "on_control_loss")) {
            return acts(reader.text_of(*loss, "action"));
        }
        return local || remote_switching(reader, output, target);
    }

    // A remote switching output (7.2): `single` or `double` on a protocol
    // driver, not `dangerous`, with `safe_state` `off` or `on`.
    [[nodiscard]] bool remote_switching(const Reader& reader, NodeId output,
                                        const TagInfo* target) const {
        if (target == nullptr || target->protocol == "io" ||
            target->point == 0) {
            return false;
        }
        const std::string kind = reader.text_of(output, "kind");
        const std::string safe = reader.text_of(output, "safe_state");
        return (kind == "single" || kind == "double") &&
               (safe == "off" || safe == "on") &&
               expanded_.text_of(target->point, "dangerous") != "true";
    }

    // 12.2 and 12.5: `config_id` and `config_state` of every process, and
    // the driver signals of each driver process.
    void collect_processes() {
        const Location where{};
        for (const std::string_view service : SERVICES) {
            add_generated(dotted({"sys.proc", service, "config_id"}), "uint64",
                          where);
            add_generated(dotted({"sys.proc", service, "config_state"}),
                          "int64", where);
        }
        for (const std::string& protocol : protocols_) {
            const std::string process =
                protocol == "io" ? std::string("io") : "drv-" + protocol;
            for (const Signal& signal : PROCESS_SIGNALS) {
                add_generated(dotted({"sys.proc", process, signal.name}),
                              signal.type, where);
            }
        }
    }

    const ExpandedTree* tree_;
    Reader expanded_;
    std::span<const CheckedFile> files_;
    Diagnostics* diagnostics_;
    TagNames tags_;
    std::set<std::string, std::less<>> protocols_;
};

// --- References ------------------------------------------------------------

class Referee {
public:
    Referee(const TagNames& tags, const ExpandedTree& tree,
            Diagnostics& diagnostics)
        : tags_(&tags),
          tree_(&tree),
          expanded_(tree.document),
          diagnostics_(&diagnostics) {}

    void check_tree() {
        for (const NodeId channel :
             expanded_.items(expanded_.document().root(), "channels")) {
            std::set<std::string, std::less<>> groups;
            if (const std::optional<NodeId> written =
                    expanded_.child(channel, "channel")) {
                channel_ = expanded_.text_of(*written, "name");
                for (std::string& group : poll_groups(*written)) {
                    groups.insert(std::move(group));
                }
            }
            for (const NodeId device : expanded_.items(channel, "devices")) {
                check_device(device, groups);
            }
        }
    }

    void check_file(const CheckedFile& file) {
        const Reader reader(*file.document);
        const NodeId root = file.document->root();
        switch (file.source->path_class.type) {
            case FileType::OUTPUTS:
                for (const NodeId output : reader.items(root, "outputs")) {
                    check_output(reader, output);
                }
                break;
            case FileType::AREAS:
                check_areas(reader, root);
                break;
            case FileType::TAGS:
                for (const NodeId tag : reader.items(root, "tags")) {
                    for (const NodeId alarm : reader.items(tag, "alarms")) {
                        check_alarm(reader, alarm);
                    }
                }
                break;
            case FileType::ALARMS:
                for (const NodeId alarm : reader.items(root, "alarms")) {
                    if (reader.text_of(alarm, "type") != "expression") {
                        need_tag(reader, reader.scalar(alarm, "tag"));
                    }
                    check_alarm(reader, alarm);
                }
                break;
            case FileType::PROGRAM:
                check_program(reader, root);
                break;
            default:
                break;
        }
    }

private:
    // --- Tags ------------------------------------------------------------

    // CF-310 when the tag at `node` of `reader` does not exist; the tag
    // otherwise.
    const TagInfo* need_tag(const Reader& reader, std::optional<NodeId> node) {
        if (!node) {
            return nullptr;
        }
        const std::string_view name = reader.document().text(*node);
        if (const TagInfo* tag = tags_->find(name)) {
            return tag;
        }
        report(reader, CF_NO_SUCH, *node,
               "tag " + quote_text(name) + " does not exist");
        return nullptr;
    }

    // A tag reference that must name a number (D-116): CF-312 otherwise.
    void need_number(const Reader& reader, std::optional<NodeId> node) {
        if (!node) {
            return;
        }
        const TagInfo* tag = need_tag(reader, node);
        if (tag != nullptr && !tag->value_type.empty() &&
            !is_number_type(tag->value_type)) {
            report(reader, CF_WRONG_TAG_TYPE, *node,
                   quote_text(reader.document().text(*node)) +
                       " must be a number tag");
        }
    }

    // An error at `node` of `reader`: of the expanded tree, placed at its
    // instance with the template note and the point; else at the node.
    void report(const Reader& reader, std::uint16_t code, NodeId node,
                std::string message) {
        if (&reader.document() == &tree_->document) {
            diagnostics_->add(expanded_diagnostic(
                *tree_, code, node, std::move(message),
                point_.empty() ? std::string() : "point " + point_));
            return;
        }
        diagnostics_->error(code, reader.document().location(node),
                            std::move(message));
    }

    void check_output(const Reader& reader, NodeId output) {
        need_tag(reader, reader.scalar(output, "target"));
        const std::optional<NodeId> feedback = reader.child(output, "feedback");
        if (!feedback) {
            return;
        }
        if (reader.document().kind(*feedback) == NodeKind::SCALAR) {
            need_tag(reader, feedback);
            return;
        }
        for (const std::string_view key : {"on", "off", "tag"}) {
            need_tag(reader, reader.scalar(*feedback, key));
        }
    }

    void check_areas(const Reader& reader, NodeId root) {
        const std::optional<NodeId> areas = reader.child(root, "areas");
        if (!areas || reader.document().kind(*areas) != NodeKind::MAP) {
            return;
        }
        for (const Entry& entry : reader.document().entries(*areas)) {
            const std::optional<NodeId> authority =
                reader.child(entry.value, "authority");
            if (!authority) {
                continue;
            }
            if (const std::optional<NodeId> panel =
                    reader.child(*authority, "local_panel")) {
                need_tag(reader, reader.scalar(*panel, "input"));
            }
        }
    }

    // An alarm's `limit` (when a tag) and `setpoint` (8): numbers.
    void check_alarm(const Reader& reader, NodeId alarm) {
        if (const std::optional<NodeId> limit = reader.scalar(alarm, "limit");
            limit && is_tag_text(reader, *limit)) {
            need_number(reader, limit);
        }
        need_number(reader, reader.scalar(alarm, "setpoint"));
    }

    [[nodiscard]] bool is_tag_text(const Reader& reader, NodeId node) const {
        const ScalarTypes& types =
            &reader.document() == &tree_->document ? tree_->types : *types_;
        return types.of(node) == ScalarType::STRING;
    }

    void check_program(const Reader& reader, NodeId root) {
        for (const NodeId block : reader.items(root, "blocks")) {
            if (const std::optional<NodeId> inputs = reader.child(block, "in");
                inputs && reader.document().kind(*inputs) == NodeKind::MAP) {
                for (const Entry& entry : reader.document().entries(*inputs)) {
                    need_tag(reader, reader.scalar(entry.value, "tag"));
                }
            }
            if (const std::optional<NodeId> outputs =
                    reader.child(block, "out");
                outputs && reader.document().kind(*outputs) == NodeKind::MAP) {
                for (const Entry& entry : reader.document().entries(*outputs)) {
                    if (reader.document().kind(entry.value) ==
                        NodeKind::SCALAR) {
                        need_tag(reader, entry.value);
                    }
                }
            }
        }
    }

    // --- Within a device --------------------------------------------------

    [[nodiscard]] std::vector<std::string> poll_groups(NodeId channel) const {
        std::vector<std::string> groups;
        if (const std::optional<NodeId> given =
                expanded_.child(channel, "poll_groups");
            given && expanded_.document().kind(*given) == NodeKind::MAP) {
            for (const Entry& entry : expanded_.document().entries(*given)) {
                groups.emplace_back(expanded_.document().text(entry.key));
            }
            return groups;
        }
        for (const std::string_view group : DEFAULT_POLL_GROUPS) {
            groups.emplace_back(group);
        }
        return groups;
    }

    void check_device(NodeId device,
                      const std::set<std::string, std::less<>>& groups) {
        std::set<std::string_view, std::less<>> points;
        for (const NodeId point : expanded_.items(device, "points")) {
            if (const std::optional<NodeId> name =
                    expanded_.scalar(point, "name")) {
                points.insert(expanded_.document().text(*name));
            }
        }
        point_.clear();
        for (const std::string_view key :
             {"health", "restart_indication", "identify"}) {
            for (const NodeId rule : expanded_.items(device, key)) {
                need_point(points, expanded_.scalar(rule, "point"));
            }
        }
        for (const NodeId point : expanded_.items(device, "points")) {
            point_ = expanded_.text_of(point, "name");
            check_point(point, points, groups);
        }
        point_.clear();
    }

    void check_point(NodeId point,
                     const std::set<std::string_view, std::less<>>& points,
                     const std::set<std::string, std::less<>>& groups) {
        if (const std::optional<NodeId> exponent =
                expanded_.child(point, "exponent_from")) {
            need_point(points, expanded_.scalar(*exponent, "point"));
        }
        // A quality rule may read the point itself (4.7: `self`).
        for (const NodeId rule : expanded_.items(point, "quality_from")) {
            if (expanded_.text_of(rule, "point") != "self") {
                need_point(points, expanded_.scalar(rule, "point"));
            }
        }
        need_point(points, expanded_.scalar(point, "source_time_from"));
        if (const std::optional<NodeId> group =
                expanded_.scalar(point, "poll_group");
            group && !groups.contains(expanded_.document().text(*group))) {
            report(expanded_, CF_NO_POLL_GROUP, *group,
                   "poll group " +
                       quote_text(expanded_.document().text(*group)) +
                       " does not exist on channel " + quote_text(channel_));
        }
        for (const NodeId alarm : expanded_.items(point, "alarms")) {
            check_alarm(expanded_, alarm);
        }
        const std::optional<NodeId> bits = expanded_.child(point, "bits");
        if (bits && expanded_.document().kind(*bits) == NodeKind::MAP) {
            for (const Entry& entry : expanded_.document().entries(*bits)) {
                for (const NodeId alarm :
                     expanded_.items(entry.value, "alarms")) {
                    check_alarm(expanded_, alarm);
                }
            }
        }
    }

    // CF-310 when `node` names no point of the device.
    void need_point(const std::set<std::string_view, std::less<>>& points,
                    std::optional<NodeId> node) {
        if (!node) {
            return;
        }
        const std::string_view name = expanded_.document().text(*node);
        if (!points.contains(name)) {
            report(expanded_, CF_NO_SUCH, *node,
                   "point " + quote_text(name) + " does not exist");
        }
    }

public:
    // The types of the file being checked (for `limit`, a number or a tag).
    void set_types(const ScalarTypes& types) { types_ = &types; }

private:
    const TagNames* tags_;
    const ExpandedTree* tree_;
    Reader expanded_;
    Diagnostics* diagnostics_;
    const ScalarTypes* types_ = nullptr;
    std::string channel_;
    std::string point_;
};

}  // namespace

const TagInfo* TagNames::find(std::string_view name) const {
    const auto found = tags_.find(name);
    return found == tags_.end() ? nullptr : &found->second;
}

const TagInfo* TagNames::add(std::string name, TagInfo info) {
    const auto [entry, added] = tags_.try_emplace(std::move(name), info);
    return added ? nullptr : &entry->second;
}

TagNames collect_tags(const ExpandedTree& tree,
                      std::span<const CheckedFile> files,
                      Diagnostics& diagnostics) {
    return Collector(tree, files, diagnostics).run();
}

void check_tag_references(const TagNames& tags, const ExpandedTree& tree,
                          std::span<const CheckedFile> files,
                          Diagnostics& diagnostics) {
    Referee referee(tags, tree, diagnostics);
    referee.check_tree();
    for (const CheckedFile& file : files) {
        referee.set_types(*file.types);
        referee.check_file(file);
    }
}

}  // namespace scada::conf
