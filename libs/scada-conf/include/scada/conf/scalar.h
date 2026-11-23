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

// `-?(0|[1-9][0-9]*)` or `0x[0-9a-fA-F]+` (no sign on hex), with a magnitude
// of at most 2^64 - 1.
[[nodiscard]] std::optional<Integer> parse_integer(std::string_view text);

// `-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][-+]?[0-9]+)?`, read as the nearest
// binary64. A value that is not finite, or not zero but rounds to zero, is
// refused (1.2: `1e-400`).
[[nodiscard]] std::optional<double> parse_decimal(std::string_view text);

// A number keeps its written form (1.2): `10` is an Integer, `10.0` a double.
using Number = std::variant<Integer, double>;

// An integer if the text is one, else a decimal.
[[nodiscard]] std::optional<Number> parse_number(std::string_view text);

// `true` or `false` exactly.
[[nodiscard]] std::optional<bool> parse_boolean(std::string_view text);

// A duration of section 2.2: a whole number without sign (`0` or no leading
// zero) and one of `ms`, `s`, `m`, `h`, `d`, `y` (365 days). In milliseconds;
// a value beyond uint64 is refused.
[[nodiscard]] std::optional<std::uint64_t> parse_duration_ms(
    std::string_view text);

// A size of section 2.2: a whole number and one of `B`, `KiB`, `MiB`, `GiB`,
// `TiB`. In bytes; a value beyond uint64 is refused.
[[nodiscard]] std::optional<std::uint64_t> parse_size_bytes(
    std::string_view text);

// A percentage of section 2.2: a decimal number and `%`. The number before
// the `%`.
[[nodiscard]] std::optional<double> parse_percentage(std::string_view text);

}  // namespace scada::conf

#endif  // SCADA_CONF_SCALAR_H
