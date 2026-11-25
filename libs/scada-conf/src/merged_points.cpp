// Stage 5 on merged points (decision D-115). Each check reads the point as
// expansion left it, so a key a template gave and one an override gave count
// alike; every error is placed by `expanded_diagnostic`, at the instance for
// a key a template wrote.

#include "merged_points.h"

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/node.h"
#include "scada/conf/references.h"
#include "scada/conf/scalar.h"
#include "scada/conf/schema.h"
#include "scada/conf/tree.h"
#include "scada/conf/types.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::conf {

namespace {

constexpr std::uint16_t CF_MISSING_KEY = 202;
constexpr std::uint16_t CF_NO_SUCH = 310;
constexpr std::uint16_t CF_VALUE_TYPE = 504;
constexpr std::uint16_t CF_SCALING_RANGE = 516;
constexpr std::uint16_t CF_INTEGER_SCALING = 517;

constexpr std::uint64_t INT64_MAGNITUDE = std::uint64_t{1} << 63;
constexpr std::uint64_t UINT64_TOP = ~std::uint64_t{0};
constexpr std::uint64_t MAX_BIT_WIDTH = 64;
constexpr std::uint64_t MOD10K_BASE = 10000;

// An integer as a sign and a magnitude of up to 64 bits, enough for every
// raw value; `overflow` is set when a result needs more, which no value type
// holds.
struct Wide {
    bool negative = false;
    std::uint64_t magnitude = 0;
    bool overflow = false;
};

Wide wide(std::int64_t value) noexcept {
    if (value >= 0) {
        return Wide{.negative = false,
                    .magnitude = static_cast<std::uint64_t>(value),
                    .overflow = false};
    }
    // -(value + 1) + 1 avoids negating INT64_MIN.
    return Wide{.negative = true,
                .magnitude = static_cast<std::uint64_t>(-(value + 1)) + 1,
                .overflow = false};
}

Wide times(const Wide& lhs, const Wide& rhs) noexcept {
    Wide out{.negative = lhs.negative != rhs.negative,
             .magnitude = 0,
             .overflow = lhs.overflow || rhs.overflow};
    if (lhs.magnitude != 0 && rhs.magnitude > UINT64_TOP / lhs.magnitude) {
        out.overflow = true;
        return out;
    }
    out.magnitude = lhs.magnitude * rhs.magnitude;
    out.negative = out.negative && out.magnitude != 0;
    return out;
}

Wide plus(const Wide& lhs, const Wide& rhs) noexcept {
    Wide out{.negative = lhs.negative,
             .magnitude = 0,
             .overflow = lhs.overflow || rhs.overflow};
    if (lhs.negative == rhs.negative) {
        if (rhs.magnitude > UINT64_TOP - lhs.magnitude) {
            out.overflow = true;
            return out;
        }
        out.magnitude = lhs.magnitude + rhs.magnitude;
    } else if (lhs.magnitude >= rhs.magnitude) {
        out.magnitude = lhs.magnitude - rhs.magnitude;
    } else {
        out.negative = rhs.negative;
        out.magnitude = rhs.magnitude - lhs.magnitude;
    }
    out.negative = out.negative && out.magnitude != 0;
    return out;
}

bool fits(const Wide& value, std::string_view type) noexcept {
    if (value.overflow) {
        return false;
    }
    if (type == "uint64") {
        return !value.negative;
    }
    return value.negative ? value.magnitude <= INT64_MAGNITUDE
                          : value.magnitude < INT64_MAGNITUDE;
}

// The lowest and highest raw value of an integer raw type; nothing for
// another type.
std::optional<std::pair<Wide, Wide>> raw_range(std::string_view raw,
                                               std::uint64_t mod10k_count) {
    struct Range {
        std::string_view name;
        std::int64_t low;
        std::uint64_t high;
    };
    static constexpr std::array<Range, 9> RANGES = {{
        {"bool", 0, 1},
        {"int8", -128, 127},
        {"uint8", 0, 255},
        {"int16", -32768, 32767},
        {"uint16", 0, 65535},
        {"int32", -2147483648, 2147483647},
        {"uint32", 0, 4294967295},
        {"int64", std::numeric_limits<std::int64_t>::min(),
         std::numeric_limits<std::int64_t>::max()},
        {"uint64", 0, UINT64_TOP},
    }};
    for (const Range& range : RANGES) {
        if (range.name == raw) {
            return std::pair{wide(range.low), Wide{.negative = false,
                                                   .magnitude = range.high,
                                                   .overflow = false}};
        }
    }
    if (raw == "mod10k") {
        std::uint64_t high = 1;
        for (std::uint64_t i = 0; i < mod10k_count; ++i) {
            high *= MOD10K_BASE;
        }
        return std::pair{
            wide(0),
            Wide{.negative = false, .magnitude = high - 1, .overflow = false}};
    }
    return std::nullopt;
}

bool is_signed(std::string_view raw) noexcept {
    return raw.starts_with("int");
}

class Checker {
public:
    Checker(const ExpandedTree& tree, std::span<const CheckedFile> files,
            Diagnostics& diagnostics)
        : tree_(&tree),
          document_(&tree.document),
          diagnostics_(&diagnostics),
          computed_(tree.computed) {
        std::ranges::sort(computed_);
        for (const CheckedFile& file : files) {
            if (file.source->path_class.type != FileType::ENUMS) {
                continue;
            }
            const Document& enums = *file.document;
            const std::optional<NodeId> map =
                enums.kind(enums.root()) == NodeKind::MAP
                    ? enums.find(enums.root(), "enums")
                    : std::nullopt;
            if (map && enums.kind(*map) == NodeKind::MAP) {
                for (const Entry& entry : enums.entries(*map)) {
                    global_enums_.emplace(enums.text(entry.key));
                }
            }
        }
    }

