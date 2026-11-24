#include "scada/conf/scalar.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace scada::conf {

namespace {

constexpr std::uint64_t INT64_MAX_MAGNITUDE =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
constexpr std::uint64_t BASE_TEN = 10;
constexpr unsigned BITS_PER_HEX_DIGIT = 4;
constexpr std::uint64_t TOP_HEX_DIGIT_MASK = 0xF000000000000000ULL;
constexpr std::uint64_t HEX_LETTER_OFFSET = 10;

bool is_digit(char chr) noexcept {
    return chr >= '0' && chr <= '9';
}

// `0|[1-9][0-9]*`: the length of the digits at the start of `text`, or 0.
std::size_t whole_number_length(std::string_view text) noexcept {
    if (text.empty() || !is_digit(text.front())) {
        return 0;
    }
    if (text.front() == '0') {
        return 1;
    }
    std::size_t length = 0;
    while (length < text.size() && is_digit(text[length])) {
        ++length;
    }
    return length;
}

// Decimal digits to a uint64; nothing on overflow.
std::optional<std::uint64_t> decimal_value(std::string_view digits) noexcept {
    std::uint64_t value = 0;
    for (const char chr : digits) {
        const auto digit = static_cast<std::uint64_t>(chr - '0');
        if (value >
            (std::numeric_limits<std::uint64_t>::max() - digit) / BASE_TEN) {
            return std::nullopt;
        }
        value = (value * BASE_TEN) + digit;
    }
    return value;
}

std::optional<std::uint64_t> hex_digit(char chr) noexcept {
    if (chr >= '0' && chr <= '9') {
        return static_cast<std::uint64_t>(chr - '0');
    }
    if (chr >= 'a' && chr <= 'f') {
        return static_cast<std::uint64_t>(chr - 'a') + HEX_LETTER_OFFSET;
    }
    if (chr >= 'A' && chr <= 'F') {
        return static_cast<std::uint64_t>(chr - 'A') + HEX_LETTER_OFFSET;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> hex_value(std::string_view digits) noexcept {
    if (digits.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char chr : digits) {
        const std::optional<std::uint64_t> digit = hex_digit(chr);
        if (!digit || (value & TOP_HEX_DIGIT_MASK) != 0) {
            return std::nullopt;
        }
        value = (value << BITS_PER_HEX_DIGIT) | *digit;
    }
    return value;
}

// True if `text` matches the decimal grammar of section 1.2.
bool is_decimal_text(std::string_view text) noexcept {
    std::size_t pos = 0;
    const auto digits_from = [&text, &pos]() {
        const std::size_t start = pos;
        while (pos < text.size() && is_digit(text[pos])) {
            ++pos;
        }
        return pos > start;
    };
    if (pos < text.size() && text[pos] == '-') {
        ++pos;
    }
    const std::size_t whole = whole_number_length(text.substr(pos));
    if (whole == 0) {
        return false;
    }
    pos += whole;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        if (!digits_from()) {
            return false;
        }
    }
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        ++pos;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
            ++pos;
        }
        if (!digits_from()) {
            return false;
        }
    }
    return pos == text.size();
}

// True if a digit other than 0 stands before any exponent of `text`.
bool has_nonzero_digit(std::string_view text) noexcept {
    for (const char chr : text) {
        if (chr == 'e' || chr == 'E') {
            return false;
        }
        if (chr >= '1' && chr <= '9') {
            return true;
        }
    }
    return false;
}

