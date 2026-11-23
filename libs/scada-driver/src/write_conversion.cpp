#include "scada/driver/write_conversion.h"

#include "checked.h"
#include "scada/driver/point_rules.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/tag_value.h"
#include "scada/tag/types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace scada::driver {

namespace {

using tag::TagType;

constexpr unsigned FLOAT32_WIDTH = 32;
constexpr unsigned FULL_WIDTH = 64;

// 2^n as a binary64, exact for n up to 64.
double power_of_two(unsigned n) noexcept {
    return std::ldexp(1.0, static_cast<int>(n));
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
            if (magnitude >= power_of_two(FULL_WIDTH) ||
                static_cast<std::uint64_t>(magnitude) != value.magnitude) {
                return std::nullopt;
            }
            const double number = value.negative ? -magnitude : magnitude;
            if (rules.raw_width == FLOAT32_WIDTH) {
                const auto narrow = static_cast<float>(number);
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

// The integer path of contract 8.4: exact, every step checked.
std::optional<RawValue> integer_path(const WriteRules& rules,
                                     const TagValue& value) noexcept {
    const detail::Wide requested = rules.tag_type == TagType::INT64
                                       ? detail::wide_from_signed(value.i)
                                       : detail::wide_from_unsigned(value.u);
    if (rules.scale == nullptr) {
        return raw_from_integer(rules, requested);
    }
    const ScaleRule& scale = *rules.scale;
    const std::optional<detail::Wide> difference = detail::wide_subtract(
        requested, detail::wide_from_signed(scale.offset_i));
    if (!difference.has_value()) {
        return std::nullopt;
    }
    const std::optional<detail::Wide> raw =
        scale.form == ScaleRule::Form::GAIN
            ? detail::wide_divide_exact(*difference,
                                        detail::wide_from_signed(scale.gain_i))
            : detail::wide_multiply(
                  *difference, detail::wide_from_unsigned(scale.divide_by));
    if (!raw.has_value()) {
        return std::nullopt;
    }
    return raw_from_integer(rules, *raw);
}

// The binary64 path of contract 8.4, contraction off (decision D-98).
std::optional<RawValue> binary64_path(const WriteRules& rules,
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
    switch (rules.raw_kind) {
        case RawKind::SIGNED: {
            // Half away from zero, then the raw type's range: [-2^(w-1),
            // 2^(w-1)), both bounds exact in binary64.
            const double rounded = std::round(raw);
            const double limit = power_of_two(rules.raw_width - 1U);
            if (rounded < -limit || rounded >= limit) {
                return std::nullopt;
            }
            return raw_signed(static_cast<std::int64_t>(rounded),
                              rules.raw_width);
        }
        case RawKind::UNSIGNED: {
            const double rounded = std::round(raw);
            if (rounded < 0.0 || rounded >= power_of_two(rules.raw_width)) {
                return std::nullopt;
            }
            return raw_unsigned(static_cast<std::uint64_t>(rounded),
                                rules.raw_width);
        }
        case RawKind::BOOL: {
            const double rounded = std::round(raw);
            if (rounded != 0.0 && rounded != 1.0) {
                return std::nullopt;
            }
            return raw_bool(rounded == 1.0);
        }
        case RawKind::FLOAT:
            if (rules.raw_width == FLOAT32_WIDTH) {
                // Nearest, ties to even: the default rounding of the
                // conversion. An overflow gives infinity and is refused.
                const auto narrow = static_cast<float>(raw);
                if (!std::isfinite(narrow)) {
                    return std::nullopt;
                }
                return raw_float32(narrow);
            }
            return raw_float64(raw);
        case RawKind::STRING:
        case RawKind::TIME:
            return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

bool is_integer_path(const WriteRules& rules) noexcept {
    const bool integer_tag =
        rules.tag_type == TagType::INT64 || rules.tag_type == TagType::UINT64;
    return integer_tag && (rules.scale == nullptr ||
                           (rules.scale->integer &&
                            rules.scale->form == ScaleRule::Form::GAIN));
}

std::optional<RawValue> to_raw_value(const WriteRules& rules,
                                     const TagValue& value) noexcept {
    if (value.type != rules.tag_type) {
        return std::nullopt;
    }
    switch (rules.tag_type) {
        case TagType::BOOL: {
            const bool device_bit = value.b != rules.invert;
            return raw_from_integer(
                rules, detail::wide_from_unsigned(device_bit ? 1U : 0U));
        }
        case TagType::STRING:
            return std::nullopt;
        case TagType::FLOAT64:
            return binary64_path(rules, value.f);
        case TagType::INT64:
        case TagType::UINT64:
            break;
    }
    if (rules.has_enum) {
        // An enumeration writes its code (CS 7.2), which must be defined.
        if (rules.tag_type != TagType::INT64 ||
            !std::binary_search(rules.enum_codes.begin(),
                                rules.enum_codes.end(), value.i)) {
            return std::nullopt;
        }
        return raw_from_integer(rules, detail::wide_from_signed(value.i));
    }
    if (is_integer_path(rules)) {
        return integer_path(rules, value);
    }
    if (rules.scale != nullptr) {
        // An integer tag with any other scaling is CF-517 (decision D-133):
        // refused, never converted through binary64.
        return std::nullopt;
    }
    const double requested = rules.tag_type == TagType::INT64
                                 ? static_cast<double>(value.i)
                                 : static_cast<double>(value.u);
    return binary64_path(rules, requested);
}

}  // namespace scada::driver
