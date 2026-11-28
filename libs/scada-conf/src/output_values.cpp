// The values an output writes, against its target's types (configuration
// schemas contract 2.3 and 7.2; decision D-124).

#include "output_values.h"

#include "scada/conf/scalar.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace scada::conf {

namespace {

constexpr std::uint64_t DECIMAL = 10;
// An exponent beyond this is as good as infinite for an order: no digit
// string of a tree comes near it (the scalar limit of 1.3).
constexpr std::int64_t EXPONENT_CAP = 1'000'000'000'000;

// A number as written, exactly: `digits` times ten to `exponent`, with no
// leading or trailing zero in `digits`; empty digits for zero.
struct Decimal {
    bool negative = false;
    std::string digits;
    std::int64_t exponent = 0;
};

[[nodiscard]] bool is_digit(char chr) {
    return chr >= '0' && chr <= '9';
}

// A decimal of the integer or decimal form of 1.2 (hex as an integer);
// nothing for another text.
[[nodiscard]] std::optional<Decimal> decimal_of(std::string_view text) {
    std::string hex_digits;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        // A hex integer is read as the decimal digits of its value.
        const std::optional<Integer> value = parse_integer(text);
        if (!value) {
            return std::nullopt;
        }
        hex_digits = std::to_string(value->magnitude);
        text = hex_digits;
    }
    if (!has_decimal_form(text)) {
        return std::nullopt;
    }
    Decimal result;
    std::size_t pos = 0;
    if (text[pos] == '-') {
        result.negative = true;
        ++pos;
    }
    std::string digits;
    std::int64_t exponent = 0;
    bool fraction = false;
    for (; pos < text.size() && text[pos] != 'e' && text[pos] != 'E'; ++pos) {
        if (text[pos] == '.') {
            fraction = true;
            continue;
        }
        digits += text[pos];
        if (fraction) {
            --exponent;
        }
    }
    if (pos < text.size()) {
        ++pos;  // the `e`
        bool minus = false;
        if (text[pos] == '-' || text[pos] == '+') {
            minus = text[pos] == '-';
            ++pos;
        }
        std::int64_t written = 0;
        for (; pos < text.size() && is_digit(text[pos]); ++pos) {
            if (written < EXPONENT_CAP) {
                written = written * static_cast<std::int64_t>(DECIMAL) +
                          (text[pos] - '0');
            }
        }
        exponent += minus ? -written : written;
    }
    const std::size_t first = digits.find_first_not_of('0');
    if (first == std::string::npos) {
        return Decimal{};  // zero, -0 included
    }
    digits.erase(0, first);
    const std::size_t last = digits.find_last_not_of('0');
    exponent += static_cast<std::int64_t>(digits.size() - 1 - last);
    digits.erase(last + 1);
    result.digits = std::move(digits);
    result.exponent = exponent;
    return result;
}

// The order of the magnitudes of two decimals.
[[nodiscard]] int compare_magnitudes(const Decimal& lhs, const Decimal& rhs) {
    if (lhs.digits.empty() || rhs.digits.empty()) {
        return static_cast<int>(!lhs.digits.empty()) -
               static_cast<int>(!rhs.digits.empty());
    }
    // The power of ten of the leading digit.
    const std::int64_t lhs_top =
        static_cast<std::int64_t>(lhs.digits.size()) + lhs.exponent;
    const std::int64_t rhs_top =
        static_cast<std::int64_t>(rhs.digits.size()) + rhs.exponent;
    if (lhs_top != rhs_top) {
        return lhs_top < rhs_top ? -1 : 1;
    }
    const std::size_t length = std::max(lhs.digits.size(), rhs.digits.size());
    for (std::size_t i = 0; i < length; ++i) {
        const char lhs_digit = i < lhs.digits.size() ? lhs.digits[i] : '0';
        const char rhs_digit = i < rhs.digits.size() ? rhs.digits[i] : '0';
        if (lhs_digit != rhs_digit) {
            return lhs_digit < rhs_digit ? -1 : 1;
        }
    }
    return 0;
}

