// The tags of the expanded tree, and the references to them outside
// expressions (decision D-116; slice 7a of #100).

#include "tag_names.h"

#include "grouped_errors.h"
#include "name_pattern.h"
#include "point_types.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
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
constexpr std::uint16_t CF_NO_MATCH = 311;
constexpr std::uint16_t CF_WRONG_TAG_TYPE = 312;
constexpr std::uint16_t CF_GENERATED_TOO_LONG = 403;
constexpr std::uint16_t CF_NO_POLL_GROUP = 604;
constexpr std::uint16_t CF_TOO_MANY_TAGS = 1001;

// 2.1: the most bytes of a tag name.
constexpr std::size_t MAX_TAG_NAME_BYTES = 127;

// The pattern and point comparisons of `applies_to` in one run; past it the
// patterns left are not checked (D-116), so that n patterns over n points
// stay bounded.
constexpr std::size_t MAX_PATTERN_WORK = 20'000'000;

constexpr std::string_view DEVICE_PREFIX = "sys.dev.";
constexpr std::string_view CHANNEL_PREFIX = "sys.drv.";
constexpr std::string_view PERIOD_SEGMENT = ".period";

// 12.2: the processes always in the tag set.
constexpr std::array<std::string_view, 7> SERVICES = {
    "tagd", "control", "cmd", "alarm", "hist", "api", "audit"};

// The value types of section 4.7.
constexpr std::array<std::string_view, 5> VALUE_TYPES = {
    "bool", "int64", "uint64", "float64", "string"};

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
// `refresh_failures` only for a device that a refreshed output writes
// ("Phase 5, devices with a refreshed output (8.12)", D-116).
constexpr std::string_view REFRESH_FAILURES = "refresh_failures";
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
    {REFRESH_FAILURES, "uint64"},
}};
constexpr std::array<Signal, 2> IDENTITY_SIGNALS = {{
    {"identity", "string"},
    {"identity_ok", "bool"},
}};
constexpr std::string_view PERIOD_TYPE = "float64";

// 4.6: the poll groups of a channel that gives none, and a point's.
constexpr std::array<std::string_view, 3> DEFAULT_POLL_GROUPS = {
    "fast", "normal", "slow"};
constexpr std::string_view DEFAULT_POLL_GROUP = "normal";

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

// `type` as one of the constant value types, so that a TagInfo holds no
// text of its own; empty for another.
std::string_view constant_type(std::string_view type) {
    for (const std::string_view known : VALUE_TYPES) {
        if (known == type) {
            return known;
        }
    }
    return {};
}

bool is_system(std::string_view tag) {
    // 2.1: a user tag name "must not start with the segment `sys`".
    return tag == "sys" || tag.starts_with("sys.");
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

// The poll groups of a written channel (4.6): its own, which replace the
// defaults, or the defaults (D-116). Views into `document`, or constant.
std::vector<std::string_view> channel_poll_groups(const Reader& reader,
                                                  NodeId channel) {
    std::vector<std::string_view> groups;
    if (const std::optional<NodeId> given =
            reader.child(channel, "poll_groups");
        given && reader.document().kind(*given) == NodeKind::MAP) {
        for (const Entry& entry : reader.document().entries(*given)) {
            groups.push_back(reader.document().text(entry.key));
        }
    } else {
        groups.assign(DEFAULT_POLL_GROUPS.begin(), DEFAULT_POLL_GROUPS.end());
    }
    std::ranges::sort(groups);
    return groups;
}

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
};

}  // namespace

// --- Collecting --------------------------------------------------------------

// Builds the TagNames of a tree in phases, so that the tags an output or an
// alarm needs are known when it is met (D-116): (0) the process tags;
// (1) areas, channels, devices, points, bits, the tags of tag files and
// interlocks; (2) the alarms; (3) the outputs. An alarm whose tag is made
// later (an output's tag, another alarm's state) waits for it.
class TagCollector {
public:
    TagCollector(const ExpandedTree& tree, std::span<const CheckedFile> files,
                 Diagnostics& diagnostics)
        : tree_(&tree),
          expanded_(tree.document),
          files_(files),
          reporter_(tree, diagnostics) {
        tags_.expanded_ = &tree.document;
    }

    TagNames run() {
        collect_processes();
        for (const CheckedFile& file : files_) {
            if (file.source->path_class.type == FileType::AREAS) {
                collect_areas(file);
            }
        }
        for (const NodeId channel :
             expanded_.items(expanded_.document().root(), "channels")) {
            collect_channel(channel);
        }
        for (const CheckedFile& file : files_) {
            collect_file_tags(file);
        }
        collect_tree_alarms();
        for (const CheckedFile& file : files_) {
            collect_file_alarms(file);
        }
        for (const CheckedFile& file : files_) {
            if (file.source->path_class.type == FileType::OUTPUTS) {
                const Reader reader(*file.document);
                for (const NodeId output :
                     reader.items(file.document->root(), "outputs")) {
                    collect_output(reader, output);
                }
            }
        }
        reporter_.flush();
        tags_.complete_ = !stopped_;
        return std::move(tags_);
    }

private:
    // What a tag is made from or defined at: a node of a Document; on the
    // expanded tree, the point it belongs to (for the note's detail, made
    // only for an error) and the instance's name.
    struct Cause {
        const Document* document = nullptr;
        NodeId node = 0;
        NodeId point = 0;
        std::string_view device;
    };

    // An alarm whose tag is not known yet.
    struct Waiting {
        std::string_view id;
        Cause cause;
    };

