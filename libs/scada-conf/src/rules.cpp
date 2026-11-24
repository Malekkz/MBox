// The rules of stage 3 that a schema cannot state (configuration schemas
// contract 1.4; D-108). Each object here is one that no merge of section 4.5
// can complete: a channel, a device instance, a tag, an alarm, an output, an
// identity check, a repeat block. A map that section 4.5 merges (a point in
// `defaults` or `overrides`, an address) is never refused here for a key it
// lacks; that is stage 5, on the merged point.

#include "scada/conf/rules.h"

#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/conf/scalar.h"
#include "scada/conf/tree.h"
#include "scada/conf/types.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace scada::conf {

namespace {

constexpr std::uint16_t CF_UNKNOWN_KEY = 201;
constexpr std::uint16_t CF_MISSING_KEY = 202;
constexpr std::uint16_t CF_WRONG_TYPE = 203;
constexpr std::uint16_t CF_OUT_OF_RANGE = 204;
constexpr std::uint16_t CF_NOT_ALLOWED = 205;
constexpr std::uint16_t CF_INTEGER_SCALING = 517;

// The data bits of a Modbus RTU frame, and those of ASCII (Annex A.1).
constexpr std::uint64_t RTU_DATA_BITS = 8;
constexpr std::uint64_t ASCII_DATA_BITS = 7;

// Bit numbers within a raw value (contract 5.4).
constexpr std::uint64_t LAST_BIT = 63;

// The keys of a Modbus channel of one connection type (Annex A.1). Reserved
// keys are not listed: the schema refuses them whatever the type (CF-210).
constexpr std::array<std::string_view, 6> SERIAL_KEYS = {
    "device", "baud", "data_bits", "parity", "stop_bits", "turnaround"};
constexpr std::array<std::string_view, 3> TCP_KEYS = {"host", "port",
                                                      "connect_timeout"};
// `tcp` channels only (contract 4.6).
constexpr std::string_view TCP_ONLY = "lost_after_timeouts";

// The alarm types with a limit (contract 8).
constexpr std::array<std::string_view, 6> LIMIT_ALARMS = {
    "high_high", "high", "low", "low_low", "deviation", "rate_of_change"};

// The words of a quality map value (contract 4.7).
constexpr std::array<std::string_view, 3> QUALITY_WORDS = {"good", "uncertain",
                                                           "bad"};

// A key of one kind of a tag (contract 5.1, "Keys of one kind only").
struct KindKey {
    std::string_view key;
    std::array<std::string_view, 2> kinds;
};
constexpr std::array<KindKey, 5> TAG_KIND_KEYS = {{
    {.key = "task", .kinds = {"internal", "virtual"}},
    {.key = "expr", .kinds = {"virtual", ""}},
    {.key = "quality", .kinds = {"virtual", ""}},
    {.key = "limits", .kinds = {"manual", ""}},
    {.key = "initial", .kinds = {"manual", "internal"}},
}};

bool listed(std::span<const std::string_view> list, std::string_view name) {
    return std::ranges::find(list, name) != list.end();
}

class Rules {
public:
    Rules(const Document& document, const ScalarTypes& types,
          Diagnostics& diagnostics)
        : document_(&document), types_(&types), diagnostics_(&diagnostics) {}

    void run(FileType type, std::string_view protocol) {
        const NodeId root = document_->root();
        if (!is_map(root)) {
            return;
        }
        check_field_order(root, "");
        switch (type) {
            case FileType::TEMPLATE:
                check_template(root);
                break;
            case FileType::CHANNEL:
                check_channel_file(root, protocol);
                break;
            case FileType::TAGS:
                for_each_map(root, "tags", "tags",
                             [this](NodeId tag, const std::string& path) {
                                 check_tag(tag, path);
                             });
                break;
            case FileType::ALARMS:
                check_alarms(root, "", true);
                break;
            case FileType::OUTPUTS:
                for_each_map(root, "outputs", "outputs",
                             [this](NodeId output, const std::string& path) {
                                 check_output(output, path);
                             });
                break;
            default:
                break;
        }
    }

private:
    [[nodiscard]] bool is_map(NodeId node) const {
        return document_->kind(node) == NodeKind::MAP;
    }

