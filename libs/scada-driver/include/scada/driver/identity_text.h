#ifndef SCADA_DRIVER_IDENTITY_TEXT_H
#define SCADA_DRIVER_IDENTITY_TEXT_H

// The text of `sys.dev.<device>.identity` (driver interface contract 3.6,
// 11.1; decision D-145): the values the identity checks read, which the
// device chose, made safe to show and joined.

#include "scada/common/realtime.h"
#include "scada/driver/tag_value.h"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace scada::driver {

// The separator between two checks' values.
inline constexpr std::string_view IDENTITY_SEPARATOR = "; ";

// The mark at the end of a value cut to its share.
inline constexpr char IDENTITY_CUT = '~';

// Copies `text` into `out` with every byte outside 0x20 to 0x7E replaced by
// `?`, at most `out.size()` bytes; returns the number written. A pattern is
// matched against this cleaned text, and the log names it, never the raw
// bytes.
SCADA_RT std::size_t clean_identity_text(std::string_view text,
                                         std::span<char> out) noexcept;

// `value` as the identity text shows it, into `out`: a number in decimal
// (a FLOAT64 in its shortest exact form), a BOOL as `true` or `false`, a
// STRING cleaned; returns the number of bytes written, at most
// `out.size()`. A value longer than `out` is cut, without the mark.
SCADA_RT std::size_t identity_field(const TagValue& value,
                                    std::span<char> out) noexcept;

// One check's part of the text: its value, if it was read.
struct IdentityField {
    bool read = false;
    TagValue value{};
};

// The joined text of `fields` in `out`, the STRING maximum of the tag
// database (TAG_STRING_MAX_BYTES): each field in turn, separated by
// IDENTITY_SEPARATOR. Each gets an equal share of the bytes left after the
// separators; a field longer than its share is cut to it and ends in
// IDENTITY_CUT; a field not read is empty. Returns the length. Allocates
// nothing.
SCADA_RT std::size_t join_identity(
    std::span<const IdentityField> fields,
    std::span<char, TAG_STRING_MAX_BYTES> out) noexcept;

}  // namespace scada::driver

#endif  // SCADA_DRIVER_IDENTITY_TEXT_H