    // --- Counting (CF-1001) -----------------------------------------------

    // Counts `n` more tags; false, with CF-1001 at `cause` once, when they
    // pass `limits.max_tags`. The tags past it are not collected.
    bool count(std::size_t n, const Cause& cause) {
        if (stopped_) {
            return false;
        }
        tags_.count_ += n;
        if (tags_.count_ <= static_cast<std::size_t>(
                                std::max<std::int64_t>(tree_->max_tags, 0))) {
            return true;
        }
        std::string message = std::to_string(tags_.count_) +
                              " tags, the limit is " +
                              std::to_string(tree_->max_tags);
        if (cause.document == nullptr) {
            reporter_.error_at({}, CF_TOO_MANY_TAGS, std::move(message));
        } else {
            report(cause, CF_TOO_MANY_TAGS, std::move(message));
        }
        stopped_ = true;
        return false;
    }

    // --- Adding a tag ------------------------------------------------------

    void report(const Cause& cause, std::uint16_t code, std::string message,
                std::string_view key = {}) {
        reporter_.error(*cause.document, cause.node, code, std::move(message),
                        cause.point == 0
                            ? std::string()
                            : "point " + std::string(expanded_.text_of(
                                             cause.point, "name")),
                        cause.device, key);
    }

    [[nodiscard]] Location place_of(const TagInfo& info) const {
        return info.document == nullptr
                   ? Location{}
                   : reporter_.place_of(*info.document, info.node);
    }

    static std::string twice(std::string_view tag, const Location& first) {
        return "tag " + quote_text(tag) + " is defined twice; first at " +
               first.file + ":" + std::to_string(first.line);
    }

    // CF-301 for a user's name in `sys`, at `cause`; true if it is one.
    bool refuse_system(std::string_view name, const Cause& cause) {
        if (!is_system(name)) {
            return false;
        }
        report(cause, CF_SYSTEM_TAG,
               "tag names starting with sys. are reserved");
        return true;
    }

    // A tag a user wrote, at `cause`. `check_system` is false for the tags
    // of a device in `sys`, refused once at its name (S2 of the review of
    // #153).
    void add_user(const std::string& name, TagInfo info, const Cause& cause,
                  bool check_system = true) {
        if (stopped_ || (check_system && refuse_system(name, cause))) {
            return;
        }
        info.document = cause.document;
        info.node = cause.node;
        const auto [entry, added] = tags_.tags_.try_emplace(name, info);
        if (!added) {
            report(cause, CF_TAG_TWICE, twice(name, place_of(entry->second)));
            return;
        }
        if (count(1, cause)) {
            made(entry->first);
        }
    }

    // A generated tag (12.5) made from `cause`; false if it is not added.
    bool add_generated(const std::string& name, std::string_view type,
                       TagOrigin origin, const Cause& cause) {
        if (stopped_) {
            return false;
        }
        if (name.size() > MAX_TAG_NAME_BYTES) {
            // The byte count differs between instances: the cause is the
            // name without the instance.
            const std::string key =
                instance_free_key("generated tag " + quote_text(name),
                                  cause.device) +
                " is too long";
            report(cause, CF_GENERATED_TOO_LONG,
                   "generated tag " + quote_text(name) + " is " +
                       std::to_string(name.size()) + " bytes, the limit is 127",
                   key);
            return false;
        }
        // The tags of devices and channels are not in the map: a name of
        // theirs is looked for there (item 4 of the delta review).
        if (const std::optional<TagInfo> family = family_tag(name)) {
            report(cause, CF_TAG_TWICE, twice(name, place_of(*family)));
            return false;
        }
        const TagInfo info{.value_type = type,
                           .origin = origin,
                           .document = cause.document,
                           .node = cause.node,
                           .point = 0,
                           .device = {},
                           .protocol = {}};
        const auto [entry, added] = tags_.tags_.try_emplace(name, info);
        if (added) {
            if (count(1, cause)) {
                made(entry->first);
            }
            return true;
        }
        const TagInfo& first = entry->second;
        if (origin == TagOrigin::ALARM_STATE &&
            first.origin == TagOrigin::ALARM_STATE) {
            return false;  // two alarms of one id on one tag: CF-812, slice 7c
        }
        if (first.origin == TagOrigin::GENERATED ||
            first.origin == TagOrigin::ALARM_STATE ||
            first.document == nullptr) {
            // Two causes made one name: at the second (M3 of the review).
            report(cause, CF_TAG_TWICE, twice(name, place_of(first)));
            return false;
        }
        // A user's tag of a generated name: at the user's tag, which is the
        // name to change; "first at" names what made the other (D-116).
        reporter_.error(
            *first.document, first.node, CF_TAG_TWICE,
            twice(name, cause.document == nullptr
                            ? Location{}
                            : reporter_.place_of(*cause.document, cause.node)),
            first.document == &tree_->document ? point_detail(first)
                                               : std::string(),
            first.device);
        return false;
    }

    // The tag of a device or a channel of this name, or nothing.
    [[nodiscard]] std::optional<TagInfo> family_tag(
        std::string_view name) const {
        if (name.starts_with(DEVICE_PREFIX)) {
            return tags_.device_tag(name);
        }
        if (name.starts_with(CHANNEL_PREFIX)) {
            return tags_.channel_tag(name);
        }
        return std::nullopt;
    }

    // "point <name>" of a point or bit tag of the expanded tree.
    [[nodiscard]] std::string point_detail(const TagInfo& info) const {
        return "point " + std::string(expanded_.text_of(info.point, "name"));
    }

