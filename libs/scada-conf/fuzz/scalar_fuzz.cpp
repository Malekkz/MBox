// Fuzz target: the scalar text forms (configuration schemas contract 1.2,
// 2.2; scalar.h) and the typed reads of a node (read.h). The input is one
// scalar's text. The oracle is the grammar of 1.2, written here a second time
// without the library's code:
//
// - an integer is `-?(0|[1-9][0-9]*)` or `0x[0-9a-fA-F]+`; read_integer says
//   so, and says FITS exactly when the magnitude is at most 2^64 - 1;
// - parse_number never reads an integer of the form as a decimal;
// - has_decimal_form matches `-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][-+]?[0-9]+)?`;
// - every to_* agrees with the parse_* it reads through, and never takes a
//   quoted scalar for a number or a boolean;
// - nearest_binary64 is parse_decimal on the decimal form, and on the hex
//   integer form the correctly rounded value std::from_chars reads in its
//   hex format;
// - read_resolution_ns and is_device_path follow their grammars (Annex C),
//   written here again.

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/conf/names.h"
#include "scada/conf/node.h"
#include "scada/conf/read.h"
#include "scada/conf/scalar.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace {

using scada::conf::IntegerForm;

constexpr unsigned MAX_WIDTH = 64;
constexpr std::array<unsigned, 6> WIDTHS = {1, 8, 16, 32, 63, MAX_WIDTH};
constexpr std::string_view UINT64_MAX_DIGITS = "18446744073709551615";
constexpr std::size_t MAX_HEX_DIGITS = 16;
constexpr std::string_view INT64_MAX_DIGITS = "9223372036854775807";
constexpr std::uint64_t NS_PER_MS = 1000000;
constexpr std::uint64_t NS_PER_S = 1000000000;
constexpr std::size_t MAX_DEVICE_PATH = 255;

void check(bool condition) {
    if (!condition) {
        std::abort();
    }
}

bool digit(char chr) {
    return chr >= '0' && chr <= '9';
}

bool hex(char chr) {
    return digit(chr) || (chr >= 'a' && chr <= 'f') ||
           (chr >= 'A' && chr <= 'F');
}

// `0|[1-9][0-9]*` from `pos`; the position after it, or `npos`.
std::size_t whole(std::string_view text, std::size_t pos) {
    if (pos >= text.size() || !digit(text[pos])) {
        return std::string_view::npos;
    }
    if (text[pos] == '0') {
        return pos + 1;
    }
    while (pos < text.size() && digit(text[pos])) {
        ++pos;
    }
    return pos;
}

// The oracle's reading of the integer form.
std::optional<bool> oracle_integer_fits(std::string_view text) {
    if (text.starts_with("0x")) {
        const std::string_view digits = text.substr(2);
        if (digits.empty()) {
            return std::nullopt;
        }
        std::size_t significant = 0;
        bool leading = true;
        for (const char chr : digits) {
            if (!hex(chr)) {
                return std::nullopt;
            }
            leading = leading && chr == '0';
            if (!leading) {
                ++significant;
            }
        }
        return significant <= MAX_HEX_DIGITS;
    }
    const std::size_t start = text.starts_with('-') ? 1 : 0;
    if (whole(text, start) != text.size()) {
        return std::nullopt;
    }
    const std::string_view digits = text.substr(start);
    // No leading zeros by the grammar, so length then text order is value
    // order.
    return digits.size() < UINT64_MAX_DIGITS.size() ||
           (digits.size() == UINT64_MAX_DIGITS.size() &&
            digits <= UINT64_MAX_DIGITS);
}

