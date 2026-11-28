#include "scada/driver/zone_table.h"

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "tzif.h"
#include "zone_file.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef SCADA_ZONEINFO_DIR
#error "SCADA_ZONEINFO_DIR must be set by the build (CMake option)"
#endif
static_assert(std::string_view(SCADA_ZONEINFO_DIR).starts_with('/'),
              "SCADA_ZONEINFO_DIR must be an absolute path");

namespace scada::driver {

namespace {

constexpr std::int64_t NS_PER_S = 1'000'000'000;

constexpr std::int64_t SECONDS_PER_DAY = 86'400;

std::int64_t year_start_s(int year) noexcept {
    return detail::days_from_civil(year, 1, 1) * SECONDS_PER_DAY;
}

// a - b, saturating at the limits of int64 (decision D-130: device data may
// hold any value).
std::int64_t saturating_subtract(std::int64_t a_value,
                                 std::int64_t b_value) noexcept {
    constexpr std::int64_t MAX = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t MIN = std::numeric_limits<std::int64_t>::min();
    if (b_value > 0 && a_value < MIN + b_value) {
        return MIN;
    }
    if (b_value < 0 && a_value > MAX + b_value) {
        return MAX;
    }
    return a_value - b_value;
}

// How many intervals on each side of the one found by a search may hold a
// local time. Two suffice when every interval is longer than the span of
// the table's offsets (under 51 h, RFC 9636), as in every real zone; a file
// with changes closer together than that needs more, and `from_tzif`
// widens the window to what the table needs (#158).
constexpr std::ptrdiff_t SEARCH_WINDOW = 2;
// The widest window taken. A file whose changes are so close that a local
// time may lie in more intervals than this is refused, so that a conversion
// stays bounded: at most 2 * 16 + 1 = 33 intervals are looked at.
constexpr std::ptrdiff_t MAX_SEARCH_WINDOW = 16;

// One change of offset, at `at_s` (UT seconds).
struct Change {
    std::int64_t at_s = 0;
    std::int32_t offset_s = 0;
    bool daylight = false;  // the start of daylight time, by a rule
    int year = 0;           // the year of the rule that gave it
};

// The changes a TZ string gives from `first_year` to `last_year`.
std::vector<Change> rule_changes(const detail::TzString& rule, int first_year,
                                 int last_year) {
    std::vector<Change> changes;
    if (!rule.has_dst) {
        return changes;
    }
    for (int year = first_year; year <= last_year; ++year) {
        // The start is read in standard time, the end in daylight time.
        changes.push_back(Change{
            .at_s = detail::rule_instant_s(rule.start, year, rule.std_offset_s),
            .offset_s = rule.dst_offset_s,
            .daylight = true,
            .year = year});
        changes.push_back(Change{
            .at_s = detail::rule_instant_s(rule.end, year, rule.dst_offset_s),
            .offset_s = rule.std_offset_s,
            .daylight = false,
            .year = year});
    }
    // A total order, so that equal instants decide the same way every time
    // (decision D-130): by instant, then by the year of the rule, then the
    // start before the end. An end that meets the next year's start gives
    // daylight time all year (RFC 9636 3.3.1, `EST5EDT,0/0,J365/25`); a
    // start and an end of one year at one instant give no daylight time, as
    // the C library reads them.
    std::sort(changes.begin(), changes.end(),
              [](const Change& left, const Change& right) {
                  if (left.at_s != right.at_s) {
                      return left.at_s < right.at_s;
                  }
                  if (left.year != right.year) {
                      return left.year < right.year;
                  }
                  return left.daylight && !right.daylight;
              });
    // One change per instant: the last of those at it.
    std::vector<Change> unique;
    unique.reserve(changes.size());
    for (const Change& change : changes) {
        if (!unique.empty() && unique.back().at_s == change.at_s) {
            unique.back() = change;
        } else {
            unique.push_back(change);
        }
    }
    return unique;
}

// The offset of `zone` just after `at_s` by the rule alone.
std::int32_t rule_offset_at(const detail::TzString& rule,
                            std::int64_t at_s) noexcept {
    if (!rule.has_dst) {
        return rule.std_offset_s;
    }
    // The last change at or before `at_s`, looking one year back.
    std::int32_t offset = rule.std_offset_s;
    std::int64_t latest = std::numeric_limits<std::int64_t>::min();
    constexpr int SECONDS_PER_YEAR_GUESS = 31'556'952;  // mean Gregorian
    const auto year = static_cast<int>(ZONE_TABLE_FIRST_YEAR +
                                       (at_s / SECONDS_PER_YEAR_GUESS));
    for (const Change& change : rule_changes(rule, year - 1, year + 1)) {
        if (change.at_s <= at_s && change.at_s >= latest) {
            latest = change.at_s;
            offset = change.offset_s;
        }
    }
    return offset;
}

// The end of what a file without a footer rule states: its last transition
// (the table's start, for a file without one), within the table.
std::int64_t last_stated_ns(const detail::TzifZone& zone, std::int64_t first_s,
                            std::int64_t end_s) noexcept {
    return zone.transitions_s.empty()
               ? first_s * NS_PER_S
               : std::clamp(zone.transitions_s.back(), first_s, end_s) *
                     NS_PER_S;
}

Result<std::vector<std::byte>> read_zone_file(std::string_view zone_name) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read on the loader thread; no thread of a service sets the environment
    const char* configured = std::getenv("TZDIR");
    const std::string directory = (configured != nullptr && *configured != '\0')
                                      ? std::string(configured)
                                      : std::string(SCADA_ZONEINFO_DIR);
    return detail::read_zone_file_beneath(directory, zone_name,
                                          detail::BeneathMethod::AUTOMATIC);
}

}  // namespace