    // A tag was made: the alarms waiting for it make their state tags.
    void made(std::string_view name) {
        if (!waiting_.empty() && waiting_.contains(name)) {
            ready_.emplace_back(name);
        }
    }

    // --- Processes and areas ----------------------------------------------

    // 12.2 and 12.5: `config_id` and `config_state` of every process, and
    // the driver signals of each driver process (`io` for the I/O server,
    // `drv-<protocol>` otherwise). They name nothing a user wrote, and are
    // counted first.
    // The process tags are made from nothing a user wrote: a CF-1001 they
    // cause is at `limits.max_tags` of system.yaml, where the limit is set,
    // else at the file.
    [[nodiscard]] Cause limit_cause() const {
        for (const CheckedFile& file : files_) {
            if (file.source->path_class.type != FileType::SYSTEM) {
                continue;
            }
            const Reader reader(*file.document);
            NodeId node = file.document->root();
            if (const std::optional<NodeId> limits =
                    reader.child(node, "limits")) {
                node = reader.scalar(*limits, "max_tags").value_or(node);
            }
            return Cause{.document = file.document,
                         .node = node,
                         .point = 0,
                         .device = {}};
        }
        return Cause{};
    }

    void collect_processes() {
        std::set<std::string_view> protocols;
        for (const NodeId channel :
             expanded_.items(expanded_.document().root(), "channels")) {
            protocols.insert(expanded_.text_of(channel, "protocol"));
        }
        const Cause none = limit_cause();
        for (const std::string_view service : SERVICES) {
            add_generated(dotted({"sys.proc", service, "config_id"}), "uint64",
                          TagOrigin::GENERATED, none);
            add_generated(dotted({"sys.proc", service, "config_state"}),
                          "int64", TagOrigin::GENERATED, none);
        }
        for (const std::string_view protocol : protocols) {
            if (protocol.empty()) {
                continue;
            }
            const std::string process = protocol == "io"
                                            ? std::string("io")
                                            : "drv-" + std::string(protocol);
            for (const Signal& signal : PROCESS_SIGNALS) {
                add_generated(dotted({"sys.proc", process, signal.name}),
                              signal.type, TagOrigin::GENERATED, none);
            }
        }
    }

    void collect_areas(const CheckedFile& file) {
        const Reader reader(*file.document);
        for (const Entry& entry :
             reader.entries(file.document->root(), "areas")) {
            add_generated(dotted({"sys.area", file.document->text(entry.key),
                                  "authority"}),
                          "int64", TagOrigin::GENERATED,
                          Cause{.document = file.document,
                                .node = entry.key,
                                .point = 0,
                                .device = {}});
        }
    }

    // --- Channels, devices and points of the expanded tree ----------------

    void collect_channel(NodeId channel) {
        const std::string_view protocol =
            expanded_.text_of(channel, "protocol");
        std::string_view backend;
        if (const std::optional<NodeId> written =
                expanded_.child(channel, "channel")) {
            backend = expanded_.text_of(*written, "backend");
            if (const std::optional<NodeId> name =
                    expanded_.scalar(*written, "name")) {
                add_channel(*name, channel_poll_groups(expanded_, *written));
            }
        }
        for (const NodeId device : expanded_.items(channel, "devices")) {
            collect_device(device, protocol, backend);
        }
    }

    // `sys.drv.<channel>.*` (11.1): found from the channel's name and groups.
    void add_channel(NodeId name, std::vector<std::string_view> groups) {
        const std::string_view channel = expanded_.document().text(name);
        const Cause cause{.document = &expanded_.document(),
                          .node = name,
                          .point = 0,
                          .device = {}};
        const std::size_t tags = CHANNEL_SIGNALS.size() + groups.size();
        const auto [entry, added] = tags_.channels_.try_emplace(
            std::string(channel),
            TagNames::Channel{.name = name, .groups = std::move(groups)});
        if (!added || stopped_) {
            return;  // a channel given twice: CF-303
        }
        // A channel's and a poll group's name are identifiers of at most 31
        // bytes (2.1): its tags stay within 127 bytes.
        count(tags, cause);
    }

    void collect_device(NodeId device, std::string_view protocol,
                        std::string_view backend) {
        const std::optional<NodeId> name = expanded_.scalar(device, "name");
        if (!name || stopped_) {
            return;
        }
        const std::string_view device_name = expanded_.document().text(*name);
        const auto [entry, added] = tags_.devices_.try_emplace(
            std::string(device_name),
            TagNames::Device{
                .name = *name,
                .identify = !expanded_.items(device, "identify").empty(),
                .refreshed = false,
                .points = {}});
        if (!added) {
            return;  // a device given twice: CF-303, its tags not collected
        }
        TagNames::Device& record = entry->second;
        const Cause cause{.document = &expanded_.document(),
                          .node = *name,
                          .point = 0,
                          .device = device_name};
        // A device in `sys`: one CF-301, at its name (S2 of the review).
        // A device's name is at most 100 bytes (2.1): its tags stay within
        // 127 bytes.
        const bool in_system = refuse_system(device_name, cause);
        // `refresh_failures` is counted with the output that refreshes it.
        if (!count(DEVICE_SIGNALS.size() - 1 +
                       (record.identify ? IDENTITY_SIGNALS.size() : 0),
                   cause)) {
            return;
        }
        const PointPlace place{.device = device_name,
                               .protocol = protocol,
                               .backend = backend,
                               .check_system = !in_system};
        for (const NodeId point : expanded_.items(device, "points")) {
            collect_point(place, point, record.points);
        }
        std::ranges::sort(record.points);
    }