    // Fails with INTERNAL when a computed value's key has no rule in its
    // protocol's schema, a defect of this software.
    [[nodiscard]] Status run() {
        const NodeId channels =
            child(document_->root(), "channels").value_or(0);
        for (const NodeId channel : document_->items(channels)) {
            protocol_ = text_of(channel, "protocol");
            backend_.clear();
            if (const std::optional<NodeId> written =
                    child(channel, "channel")) {
                backend_ = text_of(*written, "backend");
            }
            const std::optional<NodeId> devices = child(channel, "devices");
            if (!devices) {
                continue;
            }
            for (const NodeId device : document_->items(*devices)) {
                check_device(device);
                if (!status_.ok()) {
                    return status_;
                }
            }
        }
        return status_;
    }

private:
    // --- Reading the tree ---------------------------------------------------

    [[nodiscard]] std::optional<NodeId> child(NodeId map,
                                              std::string_view key) const {
        if (document_->kind(map) != NodeKind::MAP) {
            return std::nullopt;
        }
        return document_->find(map, key);
    }

    [[nodiscard]] std::optional<NodeId> child_map(NodeId map,
                                                  std::string_view key) const {
        const std::optional<NodeId> node = child(map, key);
        if (!node || document_->kind(*node) != NodeKind::MAP) {
            return std::nullopt;
        }
        return node;
    }

    [[nodiscard]] std::string text_of(NodeId map, std::string_view key) const {
        const std::optional<NodeId> node = child(map, key);
        if (!node || document_->kind(*node) != NodeKind::SCALAR) {
            return {};
        }
        return std::string(document_->text(*node));
    }

    void error(std::uint16_t code, NodeId node, std::string message) {
        diagnostics_->add(expanded_diagnostic(
            *tree_, code, node, std::move(message), "point " + point_name_));
    }

    // CF-202 for each of `keys` that `map` lacks.
    void need(NodeId map, std::string_view object,
              std::initializer_list<std::string_view> keys) {
        for (const std::string_view key : keys) {
            if (!child(map, key)) {
                error(CF_MISSING_KEY, map,
                      std::string(object) + " of " + quote_text(tag_) +
                          " needs " + std::string(key));
            }
        }
    }

    // --- A device and its points -------------------------------------------