// Splits `text` into a whole number and a unit from `units`, each with its
// factor; the value times the factor, or nothing on a bad form or overflow.
template <std::size_t N>
std::optional<std::uint64_t> number_with_unit(
    std::string_view text,
    const std::array<std::pair<std::string_view, std::uint64_t>, N>& units) {
    const std::size_t digits = whole_number_length(text);
    if (digits == 0) {
        return std::nullopt;
    }
    const std::string_view unit = text.substr(digits);
    for (const auto& [name, factor] : units) {
        if (unit != name) {
            continue;
        }
        const std::optional<std::uint64_t> value =
            decimal_value(text.substr(0, digits));
        if (!value ||
            *value > std::numeric_limits<std::uint64_t>::max() / factor) {
            return std::nullopt;
        }
        return *value * factor;
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::int64_t> as_int64(const Integer& value) noexcept {
    if (!value.negative) {
        if (value.magnitude > INT64_MAX_MAGNITUDE) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(value.magnitude);
    }
    if (value.magnitude > INT64_MAX_MAGNITUDE + 1) {
        return std::nullopt;
    }
    if (value.magnitude == INT64_MAX_MAGNITUDE + 1) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(value.magnitude);
}

std::optional<std::uint64_t> as_uint64(const Integer& value) noexcept {
    if (value.negative && value.magnitude != 0) {
        return std::nullopt;
    }
    return value.magnitude;
}

IntegerRead read_integer(std::string_view text) {
    if (text.starts_with("0x")) {
        const std::string_view digits = text.substr(2);
        if (digits.empty() || !std::ranges::all_of(digits, [](char chr) {
                return hex_digit(chr).has_value();
            })) {
            return {};
        }
        const std::optional<std::uint64_t> value = hex_value(digits);
        if (!value) {
            return IntegerRead{.form = IntegerForm::TOO_LARGE, .value = {}};
        }
        return IntegerRead{
            .form = IntegerForm::FITS,
            .value = Integer{.negative = false, .magnitude = *value}};
    }
    const bool negative = text.starts_with('-');
    const std::string_view digits = negative ? text.substr(1) : text;
    if (digits.empty() || whole_number_length(digits) != digits.size()) {
        return {};
    }
    const std::optional<std::uint64_t> value = decimal_value(digits);
    if (!value) {
        return IntegerRead{.form = IntegerForm::TOO_LARGE, .value = {}};
    }
    return IntegerRead{
        .form = IntegerForm::FITS,
        .value = Integer{.negative = negative, .magnitude = *value}};
}

std::optional<Integer> parse_integer(std::string_view text) {
    const IntegerRead read = read_integer(text);
    if (read.form != IntegerForm::FITS) {
        return std::nullopt;
    }
    return read.value;
}

bool has_decimal_form(std::string_view text) noexcept {
    return is_decimal_text(text);
}

std::optional<double> parse_decimal(std::string_view text) {
    if (!is_decimal_text(text)) {
        return std::nullopt;
    }
    double value = 0.0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    // An underflow or overflow is reported as out of range; both are refused.
    if (error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    if (value == 0.0 && has_nonzero_digit(text)) {
        return std::nullopt;
    }
    return value;
}

std::optional<Number> parse_number(std::string_view text) {
    const IntegerRead integer = read_integer(text);
    if (integer.form == IntegerForm::FITS) {
        return Number{integer.value};
    }
    if (integer.form == IntegerForm::TOO_LARGE) {
        return std::nullopt;
    }
    if (const std::optional<double> decimal = parse_decimal(text)) {
        return Number{*decimal};
    }
    return std::nullopt;
}

std::optional<bool> parse_boolean(std::string_view text) {
    if (text == "true") {
        return true;
    }
    if (text == "false") {
        return false;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> parse_duration_ms(std::string_view text) {
    constexpr std::uint64_t SECOND = 1000;
    constexpr std::uint64_t MINUTE = 60 * SECOND;
    constexpr std::uint64_t HOUR = 60 * MINUTE;
    constexpr std::uint64_t DAY = 24 * HOUR;
    constexpr std::uint64_t YEAR = 365 * DAY;
    // "ms" before "m" and "s", so that the longer unit is tried first; the
    // comparison is exact, so the order does not change the result.
    constexpr std::array<std::pair<std::string_view, std::uint64_t>, 6> UNITS{
        {{"ms", 1},
         {"s", SECOND},
         {"m", MINUTE},
         {"h", HOUR},
         {"d", DAY},
         {"y", YEAR}}};
    return number_with_unit(text, UNITS);
}

std::optional<std::uint64_t> parse_size_bytes(std::string_view text) {
    constexpr std::uint64_t KIB = 1024;
    constexpr std::uint64_t MIB = KIB * KIB;
    constexpr std::uint64_t GIB = KIB * MIB;
    constexpr std::uint64_t TIB = KIB * GIB;
    constexpr std::array<std::pair<std::string_view, std::uint64_t>, 5> UNITS{
        {{"B", 1}, {"KiB", KIB}, {"MiB", MIB}, {"GiB", GIB}, {"TiB", TIB}}};
    return number_with_unit(text, UNITS);
}

std::optional<double> parse_percentage(std::string_view text) {
    if (!text.ends_with('%')) {
        return std::nullopt;
    }
    return parse_decimal(text.substr(0, text.size() - 1));
}

}  // namespace scada::conf
