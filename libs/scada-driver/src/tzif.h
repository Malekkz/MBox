#ifndef SCADA_DRIVER_TZIF_H
#define SCADA_DRIVER_TZIF_H

// A reader of compiled time zone files (TZif, RFC 9636), version 2 and
// later, and of the POSIX TZ string in their footer (decision D-130).
//
// A zone file is outside input: it comes from the unit's file system, not
// from this repository. Every count and field is checked against the bytes
// present and against the limits below before it is used; a file that
// breaks one is refused whole.

#include "scada/common/result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace scada::driver::detail {

// Limits. A zone file of the tz database is a few kilobytes with at most
// some hundreds of transitions; these are far above it.
inline constexpr std::size_t TZIF_MAX_BYTES = std::size_t{256} * 1024;
inline constexpr std::uint32_t TZIF_MAX_TRANSITIONS = 4096;
inline constexpr std::uint32_t TZIF_MAX_TYPES = 256;
inline constexpr std::uint32_t TZIF_MAX_CHARS = 4096;
inline constexpr std::size_t TZ_STRING_MAX_BYTES = 256;
// RFC 9636, section 3.2: a UT offset is within -25:59:59 to +25:59:59.
inline constexpr std::int32_t TZIF_MIN_OFFSET_S = -89'999;
inline constexpr std::int32_t TZIF_MAX_OFFSET_S = 93'599;

// One rule date of a POSIX TZ string: `Jn`, `n` or `Mm.w.d`, with its time.
struct TzRuleDate {
    enum class Form : std::uint8_t {
        JULIAN_NO_LEAP,
        ZERO_BASED,
        MONTH_WEEK_DAY
    };
    Form form = Form::MONTH_WEEK_DAY;
    int day = 0;    // `Jn` 1 to 365, `n` 0 to 365, or the weekday 0 to 6
    int month = 0;  // 1 to 12, for `Mm.w.d`
    int week = 0;   // 1 to 5, for `Mm.w.d`
    std::int32_t time_s = 0;  // local time of day, -167 h to 167 h
};

// A POSIX TZ string (RFC 9636, section 3.3, which extends POSIX).
struct TzString {
    std::int32_t std_offset_s = 0;  // UT offset, local minus UT
    bool has_dst = false;
    std::int32_t dst_offset_s = 0;
    TzRuleDate start;  // when DST starts, in local standard time
    TzRuleDate end;    // when DST ends, in local daylight time
};

// What a zone file says: the transitions in UT seconds with the offset in
// force from each, the offset before the first, and the rule for times
// after the last.
struct TzifZone {
    std::vector<std::int64_t> transitions_s;  // strictly increasing
    std::vector<std::int32_t> offsets_s;      // one per transition
    std::int32_t initial_offset_s = 0;        // before the first transition
    std::optional<TzString> footer;           // absent: the last offset holds
};

// Parses a POSIX TZ string. Refuses one with DST and no rule.
[[nodiscard]] Result<TzString> parse_tz_string(std::string_view text) noexcept;

// Parses a zone file. Refuses version 1 files and files with leap second
// records (a `right/` zone), whose times are not POSIX times.
[[nodiscard]] Result<TzifZone> parse_tzif(
    std::span<const std::byte> file) noexcept;

// The UT second at which a rule date falls in `year`, given the UT offset
// the rule's time is read in.
[[nodiscard]] std::int64_t rule_instant_s(const TzRuleDate& date, int year,
                                          std::int32_t offset_s) noexcept;

// Days from 1970-01-01 to the first day of `month` of `year` (proleptic
// Gregorian), plus `day` - 1.
[[nodiscard]] std::int64_t days_from_civil(int year, int month,
                                           int day) noexcept;

}  // namespace scada::driver::detail

#endif  // SCADA_DRIVER_TZIF_H