    void check_device(NodeId device) {
        const std::string name = text_of(device, "name");
        device_enums_.clear();
        if (const std::optional<NodeId> enums = child_map(device, "enums")) {
            for (const Entry& entry : document_->entries(*enums)) {
                device_enums_.emplace(document_->text(entry.key));
            }
        }
        if (const std::optional<NodeId> time =
                child_map(device, "device_time")) {
            tag_ = name;
            point_name_.clear();
            check_device_time(*time);
        }
        const std::optional<NodeId> points = child(device, "points");
        if (!points) {
            return;
        }
        for (const NodeId point : document_->items(*points)) {
            if (document_->kind(point) != NodeKind::MAP) {
                continue;
            }
            point_name_ = text_of(point, "name");
            tag_ = name + "." + point_name_;
            check_point(point);
        }
        point_name_.clear();
        check_computed(device);
    }

    // --- CF-2xx: the values computed at expansion --------------------------

    // Each value computed from a configuration expression is checked
    // against its key's rules in the protocol's schema, as stage 3 checks a
    // value written there (D-115): `register: "base + 70000"` is the CF-204
    // that `register: 70000` is. The key is named from its point.
    void check_computed(NodeId device) {
        if (computed_.empty()) {
            return;
        }
        std::vector<std::string> steps;
        const std::string schema = protocol_ + ".schema.json#/$defs/device";
        for (const Entry& entry : document_->entries(device)) {
            const std::string_view key = document_->text(entry.key);
            steps.assign(1, std::string(key));
            if (key != "points") {
                walk_computed(schema, entry.value, steps, std::string(key));
                continue;
            }
            steps.emplace_back("[]");
            for (const NodeId point : document_->items(entry.value)) {
                point_name_ = text_of(point, "name");
                if (document_->kind(point) != NodeKind::MAP) {
                    continue;
                }
                for (const Entry& member : document_->entries(point)) {
                    const std::string_view name = document_->text(member.key);
                    steps.resize(2);
                    steps.emplace_back(name);
                    walk_computed(schema, member.value, steps,
                                  std::string(name));
                }
            }
            point_name_.clear();
        }
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by the nesting limit of 32
    void walk_computed(const std::string& schema, NodeId node,
                       std::vector<std::string>& steps,
                       const std::string& label) {
        if (!status_.ok()) {
            return;
        }
        switch (document_->kind(node)) {
            case NodeKind::SCALAR:
                if (std::ranges::binary_search(computed_, node)) {
                    check_value(schema, node, steps, label);
                }
                return;
            case NodeKind::MAP:
                for (const Entry& entry : document_->entries(node)) {
                    const std::string_view key = document_->text(entry.key);
                    steps.emplace_back(key);
                    walk_computed(schema, entry.value, steps,
                                  label + "." + std::string(key));
                    steps.pop_back();
                }
                return;
            case NodeKind::SEQUENCE: {
                std::size_t index = 0;
                steps.emplace_back("[]");
                for (const NodeId item : document_->items(node)) {
                    walk_computed(schema, item, steps,
                                  label + "[" + std::to_string(index) + "]");
                    ++index;
                }
                steps.pop_back();
                return;
            }
        }
    }

    void check_value(const std::string& schema, NodeId node,
                     const std::vector<std::string>& steps,
                     const std::string& label) {
        Result<std::vector<ValueError>> errors =
            SchemaSet::builtin().check_value(schema, steps,
                                             document_->text(node), label);
        if (!errors.ok()) {
            status_ = errors.status();
            return;
        }
        for (ValueError& found : errors.value()) {
            diagnostics_->add(expanded_diagnostic(
                *tree_, found.code, node, std::move(found.message),
                point_name_.empty() ? std::string() : "point " + point_name_));
        }
    }

    void check_point(NodeId point) {
        check_completeness(point);
        check_enumeration(point);
        const std::optional<NodeId> address = child_map(point, "address");
        if (protocol_ != "modbus" || !address) {
            return;
        }
        const std::string raw = raw_type_of(*address);
        const std::string value_type = value_type_of(point, raw);
        check_value_type(point, raw);
        if (value_type != "int64" && value_type != "uint64") {
            return;
        }
        if (check_integer_scaling(point, value_type)) {
            check_scaling_range(point, *address, raw, value_type);
        }
    }

    // An enumeration named in a template resolves over the instance's
    // chain (merged into the device's `enums`), then `enums.yaml`.
    void check_enumeration(NodeId point) {
        const std::optional<NodeId> name = child(point, "enum");
        if (!name || document_->kind(*name) != NodeKind::SCALAR) {
            return;
        }
        const std::string_view text = document_->text(*name);
        if (device_enums_.contains(text) || global_enums_.contains(text)) {
            return;
        }
        const Diagnostic diagnostic = expanded_diagnostic(
            *tree_, CF_NO_SUCH, *name,
            "enumeration " + quote_text(text) + " does not exist",
            "point " + point_name_);
        // Only a name a template wrote: the instance's own names are the
        // reference check's.
        if (!diagnostic.notes.empty()) {
            diagnostics_->add(diagnostic);
        }
    }

    // --- CF-202: what the merged maps still lack ---------------------------

    void check_completeness(NodeId point) {
        if (const std::optional<NodeId> exponent =
                child_map(point, "exponent_from")) {
            need(*exponent, "exponent_from", {"point", "direction"});
        }
        if (const std::optional<NodeId> bits = child_map(point, "bits")) {
            for (const Entry& entry : document_->entries(*bits)) {
                if (document_->kind(entry.value) == NodeKind::MAP) {
                    need(entry.value, "a bit", {"name"});
                }
            }
        }
        if (const std::optional<NodeId> invalid =
                child_map(point, "on_invalid")) {
            need(*invalid, "on_invalid", {"substitute"});
        }
        if (const std::optional<NodeId> time =
                child_map(point, "device_time")) {
            check_device_time(*time);
        }
        const std::optional<NodeId> address = child_map(point, "address");
        if (!address) {
            return;
        }
        if (const std::optional<NodeId> time =
                child_map(*address, "device_time")) {
            check_device_time(*time);
        }
        if (protocol_ == "modbus") {
            check_modbus_address(point, *address);
        } else if (protocol_ == "io") {
            check_io_address(point, *address);
        }
    }

    void check_device_time(NodeId time) {
        if (text_of(time, "zone") == "local") {
            need(time, "device_time", {"tz"});
        }
    }

    // Annex A.2: `table` and `register`, unless the point is read from the
    // bits of a double point's shapes (`on`, `off`, `fault`, each with its
    // own), `register` also unless `parts`, or a write-only point that
    // writes `on` and `off`; and what the raw type needs.
    void check_modbus_address(NodeId point, NodeId address) {
        const bool shapes = child(address, "on") || child(address, "off") ||
                            child(address, "fault");
        if (!shapes) {
            need(address, "the address", {"table"});
        }
        const bool write_only = text_of(point, "access") == "wo";
        const std::optional<NodeId> write = child_map(address, "write");
        const bool switched =
            write && child(*write, "on") && child(*write, "off");
        if (!shapes && !child(address, "parts") && !(write_only && switched)) {
            need(address, "the address", {"register"});
        }
        const std::string raw = text_of(address, "type");
        if (raw == "string" || raw == "bytes") {
            need(address, "the address", {"length"});
        } else if (raw == "mod10k") {
            need(address, "the address", {"count"});
        } else if (raw == "epoch") {
            need(address, "the address", {"width", "resolution"});
        } else if (raw == "datetime") {
            need(address, "the address", {"fields"});
        }
        for (const std::string_view shape : {"on", "off", "fault"}) {
            if (const std::optional<NodeId> part = child_map(address, shape)) {
                need(*part, "the address", {"table", "register"});
            }
        }
    }

    // Annex B.2: by the channel's backend.
    void check_io_address(NodeId point, NodeId address) {
        if (backend_ == "gpio") {
            need(address, "the address", {"line", "direction"});
            const std::string access = text_of(point, "access");
            if (text_of(address, "direction") == "output" || access == "rw" ||
                access == "wo") {
                need(address, "the address", {"at_start"});
            }
        } else if (backend_ == "iio") {
            need(address, "the address", {"channel"});
        } else if (backend_ == "simulator") {
            need(address, "the address", {"profile"});
            if (const std::optional<NodeId> profile =
                    child_map(address, "profile")) {
                need(*profile, "the profile", {"type"});
            }
        }
    }

    // --- Value types ---------------------------------------------------------

    // The raw type of a Modbus address (Annex A.2): one bit is `bool`; the
    // default is `bool` for coils and discrete inputs, `uint16` otherwise.
    [[nodiscard]] std::string raw_type_of(NodeId address) const {
        if (child(address, "bit")) {
            return "bool";
        }
        std::string written = text_of(address, "type");
        if (!written.empty()) {
            return written;
        }
        const std::string table = text_of(address, "table");
        return table == "coil" || table == "discrete" ? "bool" : "uint16";
    }

    [[nodiscard]] bool is_code(NodeId point) const {
        return child(point, "enum").has_value() ||
               text_of(point, "kind") == "double_point";
    }

    [[nodiscard]] bool is_integer(NodeId node) const {
        return tree_->types.of(node) == ScalarType::INTEGER;
    }

    // The point's value type, given or derived (4.7).
    [[nodiscard]] std::string value_type_of(NodeId point,
                                            const std::string& raw) const {
        std::string given = text_of(point, "value_type");
        if (!given.empty()) {
            return given;
        }
        if (is_code(point)) {
            return "int64";
        }
        if (raw == "bool") {
            return "bool";
        }
        if (!raw_range(raw, 1)) {
            return raw == "string" || raw == "bytes" ? "string" : "float64";
        }
        if (child(point, "exponent_from")) {
            return "float64";
        }
        if (const std::optional<NodeId> scale = child_map(point, "scale")) {
            if (child(*scale, "divide_by") && !is_one(*scale, "divide_by")) {
                return "float64";
            }
            for (const std::string_view key : {"gain", "offset"}) {
                const std::optional<NodeId> factor = child(*scale, key);
                if (factor && !is_integer(*factor)) {
                    return "float64";
                }
            }
        }
        return raw == "uint64" ? "uint64" : "int64";
    }

    [[nodiscard]] bool is_one(NodeId scale, std::string_view key) const {
        const std::optional<NodeId> node = child(scale, key);
        return node && is_integer(*node) && document_->text(*node) == "1";
    }

    // CF-504 (D-134): an integer value type, or a counter, on a floating
    // raw type. An enumeration or a double point reads a code and may have
    // an integer type; a counter may not, enumeration or not.
    void check_value_type(NodeId point, const std::string& raw) {
        if (raw != "float32" && raw != "float64") {
            return;
        }
        const std::string given = text_of(point, "value_type");
        const std::optional<NodeId> given_at = child(point, "value_type");
        if (given_at && (given == "int64" || given == "uint64") &&
            !is_code(point)) {
            error(CF_VALUE_TYPE, *given_at,
                  quote_text(tag_) + ": value_type " + given + " cannot hold " +
                      raw);
        }
        std::optional<NodeId> counter = child_map(point, "counter");
        if (!counter && text_of(point, "kind") == "counter") {
            counter = child(point, "kind");
        }
        if (counter) {
            error(CF_VALUE_TYPE, *counter,
                  quote_text(tag_) + ": a counter cannot hold " + raw);
        }
    }

    // CF-517 (4.7): an integer value type takes no `exponent_from`, no
    // decimal `gain` or `offset`, no `divide_by` above 1. True if the
    // scaling is integer.
    bool check_integer_scaling(NodeId point, const std::string& type) {
        const auto refuse = [&](NodeId where) {
            error(CF_INTEGER_SCALING, where,
                  quote_text(tag_) + ": value_type " + type +
                      " needs an integer scaling; use float64");
        };
        bool integer = true;
        if (const std::optional<NodeId> exponent =
                child(point, "exponent_from")) {
            refuse(*exponent);
            integer = false;
        }
        const std::optional<NodeId> scale = child_map(point, "scale");
        if (!scale) {
            return integer;
        }
        for (const std::string_view key : {"gain", "offset"}) {
            if (const std::optional<NodeId> factor = child(*scale, key);
                factor && !is_integer(*factor)) {
                refuse(*factor);
                integer = false;
            }
        }
        if (const std::optional<NodeId> divide = child(*scale, "divide_by");
            divide && !is_one(*scale, "divide_by")) {
            refuse(*divide);
            integer = false;
        }
        return integer;
    }

    // The raw range of an address: its type's, or that of its bit field.
    [[nodiscard]] std::optional<std::pair<Wide, Wide>> range_of(
        NodeId address, const std::string& raw) const {
        const std::optional<NodeId> field = child(address, "field");
        if (field && document_->items(*field).size() == 2) {
            const std::optional<Integer> low =
                parse_integer(document_->text(document_->items(*field)[0]));
            const std::optional<Integer> high =
                parse_integer(document_->text(document_->items(*field)[1]));
            if (!low || !high || high->magnitude < low->magnitude) {
                return std::nullopt;
            }
            const std::uint64_t width = high->magnitude - low->magnitude + 1;
            if (width >= MAX_BIT_WIDTH) {
                return raw_range(is_signed(raw) ? "int64" : "uint64", 1);
            }
            const std::uint64_t span = std::uint64_t{1} << width;
            if (is_signed(raw)) {
                return std::pair{Wide{.negative = true,
                                      .magnitude = span / 2,
                                      .overflow = false},
                                 Wide{.negative = false,
                                      .magnitude = span / 2 - 1,
                                      .overflow = false}};
            }
            return std::pair{wide(0), Wide{.negative = false,
                                           .magnitude = span - 1,
                                           .overflow = false}};
        }
        const std::optional<Integer> count =
            parse_integer(text_of(address, "count"));
        return raw_range(raw, count ? count->magnitude : 1);
    }

    [[nodiscard]] Wide factor_of(NodeId scale, std::string_view key,
                                 std::int64_t absent) const {
        const std::optional<NodeId> node = child(scale, key);
        if (!node) {
            return wide(absent);
        }
        const std::optional<Integer> value =
            parse_integer(document_->text(*node));
        if (!value) {
            return wide(absent);
        }
        return Wide{.negative = value->negative && value->magnitude != 0,
                    .magnitude = value->magnitude,
                    .overflow = false};
    }

    // CF-504 or CF-516 (4.7): every raw value, times `gain`, plus `offset`,
    // must fit the integer value type. Without a scaling, a raw type the
    // value type cannot hold is CF-504.
    void check_scaling_range(NodeId point, NodeId address,
                             const std::string& raw, const std::string& type) {
        const std::optional<std::pair<Wide, Wide>> range =
            range_of(address, raw);
        if (!range) {
            return;
        }
        const std::optional<NodeId> scale = child_map(point, "scale");
        const Wide gain = scale ? factor_of(*scale, "gain", 1) : wide(1);
        const Wide offset = scale ? factor_of(*scale, "offset", 0) : wide(0);
        for (const Wide& raw_value : {range->first, range->second}) {
            if (fits(plus(times(raw_value, gain), offset), type)) {
                continue;
            }
            if (scale) {
                error(CF_SCALING_RANGE, *scale,
                      quote_text(tag_) + ": raw x gain + offset can exceed " +
                          type);
            } else {
                const std::optional<NodeId> written =
                    child(point, "value_type");
                std::string message = quote_text(tag_);
                message += ": value_type " + type;
                message += " cannot hold " + raw;
                error(CF_VALUE_TYPE, written.value_or(address),
                      std::move(message));
            }
            return;
        }
    }

    const ExpandedTree* tree_;
    const Document* document_;
    Diagnostics* diagnostics_;
    std::string protocol_;
    std::string backend_;
    std::string tag_;
    std::string point_name_;
    std::set<std::string, std::less<>> global_enums_;
    std::set<std::string, std::less<>> device_enums_;
    std::vector<NodeId> computed_;
    Status status_;
};

}  // namespace

Status check_merged_points(const ExpandedTree& tree,
                           std::span<const CheckedFile> files,
                           Diagnostics& diagnostics) {
    return Checker(tree, files, diagnostics).run();
}

}  // namespace scada::conf
