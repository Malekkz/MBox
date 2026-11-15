#ifndef SCADA_CTL_FORMAT_H
#define SCADA_CTL_FORMAT_H

// How scadactl writes tag values, qualities, flags, times and text, in its
// human-readable form and as JSON. Everything shown comes from shared
// memory, which is input (coding standard, section 9): numbers that are no
// enumerator are shown as numbers, and text is escaped so that it can
// neither break the JSON nor send control sequences to a terminal.

#include "scada/tag/scada_tag.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace scada::ctl {

// The name of a tag type: "bool", "int64", "uint64", "float64", "string",
// or "unknown" for a number that is no type. The view refers to static
// storage.
[[nodiscard]] std::string_view type_name(std::uint8_t type) noexcept;

// The name of a quality: "GOOD", "UNCERTAIN", "BAD", or "UNKNOWN" for a
// number that is no quality. The view refers to static storage.
[[nodiscard]] std::string_view quality_name(std::uint8_t quality) noexcept;

// Appends the names of the bits set in `flags`, lowest bit first, separated
// by `separator`, for example "STALE,COMM_FAIL". A bit that has no name is
// appended as "0x" and its hexadecimal value. Nothing for 0.
void append_flag_names(std::string& out, std::uint16_t flags,
                       std::string_view separator);

// How a value is marked in the human-readable output, so that a reader
// never takes a bad or stale value for a good one.
enum class Mark : std::uint8_t {
    NONE,       // GOOD and not STALE
    ATTENTION,  // UNCERTAIN, or STALE with a quality that is not BAD
    BAD,        // BAD (OWNER_DOWN included), a quality that is no quality,
                // or no value could be read
};

// The mark of a value with `quality` and `flags`.
[[nodiscard]] Mark mark_of(std::uint8_t quality, std::uint16_t flags) noexcept;

// The character of `mark` at the start of a row: ' ', '~' or '!'.
[[nodiscard]] char mark_char(Mark mark) noexcept;

// True if `value` holds a value: its owner wrote the record at least once
// and the read gave a value. A record never written, or one whose owner is
// missing or died in the middle of a write, reads with times 0; its value
// 0 then means nothing (contract, section 6.2).
[[nodiscard]] bool has_value(const scada_value& value) noexcept;

// Appends the value of `value` for a person: "true" or "false", the
// integer, the shortest decimal that gives the same double ("nan", "inf"
// and "-inf" included), or for a STRING `text` in double quotes, escaped
// as by append_terminal_safe. "-" if there is no value (has_value).
void append_value_text(std::string& out, const scada_value& value,
                       std::string_view text);

// Appends the value of `value` as a JSON value: true or false, a number,
// for a double that is not finite the string "nan", "inf" or "-inf", for a
// STRING `text` as a JSON string, and null if there is no value.
void append_value_json(std::string& out, const scada_value& value,
                       std::string_view text);

// Appends `bytes` so that a terminal shows it as text and does nothing
// else: printable ASCII and well-formed UTF-8 from U+00A0 on are kept;
// `"` and `\` are preceded by `\`; newline, carriage return and tab become
// \n, \r and \t; other ASCII control bytes and bytes that are not
// well-formed UTF-8 become \xNN; C1 control characters and the
// characters that change the direction of text become \uNNNN.
void append_terminal_safe(std::string& out, std::string_view bytes);

// Appends `bytes` as a JSON string, quotes included. Well-formed UTF-8 is
// kept, `"`, `\` and control characters are escaped, and each byte that
// is not well-formed UTF-8 becomes U+FFFD.
void append_json_string(std::string& out, std::string_view bytes);

// Appends the age `age_ns` in seconds with three decimals and the unit,
// for example "0.012 s". A negative age, which the library never gives,
// is appended as 0.
void append_age(std::string& out, std::int64_t age_ns);

// Appends the CLOCK_REALTIME time `time_ns` in UTC as ISO 8601 with
// milliseconds, for example "2026-11-15T10:20:30.123Z". "-" for 0, which
// means "no time".
void append_utc_time(std::string& out, std::int64_t time_ns);

}  // namespace scada::ctl

#endif  // SCADA_CTL_FORMAT_H
