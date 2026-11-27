// Retained values, logging and alarms of the tags (configuration schemas
// contract 5.2, 6 and 8; decision D-119, slice 7c of #100).

#include "tag_rules.h"

#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/node.h"
#include "scada/conf/read.h"
#include "scada/conf/references.h"
#include "scada/conf/scalar.h"
#include "scada/conf/tree.h"
#include "scada/conf/types.h"
#include "tag_names.h"
#include "tree_reader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::conf {

namespace {

constexpr std::uint16_t CF_PERCENTAGE_WITHOUT_RANGE = 505;
constexpr std::uint16_t CF_NEEDS_RETAIN = 506;
constexpr std::uint16_t CF_STRING_RETAINED = 507;
constexpr std::uint16_t CF_AGE_NOT_ALLOWED = 508;
constexpr std::uint16_t CF_RETAIN_NOW_NOT_ALLOWED = 509;
constexpr std::uint16_t CW_AGE_NO_EFFECT = 510;
constexpr std::uint16_t CF_BAD_TIER = 801;
constexpr std::uint16_t CF_LOGGING_INCOMPLETE = 802;
constexpr std::uint16_t CF_STRING_LOGGING = 803;
constexpr std::uint16_t CF_GIVEN_TWICE = 811;
constexpr std::uint16_t CF_ALARM_ID_TWICE = 812;
constexpr std::uint16_t CF_LIMIT_ORDER = 813;
constexpr std::uint16_t CF_STRING_ALARM = 814;
constexpr std::uint16_t CF_BOOL_LIMIT_ALARM = 815;

// 8: the four limit types, which compare a number with a limit.
constexpr std::array<std::string_view, 4> LIMIT_TYPES = {"high_high", "high",
                                                         "low", "low_low"};

// 8: the keys of an alarms file that one file of the tree may give.
constexpr std::array<std::string_view, 3> ALARM_LISTS = {
    "priorities", "max_shelve", "classes"};

bool is_limit_type(std::string_view type) {
    return std::ranges::find(LIMIT_TYPES, type) != LIMIT_TYPES.end();
}

// Where a node is read: its document and the types stage 3 gave it.
struct Source {
    const Reader* reader = nullptr;
    const ScalarTypes* types = nullptr;
    bool expanded = false;
    // A point of the expanded tree: the point and its device, for the
    // template note and the grouping of D-114.
    NodeId point = 0;
    std::string_view device;
};

// A tag a user wrote: a point, a bit, or a tag of a tag file.
struct Site {
    Source source;
    std::string name;
    NodeId name_node = 0;  // where an error that has no key of its own goes
    NodeId keys = 0;       // its map; 0 for a bit given by its name only
    bool tag_file = false;
};

// An alarm on a tag (not an expression alarm).
struct AlarmRef {
    Source source;
    std::string tag;
    NodeId alarm = 0;
};

// One logging policy of logging.yaml: its settings.
struct Policy {
    const Reader* reader = nullptr;
    const ScalarTypes* types = nullptr;
    NodeId settings = 0;
};

class Checker {
public:
    Checker(const TagNames& tags, const ExpandedTree& tree,
            std::span<const CheckedFile> files, Diagnostics& diagnostics)
        : tags_(&tags),
          tree_(&tree),
          files_(files),
          expanded_(tree.document),
          reporter_(tree, diagnostics) {}

    void run() {
        readers_.reserve(files_.size());
        for (const CheckedFile& file : files_) {
            readers_.emplace_back(*file.document);
        }
        for (std::size_t i = 0; i < files_.size(); ++i) {
            if (files_[i].source->path_class.type == FileType::LOGGING) {
                read_logging(i);
            }
        }
        walk_tree();
        for (std::size_t i = 0; i < files_.size(); ++i) {
            switch (files_[i].source->path_class.type) {
                case FileType::TAGS:
                    walk_tag_file(i);
                    break;
                case FileType::ALARMS:
                    walk_alarms_file(i);
                    break;
                default:
                    break;
            }
        }
        check_alarms();
        reporter_.flush();
    }

private:
    // --- Placing errors --------------------------------------------------

