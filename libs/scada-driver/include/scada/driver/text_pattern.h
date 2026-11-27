#ifndef SCADA_DRIVER_TEXT_PATTERN_H
#define SCADA_DRIVER_TEXT_PATTERN_H

// The text pattern of configuration schemas contract 2.5, which an identity
// check (4.8) compares with a text the device gives, cleaned first
// (identity.h): `*` matches any bytes, `?` exactly one, `\` makes the next
// byte literal, and the whole text must match, case-sensitive, byte by
// byte. No other operator, so no regular expression (decision D-147).

#include "scada/common/realtime.h"

#include <cstddef>
#include <string_view>

namespace scada::driver {

// The longest pattern the compile step accepts, in bytes (D-147).
inline constexpr std::size_t TEXT_PATTERN_MAX_BYTES = 250;

// True when `text` matches `pattern` as a whole; the empty pattern matches
// only the empty text. A single `\` at the end, which the compile step
// refuses, stands for itself. Two cursors and the place of the last `*`: no
// allocation, no recursion, and at most (n + 1)(m + 1) byte comparisons for
// a text of n bytes and a pattern of m.
SCADA_RT [[nodiscard]] bool text_matches(std::string_view pattern,
                                         std::string_view text) noexcept;

// As `text_matches`, and `comparisons` is the number of byte comparisons it
// made: for the test of the bound.
SCADA_RT [[nodiscard]] bool text_matches_counted(
    std::string_view pattern, std::string_view text,
    std::size_t& comparisons) noexcept;

}  // namespace scada::driver

#endif  // SCADA_DRIVER_TEXT_PATTERN_H
