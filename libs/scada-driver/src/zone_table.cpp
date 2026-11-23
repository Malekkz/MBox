#include "scada/driver/zone_table.h"

#include "scada/common/result.h"
#include "scada/common/status.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <string_view>
#include <vector>

namespace scada::driver {

namespace {

constexpr std::int64_t NS_PER_S = 1'000'000'000;

// Seconds since 1970 of the first instant of `year` (UTC).
std::int64_t year_start_s(int year) noexcept {
    const std::chrono::sys_days day{std::chrono::year{year} / 1 / 1};
    return std::chrono::duration_cast<std::chrono::seconds>(
               day.time_since_epoch())
        .count();
}

// How many intervals on each side of the one found by a search may hold a
// local time: offsets and their changes are under a day, and no zone of the
// database changes its offset twice within a day.
constexpr std::ptrdiff_t SEARCH_WINDOW = 2;

}  // namespace

Result<ZoneTable> ZoneTable::build(std::string_view zone_name) noexcept {
    ZoneTable table;
    try {
        const std::chrono::time_zone* zone =
            std::chrono::locate_zone(zone_name);
        const std::chrono::sys_seconds first{
            std::chrono::seconds{year_start_s(ZONE_TABLE_FIRST_YEAR)}};
        const std::chrono::sys_seconds last{
            std::chrono::seconds{year_start_s(ZONE_TABLE_LAST_YEAR + 1)}};
        std::chrono::sys_seconds instant = first;
        while (instant < last) {
            const std::chrono::sys_info info = zone->get_info(instant);
            const std::int64_t begin_s =
                table.intervals_.empty()
                    ? year_start_s(ZONE_TABLE_FIRST_YEAR)
                    : info.begin.time_since_epoch().count();
            table.intervals_.push_back(
                Interval{.begin_utc_ns = begin_s * NS_PER_S,
                         .offset_ns = info.offset.count() * NS_PER_S});
            if (info.end <= instant) {
                break;  // the database's last interval: no further change
            }
            instant = info.end;
        }
        table.end_utc_ns_ = last.time_since_epoch().count() * NS_PER_S;
    } catch (const std::exception&) {
        // locate_zone reports an unknown name by an exception; get_info and
        // the vector may report an allocation failure the same way.
        return Status(StatusCode::NOT_FOUND, "time zone not found");
    }
    return table;
}

UtcTime ZoneTable::to_utc(std::int64_t local_ns) const noexcept {
    if (intervals_.empty()) {
        return UtcTime{.utc_ns = local_ns, .uncertain = false};
    }
    // Outside the table: the offset at the nearest end, marked. The bounds
    // also keep every subtraction below from overflowing.
    const std::int64_t first_ns = intervals_.front().begin_utc_ns;
    const std::int64_t last_ns = end_utc_ns_;
    if (local_ns < first_ns) {
        return UtcTime{.utc_ns = local_ns - intervals_.front().offset_ns,
                       .uncertain = true};
    }
    if (local_ns >= last_ns) {
        return UtcTime{.utc_ns = local_ns - intervals_.back().offset_ns,
                       .uncertain = true};
    }

    // The interval that holds `local_ns` read as UTC; the answer is in it or
    // next to it.
    const auto found =
        std::upper_bound(intervals_.begin(), intervals_.end(), local_ns,
                         [](std::int64_t value, const Interval& interval) {
                             return value < interval.begin_utc_ns;
                         });
    const std::ptrdiff_t center =
        std::max<std::ptrdiff_t>(0, (found - intervals_.begin()) - 1);
    const auto count = static_cast<std::ptrdiff_t>(intervals_.size());
    const std::ptrdiff_t from =
        std::max<std::ptrdiff_t>(0, center - SEARCH_WINDOW);
    const std::ptrdiff_t last_index =
        std::min(count - 1, center + SEARCH_WINDOW);

    auto end_of = [&](std::ptrdiff_t index) {
        return index + 1 < count
                   ? intervals_[static_cast<std::size_t>(index + 1)]
                         .begin_utc_ns
                   : std::numeric_limits<std::int64_t>::max();
    };

    std::size_t matches = 0;
    std::int64_t earliest = 0;
    for (std::ptrdiff_t index = from; index <= last_index; ++index) {
        const Interval& interval = intervals_[static_cast<std::size_t>(index)];
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
        const Interval& before =
            intervals_[static_cast<std::size_t>(index - 1)];
        const Interval& after = intervals_[static_cast<std::size_t>(index)];
        if (local_ns - before.offset_ns >= after.begin_utc_ns &&
            local_ns - after.offset_ns < after.begin_utc_ns) {
            return UtcTime{.utc_ns = local_ns - before.offset_ns,
                           .uncertain = true};
        }
    }
    // Not reached for a table built from the database; stated, not assumed.
    return UtcTime{
        .utc_ns =
            local_ns - intervals_[static_cast<std::size_t>(center)].offset_ns,
        .uncertain = true};
}

}  // namespace scada::driver
