#include "scada/driver/write_conversion.h"

#include "checked.h"
#include "scada/driver/point_rules.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/tag_value.h"
#include "scada/tag/types.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>

namespace scada::driver {

namespace {

using tag::TagType;

constexpr unsigned FLOAT32_WIDTH = 32;
constexpr unsigned FLOAT64_WIDTH = 64;
constexpr unsigned BOOL_WIDTH = 1;
constexpr unsigned BYTE_BITS = 8;

// The smallest magnitude that rounds to infinity in float32: the largest
// float32 plus half a step, 0x1.ffffffp127. A double below it converts to a
// finite float32 by round to nearest, ties to even; one at or above it is
// out of range ([conv.double]), so it is refused before the conversion.
constexpr double FLOAT32_OVERFLOW = 0x1.ffffffp127;

// 2^n as a binary64, exact for n up to 64.
double power_of_two(unsigned n) noexcept {
    return std::ldexp(1.0, static_cast<int>(n));
}

bool width_ok(RawKind kind, unsigned width) noexcept {
    switch (kind) {
        case RawKind::BOOL:
            return width == BOOL_WIDTH;
        case RawKind::SIGNED:
        case RawKind::UNSIGNED:
            return width == BYTE_BITS || width == 2 * BYTE_BITS ||
                   width == FLOAT32_WIDTH || width == FLOAT64_WIDTH;
        case RawKind::FLOAT:
            return width == FLOAT32_WIDTH || width == FLOAT64_WIDTH;
        case RawKind::STRING:
        case RawKind::TIME:
            return false;
    }
    return false;
}

bool is_integer_tag(TagType type) noexcept {
    return type == TagType::INT64 || type == TagType::UINT64;
}

// An exact integer as a raw value of `rules`, or nothing when it does not
// fit the raw type exactly.
std::optional<RawValue> raw_from_integer(const WriteRules& rules,
                                         detail::Wide value) noexcept {
    switch (rules.raw_kind) {
        case RawKind::SIGNED: {
            const std::optional<std::int64_t> number =
                detail::wide_to_signed(value);
            if (!number.has_value() ||
                !detail::fits_signed(value, rules.raw_width)) {
                return std::nullopt;
            }
            return raw_signed(*number, rules.raw_width);
        }
        case RawKind::UNSIGNED:
            if (!detail::fits_unsigned(value, rules.raw_width)) {
                return std::nullopt;
            }
            return raw_unsigned(value.magnitude, rules.raw_width);
        case RawKind::BOOL:
            if (value.negative || value.magnitude > 1U) {
                return std::nullopt;
            }
            return raw_bool(value.magnitude == 1U);
        case RawKind::FLOAT: {
            // Exact only: an integer the floating type cannot hold is
            // refused rather than rounded on the integer path.
            const auto magnitude = static_cast<double>(value.magnitude);
            if (magnitude >= power_of_two(FLOAT64_WIDTH) ||
                static_cast<std::uint64_t>(magnitude) != value.magnitude) {
                return std::nullopt;
            }
            const double number = value.negative ? -magnitude : magnitude;
            if (rules.raw_width == FLOAT32_WIDTH) {
                const auto narrow = static_cast<float>(number);  // |x| < 2^64
                if (static_cast<double>(narrow) != number) {
                    return std::nullopt;
                }
                return raw_float32(narrow);
            }
            return raw_float64(number);
        }
        case RawKind::STRING:
        case RawKind::TIME:
            return std::nullopt;
    }
    return std::nullopt;
}

// The integer path of contract 8.4: exact, every step checked. The written
// value is the request itself.
std::optional<WriteValue> integer_path(const WriteRules& rules,
                                       const TagValue& value) noexcept {
    const detail::Wide requested = rules.tag_type == TagType::INT64
                                       ? detail::wide_from_signed(value.i)
                                       : detail::wide_from_unsigned(value.u);
    std::optional<detail::Wide> raw = requested;
    if (rules.scale != nullptr) {
        const ScaleRule& scale = *rules.scale;
        const std::optional<detail::Wide> difference = detail::wide_subtract(
            requested, detail::wide_from_signed(scale.offset_i));
        // `divide_by: 1` is a gain of 1 (decision D-133).
        const std::int64_t gain =
            scale.form == ScaleRule::Form::GAIN ? scale.gain_i : 1;
        raw = difference.has_value()
                  ? detail::wide_divide_exact(*difference,
                                              detail::wide_from_signed(gain))
                  : std::nullopt;
    }
    if (!raw.has_value()) {
        return std::nullopt;
    }
    const std::optional<RawValue> encoded = raw_from_integer(rules, *raw);
    if (!encoded.has_value()) {
        return std::nullopt;
    }
    return WriteValue{.raw = *encoded, .written = value};
}

// The engineering value a raw number reads back as on the binary64 path
// (contract 5.1, step 4): two operations, contraction off.
double read_back(const WriteRules& rules, double raw) noexcept {
    if (rules.scale == nullptr) {
        return raw;
    }
    const ScaleRule& scale = *rules.scale;
    if (scale.form == ScaleRule::Form::GAIN) {
        const double product = raw * scale.gain;
        return product + scale.offset;
    }
    const double quotient = raw / static_cast<double>(scale.divide_by);
    return quotient + scale.offset;
}

// A raw number of the binary64 path as the raw type: rounded half away
// from zero for an integer type, nearest for float32; with the number
// actually sent.
struct Encoded {
    RawValue raw{};
    double sent = 0.0;
};

std::optional<Encoded> encode_binary(const WriteRules& rules,
                                     double raw) noexcept {
    switch (rules.raw_kind) {
        case RawKind::SIGNED: {
            // Half away from zero, then the raw type's range: [-2^(w-1),
            // 2^(w-1)), both bounds exact in binary64.
            const double rounded = std::round(raw);
            const double limit = power_of_two(rules.raw_width - 1U);
            if (rounded < -limit || rounded >= limit) {
                return std::nullopt;
            }
            // `sent` is the integer as the read sees it: a request that
            // rounds to -0.0 is sent as 0, which reads back as +0.0.
            const auto integer = static_cast<std::int64_t>(rounded);
            return Encoded{.raw = raw_signed(integer, rules.raw_width),
                           .sent = static_cast<double>(integer)};
        }
        case RawKind::UNSIGNED: {
            const double rounded = std::round(raw);
            if (rounded < 0.0 || rounded >= power_of_two(rules.raw_width)) {
                return std::nullopt;
            }
            const auto integer = static_cast<std::uint64_t>(rounded);
            return Encoded{.raw = raw_unsigned(integer, rules.raw_width),
                           .sent = static_cast<double>(integer)};
        }
        case RawKind::BOOL: {
            const double rounded = std::round(raw);
            if (rounded != 0.0 && rounded != 1.0) {
                return std::nullopt;
            }
            return Encoded{.raw = raw_bool(rounded == 1.0),
                           .sent = rounded == 1.0 ? 1.0 : 0.0};
        }
        case RawKind::FLOAT:
            if (rules.raw_width == FLOAT32_WIDTH) {
                // Nearest, ties to even; a magnitude that would round to
                // infinity is refused before the conversion.
                if (std::fabs(raw) >= FLOAT32_OVERFLOW) {
                    return std::nullopt;
                }
                const auto narrow = static_cast<float>(raw);
                return Encoded{.raw = raw_float32(narrow),
                               .sent = static_cast<double>(narrow)};
            }
            return Encoded{.raw = raw_float64(raw), .sent = raw};
        case RawKind::STRING:
        case RawKind::TIME:
            return std::nullopt;
    }
    return std::nullopt;
}

// The binary64 path of contract 8.4, contraction off (decision D-98), for a
// FLOAT64 tag.
std::optional<WriteValue> binary64_path(const WriteRules& rules,
                                        double requested) noexcept {
    if (!std::isfinite(requested)) {
        return std::nullopt;
    }
    double raw = requested;
    if (rules.scale != nullptr) {
        const ScaleRule& scale = *rules.scale;
        const double difference = requested - scale.offset;
        if (scale.form == ScaleRule::Form::GAIN) {
            if (scale.gain == 0.0) {
                return std::nullopt;
            }
            raw = difference / scale.gain;
        } else {
            raw = difference * static_cast<double>(scale.divide_by);
        }
    }
    if (!std::isfinite(raw)) {
        return std::nullopt;
    }
    const std::optional<Encoded> encoded = encode_binary(rules, raw);
    if (!encoded.has_value()) {
        return std::nullopt;
    }
    const double written = read_back(rules, encoded->sent);
    if (!std::isfinite(written)) {
        return std::nullopt;
    }
    return WriteValue{.raw = encoded->raw, .written = float64_value(written)};
}

}  // namespace

bool is_integer_path(const WriteRules& rules) noexcept {
    return is_integer_tag(rules.tag_type) &&
           (rules.scale == nullptr || is_integer_scaling(*rules.scale));
}

std::optional<WriteValue> to_raw_value(const WriteRules& rules,
                                       const TagValue& value) noexcept {
    if (value.type != rules.tag_type ||
        !width_ok(rules.raw_kind, rules.raw_width)) {
        return std::nullopt;
    }
    switch (rules.tag_type) {
        case TagType::BOOL: {
            const bool device_bit = value.b != rules.invert;
            const std::optional<RawValue> raw = raw_from_integer(
                rules, detail::wide_from_unsigned(device_bit ? 1U : 0U));
            if (!raw.has_value()) {
                return std::nullopt;
            }
            return WriteValue{.raw = *raw, .written = value};
        }
        case TagType::STRING:
            return std::nullopt;
        case TagType::FLOAT64:
            if (rules.has_enum) {
                return std::nullopt;  // an enumeration is an INT64 tag
            }
            return binary64_path(rules, value.f);
        case TagType::INT64:
        case TagType::UINT64:
            break;
    }
    if (rules.has_enum) {
        // An enumeration writes its code (CS 7.2), which must be defined; it
        // has no scale, so that the read of the raw code is the code.
        if (rules.tag_type != TagType::INT64 || rules.scale != nullptr ||
            !std::binary_search(rules.enum_codes.begin(),
                                rules.enum_codes.end(), value.i)) {
            return std::nullopt;
        }
        return integer_path(rules, value);
    }
    if (rules.raw_kind == RawKind::FLOAT) {
        // A floating raw type on an integer value type is CF-504 (decision
        // D-134); only an enumeration writes its code to one.
        return std::nullopt;
    }
    if (!is_integer_path(rules)) {
        // An integer tag with any other scaling is CF-517 (decision D-133):
        // refused, never converted through binary64.
        return std::nullopt;
    }
    return integer_path(rules, value);
}

std::optional<RawValue> raw_from_pattern(RawKind kind, std::uint8_t width,
                                         std::uint64_t pattern) noexcept {
    if (!width_ok(kind, width) || (pattern & ~width_mask(width)) != 0) {
        return std::nullopt;
    }
    switch (kind) {
        case RawKind::BOOL:
            return raw_bool(pattern == 1U);
        case RawKind::UNSIGNED:
            return raw_unsigned(pattern, width);
        case RawKind::SIGNED: {
            // Sign-extend from the width: the pattern is two's complement.
            const std::uint64_t sign = std::uint64_t{1} << (width - 1U);
            const std::uint64_t extended = (pattern & sign) != 0
                                               ? (pattern | ~width_mask(width))
                                               : pattern;
            RawValue raw =
                raw_signed(std::bit_cast<std::int64_t>(extended), width);
            raw.pattern = pattern;
            return raw;
        }
        case RawKind::FLOAT: {
            // The pattern as it stands: a NaN payload is kept in `pattern`.
            RawValue raw = width == FLOAT32_WIDTH
                               ? raw_float32(std::bit_cast<float>(
                                     static_cast<std::uint32_t>(pattern)))
                               : raw_float64(std::bit_cast<double>(pattern));
            raw.pattern = pattern;
            return raw;
        }
        case RawKind::STRING:
        case RawKind::TIME:
            return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace scada::driver