    [[nodiscard]] std::optional<NodeId> find(NodeId map,
                                             std::string_view key) const {
        return document_->find(map, key);
    }

    // The value of `key` in `map` if it is a map.
    [[nodiscard]] std::optional<NodeId> find_map(NodeId map,
                                                 std::string_view key) const {
        const std::optional<NodeId> value = find(map, key);
        if (!value || !is_map(*value)) {
            return std::nullopt;
        }
        return value;
    }

    // The text of the scalar `key` of `map`, or empty.
    [[nodiscard]] std::string text_of(NodeId map, std::string_view key) const {
        const std::optional<NodeId> value = find(map, key);
        if (!value || document_->kind(*value) != NodeKind::SCALAR) {
            return {};
        }
        return std::string(document_->text(*value));
    }

    void error(std::uint16_t code, NodeId where, std::string message) const {
        diagnostics_->error(code, document_->location(where),
                            std::move(message));
    }

    static std::string child(const std::string& path, std::string_view key) {
        return path.empty() ? std::string(key) : path + "." + std::string(key);
    }

    static std::string item(const std::string& path, std::size_t index) {
        return path + "[" + std::to_string(index) + "]";
    }

    // Calls `each` for every map item of the list `key` of `owner`.
    template <typename Each>
    void for_each_map(NodeId owner, std::string_view key,
                      const std::string& path, Each each) const {
        const std::optional<NodeId> list = find(owner, key);
        if (!list || document_->kind(*list) != NodeKind::SEQUENCE) {
            return;
        }
        const std::span<const NodeId> items = document_->items(*list);
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (is_map(items[i])) {
                each(items[i], item(path, i));
            }
        }
    }

    void needs(NodeId object, const std::string& object_name,
               std::string_view key) const {
        if (!find(object, key)) {
            error(CF_MISSING_KEY, object,
                  object_name + " needs " + std::string(key));
        }
    }

    // CF-201 for `key` of `object`, which its kind does not take.
    void refuse_key(NodeId object, std::string_view key,
                    const std::string& object_name) const {
        for (const Entry& entry : document_->entries(object)) {
            if (document_->text(entry.key) == key) {
                error(CF_UNKNOWN_KEY, entry.key,
                      "unknown key " + quote_text(key) + " in " + object_name);
            }
        }
    }

    // --- Templates and channels ------------------------------------------

    void check_template(NodeId root) {
        check_point_list(root, "points", "points");
        for_each_map(root, "repeat", "repeat",
                     [this](NodeId block, const std::string& path) {
                         check_repeat(block, path);
                     });
        if (const std::optional<NodeId> defaults = find_map(root, "defaults")) {
            check_point(*defaults, "defaults", "defaults");
        }
        check_identify(root, "");
    }

    void check_repeat(NodeId block, const std::string& path) {
        if (const std::optional<NodeId> variables = find_map(block, "for")) {
            const std::size_t count = document_->entry_count(*variables);
            if (count < 1 || count > 2) {
                error(CF_OUT_OF_RANGE, *variables,
                      "the number of variables of " + child(path, "for") +
                          " must be between 1 and 2");
            }
        }
        check_point_list(block, "points", child(path, "points"));
    }

    void check_identify(NodeId owner, const std::string& path) {
        for_each_map(owner, "identify", child(path, "identify"),
                     [this](NodeId check, const std::string& name) {
                         if (!find(check, "address") && !find(check, "point")) {
                             error(CF_MISSING_KEY, check,
                                   name + " needs address or point");
                         }
                         const bool compares =
                             find(check, "equals") || find(check, "in") ||
                             find(check, "range") || find(check, "pattern");
                         if (!compares) {
                             error(
                                 CF_MISSING_KEY, check,
                                 name + " needs equals, in, range or pattern");
                         }
                     });
    }