[[nodiscard]] int compare(const Decimal& lhs, const Decimal& rhs) {
    const bool lhs_negative = lhs.negative && !lhs.digits.empty();
    const bool rhs_negative = rhs.negative && !rhs.digits.empty();
    if (lhs_negative != rhs_negative) {
        return lhs_negative ? -1 : 1;
    }
    const int magnitudes = compare_magnitudes(lhs, rhs);
    return lhs_negative ? -magnitudes : magnitudes;
}

// The decimal as an Integer, if it is a whole number of magnitude pos most
// 2^64 - 1.
[[nodiscard]] std::optional<Integer> integer_of(const Decimal& value) {
    if (value.digits.empty()) {
        return Integer{};
    }
    constexpr std::int64_t MAX_DIGITS = 20;  // 2^64 - 1 has 20
    if (value.exponent < 0 ||
        static_cast<std::int64_t>(value.digits.size()) + value.exponent >
            MAX_DIGITS) {
        return std::nullopt;
    }
    std::uint64_t magnitude = 0;
    const auto push = [&magnitude](unsigned digit) {
        if (magnitude >
            (std::numeric_limits<std::uint64_t>::max() - digit) / DECIMAL) {
            return false;
        }
        magnitude = magnitude * DECIMAL + digit;
        return true;
    };
    for (const char digit : value.digits) {
        if (!push(static_cast<unsigned>(digit - '0'))) {
            return std::nullopt;
        }
    }
    for (std::int64_t i = 0; i < value.exponent; ++i) {
        if (!push(0)) {
            return std::nullopt;
        }
    }
    return Integer{.negative = value.negative, .magnitude = magnitude};
}

[[nodiscard]] std::optional<Integer> exact_integer(std::string_view text) {
    const std::optional<Decimal> value = decimal_of(text);
    return value ? integer_of(*value) : std::nullopt;
}

constexpr std::uint64_t INT64_MAGNITUDE_MAX =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

[[nodiscard]] bool fits_int64(const Integer& value) {
    return value.negative ? value.magnitude <= INT64_MAGNITUDE_MAX + 1
                          : value.magnitude <= INT64_MAGNITUDE_MAX;
}

[[nodiscard]] bool fits_uint64(const Integer& value) {
    return !value.negative || value.magnitude == 0;
}

// The range of an integer raw type, as the magnitudes of its bounds.
struct RawRange {
    std::uint64_t below = 0;  // the magnitude of the least value, negated
    std::uint64_t above = 0;  // the greatest value
};

[[nodiscard]] std::optional<RawRange> integer_raw_range(std::string_view raw) {
    constexpr std::uint64_t BITS_8 = 8;
    constexpr std::uint64_t BITS_16 = 16;
    constexpr std::uint64_t BITS_32 = 32;
    const auto unsigned_of = [](std::uint64_t bits) {
        return RawRange{.below = 0, .above = (std::uint64_t{1} << bits) - 1};
    };
    const auto signed_of = [](std::uint64_t bits) {
        return RawRange{.below = std::uint64_t{1} << (bits - 1),
                        .above = (std::uint64_t{1} << (bits - 1)) - 1};
    };
    if (raw == "bool") {
        return RawRange{.below = 0, .above = 1};
    }
    if (raw == "uint8") {
        return unsigned_of(BITS_8);
    }
    if (raw == "int8") {
        return signed_of(BITS_8);
    }
    if (raw == "uint16") {
        return unsigned_of(BITS_16);
    }
    if (raw == "int16") {
        return signed_of(BITS_16);
    }
    if (raw == "uint32") {
        return unsigned_of(BITS_32);
    }
    if (raw == "int32") {
        return signed_of(BITS_32);
    }
    if (raw == "uint64") {
        return RawRange{.below = 0,
                        .above = std::numeric_limits<std::uint64_t>::max()};
    }
    if (raw == "int64") {
        return RawRange{.below = INT64_MAGNITUDE_MAX + 1,
                        .above = INT64_MAGNITUDE_MAX};
    }
    return std::nullopt;
}

