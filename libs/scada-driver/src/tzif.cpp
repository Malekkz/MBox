#include "tzif.h"

#include "element.h"

#include "scada/common/result.h"
#include "scada/common/status.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::driver::detail {

namespace {

constexpr std::int32_t SECONDS_PER_MINUTE = 60;
constexpr std::int32_t SECONDS_PER_HOUR = 3600;
constexpr std::int64_t SECONDS_PER_DAY = 86'400;
constexpr int MINUTES_PER_HOUR = 60;
// POSIX: hours of an offset are 0 to 24; RFC 9636 allows rule times of -167
// to 167 hours.
constexpr int MAX_OFFSET_HOURS = 24;
constexpr int MAX_RULE_HOURS = 167;
constexpr std::int32_t DEFAULT_RULE_TIME_S = 2 * SECONDS_PER_HOUR;
constexpr std::size_t MIN_NAME_LENGTH = 3;
constexpr int MONTHS = 12;
constexpr int MAX_WEEK = 5;
constexpr int DAYS_PER_WEEK = 7;
constexpr int MAX_JULIAN_NO_LEAP = 365;
constexpr int MAX_ZERO_BASED = 365;
constexpr int LEAP_DAY_OF_YEAR = 60;  // `Jn`: the day after February 28

Status refused() noexcept {
    return {StatusCode::DATA_CORRUPT, "zone file not valid"};
}

// Reads a TZ string from left to right; every read is checked.
class TextCursor {
public:
    explicit TextCursor(std::string_view text) noexcept : text_(text) {}

    [[nodiscard]] bool done() const noexcept { return index_ >= text_.size(); }
    [[nodiscard]] char peek() const noexcept {
        return done() ? '\0' : text_[index_];
    }
    bool take(char wanted) noexcept {
        if (peek() != wanted || done()) {
            return false;
        }
        ++index_;
        return true;
    }

    // A zone abbreviation: three or more letters, or `<...>` of three or
    // more letters, digits, `+` and `-`.
    bool name() noexcept {
        std::size_t length = 0;
        if (take('<')) {
            while (!done() && peek() != '>') {
                const char character = peek();
                const bool allowed = is_alpha(character) ||
                                     is_digit(character) || character == '+' ||
                                     character == '-';
                if (!allowed) {
                    return false;
                }
                ++index_;
                ++length;
            }
            return take('>') && length >= MIN_NAME_LENGTH;
        }
        while (!done() && is_alpha(peek())) {
            ++index_;
            ++length;
        }
        return length >= MIN_NAME_LENGTH;
    }

    // `[+-]hh[:minutes_part[:seconds_part]]` with hours up to `max_hours`, in
    // seconds.
    std::optional<std::int32_t> hms(int max_hours) noexcept {
        int sign = 1;
        if (take('-')) {
            sign = -1;
        } else {
            (void)take('+');
        }
        const std::optional<int> hours = number(max_hours);
        if (!hours.has_value()) {
            return std::nullopt;
        }
        int minutes = 0;
        int seconds = 0;
        if (take(':')) {
            const std::optional<int> minutes_part =
                number(MINUTES_PER_HOUR - 1);
            if (!minutes_part.has_value()) {
                return std::nullopt;
            }
            minutes = *minutes_part;
            if (take(':')) {
                const std::optional<int> seconds_part =
                    number(SECONDS_PER_MINUTE - 1);
                if (!seconds_part.has_value()) {
                    return std::nullopt;
                }
                seconds = *seconds_part;
            }
        }
        return sign * ((*hours * SECONDS_PER_HOUR) +
                       (minutes * SECONDS_PER_MINUTE) + seconds);
    }

    // A decimal number from 0 to `max`, at most three digits.
    std::optional<int> number(int max) noexcept {
        constexpr int MAX_DIGITS = 3;
        constexpr int BASE = 10;
        int value = 0;
        int digits = 0;
        while (!done() && is_digit(peek()) && digits < MAX_DIGITS) {
            value = (value * BASE) + (peek() - '0');
            ++index_;
            ++digits;
        }
        if (digits == 0 || value > max || (!done() && is_digit(peek()))) {
            return std::nullopt;
        }
        return value;
    }

private:
    static bool is_alpha(char character) noexcept {
        return (character >= 'A' && character <= 'Z') ||
               (character >= 'a' && character <= 'z');
    }
    static bool is_digit(char character) noexcept {
        return character >= '0' && character <= '9';
    }

