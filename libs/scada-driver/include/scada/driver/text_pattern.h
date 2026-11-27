#ifndef SCADA_DRIVER_TEXT_PATTERN_H
#define SCADA_DRIVER_TEXT_PATTERN_H

// The text pattern of configuration schemas contract 2.5, which an identity
// check (4.8) compares with a text the device gives: `*` matches any
// characters, `?` exactly one, `\` makes the next character literal, and the
// whole text must match. No other operator, so no regular expression.

#include <string_view>

namespace scada::driver {

// True when `text` matches `pattern` as a whole. A `\` at the end of the
// pattern stands for itself. Bytes are compared, not characters: `?` takes
// one byte. Each part of the pattern between two stars is placed at its
// first fit after the part before, which is never a wrong choice for this
// pattern form, so there is no backtracking across stars; the time is at
// most the text's length times the pattern's (decision D-145). Allocates
// nothing.
[[nodiscard]] bool text_matches(std::string_view pattern,
                                std::string_view text) noexcept;

}  // namespace scada::driver

#endif  // SCADA_DRIVER_TEXT_PATTERN_H