    struct PointPlace {
        std::string_view device;
        std::string_view protocol;
        std::string_view backend;
        bool check_system = true;
    };

    void collect_point(const PointPlace& place, NodeId point,
                       std::vector<std::string_view>& points) {
        const std::optional<NodeId> name = expanded_.scalar(point, "name");
        if (!name) {
            return;
        }
        const std::string_view point_name = expanded_.document().text(*name);
        points.push_back(point_name);
        if (expanded_.text_of(point, "tag") != "false") {
            add_user(dotted({place.device, point_name}),
                     TagInfo{.value_type = value_type(point, place),
                             .origin = TagOrigin::POINT,
                             .document = nullptr,
                             .node = 0,
                             .point = point,
                             .device = place.device,
                             .protocol = place.protocol},
                     cause_at(*name, point, place.device), place.check_system);
        }
        for (const Entry& entry : expanded_.entries(point, "bits")) {
            const NodeId bit = entry.value;
            const NodeId bit_name =
                expanded_.document().kind(bit) == NodeKind::SCALAR
                    ? bit
                    : expanded_.scalar(bit, "name").value_or(0);
            if (bit_name == 0) {
                continue;
            }
            add_user(
                dotted({place.device, expanded_.document().text(bit_name)}),
                TagInfo{.value_type = "bool",
                        .origin = TagOrigin::BIT,
                        .document = nullptr,
                        .node = 0,
                        .point = point,
                        .device = place.device,
                        .protocol = place.protocol},
                cause_at(bit_name, point, place.device), place.check_system);
        }
    }

    // A cause at `node` of `point` of the expanded tree.
    [[nodiscard]] Cause cause_at(NodeId node, NodeId point,
                                 std::string_view device) const {
        return Cause{.document = &expanded_.document(),
                     .node = node,
                     .point = point,
                     .device = device};
    }

    [[nodiscard]] std::string_view value_type(NodeId point,
                                              const PointPlace& place) const {
        if (place.protocol == "io") {
            return constant_type(
                io_value_type(expanded_.document(), point, place.backend));
        }
        const std::optional<NodeId> address = expanded_.child(point, "address");
        const std::string raw =
            address && expanded_.document().kind(*address) == NodeKind::MAP
                ? modbus_raw_type(expanded_.document(), *address)
                : std::string("uint16");
        return constant_type(modbus_value_type(*tree_, point, raw));
    }

    // --- The tags of the other files --------------------------------------

    void collect_file_tags(const CheckedFile& file) {
        const Reader reader(*file.document);
        const NodeId root = file.document->root();
        const FileType type = file.source->path_class.type;
        if (type != FileType::TAGS && type != FileType::OUTPUTS) {
            return;
        }
        const bool tags = type == FileType::TAGS;
        for (const NodeId item :
             reader.items(root, tags ? "tags" : "interlocks")) {
            const std::optional<NodeId> name = reader.scalar(item, "name");
            if (!name) {
                continue;
            }
            // A virtual tag's type comes from its expression (slice 7b).
            add_user(
                std::string(file.document->text(*name)),
                TagInfo{
                    .value_type = tags ? reader.text_of(item, "value_type")
                                       : std::string_view("bool"),
                    .origin = tags ? TagOrigin::TAG_FILE : TagOrigin::INTERLOCK,
                    .document = nullptr,
                    .node = 0,
                    .point = 0,
                    .device = {},
                    .protocol = {}},
                Cause{.document = file.document,
                      .node = *name,
                      .point = 0,
                      .device = {}});
        }
    }

    // --- Alarms (12.5: `<tag>.alarm.<id>` and its `shelved_until`) --------

    void collect_tree_alarms() {
        for (const NodeId channel :
             expanded_.items(expanded_.document().root(), "channels")) {
            for (const NodeId device : expanded_.items(channel, "devices")) {
                const std::optional<NodeId> name =
                    expanded_.scalar(device, "name");
                if (!name || tags_.device_at(
                                 expanded_.document().text(*name)) != *name) {
                    continue;  // a device given twice (S1 of the review)
                }
                collect_device_alarms(device, expanded_.document().text(*name));
            }
        }
    }

    void collect_device_alarms(NodeId device, std::string_view device_name) {
        for (const NodeId point : expanded_.items(device, "points")) {
            const std::optional<NodeId> name = expanded_.scalar(point, "name");
            if (!name) {
                continue;
            }
            const std::span<const NodeId> alarms =
                expanded_.items(point, "alarms");
            // The alarms of a `tag: false` point have no tag of their own
            // (D-116): only those that name one make state tags.
            const std::string owner =
                alarms.empty() || expanded_.text_of(point, "tag") == "false"
                    ? std::string()
                    : dotted({device_name, expanded_.document().text(*name)});
            for (const NodeId alarm : alarms) {
                add_alarm(expanded_, alarm, owner,
                          cause_at(alarm, point, device_name));
            }
            for (const Entry& entry : expanded_.entries(point, "bits")) {
                const std::optional<NodeId> bit =
                    expanded_.scalar(entry.value, "name");
                if (!bit || expanded_.items(entry.value, "alarms").empty()) {
                    continue;
                }
                const std::string bit_tag =
                    dotted({device_name, expanded_.document().text(*bit)});
                for (const NodeId alarm :
                     expanded_.items(entry.value, "alarms")) {
                    add_alarm(expanded_, alarm, bit_tag,
                              cause_at(alarm, point, device_name));
                }
            }
        }
    }