    std::string_view text_;
    std::size_t index_ = 0;
};

std::optional<TzRuleDate> rule_date(TextCursor& cursor) noexcept {
    TzRuleDate date;
    if (cursor.take('J')) {
        const std::optional<int> day = cursor.number(MAX_JULIAN_NO_LEAP);
        if (!day.has_value() || *day < 1) {
            return std::nullopt;
        }
        date.form = TzRuleDate::Form::JULIAN_NO_LEAP;
        date.day = *day;
    } else if (cursor.take('M')) {
        const std::optional<int> month = cursor.number(MONTHS);
        if (!month.has_value() || *month < 1 || !cursor.take('.')) {
            return std::nullopt;
        }
        const std::optional<int> week = cursor.number(MAX_WEEK);
        if (!week.has_value() || *week < 1 || !cursor.take('.')) {
            return std::nullopt;
        }
        const std::optional<int> day = cursor.number(DAYS_PER_WEEK - 1);
        if (!day.has_value()) {
            return std::nullopt;
        }
        date.form = TzRuleDate::Form::MONTH_WEEK_DAY;
        date.month = *month;
        date.week = *week;
        date.day = *day;
    } else {
        const std::optional<int> day = cursor.number(MAX_ZERO_BASED);
        if (!day.has_value()) {
            return std::nullopt;
        }
        date.form = TzRuleDate::Form::ZERO_BASED;
        date.day = *day;
    }
    date.time_s = DEFAULT_RULE_TIME_S;
    if (cursor.take('/')) {
        const std::optional<std::int32_t> time = cursor.hms(MAX_RULE_HOURS);
        if (!time.has_value()) {
            return std::nullopt;
        }
        date.time_s = *time;
    }
    return date;
}

// Reads big-endian fields of a zone file; every read is checked.
class ByteCursor {
public:
    explicit ByteCursor(std::span<const std::byte> bytes) noexcept
        : bytes_(bytes) {}

    [[nodiscard]] std::size_t left() const noexcept {
        return bytes_.size() - index_;
    }
    bool skip(std::size_t count) noexcept {
        if (count > left()) {
            return false;
        }
        index_ += count;
        return true;
    }
    std::optional<std::uint64_t> unsigned_be(std::size_t width) noexcept {
        if (width > left()) {
            return std::nullopt;
        }
        constexpr unsigned BYTE_BITS = 8;
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < width; ++index) {
            value = (value << BYTE_BITS) |
                    static_cast<std::uint8_t>(bytes_[index_ + index]);
        }
        index_ += width;
        return value;
    }
    [[nodiscard]] std::span<const std::byte> rest() const noexcept {
        return bytes_.subspan(index_);
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t index_ = 0;
};

struct Counts {
    std::uint32_t isut = 0;
    std::uint32_t isstd = 0;
    std::uint32_t leap = 0;
    std::uint32_t time = 0;
    std::uint32_t type = 0;
    std::uint32_t chars = 0;
};

constexpr std::size_t MAGIC_BYTES = 4;
constexpr std::size_t HEADER_UNUSED_BYTES = 15;
constexpr std::size_t COUNT_BYTES = 4;
constexpr std::size_t TYPE_RECORD_BYTES = 6;
constexpr std::size_t V1_TIME_BYTES = 4;
constexpr std::size_t V1_LEAP_BYTES = 8;
constexpr std::size_t V2_TIME_BYTES = 8;

// The header: MAGIC, version, the six counts. Returns the counts, or
// nothing for a header that is not valid or not version 2 or later.
std::optional<Counts> header(ByteCursor& cursor) noexcept {
    constexpr std::uint64_t MAGIC = 0x545A'6966U;  // "TZif"
    const std::optional<std::uint64_t> found = cursor.unsigned_be(MAGIC_BYTES);
    const std::optional<std::uint64_t> version = cursor.unsigned_be(1);
    if (found != MAGIC || !version.has_value() ||
        (*version != '2' && *version != '3' && *version != '4') ||
        !cursor.skip(HEADER_UNUSED_BYTES)) {
        return std::nullopt;
    }
    constexpr std::size_t COUNT_FIELDS = 6;
    std::array<std::uint32_t, COUNT_FIELDS> values{};
    for (std::uint32_t& value : values) {
        const std::optional<std::uint64_t> count =
            cursor.unsigned_be(COUNT_BYTES);
        if (!count.has_value()) {
            return std::nullopt;
        }
        value = static_cast<std::uint32_t>(*count);
    }
    const Counts counts{.isut = values[0],
                        .isstd = values[1],
                        .leap = values[2],
                        .time = values[3],
                        .type = values[4],
                        .chars = values[5]};
    if (counts.time > TZIF_MAX_TRANSITIONS || counts.type == 0 ||
        counts.type > TZIF_MAX_TYPES || counts.chars > TZIF_MAX_CHARS ||
        (counts.isut != 0 && counts.isut != counts.type) ||
        (counts.isstd != 0 && counts.isstd != counts.type) ||
        counts.leap != 0) {
        // Leap second records (`right/` zones) are refused: their times are
        // not POSIX times.
        return std::nullopt;
    }
    return counts;
}

}  // namespace