[[nodiscard]] bool in_range(const Integer& value, const RawRange& range) {
    return value.negative ? value.magnitude <= range.below
                          : value.magnitude <= range.above;
}

// `lhs - rhs` in sign and magnitude, nothing when its magnitude passes
// 2^64 - 1 (7.2: "v - offset must not overflow").
[[nodiscard]] std::optional<Integer> subtract(const Integer& lhs,
                                              const Integer& rhs) {
    const bool rhs_negated = !rhs.negative;
    if (lhs.negative == rhs_negated) {  // same signs: add the magnitudes
        if (lhs.magnitude >
            std::numeric_limits<std::uint64_t>::max() - rhs.magnitude) {
            return std::nullopt;
        }
        return Integer{.negative = lhs.negative,
                       .magnitude = lhs.magnitude + rhs.magnitude};
    }
    if (lhs.magnitude >= rhs.magnitude) {
        return Integer{.negative = lhs.negative,
                       .magnitude = lhs.magnitude - rhs.magnitude};
    }
    return Integer{.negative = rhs_negated,
                   .magnitude = rhs.magnitude - lhs.magnitude};
}

// The exact path of 7.2: (v - offset) / gain, which must divide exactly.
[[nodiscard]] bool exact_survives(std::string_view value,
                                  const Scaling& scaling,
                                  const RawRange& range) {
    const std::optional<Integer> written = exact_integer(value);
    const std::optional<Integer> offset =
        scaling.offset.empty() ? std::optional<Integer>(Integer{})
                               : exact_integer(scaling.offset);
    const std::optional<Integer> gain =
        scaling.gain.empty()
            ? std::optional<Integer>(Integer{.negative = false, .magnitude = 1})
            : exact_integer(scaling.gain);
    if (!written || !offset || !gain || gain->magnitude == 0) {
        return false;
    }
    const std::optional<Integer> difference = subtract(*written, *offset);
    if (!difference || difference->magnitude % gain->magnitude != 0) {
        return false;
    }
    const Integer raw{.negative = difference->negative != gain->negative,
                      .magnitude = difference->magnitude / gain->magnitude};
    return in_range(raw, range);
}

[[nodiscard]] bool is_integer_text(std::string_view text) {
    return read_integer(text).form != IntegerForm::NOT_INTEGER;
}

// The binary64 path of 7.2.
[[nodiscard]] bool binary64_survives(std::string_view value,
                                     std::string_view raw,
                                     const Scaling& scaling) {
    const std::optional<double> written = nearest_binary64(value);
    if (!written) {
        return value == "0" || value == "-0" || value == "0.0";
    }
    const double offset = scaling.offset.empty()
                              ? 0.0
                              : nearest_binary64(scaling.offset).value_or(0.0);
    double result = *written - offset;
    if (!scaling.divide_by.empty()) {
        result *= nearest_binary64(scaling.divide_by).value_or(1.0);
    } else if (!scaling.gain.empty()) {
        const double gain = nearest_binary64(scaling.gain).value_or(1.0);
        if (gain == 0.0) {
            return false;
        }
        result /= gain;
    }
    if (!std::isfinite(result)) {
        return false;
    }
    if (raw == "float64") {
        return true;
    }
    if (raw == "float32") {
        return std::fabs(result) <=
               static_cast<double>(std::numeric_limits<float>::max());
    }
    const std::optional<RawRange> range = integer_raw_range(raw);
    if (!range) {
        return true;  // not counted here (CF-708)
    }
    const double rounded = std::round(result);  // half away from zero
    // Each bound is an integer; 2^63 and 2^64 are exact in binary64, so
    // the compares below are exact for an integer-valued `rounded`.
    constexpr double TWO_TO_63 = 9223372036854775808.0;
    constexpr double TWO_TO_64 = 18446744073709551616.0;
    double above = static_cast<double>(range->above) + 1.0;
    if (range->above == std::numeric_limits<std::uint64_t>::max()) {
        above = TWO_TO_64;
    } else if (range->above == INT64_MAGNITUDE_MAX) {
        above = TWO_TO_63;
    }
    const double below = -static_cast<double>(range->below);
    return rounded < above && rounded >= below;
}

}  // namespace