    void collect_file_alarms(const CheckedFile& file) {
        const Reader reader(*file.document);
        const NodeId root = file.document->root();
        const auto cause = [&file](NodeId node) {
            return Cause{.document = file.document,
                         .node = node,
                         .point = 0,
                         .device = {}};
        };
        if (file.source->path_class.type == FileType::TAGS) {
            for (const NodeId tag : reader.items(root, "tags")) {
                const std::string owner(reader.text_of(tag, "name"));
                for (const NodeId alarm : reader.items(tag, "alarms")) {
                    add_alarm(reader, alarm, owner, cause(alarm));
                }
            }
        } else if (file.source->path_class.type == FileType::ALARMS) {
            for (const NodeId alarm : reader.items(root, "alarms")) {
                add_alarm(reader, alarm, {}, cause(alarm));
            }
        }
    }

    // The state tags of `alarm` (12.5): on its own `tag`, else on
    // `owner_tag`; an expression alarm's on its `name`. A tag that is not
    // known yet is waited for; one never made is CF-310 of the references.
    void add_alarm(const Reader& reader, NodeId alarm,
                   const std::string& owner_tag, const Cause& cause) {
        if (stopped_) {
            return;
        }
        const std::string_view type = reader.text_of(alarm, "type");
        std::string_view alarm_id = reader.text_of(alarm, "id");
        if (alarm_id.empty()) {
            alarm_id = type;
        }
        std::string base = owner_tag;
        if (type == "expression") {
            const std::optional<NodeId> name = reader.scalar(alarm, "name");
            if (!name) {
                return;
            }
            base = std::string(reader.document().text(*name));
            // An expression alarm's name is a user's name (S3).
            if (refuse_system(base, Cause{.document = cause.document,
                                          .node = *name,
                                          .point = cause.point,
                                          .device = cause.device})) {
                return;
            }
            // An expression alarm is on no tag: its state tags are CF-302
            // against any other of their name (M3 of the delta review).
            make_state_tags(base, alarm_id, TagOrigin::GENERATED, cause);
        } else {
            if (const std::string_view given = reader.text_of(alarm, "tag");
                !given.empty()) {
                base = std::string(given);
            }
            if (base.empty()) {
                return;
            }
            if (tags_.find(base)) {
                make_state_tags(base, alarm_id, TagOrigin::ALARM_STATE, cause);
            } else {
                waiting_.emplace(std::move(base),
                                 Waiting{.id = alarm_id, .cause = cause});
            }
        }
        release();
    }

    // The state tag, then its `shelved_until`: one error for an alarm whose
    // state tag is not made.
    void make_state_tags(std::string_view base, std::string_view alarm_id,
                         TagOrigin origin, const Cause& cause) {
        const std::string state = dotted({base, "alarm", alarm_id});
        if (add_generated(state, "int64", origin, cause)) {
            add_generated(state + ".shelved_until", "int64", origin, cause);
        }
    }

    // The alarms waiting for the tags made since: a work list, not
    // recursion, for chains of alarms on alarm states.
    void release() {
        while (!ready_.empty() && !stopped_) {
            const std::string name = std::move(ready_.back());
            ready_.pop_back();
            const auto [first, last] = waiting_.equal_range(name);
            std::vector<Waiting> released;
            for (auto entry = first; entry != last; ++entry) {
                released.push_back(entry->second);
            }
            waiting_.erase(first, last);
            for (const Waiting& alarm : released) {
                make_state_tags(name, alarm.id, TagOrigin::ALARM_STATE,
                                alarm.cause);
            }
        }
        ready_.clear();
    }

    // --- Outputs ------------------------------------------------------------

    // 12.5: the tags of an output. Those of the target's type need the
    // target: an output whose target is not a tag has none (CF-310). An
    // output given twice is CF-302 once, at its `.mode`, and its tags are
    // made once: CF-303 does not name outputs (D-116).
    void collect_output(const Reader& reader, NodeId output) {
        const std::optional<NodeId> name = reader.scalar(output, "name");
        if (!name || stopped_) {
            return;
        }
        const std::string_view base = reader.document().text(*name);
        const Cause cause{.document = &reader.document(),
                          .node = *name,
                          .point = 0,
                          .device = {}};
        if (refuse_system(base, cause)) {
            return;
        }
        const bool again = !outputs_.insert(base).second;
        if (!add_generated(dotted({base, "mode"}), "int64",
                           TagOrigin::GENERATED, cause) &&
            again) {
            return;
        }
        add_generated(dotted({base, "status"}), "int64", TagOrigin::GENERATED,
                      cause);
        // A copy: the map grows below (M8 of the review).
        const std::optional<TagInfo> target =
            tags_.find(reader.text_of(output, "target"));
        if (target && !target->value_type.empty()) {
            add_generated(dotted({base, "req_manual"}), target->value_type,
                          TagOrigin::GENERATED, cause);
            add_generated(dotted({base, "req_auto"}), target->value_type,
                          TagOrigin::GENERATED, cause);
        }
        if (reader.scalar(output, "refresh")) {
            add_generated(dotted({base, "refresh"}), "int64",
                          TagOrigin::GENERATED, cause);
            refresh_device(target, cause);
        }
        if (has_failsafe_action(reader, output, target)) {
            add_generated(dotted({base, "failsafe"}), "int64",
                          TagOrigin::GENERATED, cause);
        }
        release();
    }