std::int64_t days_from_civil(int year, int month, int day) noexcept {
    // H. Hinnant, "chrono-Compatible Low-Level Date Algorithms": exact for
    // every year of int.
    constexpr int MARCH = 3;
    constexpr std::int64_t DAYS_PER_ERA = 146'097;
    constexpr std::int64_t YEARS_PER_ERA = 400;
    constexpr std::int64_t EPOCH_SHIFT = 719'468;
    constexpr std::int64_t MONTH_DAYS_NUMERATOR = 153;
    constexpr std::int64_t MONTH_DAYS_OFFSET = 2;
    constexpr std::int64_t MONTH_DAYS_DENOMINATOR = 5;
    constexpr std::int64_t DAYS_PER_YEAR = 365;
    constexpr std::int64_t LEAP_EVERY = 4;
    constexpr std::int64_t CENTURY = 100;
    const std::int64_t shifted_year =
        static_cast<std::int64_t>(year) - (month < MARCH ? 1 : 0);
    const std::int64_t era =
        (shifted_year >= 0 ? shifted_year
                           : shifted_year - (YEARS_PER_ERA - 1)) /
        YEARS_PER_ERA;
    const std::int64_t year_of_era = shifted_year - (era * YEARS_PER_ERA);
    const std::int64_t shifted_month =
        month > 2 ? month - MARCH : month + (MONTHS - MARCH);
    const std::int64_t day_of_year =
        (((MONTH_DAYS_NUMERATOR * shifted_month) + MONTH_DAYS_OFFSET) /
         MONTH_DAYS_DENOMINATOR) +
        day - 1;
    const std::int64_t day_of_era = (year_of_era * DAYS_PER_YEAR) +
                                    (year_of_era / LEAP_EVERY) -
                                    (year_of_era / CENTURY) + day_of_year;
    return (era * DAYS_PER_ERA) + day_of_era - EPOCH_SHIFT;
}

