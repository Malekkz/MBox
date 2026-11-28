// The Modbus addresses of the expanded tree (configuration schemas contract
// Annex A.1 to A.5 and 10.6; decision D-121, slice 7d-2 of #100).

#include "modbus_rules.h"

#include "point_types.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/node.h"
#include "scada/conf/scalar.h"
#include "tree_reader.h"

#include <algorithm>
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
#include <vector>

namespace scada::conf {

namespace {

using detail::Reader;
using detail::Reporter;

constexpr std::uint16_t CF_DEVICES_OVERLAP = 601;
constexpr std::uint16_t CF_POINTS_OVERLAP = 602;
constexpr std::uint16_t CF_BAD_ADDRESS = 603;
constexpr std::uint16_t CF_RANGE = 610;
constexpr std::uint16_t CF_FUNCTION = 615;
constexpr std::uint16_t CW_IDLE = 607;

// A.2: the last protocol address of a table.
constexpr std::uint64_t LAST_REGISTER = 65'535;

// The bounds the schema gives a register and a bit number (stage 3, and
// stage 5 for a computed value, D-115): a value past them is CF-204
// already, and not CF-603 as well.
constexpr std::uint64_t SCHEMA_LAST_REGISTER = 65'536;
constexpr std::uint64_t SCHEMA_LAST_BIT = 63;

// The bits of the widest raw value a pattern holds (2.3).
constexpr std::uint64_t VALUE_BITS = 64;

// The Modbus functions of A.1 and A.5.
constexpr unsigned FN_READ_COILS = 1;
constexpr unsigned FN_READ_DISCRETE = 2;
constexpr unsigned FN_READ_HOLDING = 3;
constexpr unsigned FN_READ_INPUT = 4;
constexpr unsigned FN_WRITE_COIL = 5;
constexpr unsigned FN_WRITE_REGISTER = 6;
constexpr unsigned FN_WRITE_COILS = 15;
constexpr unsigned FN_WRITE_REGISTERS = 16;

// A register's bits.
constexpr unsigned REGISTER_BITS = 16;
constexpr std::uint64_t REGISTER_MASK = 0xFFFF;

// 4.6: the poll groups of a channel that gives none, and their periods.
constexpr std::array<std::pair<std::string_view, std::uint64_t>, 3>
    DEFAULT_PERIODS_MS = {{{"fast", 200}, {"normal", 1000}, {"slow", 10000}}};
constexpr std::string_view DEFAULT_GROUP = "normal";

// 4.6: the default `backoff_max`.
constexpr std::uint64_t DEFAULT_BACKOFF_MAX_MS = 300'000;

// The four tables of A.2, in the order of their read functions' names.
enum class Table : std::uint8_t { COIL, DISCRETE, INPUT, HOLDING };

[[nodiscard]] std::optional<Table> table_named(std::string_view name) {
    if (name == "coil") {
        return Table::COIL;
    }
    if (name == "discrete") {
        return Table::DISCRETE;
    }
    if (name == "input") {
        return Table::INPUT;
    }
    if (name == "holding") {
        return Table::HOLDING;
    }
    return std::nullopt;
}

[[nodiscard]] std::string_view name_of(Table table) {
    switch (table) {
        case Table::COIL:
            return "coil";
        case Table::DISCRETE:
            return "discrete";
        case Table::INPUT:
            return "input";
        case Table::HOLDING:
            return "holding";
    }
    return "holding";
}

[[nodiscard]] bool is_bit_table(Table table) {
    return table == Table::COIL || table == Table::DISCRETE;
}

// A.1, A.5: the read function of a table.
[[nodiscard]] unsigned read_function(Table table) {
    switch (table) {
        case Table::COIL:
            return FN_READ_COILS;
        case Table::DISCRETE:
            return FN_READ_DISCRETE;
        case Table::INPUT:
            return FN_READ_INPUT;
        case Table::HOLDING:
            return FN_READ_HOLDING;
    }
    return FN_READ_HOLDING;
}

// The registers of a raw type (A.2), or nothing for one this cannot count.
[[nodiscard]] std::optional<std::uint64_t> registers_of_type(
    std::string_view raw) {
    for (const std::string_view one :
         {"bool", "int8", "uint8", "int16", "uint16"}) {
        if (raw == one) {
            return 1;
        }
    }
    for (const std::string_view two : {"int32", "uint32", "float32"}) {
        if (raw == two) {
            return 2;
        }
    }
    for (const std::string_view four : {"int64", "uint64", "float64"}) {
        if (raw == four) {
            return 4;
        }
    }
    return std::nullopt;
}

// One run of registers (or coils) of one table and unit that a point reads
// or writes, in protocol addresses (after `address_base`).
struct Use {
    std::uint32_t owner = 0;  // the point's index in its device
    std::uint32_t unit = 0;
    Table table = Table::HOLDING;
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    NodeId site = 0;  // where an error about it is placed
    // The bits written in each register: all of them, or `pattern` split
    // over the registers by the word order (A.5).
    bool whole = true;
    bool little = false;
    std::uint64_t pattern = 0;
    std::uint32_t group = 0;  // the poll group of a read
};

// The bits `use` claims in its `index`-th register.
[[nodiscard]] std::uint64_t mask_at(const Use& use, std::uint64_t index) {
    if (is_bit_table(use.table)) {
        return 1;
    }
    if (use.whole) {
        return REGISTER_MASK;
    }
    const std::uint64_t count = use.last - use.first + 1;
    const std::uint64_t word = use.little ? index : count - 1 - index;
    if (word >= VALUE_BITS / REGISTER_BITS) {
        return 0;
    }
    return (use.pattern >> (word * REGISTER_BITS)) & REGISTER_MASK;
}

// The uses of `uses` that claim a bit another owner's earlier use claims,
// in order of unit, table and first register: `found(later, earlier,
// register)` for each, once per pair of owners (A.3, A.5). A use that meets
// another is not kept, so that one cause is told once. `token` claims one
// bit per register for every use, for devices, which meet at any shared
// register whatever bits each writes (A.3).
// The owner (index + 1, 0 for none) of each bit of each register still
// ahead of a sweep.
using Owners =
    std::map<std::uint64_t, std::array<std::uint32_t, REGISTER_BITS>>;

// The bits `use` claims in its `index`-th register: one per register for a
// token, else those it reads or writes.
[[nodiscard]] std::uint64_t claimed_bits(const Use& use, std::uint64_t index,
                                         bool token) {
    return token ? std::uint64_t{1} : mask_at(use, index);
}

// The first earlier use of another owner whose bits `use` meets, and the
// register: in order of register, then bit.
[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint64_t>> first_met(
    const std::vector<Use>& uses, const Use& use, const Owners& owners,
    bool token) {
    for (std::uint64_t reg = use.first; reg <= use.last; ++reg) {
        const std::uint64_t mask = claimed_bits(use, reg - use.first, token);
        const auto held = owners.find(reg);
        if (mask == 0 || held == owners.end()) {
            continue;
        }
        for (unsigned bit = 0; bit < REGISTER_BITS; ++bit) {
            const std::uint32_t owner = held->second.at(bit);
            if ((mask >> bit & 1U) != 0 && owner != 0 &&
                uses[owner - 1].owner != use.owner) {
                return std::pair{owner - 1, reg};
            }
        }
    }
    return std::nullopt;
}

// Gives the bits of `use`, the `index`-th of the uses, that no one holds to
// it.
void claim(Owners& owners, const Use& use, std::size_t index, bool token) {
    for (std::uint64_t reg = use.first; reg <= use.last; ++reg) {
        const std::uint64_t mask = claimed_bits(use, reg - use.first, token);
        if (mask == 0) {
            continue;
        }
        auto& slot = owners[reg];
        for (unsigned bit = 0; bit < REGISTER_BITS; ++bit) {
            if ((mask >> bit & 1U) != 0 && slot.at(bit) == 0) {
                slot.at(bit) = static_cast<std::uint32_t>(index + 1);
            }
        }
    }
}

void meet(
    std::vector<Use>& uses, bool token,
    const std::function<void(const Use&, const Use&, std::uint64_t)>& found) {
    std::vector<std::size_t> order(uses.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::ranges::stable_sort(order, [&uses](std::size_t lhs, std::size_t rhs) {
        return std::tie(uses[lhs].unit, uses[lhs].table, uses[lhs].first) <
               std::tie(uses[rhs].unit, uses[rhs].table, uses[rhs].first);
    });
    Owners owners;
    std::set<std::pair<std::uint32_t, std::uint32_t>> told;
    // The unit and table being swept; `owners` is theirs.
    std::pair<std::uint32_t, Table> key{};
    bool started = false;
    for (const std::size_t index : order) {
        const Use& use = uses[index];
        if (!started || key != std::pair{use.unit, use.table}) {
            owners.clear();
            key = std::pair{use.unit, use.table};
            started = true;
        }
        // Later uses start at or after this one: what is behind is done.
        owners.erase(owners.begin(), owners.lower_bound(use.first));
        if (const auto earlier = first_met(uses, use, owners, token)) {
            const Use& other = uses[earlier->first];
            if (told.insert(std::minmax(use.owner, other.owner)).second) {
                found(use, other, earlier->second);
            }
            continue;
        }
        claim(owners, use, index, token);
    }
}

// The uses of one table and unit, sorted by first register, with a tree
// that finds, among the uses that start at or before a register, the one
// that reaches furthest and the one of another group that does (A.4,
// CF-610). A query is O(log n).
class Reach {
public:
    struct Best {
        std::uint64_t last = 0;
        std::uint32_t group = 0;
        std::uint32_t use = 0;  // index into the uses given to `add`
        bool valid = false;
    };
    struct Summary {
        Best best;
        Best other;  // the furthest of a group other than best's
    };

    void add(const Use& use, std::uint32_t index) {
        items_.push_back(Item{.first = use.first,
                              .best = Best{.last = use.last,
                                           .group = use.group,
                                           .use = index,
                                           .valid = true}});
    }

    void build() {
        std::ranges::stable_sort(items_, {}, &Item::first);
        size_ = 1;
        while (size_ < items_.size()) {
            size_ *= 2;
        }
        tree_.assign(2 * size_, Summary{});
        for (std::size_t i = 0; i < items_.size(); ++i) {
            tree_[size_ + i].best = items_[i].best;
        }
        for (std::size_t i = size_ - 1; i > 0; --i) {
            tree_[i] = combine(tree_[2 * i], tree_[2 * i + 1]);
        }
    }

    // Of the uses that start at or before `last`.
    [[nodiscard]] Summary up_to(std::uint64_t last) const {
        const auto end =
            std::ranges::upper_bound(items_, last, {}, &Item::first);
        std::size_t right =
            size_ + static_cast<std::size_t>(end - items_.begin());
        std::size_t left = size_;
        Summary result;
        while (left < right) {
            if ((left & 1U) != 0) {
                result = combine(result, tree_[left++]);
            }
            if ((right & 1U) != 0) {
                result = combine(result, tree_[--right]);
            }
            left /= 2;
            right /= 2;
        }
        return result;
    }

private:
    struct Item {
        std::uint64_t first = 0;
        Best best;
    };

    [[nodiscard]] static bool further(const Best& lhs, const Best& rhs) {
        if (!rhs.valid) {
            return lhs.valid;
        }
        return lhs.valid && (lhs.last > rhs.last ||
                             (lhs.last == rhs.last && lhs.use < rhs.use));
    }

    [[nodiscard]] static Summary combine(const Summary& lhs,
                                         const Summary& rhs) {
        const std::array<Best, 4> all = {lhs.best, lhs.other, rhs.best,
                                         rhs.other};
        Summary result;
        for (const Best& one : all) {
            if (further(one, result.best)) {
                result.best = one;
            }
        }
        for (const Best& one : all) {
            if (one.valid && one.group != result.best.group &&
                further(one, result.other)) {
                result.other = one;
            }
        }
        return result;
    }

    std::vector<Item> items_;
    std::vector<Summary> tree_;
    std::size_t size_ = 1;
};

// A read range of A.4.
struct Range {
    std::string_view kind;
    std::uint32_t unit = 0;
    Table table = Table::HOLDING;
    std::uint64_t first = 0;  // protocol addresses
    std::uint64_t last = 0;
    NodeId site = 0;   // its `kind`
    std::string text;  // "side_effect holding 100 to 109", as written
};

class Checker {
public:
    Checker(const ExpandedTree& tree, Diagnostics& diagnostics)
        : expanded_(tree.document), reporter_(tree, diagnostics) {}

    // `expanded_` is pointed into while it runs: not copied, not moved.
    Checker(const Checker&) = delete;
    Checker& operator=(const Checker&) = delete;
    Checker(Checker&&) = delete;
    Checker& operator=(Checker&&) = delete;
    ~Checker() = default;

    void run() {
        const NodeId root = expanded_.document().root();
        std::set<std::string_view> seen;
        for (const NodeId channel : expanded_.items(root, "channels")) {
            if (expanded_.text_of(channel, "protocol") != "modbus") {
                continue;
            }
            read_channel(channel);
            channel_uses_.clear();
            channel_devices_.clear();
            for (const NodeId device : expanded_.items(channel, "devices")) {
                const std::optional<NodeId> name =
                    expanded_.scalar(device, "name");
                if (name && seen.insert(text(*name)).second) {
                    check_device(device, *name);
                }
            }
            check_channel();
        }
        reporter_.flush();
    }

private:
    // --- Placing errors --------------------------------------------------

    // `detail` names what in the device the error is about ("point p",
    // "identify"), for the template note.
    // `key`, when given, names the cause in place of the message (D-114).
    void error(NodeId node, std::uint16_t code, std::string message,
               std::string_view detail = {}, std::string_view key = {}) {
        reporter_.error(expanded_.document(), node, code, std::move(message),
                        detail, device_, key);
    }

    // Who an address belongs to: the label of a CF-603 message and the
    // detail of its note.
    struct Owner {
        std::string label;   // "\"ps1.m1.p\"", or an identity check
        std::string detail;  // "point p", or "identify"
    };

    [[nodiscard]] Owner point_owner(std::string_view name) const {
        return Owner{
            .label = quote_text(std::string(device_) + "." + std::string(name)),
            .detail = "point " + std::string(name)};
    }

    [[nodiscard]] std::string_view text(NodeId node) const {
        return expanded_.document().text(node);
    }

    [[nodiscard]] std::optional<std::uint64_t> number(
        std::optional<NodeId> node) const {
        if (!node) {
            return std::nullopt;
        }
        const std::optional<Integer> value = parse_integer(text(*node));
        return value ? as_uint64(*value) : std::nullopt;
    }

    // A raw pattern (2.3) at `node`, as the bits of a value of `width` bits.
    [[nodiscard]] std::optional<std::uint64_t> pattern(NodeId node,
                                                       unsigned width) const {
        const std::optional<Integer> value = parse_integer(text(node));
        if (!value) {
            return std::nullopt;
        }
        const std::uint64_t mask =
            width >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << width) - 1;
        if (value->negative) {
            return (~value->magnitude + 1) & mask;
        }
        return value->magnitude & mask;
    }

    // --- Channels ----------------------------------------------------------

    void read_channel(NodeId channel) {
        periods_.clear();
        const std::optional<NodeId> keys = expanded_.child(channel, "channel");
        if (keys && expanded_.child(*keys, "poll_groups")) {
            for (const Entry& group : expanded_.entries(*keys, "poll_groups")) {
                if (const std::optional<std::uint64_t> period =
                        parse_duration_ms(
                            expanded_.text_of(group.value, "period"))) {
                    periods_.emplace(text(group.key), *period);
                }
            }
            return;
        }
        for (const auto& [group, period] : DEFAULT_PERIODS_MS) {
            periods_.emplace(group, period);
        }
    }

    // CF-601: the registers of two devices of the channel meet (A.3).
    void check_channel() {
        meet(channel_uses_, true,
             [this](const Use& one, const Use& other, std::uint64_t reg) {
                 // At the device given later, naming the one given first.
                 const auto [first, second] =
                     std::minmax(one.owner, other.owner);
                 const auto& [name, base] = channel_devices_.at(second);
                 device_ = text(name);
                 error(name, CF_DEVICES_OVERLAP,
                       "devices " +
                           quote_text(text(channel_devices_.at(first).first)) +
                           " and " + quote_text(device_) + " overlap at " +
                           // The register as the device reported writes it.
                           place(one.unit, one.table, reg, base));
             });
        device_ = {};
    }

    // "holding 100 of unit 1", the register as written.
    [[nodiscard]] static std::string place(std::uint32_t unit, Table table,
                                           std::uint64_t reg,
                                           std::uint64_t base) {
        return std::string(name_of(table)) + " " + std::to_string(reg + base) +
               " of unit " + std::to_string(unit);
    }

    // --- Devices -----------------------------------------------------------

    void check_device(NodeId device, NodeId name) {
        device_ = text(name);
        base_ = number(expanded_.scalar(device, "address_base")).value_or(0);
        unit_ = static_cast<std::uint32_t>(
            number(expanded_.scalar(device, "unit_id")).value_or(0));
        bit_order_msb0_ = expanded_.text_of(device, "bit_order") == "msb0";
        reads_.clear();
        writes_.clear();
        groups_.clear();
        point_names_.clear();
        point_reads_.clear();
        const std::span<const NodeId> points =
            expanded_.items(device, "points");
        for (const NodeId point : points) {
            const std::optional<NodeId> point_name =
                expanded_.scalar(point, "name");
            if (!point_name) {
                continue;
            }
            const auto index = static_cast<std::uint32_t>(point_names_.size());
            point_names_.push_back(text(*point_name));
            point_reads_.emplace_back();
            read_point(point, index);
        }
        check_points();
        check_functions(device, points);
        check_ranges(device);
        check_idle(device);
        const auto device_index =
            static_cast<std::uint32_t>(channel_devices_.size());
        channel_devices_.emplace_back(name, base_);
        for (std::vector<Use>* uses : {&reads_, &writes_}) {
            for (Use use : *uses) {
                use.owner = device_index;
                channel_uses_.push_back(use);
            }
        }
        device_ = {};
    }

    // --- One point's registers ---------------------------------------------

    void read_point(NodeId point, std::uint32_t index) {
        const std::optional<NodeId> address = expanded_.child(point, "address");
        if (!address) {
            return;
        }
        const std::string_view name = point_names_.at(index);
        const std::string_view access = expanded_.text_of(point, "access");
        const bool read = access != "wo";
        const bool write = access == "rw" || access == "wo";
        std::string_view group = expanded_.text_of(point, "poll_group");
        if (group.empty()) {
            group = DEFAULT_GROUP;
        }
        const auto group_id = static_cast<std::uint32_t>(
            groups_.try_emplace(group, groups_.size()).first->second);
        const std::uint32_t unit = static_cast<std::uint32_t>(
            number(expanded_.scalar(*address, "unit_id")).value_or(unit_));

        std::vector<Use> own;
        // A double point's shapes: each one coil or one bit (A.2).
        for (const std::string_view shape : {"on", "off", "fault"}) {
            if (const std::optional<NodeId> part =
                    expanded_.child(*address, shape)) {
                own_uses(*part, point_owner(name), unit, *part, own);
            }
        }
        if (expanded_.child(*address, "register") ||
            expanded_.child(*address, "parts")) {
            own_uses(*address, point_owner(name), unit, *address, own);
        }
        if (read) {
            for (Use use : own) {
                use.owner = index;
                use.group = group_id;
                point_reads_.at(index).push_back(
                    static_cast<std::uint32_t>(reads_.size()));
                // A bit or field of a register shares it (A.3): kept for
                // CF-610, CF-615 and CF-601, not compared for CF-602.
                reads_.push_back(use);
            }
        }
        if (write) {
            write_uses(*address, name, unit, own, index);
        }
    }

    // The registers of the address map `map` (an address, or one shape of
    // a double point), checked for CF-603, into `out`.
    void own_uses(NodeId map, const Owner& who, std::uint32_t unit, NodeId site,
                  std::vector<Use>& out) {
        const std::optional<Table> table =
            table_named(expanded_.text_of(map, "table"));
        if (!table) {
            return;
        }
        const std::string raw = modbus_raw_type(expanded_.document(), map);
        const bool part_of_register = expanded_.child(map, "bit").has_value() ||
                                      expanded_.child(map, "field").has_value();
        if (is_bit_table(*table)) {
            for (const std::string_view key :
                 {"bit", "field", "byte", "parts"}) {
                if (const std::optional<NodeId> given =
                        expanded_.child(map, key)) {
                    refuse_address(*given, who,
                                   one_bit(*table) + ": " + std::string(key) +
                                       " does not apply");
                    return;
                }
            }
            if (const std::optional<NodeId> type =
                    expanded_.scalar(map, "type");
                type && text(*type) != "bool") {
                refuse_address(*type, who, one_bit(*table));
                return;
            }
        }
        std::optional<std::uint64_t> count = registers_of(map, raw, *table);
        if (!count) {
            return;
        }
        if (const std::optional<NodeId> field = expanded_.child(map, "field");
            field && !fits_field(*field, *count)) {
            refuse_address(
                *field, who,
                "field " + field_text(*field) + " is wider than " + raw);
            return;
        }
        const Use shape{
            .owner = 0,
            .unit = unit,
            .table = *table,
            .site = site,
            .whole = !part_of_register,
            .little = expanded_.text_of(map, "word_order") == "little",
            .pattern = part_of_register ? part_bits(map, *count) : 0};
        if (expanded_.child(map, "parts")) {
            for (const NodeId part : expanded_.items(map, "parts")) {
                add_register(expanded_.scalar(part, "register"), 1, who, shape,
                             out);
            }
            return;
        }
        add_register(expanded_.scalar(map, "register"), *count, who, shape,
                     out);
    }

    [[nodiscard]] static std::string one_bit(Table table) {
        return table == Table::COIL ? "a coil holds one bit"
                                    : "a discrete input holds one bit";
    }

    // The registers a value of raw type `raw` at `map` covers (A.2).
    [[nodiscard]] std::optional<std::uint64_t> registers_of(
        NodeId map, const std::string& raw, Table table) const {
        if (is_bit_table(table)) {
            return 1;
        }
        if (const std::optional<std::uint64_t> fixed = registers_of_type(raw)) {
            return fixed;
        }
        if (raw == "string" || raw == "bytes") {
            const std::optional<std::uint64_t> length =
                number(expanded_.scalar(map, "length"));
            return length ? std::optional<std::uint64_t>((*length + 1) / 2)
                          : std::nullopt;
        }
        if (raw == "mod10k") {
            return number(expanded_.scalar(map, "count"));
        }
        if (raw == "epoch") {
            const std::optional<std::uint64_t> width =
                number(expanded_.scalar(map, "width"));
            return width ? std::optional<std::uint64_t>(*width / REGISTER_BITS)
                         : std::nullopt;
        }
        if (raw == "datetime") {
            return expanded_.items(map, "fields").size();
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::pair<std::uint64_t, std::uint64_t>>
    field_of(NodeId field) const {
        const std::span<const NodeId> bounds =
            expanded_.document().kind(field) == NodeKind::SEQUENCE
                ? expanded_.document().items(field)
                : std::span<const NodeId>();
        if (bounds.size() != 2) {
            return std::nullopt;
        }
        const std::optional<std::uint64_t> low = number(bounds[0]);
        const std::optional<std::uint64_t> high = number(bounds[1]);
        if (!low || !high || *low > *high) {
            return std::nullopt;
        }
        return std::pair{*low, *high};
    }

    [[nodiscard]] bool fits_field(NodeId field, std::uint64_t count) const {
        const auto bounds = field_of(field);
        return !bounds || bounds->second > SCHEMA_LAST_BIT ||
               bounds->second < count * REGISTER_BITS;
    }

    [[nodiscard]] std::string field_text(NodeId field) const {
        const auto bounds = field_of(field);
        return bounds ? "[" + std::to_string(bounds->first) + ", " +
                            std::to_string(bounds->second) + "]"
                      : std::string("[]");
    }

    // The bits of a value of `count` registers that `map`'s `bit` or
    // `field` names, numbered by the device's `bit_order` (A.2).
    [[nodiscard]] std::uint64_t part_bits(NodeId map,
                                          std::uint64_t count) const {
        const std::uint64_t width = count * REGISTER_BITS;
        std::uint64_t low = 0;
        std::uint64_t high = 0;
        if (const std::optional<std::uint64_t> bit =
                number(expanded_.scalar(map, "bit"))) {
            low = *bit;
            high = *bit;
        } else if (const std::optional<NodeId> field =
                       expanded_.child(map, "field")) {
            const auto bounds = field_of(*field);
            if (!bounds) {
                return 0;
            }
            low = bounds->first;
            high = bounds->second;
        }
        std::uint64_t bits = 0;
        for (std::uint64_t bit = low;
             bit <= high && bit < width && bit < VALUE_BITS; ++bit) {
            const std::uint64_t position =
                bit_order_msb0_ ? width - 1 - bit : bit;
            if (position < VALUE_BITS) {
                bits |= std::uint64_t{1} << position;
            }
        }
        return bits;
    }

    // CF-603 for a register outside the table after `address_base`, or a
    // value that runs past its end; else the use, into `out`.
    void add_register(std::optional<NodeId> node, std::uint64_t count,
                      const Owner& who, Use use, std::vector<Use>& out) {
        const std::optional<std::uint64_t> written = number(node);
        if (!node || !written || count == 0 ||
            *written > SCHEMA_LAST_REGISTER) {
            return;
        }
        if (*written < base_ || *written - base_ > LAST_REGISTER) {
            refuse_address(*node, who,
                           "register " + std::to_string(*written) +
                               " is outside 0 to 65,535 after address_base " +
                               std::to_string(base_));
            return;
        }
        const std::uint64_t first = *written - base_;
        if (count - 1 > LAST_REGISTER - first) {
            refuse_address(*node, who,
                           "registers " + std::to_string(*written) + " to " +
                               std::to_string(*written + count - 1) +
                               " run past 65,535 after address_base " +
                               std::to_string(base_));
            return;
        }
        use.first = first;
        use.last = first + count - 1;
        use.site = *node;
        out.push_back(use);
    }

    void refuse_address(NodeId node, const Owner& who,
                        const std::string& reason) {
        error(node, CF_BAD_ADDRESS, who.label + ": " + reason, who.detail);
    }

    // The registers a point with `access: rw` or `wo` writes, and their bits
    // (A.5): `write.on` and `write.off`, else its own address with the bits
    // of its `mask`, `values` or `value`, of its `bit` or `field`, or all.
    void write_uses(NodeId address, std::string_view name, std::uint32_t unit,
                    const std::vector<Use>& own, std::uint32_t index) {
        const std::optional<NodeId> write = expanded_.child(address, "write");
        const std::optional<NodeId> to_on =
            write ? expanded_.child(*write, "on") : std::nullopt;
        const std::optional<NodeId> to_off =
            write ? expanded_.child(*write, "off") : std::nullopt;
        if (to_on || to_off) {
            for (const std::optional<NodeId> target : {to_on, to_off}) {
                if (target) {
                    write_target(*target, name, unit, index);
                }
            }
            return;
        }
        for (Use use : own) {
            use.owner = index;
            const std::uint64_t width =
                (use.last - use.first + 1) * REGISTER_BITS;
            if (write) {
                if (const std::optional<std::uint64_t> quantity =
                        number(expanded_.scalar(*write, "quantity"));
                    quantity && *quantity > use.last - use.first + 1) {
                    // The rest are written 0 (A.5): every bit of them.
                    use.last =
                        std::min(LAST_REGISTER, use.first + *quantity - 1);
                    use.whole = true;
                    writes_.push_back(use);
                    continue;
                }
                if (const std::optional<std::uint64_t> bits = encoding_bits(
                        *write, static_cast<unsigned>(std::min<std::uint64_t>(
                                    width, VALUE_BITS)))) {
                    use.whole = false;
                    use.pattern = *bits;
                }
            }
            writes_.push_back(use);
        }
    }

    // The register of a `write.on` or `write.off` target, `{ table,
    // register, value }`, with the bits of its `value`.
    void write_target(NodeId target, std::string_view name, std::uint32_t unit,
                      std::uint32_t index) {
        const std::optional<Table> table =
            table_named(expanded_.text_of(target, "table"));
        if (!table) {
            return;
        }
        const std::optional<NodeId> value = expanded_.scalar(target, "value");
        const std::optional<std::uint64_t> bits =
            value ? pattern(*value, REGISTER_BITS) : std::nullopt;
        std::vector<Use> targets;
        add_register(expanded_.scalar(target, "register"), 1, point_owner(name),
                     Use{.owner = index,
                         .unit = unit,
                         .table = *table,
                         .whole = !bits,
                         .pattern = bits.value_or(0)},
                     targets);
        for (Use& use : targets) {
            use.owner = index;
            writes_.push_back(use);
        }
    }

    // The bits a `mask`, `values` or `value` write encoding writes, as a
    // value of `width` bits; nothing for another encoding.
    [[nodiscard]] std::optional<std::uint64_t> encoding_bits(
        NodeId write, unsigned width) const {
        if (const std::optional<NodeId> mask =
                expanded_.scalar(write, "mask")) {
            return pattern(*mask, width);
        }
        if (const std::optional<NodeId> value =
                expanded_.scalar(write, "value")) {
            return pattern(*value, width);
        }
        if (expanded_.child(write, "values")) {
            std::uint64_t bits = 0;
            for (const Entry& entry : expanded_.entries(write, "values")) {
                bits |= pattern(entry.value, width).value_or(0);
            }
            return bits;
        }
        return std::nullopt;
    }

    // --- CF-602 ------------------------------------------------------------

    // Whole values read twice, and bits written twice (A.3, A.5). Reads and
    // writes are not compared: a write-only point does not read.
    void check_points() {
        std::vector<Use> whole_reads;
        for (const Use& use : reads_) {
            if (use.whole) {
                whole_reads.push_back(use);
            }
        }
        const auto told = [this](const Use& later, const Use& earlier,
                                 std::uint64_t reg) {
            const std::string_view name = point_names_.at(later.owner);
            const std::string message =
                "points " + quote_text(point_names_.at(earlier.owner)) +
                " and " + quote_text(name) + " overlap at " +
                place(later.unit, later.table, reg, base_);
            // The unit is the instance's: one template line at many
            // instances is one cause (D-114).
            error(later.site, CF_POINTS_OVERLAP, message,
                  "point " + std::string(name),
                  message.substr(0, message.rfind(" of unit ")));
        };
        meet(whole_reads, false, told);
        meet(writes_, false, told);
    }

    // --- CF-615 ------------------------------------------------------------

    // A.1: the functions of the device, and the substitutions it allows.
    void check_functions(NodeId device, std::span<const NodeId> points) {
        std::set<unsigned> functions;
        const bool given = expanded_.child(device, "functions").has_value();
        for (const NodeId item : expanded_.items(device, "functions")) {
            if (const std::optional<std::uint64_t> function = number(item)) {
                functions.insert(static_cast<unsigned>(*function));
            }
        }
        if (!given) {
            return;  // all eight
        }
        const std::optional<NodeId> read = expanded_.child(device, "read");
        const bool input_as_holding =
            read && expanded_.text_of(*read, "input_as_holding") == "true";
        const auto supports = [&functions, input_as_holding](unsigned need) {
            return functions.contains(need) ||
                   (need == FN_WRITE_REGISTER &&
                    functions.contains(FN_WRITE_REGISTERS)) ||
                   (need == FN_WRITE_COILS &&
                    functions.contains(FN_WRITE_COIL)) ||
                   (need == FN_READ_INPUT && input_as_holding &&
                    functions.contains(FN_READ_HOLDING));
        };
        std::uint32_t index = 0;
        for (const NodeId point : points) {
            if (!expanded_.scalar(point, "name")) {
                continue;
            }
            const std::string_view name = point_names_.at(index);
            std::set<unsigned> told;
            const auto need = [&](unsigned function, NodeId site) {
                if (!supports(function) && told.insert(function).second) {
                    error(site, CF_FUNCTION,
                          "point " + quote_text(name) + " needs function " +
                              std::to_string(function) + ", which device " +
                              quote_text(device_) + " does not support",
                          "point " + std::string(name));
                }
            };
            for (const std::uint32_t read_index : point_reads_.at(index)) {
                const Use& use = reads_.at(read_index);
                need(read_function(use.table), use.site);
            }
            for (const Use& use : writes_) {
                if (use.owner == index) {
                    need_write(point, use, need);
                }
            }
            ++index;
        }
    }

    // A.5: `function`, else `auto`: 5 for one coil, 6 for one register, 16
    // for more.
    void need_write(NodeId point, const Use& use,
                    const std::function<void(unsigned, NodeId)>& need) const {
        const std::optional<NodeId> address = expanded_.child(point, "address");
        const std::optional<NodeId> write =
            address ? expanded_.child(*address, "write") : std::nullopt;
        if (const std::optional<std::uint64_t> function = number(
                write ? expanded_.scalar(*write, "function") : std::nullopt)) {
            need(static_cast<unsigned>(*function), use.site);
            return;
        }
        if (use.table == Table::COIL) {
            need(FN_WRITE_COIL, use.site);
        } else if (use.table == Table::HOLDING) {
            need(use.last == use.first ? FN_WRITE_REGISTER : FN_WRITE_REGISTERS,
                 use.site);
        }
    }

    // --- CF-610 ------------------------------------------------------------

    void check_ranges(NodeId device) {
        const std::optional<NodeId> read = expanded_.child(device, "read");
        if (!read) {
            return;
        }
        std::vector<Range> ranges;
        for (const NodeId entry : expanded_.items(*read, "ranges")) {
            if (std::optional<Range> range = range_of(entry)) {
                ranges.push_back(std::move(*range));
            }
        }
        check_kinds(ranges);
        const std::vector<Use> identity = identity_uses(device);
        const Reaches reads = reaches_of(reads_);
        const Reaches writes = reaches_of(writes_);
        const Reaches identities = reaches_of(identity);
        for (const Range& range : ranges) {
            if (range.kind != "side_effect" && range.kind != "exclusive") {
                continue;
            }
            if (const std::optional<Reach::Summary> seen =
                    reached(reads, range);
                seen && seen->other.valid && seen->other.last >= range.first) {
                refuse_range(range, "points " +
                                        quote_text(point_names_.at(
                                            reads_.at(seen->best.use).owner)) +
                                        " and " +
                                        quote_text(point_names_.at(
                                            reads_.at(seen->other.use).owner)) +
                                        " are in different poll groups");
            }
            if (const std::optional<Reach::Summary> seen =
                    reached(identities, range)) {
                refuse_range(
                    range,
                    "identity check " +
                        std::to_string(identity.at(seen->best.use).owner + 1) +
                        " reads it");
            }
            if (const std::optional<Reach::Summary> seen =
                    reached(writes, range)) {
                refuse_range(range, "point " +
                                        quote_text(point_names_.at(
                                            writes_.at(seen->best.use).owner)) +
                                        " writes it");
            }
        }
    }

    // The uses of each unit and table, indexed by their first register.
    using Reaches = std::map<std::pair<std::uint32_t, Table>, Reach>;

    [[nodiscard]] static Reaches reaches_of(const std::vector<Use>& uses) {
        Reaches reaches;
        for (std::uint32_t i = 0; i < uses.size(); ++i) {
            reaches[{uses[i].unit, uses[i].table}].add(uses[i], i);
        }
        for (auto& [key, reach] : reaches) {
            reach.build();
        }
        return reaches;
    }

    // What of `reaches` meets `range`: the furthest use that starts at or
    // before its end and reaches its start, and the furthest of another
    // group; nothing when no use meets it.
    [[nodiscard]] static std::optional<Reach::Summary> reached(
        const Reaches& reaches, const Range& range) {
        const auto found = reaches.find({range.unit, range.table});
        if (found == reaches.end()) {
            return std::nullopt;
        }
        const Reach::Summary seen = found->second.up_to(range.last);
        if (!seen.best.valid || seen.best.last < range.first) {
            return std::nullopt;
        }
        return seen;
    }

    // A range of A.4, checked for CF-603; nothing for one that cannot be
    // read.
    [[nodiscard]] std::optional<Range> range_of(NodeId entry) {
        const std::optional<NodeId> kind = expanded_.scalar(entry, "kind");
        const std::optional<Table> table =
            table_named(expanded_.text_of(entry, "table"));
        const std::optional<NodeId> first_node =
            expanded_.scalar(entry, "first");
        const std::optional<NodeId> last_node = expanded_.scalar(entry, "last");
        const std::optional<std::uint64_t> first = number(first_node);
        const std::optional<std::uint64_t> last = number(last_node);
        if (!kind || !table || !first_node || !last_node || !first || !last ||
            *first > SCHEMA_LAST_REGISTER || *last > SCHEMA_LAST_REGISTER) {
            return std::nullopt;
        }
        Range range{
            .kind = text(*kind),
            .unit = static_cast<std::uint32_t>(
                number(expanded_.scalar(entry, "unit_id")).value_or(unit_)),
            .table = *table,
            .site = *kind,
            .text = std::string(text(*kind)) + " " +
                    std::string(name_of(*table)) + " " +
                    std::to_string(*first) + " to " + std::to_string(*last)};
        for (const auto& [value, node] :
             {std::pair{*first, *first_node}, std::pair{*last, *last_node}}) {
            if (value < base_ || value - base_ > LAST_REGISTER) {
                error(node, CF_BAD_ADDRESS,
                      "range " + range.text + " of device " +
                          quote_text(device_) + ": register " +
                          std::to_string(value) +
                          " is outside 0 to 65,535 after address_base " +
                          std::to_string(base_),
                      "read");
                return std::nullopt;
            }
        }
        if (*first > *last) {
            error(*first_node, CF_BAD_ADDRESS,
                  "range " + range.text + " of device " + quote_text(device_) +
                      ": first is above last",
                  "read");
            return std::nullopt;
        }
        range.first = *first - base_;
        range.last = *last - base_;
        return range;
    }

    // A `side_effect` or `exclusive` range that overlaps a range of another
    // kind (A.4): once per range, naming the first such range before it.
    void check_kinds(const std::vector<Range>& ranges) {
        std::vector<std::size_t> order(ranges.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::ranges::stable_sort(
            order, [&ranges](std::size_t lhs, std::size_t rhs) {
                return std::tie(ranges[lhs].unit, ranges[lhs].table,
                                ranges[lhs].first) <
                       std::tie(ranges[rhs].unit, ranges[rhs].table,
                                ranges[rhs].first);
            });
        // Per kind, the range that reaches furthest so far in this table.
        std::map<std::string_view, std::size_t> furthest;
        std::optional<std::pair<std::uint32_t, Table>> key;
        for (const std::size_t range_index : order) {
            const Range& range = ranges[range_index];
            if (!key || key->first != range.unit ||
                key->second != range.table) {
                furthest.clear();
                key = std::pair{range.unit, range.table};
            }
            const bool strict =
                range.kind == "side_effect" || range.kind == "exclusive";
            std::optional<std::size_t> met;
            for (const auto& [kind, other] : furthest) {
                const bool other_strict =
                    kind == "side_effect" || kind == "exclusive";
                if (kind != range.kind && (strict || other_strict) &&
                    ranges[other].last >= range.first &&
                    (!met || other < *met)) {
                    met = other;
                }
            }
            if (met) {
                refuse_range(range, "it overlaps range " + ranges[*met].text);
            }
            auto [slot, added] = furthest.try_emplace(range.kind, range_index);
            if (!added && ranges[slot->second].last < range.last) {
                slot->second = range_index;
            }
        }
    }

    void refuse_range(const Range& range, const std::string& reason) {
        error(range.site, CF_RANGE,
              "range " + range.text + " of device " + quote_text(device_) +
                  ": " + reason,
              "read");
    }

    // The registers the identity checks read (4.8): an `address`, or the
    // reads of a `point`; `owner` is the check's index.
    [[nodiscard]] std::vector<Use> identity_uses(NodeId device) {
        std::vector<Use> uses;
        std::uint32_t number_of = 0;
        for (const NodeId check : expanded_.items(device, "identify")) {
            const std::uint32_t check_index = number_of++;
            if (const std::optional<NodeId> address =
                    expanded_.child(check, "address")) {
                std::vector<Use> own;
                own_uses(*address,
                         Owner{.label = "identity check " +
                                        std::to_string(check_index + 1) +
                                        " of " + quote_text(device_),
                               .detail = "identify"},
                         unit_, *address, own);
                for (Use& use : own) {
                    use.owner = check_index;
                    uses.push_back(use);
                }
                continue;
            }
            const std::string_view point = expanded_.text_of(check, "point");
            const auto found = std::ranges::find(point_names_, point);
            if (found == point_names_.end()) {
                continue;  // CF-310 of the references (D-116)
            }
            const auto index =
                static_cast<std::size_t>(found - point_names_.begin());
            for (const std::uint32_t read : point_reads_.at(index)) {
                Use use = reads_.at(read);
                use.owner = check_index;
                uses.push_back(use);
            }
        }
        return uses;
    }

    // --- CW-607 ------------------------------------------------------------

    // The device's `idle_timeout` is shorter than its slowest poll period or
    // its `backoff_max`, and `keepalive` is off (4.6).
    void check_idle(NodeId device) {
        const std::optional<NodeId> idle =
            expanded_.scalar(device, "idle_timeout");
        const std::string_view keepalive =
            expanded_.text_of(device, "keepalive");
        if (!idle || (!keepalive.empty() && keepalive != "off")) {
            return;
        }
        const std::optional<std::uint64_t> idle_ms =
            parse_duration_ms(text(*idle));
        if (!idle_ms) {
            return;
        }
        std::uint64_t longest =
            parse_duration_ms(expanded_.text_of(device, "backoff_max"))
                .value_or(DEFAULT_BACKOFF_MAX_MS);
        std::set<std::uint32_t> used;
        for (const Use& use : reads_) {
            used.insert(use.group);
        }
        for (const auto& [group, id] : groups_) {
            if (const auto period = periods_.find(group);
                used.contains(static_cast<std::uint32_t>(id)) &&
                period != periods_.end()) {
                longest = std::max(longest, period->second);
            }
        }
        if (longest > *idle_ms) {
            reporter_.warning(expanded_.document(), *idle, CW_IDLE,
                              "device " + quote_text(device_) +
                                  ": the connection may be closed between "
                                  "polls; set keepalive");
        }
    }

    Reader expanded_;
    Reporter reporter_;
    // Of the channel being checked.
    std::map<std::string_view, std::uint64_t, std::less<>> periods_;
    std::vector<Use> channel_uses_;  // owner: index into channel_devices_
    std::vector<std::pair<NodeId, std::uint64_t>> channel_devices_;
    // Of the device being checked: views into the expanded tree.
    std::string_view device_;
    std::uint64_t base_ = 0;
    std::uint32_t unit_ = 0;
    bool bit_order_msb0_ = false;
    std::vector<std::string_view> point_names_;
    std::vector<std::vector<std::uint32_t>> point_reads_;  // into reads_
    std::vector<Use> reads_;
    std::vector<Use> writes_;
    std::map<std::string_view, std::size_t, std::less<>> groups_;
};

}  // namespace

void check_modbus_rules(const ExpandedTree& tree, Diagnostics& diagnostics) {
    Checker checker(tree, diagnostics);
    checker.run();
}

}  // namespace scada::conf