    // The device a refreshed output writes gets `refresh_failures`.
    void refresh_device(const std::optional<TagInfo>& target,
                        const Cause& cause) {
        if (!target || target->device.empty()) {
            return;
        }
        const auto device = tags_.devices_.find(target->device);
        if (device == tags_.devices_.end() || device->second.refreshed) {
            return;
        }
        device->second.refreshed = true;
        const std::string name =
            dotted({"sys.dev", target->device, REFRESH_FAILURES});
        // A stored tag of this name came first (item 4 of the delta review).
        if (const auto stored = tags_.tags_.find(name);
            stored != tags_.tags_.end()) {
            report(cause, CF_TAG_TWICE, twice(name, place_of(stored->second)));
            return;
        }
        if (count(1, cause)) {
            made(name);
        }
    }

    // True if a fail-safe key of `output`, given or by its default (7.2,
    // 7.4), is `safe` or `release` (12.5). A local output (`io`) shuts down
    // to its safe state by default; a `step`, `pulse` or `trigger` never
    // has a fail-safe tag (D-116).
    [[nodiscard]] bool has_failsafe_action(
        const Reader& reader, NodeId output,
        const std::optional<TagInfo>& target) const {
        const auto acts = [](std::string_view action) {
            return action == "safe" || action == "release";
        };
        // A `step`, `pulse` or `trigger` has no safe action (7.2, CF-719).
        if (const std::string_view kind = reader.text_of(output, "kind");
            kind == "step" || kind == "pulse" || kind == "trigger") {
            return false;
        }
        const bool local = target && target->protocol == "io";
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
    [[nodiscard]] bool remote_switching(
        const Reader& reader, NodeId output,
        const std::optional<TagInfo>& target) const {
        if (!target || target->protocol == "io" || target->point == 0) {
            return false;
        }
        const std::string_view kind = reader.text_of(output, "kind");
        const std::string_view safe = reader.text_of(output, "safe_state");
        return (kind == "single" || kind == "double") &&
               (safe == "off" || safe == "on") &&
               expanded_.text_of(target->point, "dangerous") != "true";
    }

    const ExpandedTree* tree_;
    Reader expanded_;
    std::span<const CheckedFile> files_;
    Reporter reporter_;
    TagNames tags_;
    bool stopped_ = false;
    std::set<std::string_view> outputs_;
    std::multimap<std::string, Waiting, std::less<>> waiting_;
    std::vector<std::string> ready_;
};

// --- References ------------------------------------------------------------

namespace {

class Referee {
public:
    Referee(const TagNames& tags, const ExpandedTree& tree,
            Diagnostics& diagnostics)
        : tags_(&tags),
          tree_(&tree),
          expanded_(tree.document),
          reporter_(tree, diagnostics) {}

    void check_tree() {
        for (const NodeId channel :
             expanded_.items(expanded_.document().root(), "channels")) {
            channel_ = {};
            groups_ = nullptr;
            if (const std::optional<NodeId> written =
                    expanded_.child(channel, "channel")) {
                channel_ = expanded_.text_of(*written, "name");
                groups_ = tags_->poll_groups_of(channel_);
            }
            for (const NodeId device : expanded_.items(channel, "devices")) {
                check_device(device);
            }
        }
    }