bool oracle_decimal_form(std::string_view text) {
    std::size_t pos = text.starts_with('-') ? 1 : 0;
    pos = whole(text, pos);
    if (pos == std::string_view::npos) {
        return false;
    }
    if (pos < text.size() && text[pos] == '.') {
        const std::size_t start = ++pos;
        while (pos < text.size() && digit(text[pos])) {
            ++pos;
        }
        if (pos == start) {
            return false;
        }
    }
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        ++pos;
        if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) {
            ++pos;
        }
        const std::size_t start = pos;
        while (pos < text.size() && digit(text[pos])) {
            ++pos;
        }
        if (pos == start) {
            return false;
        }
    }
    return pos == text.size();
}

// nearest_binary64 against a second reading of the same value.
void check_nearest(std::string_view text) {
    const std::optional<double> nearest = scada::conf::nearest_binary64(text);
    if (oracle_decimal_form(text)) {
        const std::optional<double> decimal = scada::conf::parse_decimal(text);
        check(nearest.has_value() == decimal.has_value());
        if (nearest && decimal) {
            check(*nearest == *decimal);
        }
        return;
    }
    const std::optional<bool> hex_form =
        text.starts_with("0x") ? oracle_integer_fits(text) : std::nullopt;
    if (!hex_form) {
        check(!nearest);
        return;
    }
    const std::string_view digits = text.substr(2);
    double value = 0;
    const std::from_chars_result read =
        std::from_chars(digits.data(), digits.data() + digits.size(), value,
                        std::chars_format::hex);
    check(read.ptr == digits.data() + digits.size() ||
          read.ec == std::errc::result_out_of_range);
    if (read.ec == std::errc::result_out_of_range || !std::isfinite(value)) {
        check(!nearest);
        return;
    }
    check(nearest.has_value() && *nearest == value);
}

// `1s`, `1ms` or `[1-9][0-9]*ns` up to int64 nanoseconds.
void check_resolution(std::string_view text) {
    const scada::conf::UnitRead read = scada::conf::read_resolution_ns(text);
    if (text == "1s" || text == "1ms") {
        check(read.form == IntegerForm::FITS &&
              read.value == (text == "1s" ? NS_PER_S : NS_PER_MS));
        return;
    }
    const bool form = text.size() > 2 && text.ends_with("ns") &&
                      text.front() != '0' && whole(text, 0) == text.size() - 2;
    if (!form) {
        check(read.form == IntegerForm::NOT_INTEGER);
        return;
    }
    const std::string_view digits = text.substr(0, text.size() - 2);
    const bool fits = digits.size() < INT64_MAX_DIGITS.size() ||
                      (digits.size() == INT64_MAX_DIGITS.size() &&
                       digits <= INT64_MAX_DIGITS);
    check(read.form == (fits ? IntegerForm::FITS : IntegerForm::TOO_LARGE));
    if (fits) {
        check(std::to_string(read.value) == digits);
    }
}

