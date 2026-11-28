// The keys of the points that need no protocol's address rules
// (configuration schemas contract 4.6 to 4.9 and 5.3; decision D-120, slice
// 7d-1 of #100).

#include "point_rules.h"

#include "name_pattern.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/node.h"
#include "scada/conf/references.h"
#include "scada/conf/scalar.h"
#include "scada/conf/tree.h"
#include "tree_reader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace scada::conf {

namespace {

using detail::Reader;
using detail::Reporter;

constexpr std::uint16_t CF_MAX_AGE_BELOW_UPDATE = 611;
constexpr std::uint16_t CF_COMPANION_GROUP = 612;
constexpr std::uint16_t CF_NO_STATES = 613;
constexpr std::uint16_t CF_WRITE_ONLY_READ = 614;
constexpr std::uint16_t CF_IDENTITY_NOT_READ = 616;
constexpr std::uint16_t CF_NO_BIT_ORDER = 618;
constexpr std::uint16_t CF_NO_STORAGE = 619;
constexpr std::uint16_t CF_KEEP_LAST_NO_MAX_AGE = 640;

// 4.6: the poll groups of a channel that gives none, and their periods.
constexpr std::array<std::pair<std::string_view, std::uint64_t>, 3>
    DEFAULT_PERIODS_MS = {{{"fast", 200}, {"normal", 1000}, {"slow", 10000}}};

// 4.7: a point without `poll_group` is in `normal`.
constexpr std::string_view DEFAULT_GROUP = "normal";

// 4.7: the default `max_age` is this many poll group periods.
constexpr std::uint64_t MAX_AGE_PERIODS = 3;

// The work of matching health rules' `applies_to` patterns in one run, as
// for run-time patterns (D-117): RUN_TIME_NAME_WORK per point name compared
// and one per step of the matcher. Past it the patterns left are not
// checked (D-120).
constexpr std::uint64_t MAX_HEALTH_PATTERN_WORK = 1'000'000'000;

// The keys a `wo` point must not have (CF-614).
constexpr std::array<std::string_view, 3> READ_KEYS = {"poll_group", "alarms",
                                                       "logging"};

// A point of the device being checked.
struct Point {
    NodeId node = 0;
    bool read = true;  // not `wo`
    // Its `poll_group`, or `normal`; a `wo` point is in none, whatever this
    // says (4.7). A view into the expanded tree, or a constant.
    std::string_view group;
};

class Checker {
public:
    Checker(const ExpandedTree& tree, std::span<const CheckedFile> files,
            Diagnostics& diagnostics)
        : tree_(&tree),
          files_(files),
          expanded_(tree.document),
          reporter_(tree, diagnostics) {
        index_templates();
    }

    // `expanded_` is pointed into while it runs: not copied, not moved.
    Checker(const Checker&) = delete;
    Checker& operator=(const Checker&) = delete;
    Checker(Checker&&) = delete;
    Checker& operator=(Checker&&) = delete;
    ~Checker() = default;

    void run() {
        const NodeId root = expanded_.document().root();
        // A device given twice (CF-303) is checked once, by its name.
        std::set<std::string_view> devices;
        for (const NodeId channel : expanded_.items(root, "channels")) {
            read_channel(channel);
            for (const NodeId device : expanded_.items(channel, "devices")) {
                const std::optional<NodeId> name =
                    expanded_.scalar(device, "name");
                if (name &&
                    devices.insert(expanded_.document().text(*name)).second) {
                    check_device(device, expanded_.document().text(*name));
                }
            }
        }
        reporter_.flush();
    }

private:
    // --- Placing errors --------------------------------------------------

    // The note names the point being checked, or `detail` outside one.
    void error(NodeId node, std::uint16_t code, std::string message,
               std::string_view detail = {}) {
        reporter_.error(expanded_.document(), node, code, std::move(message),
                        point_name_.empty()
                            ? std::string(detail)
                            : "point " + std::string(point_name_),
                        device_);
    }

    [[nodiscard]] std::string_view text(NodeId node) const {
        return expanded_.document().text(node);
    }

    // --- Channels ----------------------------------------------------------

    // The protocol and the poll group periods of a channel (4.6): its own
    // groups replace the defaults (D-116).
    void read_channel(NodeId channel) {
        protocol_ = expanded_.text_of(channel, "protocol");
        periods_.clear();
        const std::optional<NodeId> keys = expanded_.child(channel, "channel");
        const std::span<const Entry> groups =
            keys ? expanded_.entries(*keys, "poll_groups")
                 : std::span<const Entry>();
        if (keys && expanded_.child(*keys, "poll_groups")) {
            for (const Entry& group : groups) {
                const std::optional<std::uint64_t> period =
                    parse_duration_ms(expanded_.text_of(group.value, "period"));
                if (period) {
                    periods_.emplace(text(group.key), *period);
                }
            }
            return;
        }
        for (const auto& [group, period] : DEFAULT_PERIODS_MS) {
            periods_.emplace(group, period);
        }
    }