bool is_zone_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > ZONE_NAME_MAX_BYTES) {
        return false;
    }
    std::size_t component = 0;
    for (std::size_t index = 0; index <= name.size(); ++index) {
        if (index == name.size() || name[index] == '/') {
            if (component == 0) {
                return false;  // empty, leading, trailing or double `/`
            }
            component = 0;
            continue;
        }
        const char character = name[index];
        const bool allowed = (character >= 'A' && character <= 'Z') ||
                             (character >= 'a' && character <= 'z') ||
                             (character >= '0' && character <= '9') ||
                             character == '_' || character == '+' ||
                             character == '-';
        if (!allowed) {
            return false;  // `.` among them: no `..`, no hidden file
        }
        ++component;
    }
    // Names the system chooses rather than the database (`localtime`,
    // `posixrules`), and the copies on other time scales.
    const std::string_view first = name.substr(0, name.find('/'));
    return name != "localtime" && name != "posixrules" && first != "posix" &&
           first != "right";
}

Result<ZoneTable> ZoneTable::build(std::string_view zone_name) noexcept {
    if (!is_zone_name(zone_name)) {
        return Status(StatusCode::INVALID_ARGUMENT, "not a time zone name");
    }
    try {
        Result<std::vector<std::byte>> bytes = read_zone_file(zone_name);
        if (!bytes.ok()) {
            return bytes.status();
        }
        return from_tzif(bytes.value());
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::RESOURCE_EXHAUSTED, "out of memory");
    }
}

Result<ZoneTable> ZoneTable::from_tzif(
    std::span<const std::byte> file) noexcept {
    try {
        Result<detail::TzifZone> parsed = detail::parse_tzif(file);
        if (!parsed.ok()) {
            return parsed.status();
        }
        const detail::TzifZone& zone = parsed.value();
        const std::int64_t first_s = year_start_s(ZONE_TABLE_FIRST_YEAR);
        const std::int64_t end_s = year_start_s(ZONE_TABLE_LAST_YEAR + 1);

        // The offset in force at the start of the table.
        std::int32_t offset = zone.initial_offset_s;
        std::int64_t last_s = first_s;  // the last instant the file decides
        std::vector<Change> changes;
        for (std::size_t index = 0; index < zone.transitions_s.size();
             ++index) {
            const std::int64_t when = zone.transitions_s[index];
            if (when <= first_s) {
                offset = zone.offsets_s[index];
            } else if (when < end_s) {
                changes.push_back(
                    Change{.at_s = when, .offset_s = zone.offsets_s[index]});
            }
            last_s = std::max(last_s, when);
        }
        // After the last transition the footer's rule decides (RFC 9636,
        // 3.3). With no transition at or after the table's start, the rule
        // decides the start too.
        if (zone.footer.has_value()) {
            const detail::TzString& rule = *zone.footer;
            if (zone.transitions_s.empty() ||
                zone.transitions_s.back() <= first_s) {
                offset = rule_offset_at(rule, first_s);
            }
            // One year beyond each end, so that a change at the edge of
            // the table meets its neighbour of the next year.
            for (const Change& change :
                 rule_changes(rule, ZONE_TABLE_FIRST_YEAR - 1,
                              ZONE_TABLE_LAST_YEAR + 1)) {
                if (change.at_s > last_s && change.at_s < end_s) {
                    changes.push_back(change);
                }
            }
            if (!rule.has_dst && last_s < end_s &&
                (changes.empty() ? offset : changes.back().offset_s) !=
                    rule.std_offset_s) {
                changes.push_back(Change{.at_s = std::max(last_s, first_s + 1),
                                         .offset_s = rule.std_offset_s});
            }
        }

        ZoneTable table;
        table.end_utc_ns_ = end_s * NS_PER_S;
        if (!zone.footer.has_value()) {
            table.certain_until_utc_ns_ = last_stated_ns(zone, first_s, end_s);
        }
        table.intervals_.push_back(Interval{
            .begin_utc_ns = first_s * NS_PER_S,
            .offset_ns = static_cast<std::int64_t>(offset) * NS_PER_S});
        for (const Change& change : changes) {
            add_change(table.intervals_, change.at_s * NS_PER_S,
                       static_cast<std::int64_t>(change.offset_s) * NS_PER_S);
        }
        return with_search_window(std::move(table));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::RESOURCE_EXHAUSTED, "out of memory");
    }
}

