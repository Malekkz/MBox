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
//   quoted scalar for a number or a boolean.

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/conf/node.h"
#include "scada/conf/read.h"
#include "scada/conf/scalar.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {

using scada::conf::IntegerForm;

constexpr unsigned MAX_WIDTH = 64;
constexpr std::array<unsigned, 6> WIDTHS = {1, 8, 16, 32, 63, MAX_WIDTH};
constexpr std::string_view UINT64_MAX_DIGITS = "18446744073709551615";
constexpr std::size_t MAX_HEX_DIGITS = 16;

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
    check_reads(text);
    return 0;
}
