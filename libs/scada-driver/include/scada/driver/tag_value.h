#ifndef SCADA_DRIVER_TAG_VALUE_H
#define SCADA_DRIVER_TAG_VALUE_H

// An engineering value in a tag's type (tag database contract 6), as the
// framework computes it from a raw value and as a write request carries it.

#include "scada/common/realtime.h"
#include "scada/tag/types.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace scada::driver {

// The longest STRING value, in bytes (tag database contract 7, 14).
inline constexpr std::size_t TAG_STRING_MAX_BYTES = 250;

// One value. Only the field of `type` is meaningful. `text` is borrowed; who
// returns a TagValue states what it points into.
struct TagValue {
    tag::TagType type = tag::TagType::INT64;
    bool b = false;
    std::int64_t i = 0;
    std::uint64_t u = 0;
    double f = 0.0;
    // NOLINTBEGIN(readability-redundant-member-init): GCC\'s
    // -Wmissing-field-initializers needs it where a designated initializer
    // leaves the member out.
    std::string_view text{};
    // NOLINTEND(readability-redundant-member-init)
};

// Each returns a value of one type.
SCADA_RT [[nodiscard]] constexpr TagValue bool_value(bool value) noexcept {
    return TagValue{.type = tag::TagType::BOOL, .b = value};
}
SCADA_RT [[nodiscard]] constexpr TagValue int64_value(
    std::int64_t value) noexcept {
    return TagValue{.type = tag::TagType::INT64, .i = value};
}
SCADA_RT [[nodiscard]] constexpr TagValue uint64_value(
    std::uint64_t value) noexcept {
    return TagValue{.type = tag::TagType::UINT64, .u = value};
}
SCADA_RT [[nodiscard]] constexpr TagValue float64_value(double value) noexcept {
    return TagValue{.type = tag::TagType::FLOAT64, .f = value};
}
// `text` is borrowed by the returned value.
SCADA_RT [[nodiscard]] constexpr TagValue string_value(
    std::string_view text) noexcept {
    return TagValue{.type = tag::TagType::STRING, .text = text};
}

// The value a tag holds before anything was written: 0 of its type, or the
// empty string (contract 5.2: "0 if it never had one").
SCADA_RT [[nodiscard]] constexpr TagValue zero_value(
    tag::TagType type) noexcept {
    return TagValue{.type = type};
}

}  // namespace scada::driver

#endif  // SCADA_DRIVER_TAG_VALUE_H