void ZoneTable::add_change(std::vector<Interval>& intervals,
                           std::int64_t begin_ns,
                           std::int64_t offset_ns) noexcept {
    if (intervals.back().begin_utc_ns == begin_ns) {
        if (intervals.size() == 1) {
            // The table's first interval takes the offset. Not reached
            // today: every change is after the table's start.
            intervals.back().offset_ns = offset_ns;
            return;
        }
        intervals.pop_back();  // replaced: the interval would be empty
    }
    if (offset_ns == intervals.back().offset_ns) {
        return;  // merged: no change of offset
    }
    intervals.push_back(
        Interval{.begin_utc_ns = begin_ns, .offset_ns = offset_ns});
}

std::optional<std::ptrdiff_t> ZoneTable::search_window_for(
    std::span<const Interval> intervals) noexcept {
    // A local time `l` held by interval `m` and found from interval `c`
    // (the one that holds `l` read as UTC) has `c - m` interval begins in
    // less than the largest offset before it, or `m - c` in less than the
    // smallest offset after it: both are within the span of the offsets.
    std::int64_t lowest = 0;
    std::int64_t highest = 0;
    for (const Interval& interval : intervals) {
        lowest = std::min(lowest, interval.offset_ns);
        highest = std::max(highest, interval.offset_ns);
    }
    // Offsets are within RFC 9636's bounds (checked by the reader), so the
    // span is under 51 h and the begins lie within the table's years: no
    // subtraction below overflows. The span bounds both directions at
    // once, so it may be up to twice what one direction needs: it refuses
    // more hand-made files than it must, never a real one (D-144).
    const std::int64_t span = highest - lowest;
    std::size_t widest = 1;
    std::size_t last = 0;
    for (std::size_t first = 0; first < intervals.size(); ++first) {
        last = std::max(last, first);
        while (last + 1 < intervals.size() &&
               intervals[last + 1].begin_utc_ns -
                       intervals[first].begin_utc_ns <
                   span) {
            ++last;
        }
        widest = std::max(widest, last - first + 1);
    }
    if (widest > static_cast<std::size_t>(MAX_SEARCH_WINDOW)) {
        return std::nullopt;
    }
    return std::max(SEARCH_WINDOW, static_cast<std::ptrdiff_t>(widest));
}

Result<ZoneTable> ZoneTable::with_search_window(ZoneTable table) {
    const std::optional<std::ptrdiff_t> window =
        search_window_for(table.intervals_);
    if (!window.has_value()) {
        return Status(StatusCode::DATA_CORRUPT,
                      "the zone file's changes of offset are too close "
                      "together for its offsets");
    }
    table.search_window_ = *window;
    return table;
}

UtcTime ZoneTable::to_utc(std::int64_t local_ns) const noexcept {
    return marked(convert(local_ns, search_window_));
}