    void error(const Source& source, NodeId node, std::uint16_t code,
               std::string message) {
        if (source.expanded) {
            reporter_.error(source.reader->document(), node, code,
                            std::move(message), detail(source), source.device);
            return;
        }
        reporter_.error(source.reader->document(), node, code,
                        std::move(message));
    }

    void warning(const Source& source, NodeId node, std::uint16_t code,
                 std::string message) {
        reporter_.warning(source.reader->document(), node, code,
                          std::move(message),
                          source.expanded ? detail(source) : std::string());
    }

    [[nodiscard]] std::string detail(const Source& source) const {
        return "point " + std::string(expanded_.text_of(source.point, "name"));
    }

    // The value type of the tag `name`, empty when the tags are not whole
    // or it has none yet.
    [[nodiscard]] std::string_view value_type(std::string_view name) const {
        if (!tags_->complete()) {
            return {};
        }
        const std::optional<TagInfo> info = tags_->find(name);
        return info ? info->value_type : std::string_view();
    }

    // --- logging.yaml ----------------------------------------------------

    void read_logging(std::size_t index) {
        const Reader& reader = readers_[index];
        const NodeId root = reader.document().root();
        check_tiers(reader, root);
        for (const Entry& entry : reader.entries(root, "policies")) {
            policies_.try_emplace(reader.document().text(entry.key),
                                  Policy{.reader = &reader,
                                         .types = files_[index].types,
                                         .settings = entry.value});
        }
        if (const std::optional<NodeId> name =
                reader.scalar(root, "default_policy")) {
            default_policy_ = reader.document().text(*name);
        }
    }

    // CF-801: each interval longer than the one before and a whole
    // multiple of it. An interval the schema refused is skipped.
    void check_tiers(const Reader& reader, NodeId root) {
        const Source source{.reader = &reader,
                            .types = nullptr,
                            .expanded = false,
                            .point = 0,
                            .device = {}};
        // The tier before: its interval, 0 for none or one refused.
        std::uint64_t before_ms = 0;
        std::string_view before_name;
        for (const NodeId tier : reader.items(root, "aggregate_tiers")) {
            const std::optional<NodeId> interval =
                reader.scalar(tier, "interval");
            const std::uint64_t interval_ms =
                interval ? parse_duration_ms(reader.document().text(*interval))
                               .value_or(0)
                         : 0;
            const std::string_view name = reader.text_of(tier, "name");
            if (interval && interval_ms != 0 && before_ms != 0) {
                const std::string head =
                    "aggregate tier " + quote_text(name) + ": ";
                if (interval_ms <= before_ms) {
                    error(source, *interval, CF_BAD_TIER,
                          head + "its interval must be longer than that of " +
                              quote_text(before_name));
                } else if (interval_ms % before_ms != 0) {
                    error(source, *interval, CF_BAD_TIER,
                          head +
                              "its interval must be a whole multiple of "
                              "that of " +
                              quote_text(before_name));
                }
            }
            before_ms = interval_ms;
            before_name = name;
        }
    }

    // --- The tags --------------------------------------------------------

    void walk_tree() {
        const NodeId root = expanded_.document().root();
        for (const NodeId channel : expanded_.items(root, "channels")) {
            for (const NodeId device : expanded_.items(channel, "devices")) {
                const std::optional<NodeId> name =
                    expanded_.scalar(device, "name");
                if (!name || tags_->device_at(
                                 expanded_.document().text(*name)) != *name) {
                    continue;  // a device given twice is checked once
                }
                const std::string_view device_name =
                    expanded_.document().text(*name);
                for (const NodeId point : expanded_.items(device, "points")) {
                    walk_point(device_name, point);
                }
            }
        }
    }