// An absolute path under /dev/ of named components, free text.
void check_device_path(std::string_view text) {
    bool valid = text.starts_with("/dev/") && text.size() <= MAX_DEVICE_PATH &&
                 scada::conf::is_free_text(text);
    std::size_t start = 1;
    while (valid) {
        std::size_t end = text.find('/', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view part = text.substr(start, end - start);
        valid = !part.empty() && part != "." && part != "..";
        if (end == text.size()) {
            break;
        }
        start = end + 1;
    }
    check(scada::conf::is_device_path(text) == valid);
}

// A document of one scalar of `text` in `style`.
std::optional<scada::conf::Document> one_scalar(
    std::string_view text, scada::conf::ScalarStyle style) {
    scada::conf::DocumentBuilder builder;
    if (!builder.add_file("f.yaml").ok() ||
        !builder.scalar(text, style, scada::conf::SourcePosition{}).ok()) {
        return std::nullopt;
    }
    scada::Result<scada::conf::Document> built = builder.finish();
    if (!built.ok()) {
        return std::nullopt;
    }
    return std::move(built).value();
}

void check_reads(std::string_view text) {
    using scada::StatusCode;
    using scada::conf::Document;
    using scada::conf::ScalarStyle;
    const std::optional<Document> plain = one_scalar(text, ScalarStyle::PLAIN);
    const std::optional<Document> quoted =
        one_scalar(text, ScalarStyle::DOUBLE_QUOTED);
    check(plain.has_value() && quoted.has_value());
    if (!plain || !quoted) {
        return;
    }
    const scada::conf::NodeId root = plain->root();
    const scada::conf::IntegerRead integer = scada::conf::read_integer(text);

    const auto int64 = scada::conf::to_int64(*plain, root);
    const auto uint64 = scada::conf::to_uint64(*plain, root);
    if (integer.form == IntegerForm::NOT_INTEGER) {
        check(!int64.ok() &&
              int64.status().code() == StatusCode::INVALID_ARGUMENT);
        check(!uint64.ok() &&
              uint64.status().code() == StatusCode::INVALID_ARGUMENT);
    } else if (integer.form == IntegerForm::TOO_LARGE) {
        check(!int64.ok() && int64.status().code() == StatusCode::OUT_OF_RANGE);
        check(!uint64.ok() &&
              uint64.status().code() == StatusCode::OUT_OF_RANGE);
    } else {
        check(int64.ok() == scada::conf::as_int64(integer.value).has_value());
        check(uint64.ok() == scada::conf::as_uint64(integer.value).has_value());
    }
    check(scada::conf::is_integer_form(*plain, root) ==
          (integer.form != IntegerForm::NOT_INTEGER));
    for (const unsigned width : WIDTHS) {
        const auto raw = scada::conf::to_raw_pattern(*plain, root, width);
        if (raw.ok()) {
            check(integer.form == IntegerForm::FITS);
            check(width == MAX_WIDTH ||
                  raw.value() < (std::uint64_t{1} << width));
        }
    }

    // A quoted scalar is never a number or a boolean (1.2).
    check(!scada::conf::to_int64(*quoted, quoted->root()).ok());
    check(!scada::conf::to_decimal(*quoted, quoted->root()).ok());
    check(!scada::conf::to_bool(*quoted, quoted->root()).ok());
    check(!scada::conf::is_integer_form(*quoted, quoted->root()));

    const auto boolean = scada::conf::to_bool(*plain, root);
    check(boolean.ok() == (text == "true" || text == "false"));

    // A duration readable in nanoseconds is one in milliseconds too.
    const auto duration = scada::conf::to_duration_ns(*plain, root);
    if (duration.ok()) {
        check(scada::conf::parse_duration_ms(text).has_value());
        check(duration.value() >= 0);
    }
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    const std::string text(bytes.begin(), bytes.end());

    const std::optional<bool> fits = oracle_integer_fits(text);
    const scada::conf::IntegerRead integer = scada::conf::read_integer(text);
    if (!fits) {
        check(integer.form == IntegerForm::NOT_INTEGER);
    } else {
        check(integer.form ==
              (*fits ? IntegerForm::FITS : IntegerForm::TOO_LARGE));
    }
    check(scada::conf::parse_integer(text).has_value() ==
          (integer.form == IntegerForm::FITS));
    check(scada::conf::has_decimal_form(text) == oracle_decimal_form(text));

    const std::optional<scada::conf::Number> number =
        scada::conf::parse_number(text);
    if (integer.form != IntegerForm::NOT_INTEGER) {
        // An integer of the form is never a decimal.
        check(!number || std::holds_alternative<scada::conf::Integer>(*number));
        check(number.has_value() == (integer.form == IntegerForm::FITS));
    } else if (number) {
        check(std::holds_alternative<double>(*number));
        check(oracle_decimal_form(text));
    }
    if (scada::conf::parse_decimal(text)) {
        check(oracle_decimal_form(text));
    }
    if (const std::optional<double> percent =
            scada::conf::parse_percentage(text)) {
        check(text.ends_with('%'));
    }
    static_cast<void>(scada::conf::parse_size_bytes(text));
    check_nearest(text);
    check_resolution(text);
    check_device_path(text);
    check_reads(text);
    return 0;
}