    void check_file(const CheckedFile& file) {
        types_ = file.types;
        const Reader reader(*file.document);
        const NodeId root = file.document->root();
        switch (file.source->path_class.type) {
            case FileType::OUTPUTS:
                for (const NodeId output : reader.items(root, "outputs")) {
                    check_output(reader, output);
                }
                break;
            case FileType::AREAS:
                for (const Entry& entry : reader.entries(root, "areas")) {
                    if (const std::optional<NodeId> authority =
                            reader.child(entry.value, "authority")) {
                        if (const std::optional<NodeId> panel =
                                reader.child(*authority, "local_panel")) {
                            need_tag(reader, reader.scalar(*panel, "input"));
                        }
                    }
                }
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

    void flush() { reporter_.flush(); }

private:
    // --- Tags ------------------------------------------------------------

    // An error at `node` of `reader`: of the expanded tree, at its
    // instance, with the point, grouped.
    void report(const Reader& reader, std::uint16_t code, NodeId node,
                std::string message) {
        const bool expanded = &reader.document() == &tree_->document;
        reporter_.error(reader.document(), node, code, std::move(message),
                        expanded ? detail_ : std::string_view(),
                        expanded ? device_ : std::string_view());
    }

    // CF-310 when the tag at `node` of `reader` does not exist; the tag
    // otherwise.
    std::optional<TagInfo> need_tag(const Reader& reader,
                                    std::optional<NodeId> node) {
        if (!node) {
            return std::nullopt;
        }
        const std::string_view name = reader.document().text(*node);
        if (std::optional<TagInfo> tag = tags_->find(name)) {
            return tag;
        }
        report(reader, CF_NO_SUCH, *node,
               "tag " + quote_text(name) + " does not exist");
        return std::nullopt;
    }

    // A tag reference that must name a number (D-116): CF-312 otherwise.
    void need_number(const Reader& reader, std::optional<NodeId> node) {
        if (!node) {
            return;
        }
        const std::optional<TagInfo> tag = need_tag(reader, node);
        if (tag && !tag->value_type.empty() &&
            !is_number_type(tag->value_type)) {
            report(reader, CF_WRONG_TAG_TYPE, *node,
                   quote_text(reader.document().text(*node)) +
                       " must be a number tag");
        }
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

    // An alarm's `tag` (M4 of the review), wherever it stands, `limit`
    // (when a tag) and `setpoint` (8): numbers. `tagless` for an alarm of
    // a `tag: false` point, which has no tag unless it names one (D-116).
    void check_alarm(const Reader& reader, NodeId alarm, bool tagless = false) {
        if (reader.text_of(alarm, "type") != "expression") {
            if (const std::optional<NodeId> tag = reader.scalar(alarm, "tag")) {
                need_tag(reader, tag);
            } else if (tagless) {
                report(reader, CF_NO_SUCH, alarm,
                       "tag " +
                           quote_text(std::string(device_) + "." +
                                      std::string(point_)) +
                           " does not exist");
            }
        }
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

    // A program's sources (9.2): `{ tag, max_age }`, or a scalar that is a
    // tag unless it is a literal or a pin `<block id>.<pin>` (M5 of the
    // review); and the tags its `out` writes.
    void check_program(const Reader& reader, NodeId root) {
        std::set<std::string_view, std::less<>> blocks;
        for (const NodeId block : reader.items(root, "blocks")) {
            blocks.insert(reader.text_of(block, "id"));
        }
        for (const NodeId block : reader.items(root, "blocks")) {
            for (const Entry& entry : reader.entries(block, "in")) {
                if (reader.document().kind(entry.value) == NodeKind::SCALAR) {
                    const std::string_view source =
                        reader.document().text(entry.value);
                    if (!is_literal(source) &&
                        !blocks.contains(source.substr(0, source.find('.')))) {
                        need_tag(reader, entry.value);
                    }
                } else {
                    need_tag(reader, reader.scalar(entry.value, "tag"));
                }
            }
            for (const Entry& entry : reader.entries(block, "out")) {
                if (reader.document().kind(entry.value) == NodeKind::SCALAR) {
                    need_tag(reader, entry.value);
                }
            }
        }
    }

    // A literal source of a block (9.2): a boolean, a number or a
    // duration.
    static bool is_literal(std::string_view text) {
        return parse_boolean(text) || parse_number(text) ||
               parse_duration_ms(text);
    }

    // --- Within a device --------------------------------------------------

    void check_device(NodeId device) {
        const std::optional<NodeId> name = expanded_.scalar(device, "name");
        if (!name) {
            return;
        }
        device_ = expanded_.document().text(*name);
        if (tags_->device_at(device_) != *name) {
            return;  // a device given twice (S1 of the review)
        }
        points_ = tags_->points_of(device_);
        detail_.clear();
        point_ = {};
        for (const NodeId rule : expanded_.items(device, "health")) {
            need_point(expanded_.scalar(rule, "point"));
            for (const NodeId entry : expanded_.items(rule, "applies_to")) {
                need_points(entry);
            }
        }
        for (const std::string_view key : {"restart_indication", "identify"}) {
            for (const NodeId rule : expanded_.items(device, key)) {
                need_point(expanded_.scalar(rule, "point"));
            }
        }
        for (const NodeId point : expanded_.items(device, "points")) {
            point_ = expanded_.text_of(point, "name");
            detail_ = "point " + std::string(point_);
            check_point(point);
        }
        detail_.clear();
        point_ = {};
    }

    void check_point(NodeId point) {
        if (const std::optional<NodeId> exponent =
                expanded_.child(point, "exponent_from")) {
            need_point(expanded_.scalar(*exponent, "point"));
        }
        // A quality rule may read the point itself (4.7: `self`).
        for (const NodeId rule : expanded_.items(point, "quality_from")) {
            if (expanded_.text_of(rule, "point") != "self") {
                need_point(expanded_.scalar(rule, "point"));
            }
        }
        need_point(expanded_.scalar(point, "source_time_from"));
        check_poll_group(point);
        const bool has_tag = expanded_.text_of(point, "tag") != "false";
        for (const NodeId alarm : expanded_.items(point, "alarms")) {
            check_alarm(expanded_, alarm, !has_tag);
        }
        for (const Entry& entry : expanded_.entries(point, "bits")) {
            for (const NodeId alarm : expanded_.items(entry.value, "alarms")) {
                check_alarm(expanded_, alarm);
            }
        }
    }

    // CF-604 for a point's poll group, given or the default `normal`, that
    // its channel does not have (M6 of the review): at the poll group, or
    // at the point's name for the default. A `wo` point is never read
    // (4.7) and has no default group; a group given to it is checked as
    // any other, and refused by CF-614 (slice 7d).
    void check_poll_group(NodeId point) {
        if (groups_ == nullptr) {
            return;
        }
        const std::optional<NodeId> given =
            expanded_.scalar(point, "poll_group");
        if (!given && expanded_.text_of(point, "access") == "wo") {
            return;
        }
        const std::optional<NodeId> site =
            given ? given : expanded_.scalar(point, "name");
        const std::string_view group =
            given ? expanded_.document().text(*given) : DEFAULT_POLL_GROUP;
        if (site && !std::ranges::binary_search(*groups_, group)) {
            report(expanded_, CF_NO_POLL_GROUP, *site,
                   "poll group " + quote_text(group) +
                       " does not exist on channel " + quote_text(channel_));
        }
    }

    // CF-310 when `node` names no point of the device.
    void need_point(std::optional<NodeId> node) {
        if (!node || points_ == nullptr) {
            return;
        }
        const std::string_view name = expanded_.document().text(*node);
        if (!std::ranges::binary_search(*points_, name)) {
            report(expanded_, CF_NO_SUCH, *node,
                   "point " + quote_text(name) + " does not exist");
        }
    }

    // An `applies_to` entry (4.9, S4 of the review): a point's name, or a
    // pattern that matches one (CF-311).
    void need_points(NodeId entry) {
        if (expanded_.document().kind(entry) != NodeKind::SCALAR ||
            points_ == nullptr) {
            return;
        }
        const std::string_view text = expanded_.document().text(entry);
        if (!is_name_pattern(text)) {
            need_point(entry);
            return;
        }
        if (pattern_work_ + points_->size() > MAX_PATTERN_WORK) {
            return;  // bounded (D-116)
        }
        pattern_work_ += points_->size();
        if (std::ranges::none_of(*points_, [text](std::string_view point) {
                return matches_name_pattern(text, point);
            })) {
            report(expanded_, CF_NO_MATCH, entry,
                   "pattern " + quote_text(text) + " matches no tag");
        }
    }

    const TagNames* tags_;
    const ExpandedTree* tree_;
    Reader expanded_;
    Reporter reporter_;
    const ScalarTypes* types_ = nullptr;
    std::string_view channel_;
    const std::vector<std::string_view>* groups_ = nullptr;
    std::string_view device_;
    const std::vector<std::string_view>* points_ = nullptr;
    std::string_view point_;
    std::string detail_;
    std::size_t pattern_work_ = 0;
};

}  // namespace

// --- TagNames ----------------------------------------------------------------

std::optional<TagInfo> TagNames::find(std::string_view name) const {
    if (const auto found = tags_.find(name); found != tags_.end()) {
        return found->second;
    }
    if (name.starts_with(DEVICE_PREFIX)) {
        return device_tag(name);
    }
    if (name.starts_with(CHANNEL_PREFIX)) {
        return channel_tag(name);
    }
    return std::nullopt;
}

std::optional<TagInfo> TagNames::device_tag(std::string_view name) const {
    const std::string_view rest = name.substr(DEVICE_PREFIX.size());
    const std::size_t dot = rest.rfind('.');
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    const auto device = devices_.find(rest.substr(0, dot));
    if (device == devices_.end()) {
        return std::nullopt;
    }
    const std::string_view signal = rest.substr(dot + 1);
    const auto tag = [&](std::string_view type) {
        return TagInfo{.value_type = type,
                       .origin = TagOrigin::GENERATED,
                       .document = expanded_,
                       .node = device->second.name,
                       .point = 0,
                       .device = {},
                       .protocol = {}};
    };
    for (const Signal& known : DEVICE_SIGNALS) {
        if (known.name == signal &&
            (signal != REFRESH_FAILURES || device->second.refreshed)) {
            return tag(known.type);
        }
    }
    if (device->second.identify) {
        for (const Signal& known : IDENTITY_SIGNALS) {
            if (known.name == signal) {
                return tag(known.type);
            }
        }
    }
    return std::nullopt;
}

std::optional<TagInfo> TagNames::channel_tag(std::string_view name) const {
    const std::string_view rest = name.substr(CHANNEL_PREFIX.size());
    const std::size_t dot = rest.rfind('.');
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view head = rest.substr(0, dot);
    const std::string_view last = rest.substr(dot + 1);
    const auto tag = [&](const Channel& channel, std::string_view type) {
        return TagInfo{.value_type = type,
                       .origin = TagOrigin::GENERATED,
                       .document = expanded_,
                       .node = channel.name,
                       .point = 0,
                       .device = {},
                       .protocol = {}};
    };
    if (const auto channel = channels_.find(head); channel != channels_.end()) {
        for (const Signal& known : CHANNEL_SIGNALS) {
            if (known.name == last) {
                return tag(channel->second, known.type);
            }
        }
    }
    if (head.ends_with(PERIOD_SEGMENT)) {
        const auto channel =
            channels_.find(head.substr(0, head.size() - PERIOD_SEGMENT.size()));
        if (channel != channels_.end() &&
            std::ranges::binary_search(channel->second.groups, last)) {
            return tag(channel->second, PERIOD_TYPE);
        }
    }
    return std::nullopt;
}

const std::vector<std::string_view>* TagNames::points_of(
    std::string_view device) const {
    const auto found = devices_.find(device);
    return found == devices_.end() ? nullptr : &found->second.points;
}

const std::vector<std::string_view>* TagNames::poll_groups_of(
    std::string_view channel) const {
    const auto found = channels_.find(channel);
    return found == channels_.end() ? nullptr : &found->second.groups;
}

NodeId TagNames::device_at(std::string_view device) const {
    const auto found = devices_.find(device);
    return found == devices_.end() ? 0 : found->second.name;
}

void TagNames::set_value_type(std::string_view name, std::string value_type) {
    if (const auto found = tags_.find(name); found != tags_.end()) {
        found->second.value_type = std::move(value_type);
    }
}

void TagNames::each_with_prefix(
    std::string_view prefix,
    const std::function<void(std::string_view, const TagInfo&)>& visit) const {
    for (auto entry = tags_.lower_bound(prefix);
         entry != tags_.end() && entry->first.starts_with(prefix); ++entry) {
        visit(entry->first, entry->second);
    }
}

TagNames collect_tags(const ExpandedTree& tree,
                      std::span<const CheckedFile> files,
                      Diagnostics& diagnostics) {
    return TagCollector(tree, files, diagnostics).run();
}

void check_tag_references(const TagNames& tags, const ExpandedTree& tree,
                          std::span<const CheckedFile> files,
                          Diagnostics& diagnostics) {
    if (!tags.complete()) {
        return;
    }
    Referee referee(tags, tree, diagnostics);
    referee.check_tree();
    for (const CheckedFile& file : files) {
        referee.check_file(file);
    }
    referee.flush();
}

}  // namespace scada::conf