    // --- Devices -----------------------------------------------------------

    void check_device(NodeId device, std::string_view name) {
        device_ = name;
        points_.clear();
        const std::span<const NodeId> points =
            expanded_.items(device, "points");
        for (const NodeId point : points) {
            const std::optional<NodeId> point_name =
                expanded_.scalar(point, "name");
            if (!point_name) {
                continue;
            }
            points_.try_emplace(
                text(*point_name),
                Point{.node = point,
                      .read = expanded_.text_of(point, "access") != "wo",
                      .group = group_of(point)});
        }
        first_read_ = {};
        first_other_group_ = {};
        for (const auto& [point_name, point] : points_) {
            if (!point.read) {
                continue;
            }
            if (first_read_.empty()) {
                first_read_ = point_name;
            } else if (point.group != points_.find(first_read_)->second.group) {
                first_other_group_ = point_name;
                break;
            }
        }
        for (const NodeId point : points) {
            const std::optional<NodeId> point_name =
                expanded_.scalar(point, "name");
            if (!point_name) {
                continue;
            }
            point_name_ = text(*point_name);
            check_point(point);
        }
        point_name_ = {};
        check_bit_order(device, points);
        check_health(device);
        check_identity(device);
    }

    // --- One point ---------------------------------------------------------

    void check_point(NodeId point) {
        const std::string_view access = expanded_.text_of(point, "access");
        const bool read = access != "wo";
        const std::optional<NodeId> access_node =
            expanded_.scalar(point, "access");
        if (access_node && (access == "rw" || access == "wo") &&
            !expanded_.child(point, "storage")) {
            error(*access_node, CF_NO_STORAGE,
                  "point " + quote_text(point_name_) +
                      " is writable and needs storage: volatile or nvm");
        }
        if (!read) {
            for (const std::string_view key : READ_KEYS) {
                if (const std::optional<NodeId> given = own_key(point, key);
                    given && !says_nothing(*given)) {
                    error(*given, CF_WRITE_ONLY_READ,
                          "point " + quote_text(point_name_) +
                              " is write-only and cannot be read");
                }
            }
        }
        // A `wo` point is never read: no value of it is invalid, stale or
        // refreshed, and it has no companions (4.7).
        if (read) {
            check_keep_last(point);
            check_max_age(point);
            check_companions(point);
        }
        // `states` maps the codes read; a `wo` double point writes its codes
        // by its write encoding (A.5).
        if (read && protocol_ == "modbus") {
            check_states(point);
        }
    }

    // The value of `key` of `point` when the point or an override wrote it;
    // a key only a `defaults` map gave is not the point's (D-120: M2 of the
    // review of #174).
    [[nodiscard]] std::optional<NodeId> own_key(NodeId point,
                                                std::string_view key) const {
        if (expanded_.document().kind(point) != NodeKind::MAP) {
            return std::nullopt;
        }
        for (const Entry& entry : expanded_.document().entries(point)) {
            if (text(entry.key) == key) {
                if (from_defaults(*tree_, entry.key)) {
                    return std::nullopt;
                }
                return entry.value;
            }
        }
        return std::nullopt;
    }

    // `none` and an empty list say "nothing": `logging: none`, `alarms: []`.
    [[nodiscard]] bool says_nothing(NodeId value) const {
        const Document& document = expanded_.document();
        return (document.kind(value) == NodeKind::SCALAR &&
                document.text(value) == "none") ||
               (document.kind(value) == NodeKind::SEQUENCE &&
                document.items(value).empty());
    }

    // CF-640: `keep_last` needs a `max_age` the point gives (D-120).
    void check_keep_last(NodeId point) {
        if (expanded_.child(point, "max_age")) {
            return;
        }
        const auto refuse = [this](NodeId site) {
            error(site, CF_KEEP_LAST_NO_MAX_AGE,
                  "point " + quote_text(point_name_) +
                      ": keep_last needs max_age");
        };
        if (const std::optional<NodeId> on_invalid =
                expanded_.scalar(point, "on_invalid");
            on_invalid && text(*on_invalid) == "keep_last") {
            refuse(*on_invalid);
        }
        for (const NodeId entry : expanded_.items(point, "invalid_values")) {
            if (const std::optional<NodeId> action =
                    expanded_.scalar(entry, "action");
                action && text(*action) == "keep_last") {
                refuse(*action);
            }
        }
    }