    void check_channel_file(NodeId root, std::string_view protocol) {
        if (const std::optional<NodeId> channel = find_map(root, "channel")) {
            if (protocol == "modbus") {
                check_modbus_channel(*channel);
            } else if (protocol == "io") {
                check_io_channel(*channel);
            }
        }
        for_each_map(root, "devices", "devices",
                     [this](NodeId device, const std::string& path) {
                         check_device(device, path);
                     });
    }

    void check_modbus_channel(NodeId channel) {
        const std::string type = text_of(channel, "type");
        const std::string name = "channel (type " + type + ")";
        check_data_bits(channel, type);
        if (type == "rtu") {
            needs(channel, "channel", "device");
            for (const std::string_view key : TCP_KEYS) {
                refuse_key(channel, key, name);
            }
            refuse_key(channel, TCP_ONLY, name);
        } else if (type == "tcp" || type == "rtu_over_tcp") {
            needs(channel, "channel", "host");
            for (const std::string_view key : SERIAL_KEYS) {
                refuse_key(channel, key, name);
            }
            if (type == "rtu_over_tcp") {
                refuse_key(channel, TCP_ONLY, name);
            }
        }
    }

    // RTU frames have 8 data bits; 7 belongs to ASCII and is CF-622 at
    // stage 5 (Annex A.1), so stage 3 lets 7 pass without naming it.
    void check_data_bits(NodeId channel, const std::string& type) const {
        const std::optional<NodeId> bits = find(channel, "data_bits");
        if (!bits || types_->of(*bits) != ScalarType::INTEGER ||
            (type != "rtu" && !type.empty())) {
            return;
        }
        const std::optional<Integer> number =
            parse_integer(document_->text(*bits));
        const bool seven_or_eight = number && !number->negative &&
                                    (number->magnitude == RTU_DATA_BITS ||
                                     number->magnitude == ASCII_DATA_BITS);
        if (!seven_or_eight) {
            error(CF_NOT_ALLOWED, *bits, "channel.data_bits must be one of 8");
        }
    }

    void check_io_channel(NodeId channel) {
        const std::string backend = text_of(channel, "backend");
        if (backend == "gpio") {
            needs(channel, "channel", "chip");
        } else if (backend == "iio") {
            needs(channel, "channel", "device");
        }
    }

    void check_device(NodeId device, const std::string& path) {
        // An instance that names no template lists its points (4.5).
        if (!find(device, "template")) {
            for (const std::string_view key : {"params", "disable", "only"}) {
                refuse_key(device, key, path + " (no template)");
            }
        }
        check_point_list(device, "points", child(path, "points"));
        if (const std::optional<NodeId> defaults =
                find_map(device, "defaults")) {
            check_point(*defaults, "defaults", child(path, "defaults"));
        }
        if (const std::optional<NodeId> overrides =
                find_map(device, "overrides")) {
            for (const Entry& entry : document_->entries(*overrides)) {
                if (is_map(entry.value)) {
                    const std::string key(document_->text(entry.key));
                    check_point(entry.value, quote_text(key),
                                child(child(path, "overrides"), key));
                }
            }
        }
        check_identify(device, path);
    }

    // --- Points ------------------------------------------------------------

    void check_point_list(NodeId owner, std::string_view key,
                          const std::string& path) {
        for_each_map(owner, key, path,
                     [this](NodeId point, const std::string& name) {
                         check_point(point, point_label(point), name);
                     });
    }

    // The name a message gives a point object: its `name`, else "point".
    [[nodiscard]] std::string point_label(NodeId point) const {
        const std::optional<NodeId> name = find(point, "name");
        if (name && document_->kind(*name) == NodeKind::SCALAR) {
            return quote_text(document_->text(*name));
        }
        return "point";
    }

    void check_point(NodeId point, const std::string& name,
                     const std::string& path) {
        check_integer_scaling(point, name);
        check_bits(point, path);
        check_quality_rules(point, path);
        check_alarms(point, path, false);
    }