std::int64_t rule_instant_s(const TzRuleDate& date, int year,
                            std::int32_t offset_s) noexcept {
    constexpr int LEAP_CYCLE = 4;
    constexpr int CENTURY = 100;
    constexpr int LEAP_CENTURY = 400;
    const bool leap = (year % LEAP_CYCLE == 0 && year % CENTURY != 0) ||
                      year % LEAP_CENTURY == 0;
    const std::int64_t first_of_year = days_from_civil(year, 1, 1);
    std::int64_t day = 0;
    switch (date.form) {
        case TzRuleDate::Form::JULIAN_NO_LEAP:
            day = first_of_year + date.day - 1 +
                  ((leap && date.day >= LEAP_DAY_OF_YEAR) ? 1 : 0);
            break;
        case TzRuleDate::Form::ZERO_BASED:
            day = first_of_year + date.day;
            break;
        case TzRuleDate::Form::MONTH_WEEK_DAY: {
            const std::int64_t first = days_from_civil(year, date.month, 1);
            const int next_month = date.month == MONTHS ? 1 : date.month + 1;
            const int next_year = date.month == MONTHS ? year + 1 : year;
            const std::int64_t month_days =
                days_from_civil(next_year, next_month, 1) - first;
            // 1970-01-01 was a Thursday (weekday 4).
            constexpr std::int64_t THURSDAY = 4;
            const std::int64_t weekday_of_first =
                (((first + THURSDAY) % DAYS_PER_WEEK) + DAYS_PER_WEEK) %
                DAYS_PER_WEEK;
            std::int64_t offset =
                ((date.day - weekday_of_first) + DAYS_PER_WEEK) % DAYS_PER_WEEK;
            offset += static_cast<std::int64_t>(date.week - 1) * DAYS_PER_WEEK;
            while (offset >= month_days) {
                offset -= DAYS_PER_WEEK;  // week 5: the last such day
            }
            day = first + offset;
            break;
        }
    }
    return (day * SECONDS_PER_DAY) + date.time_s - offset_s;
}

Result<TzString> parse_tz_string(std::string_view text) noexcept {
    if (text.size() > TZ_STRING_MAX_BYTES) {
        return refused();
    }
    TextCursor cursor(text);
    TzString out;
    if (!cursor.name()) {
        return refused();
    }
    // POSIX offsets are west of UT: "MST7" is UT - 7 h.
    const std::optional<std::int32_t> std_offset = cursor.hms(MAX_OFFSET_HOURS);
    if (!std_offset.has_value()) {
        return refused();
    }
    out.std_offset_s = -*std_offset;
    if (cursor.done()) {
        return out;
    }
    if (!cursor.name()) {
        return refused();
    }
    out.has_dst = true;
    out.dst_offset_s = out.std_offset_s + SECONDS_PER_HOUR;
    if (cursor.peek() != ',') {
        const std::optional<std::int32_t> dst_offset =
            cursor.hms(MAX_OFFSET_HOURS);
        if (!dst_offset.has_value()) {
            return refused();
        }
        out.dst_offset_s = -*dst_offset;
    }
    // A TZ string in a zone file always gives its rule (RFC 9636, 3.3); the
    // POSIX default rule is not assumed.
    if (!cursor.take(',')) {
        return refused();
    }
    const std::optional<TzRuleDate> start = rule_date(cursor);
    if (!start.has_value() || !cursor.take(',')) {
        return refused();
    }
    const std::optional<TzRuleDate> end = rule_date(cursor);
    if (!end.has_value() || !cursor.done()) {
        return refused();
    }
    out.start = *start;
    out.end = *end;
    return out;
}