    // CF-611: the `max_age`, given or 3 periods of its poll group, below
    // `update_period`.
    void check_max_age(NodeId point) {
        const std::optional<NodeId> update =
            expanded_.scalar(point, "update_period");
        if (!update) {
            return;
        }
        const std::optional<std::uint64_t> update_ms =
            parse_duration_ms(text(*update));
        const std::optional<NodeId> given = expanded_.scalar(point, "max_age");
        std::optional<std::uint64_t> max_age_ms;
        if (given) {
            max_age_ms = parse_duration_ms(text(*given));
        } else if (const auto period = periods_.find(group_of(point));
                   period != periods_.end()) {
            max_age_ms = MAX_AGE_PERIODS * period->second;
        }
        if (update_ms && max_age_ms && *max_age_ms < *update_ms) {
            error(given.value_or(*update), CF_MAX_AGE_BELOW_UPDATE,
                  "point " + quote_text(point_name_) +
                      ": max_age is shorter than the device's update period");
        }
    }

    [[nodiscard]] std::string_view group_of(NodeId point) const {
        const std::string_view group = expanded_.text_of(point, "poll_group");
        return group.empty() ? DEFAULT_GROUP : group;
    }

    // CF-612: each companion of a read point is read in its poll group.
    void check_companions(NodeId point) {
        const std::string_view group = group_of(point);
        const auto need = [this, group](std::optional<NodeId> reference) {
            if (!reference) {
                return;
            }
            const std::string_view companion = text(*reference);
            const auto found = points_.find(companion);
            if (found == points_.end()) {
                return;  // CF-310 of the references (D-116)
            }
            if (!found->second.read || found->second.group != group) {
                error(*reference, CF_COMPANION_GROUP,
                      "point " + quote_text(point_name_) + ": " +
                          quote_text(companion) + " must be in poll group " +
                          quote_text(group));
            }
        };
        if (const std::optional<NodeId> exponent =
                expanded_.child(point, "exponent_from")) {
            need(expanded_.scalar(*exponent, "point"));
        }
        need(expanded_.scalar(point, "source_time_from"));
        for (const NodeId rule : expanded_.items(point, "quality_from")) {
            const std::optional<NodeId> companion =
                expanded_.scalar(rule, "point");
            if (companion && text(*companion) != "self") {
                need(companion);
            }
        }
    }

    // CF-613: a double point that is read, whose Modbus address is a field
    // or an integer (not `on` and `off`), needs `states` (5.3, A.2).
    void check_states(NodeId point) {
        const std::optional<NodeId> kind = expanded_.scalar(point, "kind");
        if (!kind || text(*kind) != "double_point" ||
            expanded_.child(point, "states")) {
            return;
        }
        // The field and integer shapes have a `field`, a `register` or
        // `parts`; an address with none of them is the two- or three-bit or
        // two-coil shape, given whole or refused by CF-202 already.
        const std::optional<NodeId> address = expanded_.child(point, "address");
        if (!address || expanded_.child(*address, "on") ||
            (!expanded_.child(*address, "field") &&
             !expanded_.child(*address, "register") &&
             !expanded_.child(*address, "parts"))) {
            return;
        }
        error(*kind, CF_NO_STATES,
              "point " + quote_text(point_name_) + ": states must be given");
    }

    // The first key of `point` that uses a bit, a field or `bits` (CF-618):
    // `bits`, a `bit` or `field` of its address or of a part of it, or of a
    // quality rule.
    [[nodiscard]] std::optional<NodeId> uses_bits(NodeId point) const {
        if (const std::optional<NodeId> bits = expanded_.child(point, "bits")) {
            return bits;
        }
        if (const std::optional<NodeId> address =
                expanded_.child(point, "address")) {
            if (const std::optional<NodeId> found = bit_of(*address)) {
                return found;
            }
            for (const std::string_view part : {"on", "off", "fault"}) {
                if (const std::optional<NodeId> sub =
                        expanded_.child(*address, part)) {
                    if (const std::optional<NodeId> found = bit_of(*sub)) {
                        return found;
                    }
                }
            }
        }
        for (const NodeId rule : expanded_.items(point, "quality_from")) {
            if (const std::optional<NodeId> found = bit_of(rule)) {
                return found;
            }
        }
        return std::nullopt;
    }