    // CF-517 (contract 4.7) where one point object gives both keys: a
    // `value_type` of int64 or uint64 written in it, with a scaling that is
    // not integer. A value type derived from `enum`, `kind` or the raw type,
    // and keys that merging brings together, are stage 5 (D-108).
    void check_integer_scaling(NodeId point, const std::string& name) const {
        const std::string type = text_of(point, "value_type");
        if (type != "int64" && type != "uint64") {
            return;
        }
        const auto refuse = [&](NodeId where) {
            error(CF_INTEGER_SCALING, where,
                  name + ": value_type " + type +
                      " needs an integer scaling; use float64");
        };
        if (const std::optional<NodeId> exponent =
                find(point, "exponent_from")) {
            refuse(*exponent);
        }
        const std::optional<NodeId> scale = find_map(point, "scale");
        if (!scale) {
            return;
        }
        for (const std::string_view key : {"gain", "offset"}) {
            const std::optional<NodeId> factor = find(*scale, key);
            if (factor && types_->of(*factor) == ScalarType::DECIMAL) {
                refuse(*factor);
            }
        }
        if (const std::optional<NodeId> divide = find(*scale, "divide_by")) {
            const ScalarType divisor = types_->of(*divide);
            const std::optional<Integer> number =
                parse_integer(document_->text(*divide));
            const bool above_one = divisor == ScalarType::INTEGER && number &&
                                   !number->negative && number->magnitude > 1;
            if (above_one || divisor == ScalarType::DECIMAL) {
                refuse(*divide);
            }
        }
    }

    // Bit numbers of `bits` are 0 to 63 (contract 5.4), and a bit's own
    // alarms follow the alarm rules.
    void check_bits(NodeId point, const std::string& path) {
        const std::optional<NodeId> bits = find_map(point, "bits");
        if (!bits) {
            return;
        }
        const std::string bits_path = child(path, "bits");
        for (const Entry& entry : document_->entries(*bits)) {
            const std::string key(document_->text(entry.key));
            if (types_->of(entry.key) == ScalarType::INTEGER) {
                const std::optional<Integer> number = parse_integer(key);
                if (number &&
                    (number->negative || number->magnitude > LAST_BIT)) {
                    error(CF_OUT_OF_RANGE, entry.key,
                          "key " + quote_text(key) + " in " + bits_path +
                              " must be between 0 and 63");
                }
            }
            if (is_map(entry.value)) {
                check_alarms(entry.value, child(bits_path, key), false);
            }
        }
    }

    // A quality map value that is a list holds one quality word and flags
    // (contract 4.7).
    void check_quality_rules(NodeId point, const std::string& path) const {
        for_each_map(
            point, "quality_from", child(path, "quality_from"),
            [this](NodeId rule, const std::string& rule_path) {
                const std::optional<NodeId> map = find_map(rule, "map");
                if (!map) {
                    return;
                }
                for (const Entry& entry : document_->entries(*map)) {
                    check_quality_list(entry.value,
                                       child(child(rule_path, "map"),
                                             document_->text(entry.key)));
                }
            });
    }

    void check_quality_list(NodeId value, const std::string& path) const {
        if (document_->kind(value) != NodeKind::SEQUENCE) {
            return;
        }
        std::size_t words = 0;
        for (const NodeId entry : document_->items(value)) {
            if (document_->kind(entry) == NodeKind::SCALAR &&
                listed(QUALITY_WORDS, document_->text(entry))) {
                ++words;
            }
        }
        if (words != 1) {
            error(CF_WRONG_TYPE, value,
                  path +
                      " must be good, uncertain or bad, or a list of one of "
                      "them and flags");
        }
    }

    // --- Alarms, tags and outputs ------------------------------------------

    // The alarms of `owner`; `tag_required` in alarms/*.yaml (contract 8).
    void check_alarms(NodeId owner, const std::string& path,
                      bool tag_required) const {
        for_each_map(
            owner, "alarms", child(path, "alarms"),
            [this, tag_required](NodeId alarm, const std::string& alarm_path) {
                check_alarm(alarm, alarm_path, tag_required);
            });
    }