    void walk_point(std::string_view device, NodeId point) {
        const std::optional<NodeId> name = expanded_.scalar(point, "name");
        if (!name) {
            return;
        }
        const Source source{.reader = &expanded_,
                            .types = &tree_->types,
                            .expanded = true,
                            .point = point,
                            .device = device};
        const auto tag_of = [device](std::string_view own) {
            std::string tag(device);
            tag += '.';
            tag += own;
            return tag;
        };
        if (expanded_.text_of(point, "tag") != "false") {
            check_site(Site{.source = source,
                            .name = tag_of(expanded_.document().text(*name)),
                            .name_node = *name,
                            .keys = point,
                            .tag_file = false});
        } else {
            // No tag of its own (D-116): only alarms that name a tag.
            collect_alarms(source, point, {});
        }
        for (const Entry& entry : expanded_.entries(point, "bits")) {
            const bool by_name =
                expanded_.document().kind(entry.value) == NodeKind::SCALAR;
            const std::optional<NodeId> bit =
                by_name ? std::optional<NodeId>(entry.value)
                        : expanded_.scalar(entry.value, "name");
            if (!bit) {
                continue;
            }
            check_site(Site{.source = source,
                            .name = tag_of(expanded_.document().text(*bit)),
                            .name_node = *bit,
                            .keys = by_name ? 0 : entry.value,
                            .tag_file = false});
        }
    }

    void walk_tag_file(std::size_t index) {
        const Reader& reader = readers_[index];
        const Source source{.reader = &reader,
                            .types = files_[index].types,
                            .expanded = false,
                            .point = 0,
                            .device = {}};
        for (const NodeId tag :
             reader.items(reader.document().root(), "tags")) {
            const std::optional<NodeId> name = reader.scalar(tag, "name");
            if (!name) {
                continue;
            }
            check_site(Site{.source = source,
                            .name = std::string(reader.document().text(*name)),
                            .name_node = *name,
                            .keys = tag,
                            .tag_file = true});
        }
    }

    void check_site(const Site& site) {
        if (site.keys == 0) {
            check_logging(site, value_type(site.name));
            return;
        }
        const Reader& reader = *site.source.reader;
        if (reader.child(site.keys, "range")) {
            ranged_.insert(site.name);
        }
        const std::string_view type = value_type(site.name);
        check_retain(site, type);
        check_logging(site, type);
        collect_alarms(site.source, site.keys, site.name);
    }

    // --- Retained values (5.2) -------------------------------------------

    void check_retain(const Site& site, std::string_view type) {
        const Reader& reader = *site.source.reader;
        const std::optional<NodeId> retain_node =
            reader.scalar(site.keys, "retain");
        const bool retain =
            retain_node ? reader.document().text(*retain_node) == "true"
                        : site.tag_file &&
                              reader.text_of(site.keys, "type") == "manual";
        if (const std::optional<NodeId> now =
                reader.scalar(site.keys, "retain_now")) {
            if (reader.text_of(site.keys, "type") == "virtual") {
                error(site.source, *now, CF_RETAIN_NOW_NOT_ALLOWED,
                      "retain_now is allowed on internal and manual tags only");
            } else if (reader.document().text(*now) == "true" && !retain) {
                error(site.source, *now, CF_NEEDS_RETAIN,
                      "retain_now needs retain: true");
            }
        }
        if (const std::optional<NodeId> age =
                reader.scalar(site.keys, "retain_max_age")) {
            if (!retain) {
                error(site.source, *age, CF_NEEDS_RETAIN,
                      "retain_max_age needs retain: true");
            }
            if (reader.text_of(site.keys, "kind") == "counter") {
                error(site.source, *age, CF_AGE_NOT_ALLOWED,
                      "retain_max_age is not allowed on counter tags");
            }
            warning(site.source, *age, CW_AGE_NO_EFFECT,
                    "retain_max_age has no effect until the tag database "
                    "supports it (#89)");
        }
        if (retain && type == "string") {
            // At `retain`, or at what makes a manual tag retained.
            const NodeId retained_at =
                retain_node ? *retain_node
                            : reader.scalar(site.keys, "value_type")
                                  .value_or(site.name_node);
            error(site.source, retained_at, CF_STRING_RETAINED,
                  "string tags cannot be retained");
        }
    }

    // --- Logging (6) -----------------------------------------------------

