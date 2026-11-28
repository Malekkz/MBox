// The channels against the limits and against each other (configuration
// schemas contract 4.1, 10.6 and Annex A.1; decision D-123, slice 7d-3 of
// #100).

#include "channel_rules.h"

#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/names.h"
#include "scada/conf/node.h"
#include "scada/conf/references.h"
#include "scada/conf/scalar.h"
#include "scada/conf/tree.h"
#include "tree_reader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace scada::conf {

namespace {

using detail::Reader;
using detail::Reporter;

constexpr std::uint16_t CF_TOO_MANY_DEVICES = 605;
constexpr std::uint16_t CF_SHARED_SERIAL_DEVICE = 620;
constexpr std::uint16_t CW_SHARED_ENDPOINT = 621;

// 4.1: `limits.max_devices_per_channel`, its range and default.
constexpr std::int64_t MIN_DEVICES_PER_CHANNEL = 1;
constexpr std::int64_t MAX_DEVICES_PER_CHANNEL = 256;
constexpr std::int64_t DEFAULT_DEVICES_PER_CHANNEL = 247;

// A.1: the port of a `tcp` or `rtu_over_tcp` channel that gives none.
constexpr std::int64_t DEFAULT_PORT = 502;
constexpr std::int64_t MAX_PORT = 65535;

// An address as 16 bytes, an IPv4 address mapped as ::ffff:a.b.c.d (RFC
// 4291 2.5.5.2), so that both spellings of one address meet.
using Address = std::array<std::uint8_t, IPV6_BYTES>;

[[nodiscard]] std::optional<Address> address_of(std::string_view host) {
    if (const std::optional<Address> six = ipv6_bytes(host)) {
        return six;
    }
    const std::optional<std::array<std::uint8_t, IPV4_BYTES>> four =
        ipv4_bytes(host);
    if (!four) {
        return std::nullopt;
    }
    constexpr std::size_t MAPPED_AT = IPV6_BYTES - IPV4_BYTES;
    constexpr std::uint8_t ALL_ONES = 0xFF;
    Address mapped{};
    mapped.at(MAPPED_AT - 2) = ALL_ONES;
    mapped.at(MAPPED_AT - 1) = ALL_ONES;
    for (std::size_t i = 0; i < IPV4_BYTES; ++i) {
        mapped.at(MAPPED_AT + i) = four->at(i);
    }
    return mapped;
}

// An integer key within [low, high], or nothing: a value the schema refused
// gives no check here, its own error already refuses the tree.
[[nodiscard]] std::optional<std::int64_t> integer_in(std::string_view text,
                                                     std::int64_t low,
                                                     std::int64_t high) {
    const std::optional<Integer> value = parse_integer(text);
    const std::optional<std::int64_t> number =
        value ? as_int64(*value) : std::nullopt;
    if (!number || *number < low || *number > high) {
        return std::nullopt;
    }
    return number;
}

// `limits.max_devices_per_channel` of system.yaml, its default without one,
// nothing for a value the schema refused.
[[nodiscard]] std::optional<std::int64_t> devices_per_channel(
    std::span<const CheckedFile> files) {
    for (const CheckedFile& file : files) {
        if (file.source->path_class.type != FileType::SYSTEM) {
            continue;
        }
        const Reader reader(*file.document);
        const std::optional<NodeId> limits =
            reader.child(reader.document().root(), "limits");
        const std::optional<NodeId> limit =
            limits ? reader.scalar(*limits, "max_devices_per_channel")
                   : std::nullopt;
        if (limit) {
            return integer_in(reader.document().text(*limit),
                              MIN_DEVICES_PER_CHANNEL, MAX_DEVICES_PER_CHANNEL);
        }
    }
    return DEFAULT_DEVICES_PER_CHANNEL;
}

class Checker {
public:
    Checker(const ExpandedTree& tree, std::span<const CheckedFile> files,
            Diagnostics& diagnostics)
        : files_(files),
          expanded_(tree.document),
          reporter_(tree, diagnostics) {}