    void check_alarm(NodeId alarm, const std::string& path,
                     bool tag_required) const {
        const std::string type = text_of(alarm, "type");
        if (type.empty()) {
            return;
        }
        const std::string name = path + " (type " + type + ")";
        if (listed(LIMIT_ALARMS, type)) {
            needs(alarm, name, "limit");
        }
        if (type == "deviation") {
            needs(alarm, name, "setpoint");
        } else if (type == "state") {
            needs(alarm, name, "alarm_on");
        } else if (type == "expression") {
            needs(alarm, name, "expr");
            needs(alarm, name, "name");
        }
        if (tag_required && type != "expression") {
            needs(alarm, name, "tag");
        }
    }

    void check_tag(NodeId tag, const std::string& path) const {
        const std::string type = text_of(tag, "type");
        if (type.empty()) {
            return;
        }
        const std::string name = path + " (type " + type + ")";
        if (type == "internal" || type == "manual") {
            needs(tag, name, "value_type");
        }
        if (type == "manual") {
            needs(tag, name, "area");
        }
        if (type == "virtual") {
            needs(tag, name, "expr");
        }
        for (const KindKey& rule : TAG_KIND_KEYS) {
            if (!listed(rule.kinds, type)) {
                refuse_key(tag, rule.key, name);
            }
        }
        check_alarms(tag, path, false);
    }

    // A `duration` belongs to a `pulse` output only, where it is CF-720
    // (stage 5): the device times a pulse (contract 7.2).
    void check_output(NodeId output, const std::string& path) const {
        const std::string kind = text_of(output, "kind");
        if (!kind.empty() && kind != "pulse") {
            refuse_key(output, "duration", path + " (kind " + kind + ")");
        }
    }

    // --- Across the items of one value -----------------------------------

    // A bit field `[lo, hi]` of two literal numbers needs lo at most hi
    // (contract 4.7, A.2): in an address, a quality rule or a health rule.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by the document's depth (32)
    void check_field_order(NodeId node, const std::string& path) const {
        if (document_->kind(node) == NodeKind::SEQUENCE) {
            const std::span<const NodeId> items = document_->items(node);
            for (std::size_t i = 0; i < items.size(); ++i) {
                check_field_order(items[i], item(path, i));
            }
            return;
        }
        if (!is_map(node)) {
            return;
        }
        for (const Entry& entry : document_->entries(node)) {
            const std::string key_path =
                child(path, document_->text(entry.key));
            if (document_->text(entry.key) == "field") {
                check_one_field(entry.value, key_path);
            }
            check_field_order(entry.value, key_path);
        }
    }

    void check_one_field(NodeId field, const std::string& path) const {
        if (document_->kind(field) != NodeKind::SEQUENCE) {
            return;
        }
        const std::span<const NodeId> bounds = document_->items(field);
        if (bounds.size() != 2 ||
            types_->of(bounds[0]) != ScalarType::INTEGER ||
            types_->of(bounds[1]) != ScalarType::INTEGER) {
            return;
        }
        const std::optional<Integer> low =
            parse_integer(document_->text(bounds[0]));
        const std::optional<Integer> high =
            parse_integer(document_->text(bounds[1]));
        if (low && high && compare(*low, *high) > 0) {
            error(CF_OUT_OF_RANGE, bounds[0],
                  item(path, 0) + " must be between 0 and " +
                      std::string(document_->text(bounds[1])));
        }
    }

    static int compare(const Integer& lhs, const Integer& rhs) {
        if (lhs.negative != rhs.negative) {
            return lhs.negative ? -1 : 1;
        }
        if (lhs.magnitude == rhs.magnitude) {
            return 0;
        }
        const bool smaller = lhs.magnitude < rhs.magnitude;
        return smaller != lhs.negative ? -1 : 1;
    }

    const Document* document_;
    const ScalarTypes* types_;
    Diagnostics* diagnostics_;
};

}  // namespace

void check_file_rules(const Document& document, const ScalarTypes& types,
                      FileType type, std::string_view protocol,
                      Diagnostics& diagnostics) {
    Rules(document, types, diagnostics).run(type, protocol);
}

}  // namespace scada::conf