    // The settings a tag is logged with, as far as these rules read them.
    struct Logged {
        std::string_view mode;
        const Reader* deadband_reader = nullptr;
        const ScalarTypes* deadband_types = nullptr;
        NodeId deadband = 0;
    };

    // Takes the mode and deadband of `policy`; false for a name that is no
    // policy (CF-310 of the references).
    bool take_policy(std::string_view policy, Logged& logged) const {
        const auto found = policies_.find(policy);
        if (found == policies_.end()) {
            return false;
        }
        const Policy& given = found->second;
        logged.mode = given.reader->text_of(given.settings, "mode");
        if (const std::optional<NodeId> deadband =
                given.reader->scalar(given.settings, "deadband")) {
            logged.deadband_reader = given.reader;
            logged.deadband_types = given.types;
            logged.deadband = *deadband;
        }
        return true;
    }

    // The settings `site` is logged with, from its `logging` or the default
    // policy; false when it is not logged, or names no policy.
    bool resolve_logging(const Site& site, std::optional<NodeId> logging,
                         Logged& logged) {
        const Reader& reader = *site.source.reader;
        if (!logging) {
            return !default_policy_.empty() &&
                   take_policy(default_policy_, logged);
        }
        if (reader.document().kind(*logging) == NodeKind::SCALAR) {
            const std::string_view policy = reader.document().text(*logging);
            return policy != "none" && take_policy(policy, logged);
        }
        if (reader.document().kind(*logging) != NodeKind::MAP) {
            return false;
        }
        if (const std::optional<NodeId> policy =
                reader.scalar(*logging, "policy")) {
            if (!take_policy(reader.document().text(*policy), logged)) {
                return false;
            }
        } else {
            for (const std::string_view key : {"mode", "raw_retention"}) {
                if (!reader.child(*logging, key)) {
                    error(site.source, *logging, CF_LOGGING_INCOMPLETE,
                          "logging of " + quote_text(site.name) + " needs " +
                              std::string(key));
                }
            }
        }
        if (const std::optional<NodeId> mode =
                reader.scalar(*logging, "mode")) {
            logged.mode = reader.document().text(*mode);
        }
        if (const std::optional<NodeId> deadband =
                reader.scalar(*logging, "deadband")) {
            logged.deadband_reader = &reader;
            logged.deadband_types = site.source.types;
            logged.deadband = *deadband;
        }
        return true;
    }

    void check_logging(const Site& site, std::string_view type) {
        const Reader& reader = *site.source.reader;
        const std::optional<NodeId> logging =
            site.keys == 0 ? std::nullopt : reader.child(site.keys, "logging");
        Logged logged;
        if (!resolve_logging(site, logging, logged)) {
            return;
        }
        const NodeId place = logging ? *logging : site.name_node;
        if (type == "string" && !logged.mode.empty() &&
            logged.mode != "on_change") {
            error(site.source, place, CF_STRING_LOGGING,
                  "string tag " + quote_text(site.name) +
                      " can be logged on change only");
        }
        if (logged.deadband_types != nullptr &&
            logged.deadband_types->of(logged.deadband) ==
                ScalarType::PERCENTAGE &&
            !has_range(site)) {
            // At the tag's own deadband, else at what names the policy.
            const NodeId deadband_place =
                logged.deadband_reader == &reader ? logged.deadband : place;
            error(site.source, deadband_place, CF_PERCENTAGE_WITHOUT_RANGE,
                  quote_text(site.name) + ": a percentage needs range");
        }
    }

    [[nodiscard]] static bool has_range(const Site& site) {
        return site.keys != 0 && site.source.reader->child(site.keys, "range");
    }

    // --- Alarms (8) ------------------------------------------------------

    // The alarms listed at `keys`, on their `tag`, else on `owner`.
    void collect_alarms(const Source& source, NodeId keys,
                        std::string_view owner) {
        const Reader& reader = *source.reader;
        for (const NodeId alarm : reader.items(keys, "alarms")) {
            if (reader.text_of(alarm, "type") == "expression") {
                continue;
            }
            std::string_view tag = reader.text_of(alarm, "tag");
            if (tag.empty()) {
                tag = owner;
            }
            if (!tag.empty()) {
                alarms_.push_back(AlarmRef{
                    .source = source, .tag = std::string(tag), .alarm = alarm});
            }
        }
    }

