#ifndef SCADA_DRIVER_CHECKED_H
#define SCADA_DRIVER_CHECKED_H

// Checked integer arithmetic for scaling and value conversion (driver
// interface contract 5.1 step 4, 8.4; coding standard 5). Portable C++: no
// compiler builtins, no 128-bit type.

#include "scada/common/realtime.h"

#include <cstdint>
#include <limits>
#include <optional>

namespace scada::driver::detail {

// An integer of 65 bits: a sign and a 64-bit magnitude. It holds every
// int64 and every uint64, and their differences, exactly. Zero is never
// negative.
struct Wide {
    bool negative = false;
    std::uint64_t magnitude = 0;
};

SCADA_RT [[nodiscard]] constexpr Wide wide_from_signed(
    std::int64_t value) noexcept {
    if (value >= 0) {
        return Wide{.negative = false,
                    .magnitude = static_cast<std::uint64_t>(value)};
    }
    // The magnitude of INT64_MIN is 2^63, which fits: compute it unsigned.
    return Wide{
        .negative = true,
        .magnitude = std::uint64_t{0} - static_cast<std::uint64_t>(value)};
}

SCADA_RT [[nodiscard]] constexpr Wide wide_from_unsigned(
    std::uint64_t value) noexcept {
    return Wide{.negative = false, .magnitude = value};
}

SCADA_RT [[nodiscard]] constexpr Wide wide_negate(Wide value) noexcept {
    if (value.magnitude == 0) {
        return value;
    }
    return Wide{.negative = !value.negative, .magnitude = value.magnitude};
}

// a + b, or nothing when the magnitude exceeds 2^64 - 1.
SCADA_RT [[nodiscard]] constexpr std::optional<Wide> wide_add(
    Wide left, Wide right) noexcept {
    if (left.negative == right.negative) {
        if (left.magnitude >
            std::numeric_limits<std::uint64_t>::max() - right.magnitude) {
            return std::nullopt;
        }
        return Wide{.negative = left.negative,
                    .magnitude = left.magnitude + right.magnitude};
    }
    if (left.magnitude >= right.magnitude) {
        const std::uint64_t magnitude = left.magnitude - right.magnitude;
        return Wide{.negative = magnitude != 0 && left.negative,
                    .magnitude = magnitude};
    }
    return Wide{.negative = right.negative,
                .magnitude = right.magnitude - left.magnitude};
}

SCADA_RT [[nodiscard]] constexpr std::optional<Wide> wide_subtract(
    Wide left, Wide right) noexcept {
    return wide_add(left, wide_negate(right));
}

// a * b, or nothing when the magnitude exceeds 2^64 - 1.
SCADA_RT [[nodiscard]] constexpr std::optional<Wide> wide_multiply(
    Wide left, Wide right) noexcept {
    if (left.magnitude != 0 &&
        right.magnitude >
            std::numeric_limits<std::uint64_t>::max() / left.magnitude) {
        return std::nullopt;
    }
    const std::uint64_t magnitude = left.magnitude * right.magnitude;
    return Wide{.negative = magnitude != 0 && (left.negative != right.negative),
                .magnitude = magnitude};
}

// a / b when b divides a exactly; nothing otherwise, or when b is 0.
SCADA_RT [[nodiscard]] constexpr std::optional<Wide> wide_divide_exact(
    Wide left, Wide right) noexcept {
    if (right.magnitude == 0 || left.magnitude % right.magnitude != 0) {
        return std::nullopt;
    }
    const std::uint64_t magnitude = left.magnitude / right.magnitude;
    return Wide{.negative = magnitude != 0 && (left.negative != right.negative),
                .magnitude = magnitude};
}

// The value as an int64, or nothing when it does not fit.
SCADA_RT [[nodiscard]] constexpr std::optional<std::int64_t> wide_to_signed(
    Wide value) noexcept {
    constexpr auto MAX_POSITIVE =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (!value.negative) {
        if (value.magnitude > MAX_POSITIVE) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(value.magnitude);
    }
    if (value.magnitude > MAX_POSITIVE + 1U) {
        return std::nullopt;
    }
    if (value.magnitude == MAX_POSITIVE + 1U) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(value.magnitude);
}

// The value as a uint64, or nothing when it is negative.
SCADA_RT [[nodiscard]] constexpr std::optional<std::uint64_t> wide_to_unsigned(
    Wide value) noexcept {
    if (value.negative) {
        return std::nullopt;
    }
    return value.magnitude;
}

// True when `value` fits a signed integer of `width` bits (8 to 64).
SCADA_RT [[nodiscard]] constexpr bool fits_signed(Wide value,
                                                  unsigned width) noexcept {
    const std::uint64_t limit = std::uint64_t{1} << (width - 1U);  // 2^(w-1)
    return value.negative ? value.magnitude <= limit : value.magnitude < limit;
}

// True when `value` fits an unsigned integer of `width` bits (8 to 64).
SCADA_RT [[nodiscard]] constexpr bool fits_unsigned(Wide value,
                                                    unsigned width) noexcept {
    if (value.negative) {
        return false;
    }
    constexpr unsigned FULL_WIDTH = 64;
    return width >= FULL_WIDTH || value.magnitude < (std::uint64_t{1} << width);
}

}  // namespace scada::driver::detail

#endif  // SCADA_DRIVER_CHECKED_H