    // `expanded_` and the maps point into the tree: not copied, not moved.
    Checker(const Checker&) = delete;
    Checker& operator=(const Checker&) = delete;
    Checker(Checker&&) = delete;
    Checker& operator=(Checker&&) = delete;
    ~Checker() = default;

    void run() {
        const std::optional<std::int64_t> limit = devices_per_channel(files_);
        const NodeId root = expanded_.document().root();
        for (const NodeId channel : expanded_.items(root, "channels")) {
            const std::optional<NodeId> keys =
                expanded_.child(channel, "channel");
            if (!keys) {
                continue;  // CF-202 already
            }
            const std::optional<NodeId> name = expanded_.scalar(*keys, "name");
            if (!name) {
                continue;  // CF-202 already
            }
            if (limit) {
                check_devices(channel, *name, *limit);
            }
            if (expanded_.text_of(channel, "protocol") == "modbus") {
                check_modbus(*keys, text(*name));
            }
        }
        reporter_.flush();
    }

private:
    [[nodiscard]] std::string_view text(NodeId node) const {
        return expanded_.document().text(node);
    }

    // CF-605. Every device written counts: a disabled one keeps its tags
    // (4.6), and a device given twice (CF-303) twice.
    void check_devices(NodeId channel, NodeId name, std::int64_t limit) {
        const std::size_t devices = expanded_.items(channel, "devices").size();
        if (devices > static_cast<std::size_t>(limit)) {
            reporter_.error(expanded_.document(), name, CF_TOO_MANY_DEVICES,
                            "channel " + std::string(text(name)) + " has " +
                                std::to_string(devices) +
                                " devices, the limit is " +
                                std::to_string(limit));
        }
    }

    // CF-620 and CW-621 (A.1). A disabled channel counts: enabling it is a
    // live change (4.6), which must not meet a refusal.
    void check_modbus(NodeId keys, std::string_view name) {
        const std::string_view type = expanded_.text_of(keys, "type");
        if (type == "rtu") {
            const std::optional<NodeId> device =
                expanded_.scalar(keys, "device");
            if (!device) {
                return;
            }
            const auto [first, added] = paths_.try_emplace(text(*device), name);
            if (!added) {
                reporter_.error(expanded_.document(), *device,
                                CF_SHARED_SERIAL_DEVICE,
                                "channels " + std::string(first->second) +
                                    " and " + std::string(name) + " use " +
                                    std::string(text(*device)));
            }
            return;
        }
        if (type != "tcp" && type != "rtu_over_tcp") {
            return;
        }
        const std::optional<NodeId> host = expanded_.scalar(keys, "host");
        if (!host) {
            return;  // CF-202 already
        }
        const std::optional<Address> address = address_of(text(*host));
        const std::optional<NodeId> port_node = expanded_.scalar(keys, "port");
        const std::optional<std::int64_t> port =
            port_node ? integer_in(text(*port_node), 1, MAX_PORT)
                      : DEFAULT_PORT;
        if (!address || !port) {
            return;  // the schema's error already
        }
        const auto [first, added] = endpoints_.try_emplace(
            std::make_pair(*address, static_cast<std::uint16_t>(*port)), name);
        if (!added) {
            const std::string_view written = text(*host);
            const std::string endpoint =
                (written.find(':') != std::string_view::npos
                     ? "[" + std::string(written) + "]"
                     : std::string(written)) +
                ":" + std::to_string(*port);
            reporter_.warning(expanded_.document(), *host, CW_SHARED_ENDPOINT,
                              "channels " + std::string(first->second) +
                                  " and " + std::string(name) +
                                  " both connect to " + endpoint +
                                  "; most devices allow one client");
        }
    }

    std::span<const CheckedFile> files_;
    Reader expanded_;
    Reporter reporter_;
    // The first channel of each serial path, and of each endpoint; views
    // into the expanded tree.
    std::map<std::string_view, std::string_view> paths_;
    std::map<std::pair<Address, std::uint16_t>, std::string_view> endpoints_;
};

}  // namespace

void check_channel_rules(const ExpandedTree& tree,
                         std::span<const CheckedFile> files,
                         Diagnostics& diagnostics) {
    Checker(tree, files, diagnostics).run();
}

}  // namespace scada::conf