    // CF-811 for the lists given again; the alarms of the file.
    void walk_alarms_file(std::size_t index) {
        const Reader& reader = readers_[index];
        const NodeId root = reader.document().root();
        const Source source{.reader = &reader,
                            .types = files_[index].types,
                            .expanded = false,
                            .point = 0,
                            .device = {}};
        const std::string_view path = files_[index].source->path;
        for (std::size_t key = 0; key < ALARM_LISTS.size(); ++key) {
            const std::optional<NodeId> given =
                reader.child(root, ALARM_LISTS.at(key));
            if (!given) {
                continue;
            }
            if (list_files_.at(key).empty()) {
                list_files_.at(key) = path;
            } else {
                error(source, *given, CF_GIVEN_TWICE,
                      std::string(ALARM_LISTS.at(key)) + " is given in " +
                          quote_text(list_files_.at(key)) + " and " +
                          quote_text(path));
            }
        }
        collect_alarms(source, root, {});
    }

    void check_alarms() {
        // An alarm on a tag that does not exist is CF-310 of the
        // references, and nothing more.
        if (tags_->complete()) {
            std::erase_if(alarms_, [this](const AlarmRef& ref) {
                return !tags_->find(ref.tag);
            });
        }
        for (const AlarmRef& ref : alarms_) {
            check_alarm(ref);
        }
        std::vector<std::size_t> order(alarms_.size());
        std::iota(order.begin(), order.end(), 0);
        std::ranges::stable_sort(
            order, std::less<>(),
            [this](std::size_t index) -> const std::string& {
                return alarms_[index].tag;
            });
        std::size_t first = 0;
        while (first < order.size()) {
            std::size_t last = first + 1;
            while (last < order.size() &&
                   alarms_[order[last]].tag == alarms_[order[first]].tag) {
                ++last;
            }
            check_tag_alarms(std::span(order).subspan(first, last - first));
            first = last;
        }
    }

    // The rules of one alarm that read its tag: CF-505, CF-814, CF-815.
    void check_alarm(const AlarmRef& ref) {
        const Reader& reader = *ref.source.reader;
        if (const std::optional<NodeId> deadband =
                reader.scalar(ref.alarm, "deadband");
            deadband &&
            ref.source.types->of(*deadband) == ScalarType::PERCENTAGE &&
            !ranged_.contains(ref.tag)) {
            error(ref.source, *deadband, CF_PERCENTAGE_WITHOUT_RANGE,
                  quote_text(ref.tag) + ": a percentage needs range");
        }
        const std::string_view tag_type = value_type(ref.tag);
        const std::optional<NodeId> type = reader.scalar(ref.alarm, "type");
        if (tag_type == "string") {
            error(ref.source, ref.alarm, CF_STRING_ALARM,
                  "string tag " + quote_text(ref.tag) + " cannot have alarms");
        } else if (tag_type == "bool" && type &&
                   is_limit_type(reader.document().text(*type))) {
            error(ref.source, *type, CF_BOOL_LIMIT_ALARM,
                  "BOOL tag " + quote_text(ref.tag) + " cannot have a " +
                      std::string(reader.document().text(*type)) + " alarm");
        }
    }

    // A numeric limit of an alarm: its value, its node and the alarm.
    struct Limit {
        double value = 0;
        NodeId node = 0;
        std::size_t alarm = 0;  // in `alarms_`
    };