namespace {

// The version 2 transitions: strictly increasing times, each with a type.
bool read_transitions(ByteCursor& cursor, const Counts& counts,
                      std::vector<std::int64_t>& times,
                      std::vector<std::uint8_t>& type_of) {
    times.reserve(counts.time);
    for (std::uint32_t index = 0; index < counts.time; ++index) {
        const std::optional<std::uint64_t> raw =
            cursor.unsigned_be(V2_TIME_BYTES);
        if (!raw.has_value()) {
            return false;
        }
        const auto time = static_cast<std::int64_t>(*raw);
        if (!times.empty() && time <= times.back()) {
            return false;
        }
        times.push_back(time);
    }
    type_of.reserve(counts.time);
    for (std::uint32_t index = 0; index < counts.time; ++index) {
        const std::optional<std::uint64_t> type = cursor.unsigned_be(1);
        if (!type.has_value() || *type >= counts.type) {
            return false;
        }
        type_of.push_back(static_cast<std::uint8_t>(*type));
    }
    return true;
}

// The local time type records: UT offset, DST flag, designation index.
bool read_types(ByteCursor& cursor, const Counts& counts,
                std::vector<std::int32_t>& offsets) {
    constexpr std::size_t UTOFF_BYTES = 4;
    offsets.reserve(counts.type);
    for (std::uint32_t index = 0; index < counts.type; ++index) {
        const std::optional<std::uint64_t> raw =
            cursor.unsigned_be(UTOFF_BYTES);
        const std::optional<std::uint64_t> isdst = cursor.unsigned_be(1);
        const std::optional<std::uint64_t> designation = cursor.unsigned_be(1);
        if (!raw.has_value() || !isdst.has_value() ||
            !designation.has_value() || *isdst > 1 ||
            *designation >= counts.chars) {
            return false;
        }
        const auto offset =
            static_cast<std::int32_t>(static_cast<std::uint32_t>(*raw));
        if (offset < TZIF_MIN_OFFSET_S || offset > TZIF_MAX_OFFSET_S) {
            return false;
        }
        offsets.push_back(offset);
    }
    return true;
}

// The footer: a newline, the TZ string (possibly empty), a newline, and
// nothing after it.
Result<std::optional<TzString>> read_footer(std::span<const std::byte> footer) {
    if (footer.size() < 2 || footer.front() != std::byte{'\n'} ||
        footer.back() != std::byte{'\n'} ||
        footer.size() - 2 > TZ_STRING_MAX_BYTES) {
        return refused();
    }
    std::array<char, TZ_STRING_MAX_BYTES> buffer{};
    for (std::size_t index = 1; index + 1 < footer.size(); ++index) {
        const auto character = static_cast<char>(footer[index]);
        if (character == '\n') {
            return refused();
        }
        // In range: the footer's size was checked against the buffer's.
        detail::element(buffer, index - 1) = character;
    }
    const std::string_view text(buffer.data(), footer.size() - 2);
    if (text.empty()) {
        return std::optional<TzString>{};
    }
    Result<TzString> parsed = parse_tz_string(text);
    if (!parsed.ok()) {
        return parsed.status();
    }
    return std::optional<TzString>(parsed.value());
}

Result<TzifZone> parse_tzif_throwing(std::span<const std::byte> file) {
    ByteCursor cursor(file);
    // The version 1 header and data block, skipped.
    const std::optional<Counts> first = header(cursor);
    if (!first.has_value()) {
        return refused();
    }
    const std::size_t first_block =
        (static_cast<std::size_t>(first->time) * (V1_TIME_BYTES + 1)) +
        (static_cast<std::size_t>(first->type) * TYPE_RECORD_BYTES) +
        first->chars + (static_cast<std::size_t>(first->leap) * V1_LEAP_BYTES) +
        first->isstd + first->isut;
    if (!cursor.skip(first_block)) {
        return refused();
    }
    // The version 2 header and data block, with 64-bit times.
    const std::optional<Counts> counts = header(cursor);
    if (!counts.has_value()) {
        return refused();
    }
    std::vector<std::int64_t> times;
    std::vector<std::uint8_t> type_of;
    std::vector<std::int32_t> type_offsets;
    if (!read_transitions(cursor, *counts, times, type_of) ||
        !read_types(cursor, *counts, type_offsets) ||
        !cursor.skip(static_cast<std::size_t>(counts->chars) + counts->isstd +
                     counts->isut)) {
        return refused();
    }
    Result<std::optional<TzString>> footer = read_footer(cursor.rest());
    if (!footer.ok()) {
        return footer.status();
    }
    TzifZone zone;
    zone.footer = footer.value();
    zone.transitions_s = std::move(times);
    zone.offsets_s.reserve(type_of.size());
    for (const std::uint8_t type : type_of) {
        // `read_transitions` refused a type at or above the count, and
        // `read_types` read the count: a file that still breaks it is
        // refused, never an abort on outside input (issue #181).
        if (type >= type_offsets.size()) {
            return refused();
        }
        zone.offsets_s.push_back(type_offsets[type]);
    }
    if (type_offsets.empty()) {
        return refused();  // the header refused a count of 0 already
    }
    zone.initial_offset_s = type_offsets.front();
    return zone;
}

}  // namespace

Result<TzifZone> parse_tzif(std::span<const std::byte> file) noexcept {
    if (file.size() > TZIF_MAX_BYTES) {
        return refused();
    }
    try {
        return parse_tzif_throwing(file);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::RESOURCE_EXHAUSTED, "out of memory");
    }
}

}  // namespace scada::driver::detail
