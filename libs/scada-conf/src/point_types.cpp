// The raw and value types of a point (D-115, D-116).

#include "point_types.h"

#include "scada/conf/expand.h"
#include "scada/conf/node.h"
#include "scada/conf/rules.h"
#include "scada/conf/types.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace scada::conf {

namespace {

std::optional<NodeId> child(const Document& document, NodeId map,
                            std::string_view key) {
    if (document.kind(map) != NodeKind::MAP) {
        return std::nullopt;
    }
    return document.find(map, key);
}

std::string text_of(const Document& document, NodeId map,
                    std::string_view key) {
    const std::optional<NodeId> node = child(document, map, key);
    if (!node || document.kind(*node) != NodeKind::SCALAR) {
        return {};
    }
    return std::string(document.text(*node));
}

}  // namespace

bool is_integer_raw(std::string_view raw) noexcept {
    static constexpr std::array<std::string_view, 10> INTEGER_RAW = {
        "bool",  "int8",   "uint8", "int16",  "uint16",
        "int32", "uint32", "int64", "uint64", "mod10k"};
    return std::ranges::find(INTEGER_RAW, raw) != INTEGER_RAW.end();
}

std::string modbus_raw_type(const Document& document, NodeId address) {
    if (child(document, address, "bit")) {
        return "bool";
    }
    std::string written = text_of(document, address, "type");
    if (!written.empty()) {
        return written;
    }
    const std::string table = text_of(document, address, "table");
    return table == "coil" || table == "discrete" ? "bool" : "uint16";
}

std::string modbus_value_type(const ExpandedTree& tree, NodeId point,
                              std::string_view raw) {
    const Document& document = tree.document;
    std::string given = text_of(document, point, "value_type");
    if (!given.empty()) {
        return given;
    }
    if (child(document, point, "enum") ||
        text_of(document, point, "kind") == "double_point") {
        return "int64";
    }
    if (raw == "bool") {
        return "bool";
    }
    if (!is_integer_raw(raw)) {
        return raw == "string" || raw == "bytes" ? "string" : "float64";
    }
    if (child(document, point, "exponent_from")) {
        return "float64";
    }
    const auto is_integer = [&tree](NodeId node) {
        return tree.types.of(node) == ScalarType::INTEGER;
    };
    if (const std::optional<NodeId> scale = child(document, point, "scale");
        scale && document.kind(*scale) == NodeKind::MAP) {
        if (const std::optional<NodeId> divide =
                child(document, *scale, "divide_by");
            divide && !is_integer_one(document, tree.types, *divide)) {
            return "float64";
        }
        for (const std::string_view key : {"gain", "offset"}) {
            const std::optional<NodeId> factor = child(document, *scale, key);
            if (factor && !is_integer(*factor)) {
                return "float64";
            }
        }
    }
    return raw == "uint64" ? "uint64" : "int64";
}

std::string io_value_type(const Document& document, NodeId point,
                          std::string_view backend) {
    std::string given = text_of(document, point, "value_type");
    if (!given.empty()) {
        return given;
    }
    return backend == "gpio" ? "bool" : "float64";
}

}  // namespace scada::conf