    // The limit of `ref` when it is a number; nothing for a tag reference
    // or none.
    [[nodiscard]] static std::optional<Limit> limit_of(const AlarmRef& ref,
                                                       std::size_t alarm) {
        const std::optional<NodeId> limit =
            ref.source.reader->scalar(ref.alarm, "limit");
        if (!limit) {
            return std::nullopt;
        }
        const ScalarType type = ref.source.types->of(*limit);
        if (type != ScalarType::INTEGER && type != ScalarType::DECIMAL) {
            return std::nullopt;
        }
        const Result<double> value =
            to_decimal(ref.source.reader->document(), *limit);
        if (!value.ok()) {
            return std::nullopt;
        }
        return Limit{.value = value.value(), .node = *limit, .alarm = alarm};
    }

    // The id of `ref`, its type by default.
    [[nodiscard]] static std::string_view id_of(const AlarmRef& ref) {
        const std::string_view given =
            ref.source.reader->text_of(ref.alarm, "id");
        return given.empty() ? ref.source.reader->text_of(ref.alarm, "type")
                             : given;
    }

    // The rules among the alarms of one tag, in the order they were
    // collected.
    void check_tag_alarms(std::span<const std::size_t> group) {
        check_ids(group);
        check_limits(group);
    }

    // CF-812 at each alarm after the first of an id.
    void check_ids(std::span<const std::size_t> group) {
        std::set<std::string_view> ids;
        for (const std::size_t index : group) {
            const AlarmRef& ref = alarms_[index];
            const std::string_view alarm_id = id_of(ref);
            if (!alarm_id.empty() && !ids.insert(alarm_id).second) {
                error(ref.source,
                      ref.source.reader->scalar(ref.alarm, "id")
                          .value_or(ref.alarm),
                      CF_ALARM_ID_TWICE,
                      "tag " + quote_text(ref.tag) +
                          " has two alarms with id " + quote_text(alarm_id));
            }
        }
    }

    // CF-813: a `high_high` limit is compared with the highest `high` one,
    // a `low` limit with the highest `low_low` one: one error per alarm
    // whatever the number of the others (D-119).
    void check_limits(std::span<const std::size_t> group) {
        std::optional<Limit> highest_high;
        std::optional<Limit> highest_low_low;
        for (const std::size_t index : group) {
            const AlarmRef& ref = alarms_[index];
            const std::string_view type =
                ref.source.reader->text_of(ref.alarm, "type");
            std::optional<Limit>* kept = nullptr;
            if (type == "high") {
                kept = &highest_high;
            } else if (type == "low_low") {
                kept = &highest_low_low;
            } else {
                continue;
            }
            const std::optional<Limit> limit = limit_of(ref, index);
            if (limit && (!*kept || limit->value > (*kept)->value)) {
                *kept = limit;
            }
        }
        for (const std::size_t index : group) {
            const AlarmRef& ref = alarms_[index];
            const std::string_view type =
                ref.source.reader->text_of(ref.alarm, "type");
            const std::optional<Limit>& below =
                type == "high_high" ? highest_high : highest_low_low;
            if ((type != "high_high" && type != "low") || !below) {
                continue;
            }
            const std::optional<Limit> limit = limit_of(ref, index);
            if (limit && limit->value <= below->value) {
                error(ref.source, limit->node, CF_LIMIT_ORDER,
                      "tag " + quote_text(ref.tag) + ": " +
                          quote_text(id_of(ref)) + " must be above " +
                          quote_text(id_of(alarms_[below->alarm])));
            }
        }
    }

    const TagNames* tags_;
    const ExpandedTree* tree_;
    std::span<const CheckedFile> files_;
    Reader expanded_;
    Reporter reporter_;
    std::vector<Reader> readers_;  // one per file of `files_`
    std::map<std::string_view, Policy, std::less<>> policies_;
    std::string_view default_policy_;
    std::set<std::string, std::less<>> ranged_;  // the tags with `range`
    std::vector<AlarmRef> alarms_;
    // The first alarms file of each of ALARM_LISTS.
    std::array<std::string_view, ALARM_LISTS.size()> list_files_{};
};

}  // namespace

void check_tag_rules(const TagNames& tags, const ExpandedTree& tree,
                     std::span<const CheckedFile> files,
                     Diagnostics& diagnostics) {
    Checker checker(tags, tree, files, diagnostics);
    checker.run();
}

}  // namespace scada::conf