    // A `bit` or `field` of the map `map`.
    [[nodiscard]] std::optional<NodeId> bit_of(NodeId map) const {
        for (const std::string_view key : {"bit", "field"}) {
            if (const std::optional<NodeId> found = expanded_.child(map, key)) {
                return found;
            }
        }
        return std::nullopt;
    }

    // CF-618: `bit_order` wherever a bit or a field is used (4.2, 4.3, the
    // lead's ruling M3 of the review of #174): the points of the expanded
    // device, the instance's own included, a health rule's `good_when` and
    // an identity check's address. Once per device, at the first use, named
    // by the device's template, else by the device.
    void check_bit_order(NodeId device, std::span<const NodeId> points) {
        if (expanded_.scalar(device, "bit_order")) {
            return;
        }
        std::optional<NodeId> site;
        std::string detail;
        for (const NodeId point : points) {
            if (const std::optional<NodeId> bits = uses_bits(point)) {
                site = bits;
                detail =
                    "point " + std::string(expanded_.text_of(point, "name"));
                break;
            }
        }
        for (const auto& [key, part] : {std::pair{"health", "good_when"},
                                        std::pair{"identify", "address"}}) {
            for (const NodeId rule : expanded_.items(device, key)) {
                if (site) {
                    break;
                }
                if (const std::optional<NodeId> map =
                        expanded_.child(rule, part)) {
                    site = bit_of(*map);
                    detail = key;
                }
            }
        }
        if (!site) {
            return;
        }
        const std::string_view template_name = template_of(device);
        error(
            *site, CF_NO_BIT_ORDER,
            (template_name.empty() ? "device " + quote_text(device_)
                                   : "template " + quote_text(template_name)) +
                " uses bits and needs bit_order",
            detail);
    }

    // The `template` of the instance `device` was made from, as its channel
    // file gives it; empty for a device without one.
    [[nodiscard]] std::string_view template_of(NodeId device) const {
        const std::optional<NodeId> name = expanded_.scalar(device, "name");
        if (!name || *name >= tree_->instance_of.size() ||
            tree_->instance_of[*name] >= tree_->instances.size()) {
            return {};
        }
        const Location& place = tree_->instances[tree_->instance_of[*name]];
        const auto found = templates_.find(
            std::tuple{std::string_view(place.file), place.line, place.column});
        return found == templates_.end() ? std::string_view() : found->second;
    }

    // The `template` of each device of the channel files, by the place of
    // its `name` key, as the expanded tree records instances.
    void index_templates() {
        for (const CheckedFile& checked : files_) {
            if (checked.source->path_class.type != FileType::CHANNEL ||
                checked.document == nullptr) {
                continue;
            }
            const Document& document = *checked.document;
            const std::optional<NodeId> devices =
                document.find(document.root(), "devices");
            if (!devices || document.kind(*devices) != NodeKind::SEQUENCE) {
                continue;
            }
            for (const NodeId item : document.items(*devices)) {
                if (document.kind(item) != NodeKind::MAP) {
                    continue;
                }
                const std::optional<NodeId> name = document.find(item, "name");
                const std::optional<NodeId> used =
                    document.find(item, "template");
                if (!name || !used ||
                    document.kind(*used) != NodeKind::SCALAR) {
                    continue;
                }
                // A copy: `location()` returns a temporary (S3 of the
                // review of #174).
                const Location place = document.location(*name);
                templates_.emplace(
                    std::tuple{std::string_view(checked.source->path),
                               place.line, place.column},
                    document.text(*used));
            }
        }
    }

    // --- Device rules ------------------------------------------------------

    // CF-612: a health point is in the poll group of every read point it
    // applies to; one error per rule, at the first such point by name.
    void check_health(NodeId device) {
        for (const NodeId rule : expanded_.items(device, "health")) {
            const std::optional<NodeId> health =
                expanded_.scalar(rule, "point");
            if (!health) {
                continue;
            }
            const auto found = points_.find(text(*health));
            if (found == points_.end()) {
                continue;  // CF-310 of the references (D-116)
            }
            const std::optional<std::string_view> offender =
                first_unserved(rule, found->second);
            if (!offender) {
                continue;
            }
            const std::string_view group =
                points_.find(*offender)->second.group;
            point_name_ = *offender;
            error(*health, CF_COMPANION_GROUP,
                  "point " + quote_text(*offender) + ": " +
                      quote_text(text(*health)) + " must be in poll group " +
                      quote_text(group));
            point_name_ = {};
        }
    }