UtcTime ZoneTable::to_utc_searching_all(std::int64_t local_ns) const noexcept {
    // Written apart from `convert` on purpose: no binary search and no
    // window, so that a fault in either shows against it.
    if (intervals_.empty() || local_ns < intervals_.front().begin_utc_ns ||
        local_ns >= end_utc_ns_) {
        return to_utc(local_ns);  // outside the table: no search at all
    }
    std::optional<std::int64_t> earliest;
    std::size_t matches = 0;
    for (std::size_t index = 0; index < intervals_.size(); ++index) {
        const std::int64_t utc = local_ns - intervals_[index].offset_ns;
        const bool last = index + 1 == intervals_.size();
        if (utc >= intervals_[index].begin_utc_ns &&
            (last || utc < intervals_[index + 1].begin_utc_ns)) {
            earliest = std::min(utc, earliest.value_or(utc));
            ++matches;
        }
    }
    if (earliest.has_value()) {
        return marked(UtcTime{.utc_ns = *earliest, .uncertain = matches > 1});
    }
    for (std::size_t index = 1; index < intervals_.size(); ++index) {
        const Interval& before = intervals_[index - 1];
        const Interval& after = intervals_[index];
        if (local_ns - before.offset_ns >= after.begin_utc_ns &&
            local_ns - after.offset_ns < after.begin_utc_ns) {
            return marked(UtcTime{.utc_ns = local_ns - before.offset_ns,
                                  .uncertain = true});
        }
    }
    return UtcTime{.utc_ns = 0, .uncertain = true};  // no answer: a fault
}

UtcTime ZoneTable::marked(UtcTime time) const noexcept {
    if (time.utc_ns >= certain_until_utc_ns_) {
        time.uncertain = true;
    }
    return time;
}

UtcTime ZoneTable::convert(std::int64_t local_ns,
                           std::ptrdiff_t window) const noexcept {
    if (intervals_.empty()) {
        return UtcTime{.utc_ns = local_ns, .uncertain = false};
    }
    // Outside the table: the offset at the nearest end, marked, saturating.
    // Inside it every subtraction below stays far from the int64 limits.
    if (local_ns < intervals_.front().begin_utc_ns) {
        return UtcTime{.utc_ns = saturating_subtract(
                           local_ns, intervals_.front().offset_ns),
                       .uncertain = true};
    }
    if (local_ns >= end_utc_ns_) {
        return UtcTime{.utc_ns = saturating_subtract(
                           local_ns, intervals_.back().offset_ns),
                       .uncertain = true};
    }

    // The interval that holds `local_ns` read as UTC; the answer is within
    // `window` intervals of it on either side (search_window_for).
    const auto found =
        std::upper_bound(intervals_.begin(), intervals_.end(), local_ns,
                         [](std::int64_t value, const Interval& interval) {
                             return value < interval.begin_utc_ns;
                         });
    const std::ptrdiff_t center =
        std::max<std::ptrdiff_t>(0, (found - intervals_.begin()) - 1);
    const auto count = static_cast<std::ptrdiff_t>(intervals_.size());
    const std::ptrdiff_t from = std::max<std::ptrdiff_t>(0, center - window);
    const std::ptrdiff_t last_index = std::min(count - 1, center + window);

    auto interval_at = [&](std::ptrdiff_t index) -> const Interval& {
        return intervals_[static_cast<std::size_t>(index)];
    };
    auto end_of = [&](std::ptrdiff_t index) {
        return index + 1 < count ? interval_at(index + 1).begin_utc_ns
                                 : std::numeric_limits<std::int64_t>::max();
    };

    std::size_t matches = 0;
    std::int64_t earliest = 0;
    for (std::ptrdiff_t index = from; index <= last_index; ++index) {
        const Interval& interval = interval_at(index);
        const std::int64_t utc = local_ns - interval.offset_ns;
        if (utc >= interval.begin_utc_ns && utc < end_of(index)) {
            earliest = matches == 0 ? utc : std::min(earliest, utc);
            ++matches;
        }
    }
    if (matches == 1) {
        return UtcTime{.utc_ns = earliest, .uncertain = false};
    }
    if (matches > 1) {
        // A repeated hour: the first occurrence is the earlier instant.
        return UtcTime{.utc_ns = earliest, .uncertain = true};
    }
    // A skipped hour: moved forward by the change, which is the time read
    // with the offset before the change.
    for (std::ptrdiff_t index = std::max<std::ptrdiff_t>(from, 1);
         index <= last_index; ++index) {
        const Interval& before = interval_at(index - 1);
        const Interval& after = interval_at(index);
        if (local_ns - before.offset_ns >= after.begin_utc_ns &&
            local_ns - after.offset_ns < after.begin_utc_ns) {
            return UtcTime{.utc_ns = local_ns - before.offset_ns,
                           .uncertain = true};
        }
    }
    // Not reached for a table `from_tzif` accepts, whose window holds every
    // interval that may hold a local time; stated, not assumed.
    return UtcTime{.utc_ns = local_ns - interval_at(center).offset_ns,
                   .uncertain = true};
}

}  // namespace scada::driver
