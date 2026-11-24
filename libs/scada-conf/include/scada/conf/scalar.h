#ifndef SCADA_CONF_SCALAR_H
#define SCADA_CONF_SCALAR_H

// The text forms of the scalar types of configuration schemas contract 1.2
// and 2.2. Each function reads one scalar's text and gives its value, or
// nothing if the text matches no form of the type (CF-203 or CF-206 for the
// caller). Whether a quoted scalar may have the type is the caller's rule
// (1.2: numbers and booleans are plain only).

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace scada::conf {

// An integer as written: its sign and magnitude, so that every value of
// int64 and uint64 is held (section 2.3).
struct Integer {
    bool negative = false;
    std::uint64_t magnitude = 0;
};

// The value of `value` as int64, if it fits.
[[nodiscard]] std::optional<std::int64_t> as_int64(
    const Integer& value) noexcept;

// The value of `value` as uint64, if it fits (not negative; -0 is 0).
[[nodiscard]] std::optional<std::uint64_t> as_uint64(
    const Integer& value) noexcept;

// How a text reads as an integer (D-107): not of the integer form; of the
// form, with a magnitude of at most 2^64 - 1; of the form but larger. An
// integer of the form that does not fit its key's range is CF-204, never
// CF-203 and never a decimal.
enum class IntegerForm : std::uint8_t { NOT_INTEGER, FITS, TOO_LARGE };

struct IntegerRead {
    IntegerForm form = IntegerForm::NOT_INTEGER;
    // The value when `form` is FITS.
    Integer value;
};

// `-?(0|[1-9][0-9]*)` or `0x[0-9a-fA-F]+` (no sign on hex), of any size.
[[nodiscard]] IntegerRead read_integer(std::string_view text);

// The value of an integer of the form whose magnitude is at most 2^64 - 1;
// nothing for any other text (`read_integer` tells the two apart).
[[nodiscard]] std::optional<Integer> parse_integer(std::string_view text);

// `-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][-+]?[0-9]+)?`, read as the nearest
// binary64. A value that is not finite, or not zero but rounds to zero, is
// refused (1.2: `1e-400`).
[[nodiscard]] std::optional<double> parse_decimal(std::string_view text);

// The nearest binary64 of a text of the integer form (any size, hex too) or
// of the decimal form, for `decimal` keys, which do not keep the written form
// (D-107). Nothing for another text, or a value that is not finite or rounds
// to zero.
[[nodiscard]] std::optional<double> nearest_binary64(std::string_view text);

// True if `text` matches the decimal grammar above, whatever its value: an
// integer of the decimal form matches too.
[[nodiscard]] bool has_decimal_form(std::string_view text) noexcept;

// A number keeps its written form (1.2): `10` is an Integer, `10.0` a double.
using Number = std::variant<Integer, double>;

// An integer if the text is of the integer form and fits 64 bits, else a
// decimal. An integer of the form that is too large is nothing: it is never
// read as a decimal.
[[nodiscard]] std::optional<Number> parse_number(std::string_view text);

// `true` or `false` exactly.
[[nodiscard]] std::optional<bool> parse_boolean(std::string_view text);

// A duration or a size as read: of its form or not, and if of its form,
// within uint64 or not, as for integers (D-107).
struct UnitRead {
    IntegerForm form = IntegerForm::NOT_INTEGER;
    // The value when `form` is FITS.
    std::uint64_t value = 0;
};

// A duration of section 2.2: a whole number without sign (`0` or no leading
// zero) and one of `ms`, `s`, `m`, `h`, `d`, `y` (365 days), in milliseconds.
// TOO_LARGE for one of the form beyond uint64 milliseconds.
[[nodiscard]] UnitRead read_duration_ms(std::string_view text);

// The milliseconds of a duration that fits; nothing for any other text.
[[nodiscard]] std::optional<std::uint64_t> parse_duration_ms(
    std::string_view text);

// A size of section 2.2: a whole number and one of `B`, `KiB`, `MiB`, `GiB`,
// `TiB`, in bytes. TOO_LARGE for one of the form beyond uint64 bytes.
[[nodiscard]] UnitRead read_size_bytes(std::string_view text);

// The bytes of a size that fits; nothing for any other text.
[[nodiscard]] std::optional<std::uint64_t> parse_size_bytes(
    std::string_view text);

// A percentage of section 2.2: a decimal number and `%`. The number before
// the `%`.
[[nodiscard]] std::optional<double> parse_percentage(std::string_view text);

}  // namespace scada::conf

#endif  // SCADA_CONF_SCALAR_H