bool fits_value_type(std::string_view value, std::string_view value_type) {
    const bool boolean = value == "true" || value == "false";
    if (value_type == "bool" || boolean) {
        return value_type == "bool" && boolean;
    }
    if (value_type == "float64") {
        if (!is_integer_text(value)) {
            return nearest_binary64(value).has_value() || value == "0.0";
        }
        const std::optional<Integer> integer = parse_integer(value);
        if (!integer) {
            return false;  // past 64 bits: refused, as no compare is cheap
        }
        constexpr int SIGNIFICAND_BITS = 53;
        const std::uint64_t odd =
            integer->magnitude == 0
                ? 0
                : integer->magnitude >> std::countr_zero(integer->magnitude);
        return std::bit_width(odd) <= SIGNIFICAND_BITS;
    }
    const std::optional<Integer> integer = exact_integer(value);
    if (!integer) {
        return false;
    }
    if (value_type == "int64") {
        return fits_int64(*integer);
    }
    if (value_type == "uint64") {
        return fits_uint64(*integer);
    }
    return false;
}

std::optional<int> compare_numbers(std::string_view lhs, std::string_view rhs) {
    const std::optional<Decimal> left = decimal_of(lhs);
    const std::optional<Decimal> right = decimal_of(rhs);
    if (!left || !right) {
        return std::nullopt;
    }
    return compare(*left, *right);
}

bool fits_raw_width(std::string_view pattern, std::string_view raw) {
    constexpr std::uint64_t WIDTH_8 = 8;
    constexpr std::uint64_t WIDTH_16 = 16;
    constexpr std::uint64_t WIDTH_32 = 32;
    constexpr std::uint64_t WIDTH_64 = 64;
    std::uint64_t bits = 0;
    if (raw == "bool") {
        bits = 1;
    } else if (raw == "int8" || raw == "uint8") {
        bits = WIDTH_8;
    } else if (raw == "int16" || raw == "uint16") {
        bits = WIDTH_16;
    } else if (raw == "int32" || raw == "uint32" || raw == "float32") {
        bits = WIDTH_32;
    } else if (raw == "int64" || raw == "uint64" || raw == "float64") {
        bits = WIDTH_64;
    } else {
        return false;
    }
    const std::optional<Integer> value = parse_integer(pattern);
    if (!value) {
        return false;
    }
    constexpr std::uint64_t ALL_BITS = 64;
    if (value->negative) {
        return value->magnitude <= (std::uint64_t{1} << (bits - 1));
    }
    return bits == ALL_BITS || value->magnitude < (std::uint64_t{1} << bits);
}

bool survives_scaling(std::string_view value, std::string_view value_type,
                      std::string_view raw, const Scaling& scaling) {
    if (value_type == "bool") {
        return true;
    }
    const std::optional<RawRange> range = integer_raw_range(raw);
    const bool integer_scaling =
        (scaling.gain.empty() || is_integer_text(scaling.gain)) &&
        (scaling.offset.empty() || is_integer_text(scaling.offset)) &&
        (scaling.divide_by.empty() || scaling.divide_by == "1");
    if ((value_type == "int64" || value_type == "uint64") && integer_scaling &&
        range) {
        return exact_survives(value, scaling, *range);
    }
    return binary64_survives(value, raw, scaling);
}

}  // namespace scada::conf