    // The first read point by name that health rule `rule` applies to (4.9:
    // `all`, or listed names and patterns) and that `health` does not serve:
    // `health` is not read, or is in another group. `all` is O(1); a name
    // is one look-up; the patterns of a run do at most
    // MAX_HEALTH_PATTERN_WORK of matching work in all, counted as D-117
    // counts it, and a pattern the budget runs out in, and those after it,
    // are not checked (D-120), so that their time stays bounded.
    [[nodiscard]] std::optional<std::string_view> first_unserved(
        NodeId rule, const Point& health) {
        const auto serves = [&health](const Point& point) {
            return !point.read || (health.read && point.group == health.group);
        };
        const std::optional<NodeId> applies_to =
            expanded_.child(rule, "applies_to");
        if (!applies_to) {
            return std::nullopt;
        }
        if (expanded_.document().kind(*applies_to) == NodeKind::SCALAR) {
            if (text(*applies_to) != "all" || first_read_.empty()) {
                return std::nullopt;
            }
            // The first read point, or else the first in another group.
            if (!serves(points_.find(first_read_)->second)) {
                return first_read_;
            }
            if (first_other_group_.empty()) {
                return std::nullopt;
            }
            return first_other_group_;
        }
        return first_listed(*applies_to, serves);
    }

    // As `first_unserved`, for an `applies_to` list of names and patterns.
    template <typename Serves>
    [[nodiscard]] std::optional<std::string_view> first_listed(
        NodeId applies_to, const Serves& serves) {
        std::optional<std::string_view> first;
        const auto consider = [&first](std::string_view name) {
            if (!first || name < *first) {
                first = name;
            }
        };
        for (const NodeId entry : expanded_.document().items(applies_to)) {
            if (expanded_.document().kind(entry) != NodeKind::SCALAR) {
                continue;
            }
            const std::string_view listed = text(entry);
            if (!is_name_pattern(listed)) {
                const auto point = points_.find(listed);
                if (point != points_.end() && !serves(point->second)) {
                    consider(point->first);
                }
                continue;
            }
            // In name order, the first point matched and not served.
            std::optional<std::string_view> matched;
            for (const auto& [name, point] : points_) {
                if (pattern_work_ >= MAX_HEALTH_PATTERN_WORK) {
                    matched.reset();  // bounded (D-120): not checked
                    break;
                }
                if (serves(point)) {
                    continue;
                }
                std::uint64_t steps = 0;
                const bool hit = matches_name_pattern(listed, name, steps);
                pattern_work_ += RUN_TIME_NAME_WORK + steps;
                if (hit) {
                    matched = name;
                    break;
                }
            }
            if (matched) {
                consider(*matched);
            }
        }
        return first;
    }

    // CF-616: an identity check on a point reads it.
    void check_identity(NodeId device) {
        std::size_t number = 0;
        for (const NodeId check : expanded_.items(device, "identify")) {
            ++number;
            const std::optional<NodeId> point =
                expanded_.scalar(check, "point");
            if (!point) {
                continue;
            }
            const auto found = points_.find(text(*point));
            if (found != points_.end() && !found->second.read) {
                error(*point, CF_IDENTITY_NOT_READ,
                      "identity check " + std::to_string(number) + " of " +
                          quote_text(device_) + ": point " +
                          quote_text(text(*point)) + " is not read",
                      "identify");
            }
        }
    }

    const ExpandedTree* tree_;
    // The files of the tree, which outlive the check.
    std::span<const CheckedFile> files_;
    // The `template` of each instance, by its channel file and the line and
    // column of its `name` key: views into the files.
    std::map<std::tuple<std::string_view, std::uint32_t, std::uint32_t>,
             std::string_view>
        templates_;
    Reader expanded_;
    Reporter reporter_;
    // Of the channel being checked: views into the expanded tree; the keys
    // of `periods_` are the channel's group names, or the constants of
    // DEFAULT_PERIODS_MS.
    std::string_view protocol_;
    std::map<std::string_view, std::uint64_t, std::less<>> periods_;
    // Of the device being checked: views into the expanded tree.
    std::string_view device_;
    std::string_view point_name_;
    std::map<std::string_view, Point, std::less<>> points_;
    // The first read point by name, and the first read point after it in
    // another poll group than its own; empty if none.
    std::string_view first_read_;
    std::string_view first_other_group_;
    // Of the whole run.
    std::uint64_t pattern_work_ = 0;
};

}  // namespace

void check_point_rules(const ExpandedTree& tree,
                       std::span<const CheckedFile> files,
                       Diagnostics& diagnostics) {
    Checker checker(tree, files, diagnostics);
    checker.run();
}

}  // namespace scada::conf
