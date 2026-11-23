#ifndef SCADA_DRIVER_ZONE_TABLE_H
#define SCADA_DRIVER_ZONE_TABLE_H

// Conversion of a device's local time to UTC (driver interface contract
// 5.8; configuration schemas contract 4.6, `device_time`).
//
// A ZoneTable is built once, when a configuration is compiled, from the
// system's time zone database; converting a time is then a search in a
// table that does not change, with no allocation, lock or system call, so
// that it can run on any worker (decision D-130).

#include "scada/common/realtime.h"
#include "scada/common/result.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace scada::driver {

// The span of years a table covers. A local time outside it is converted
// with the offset at the nearest end and marked uncertain.
inline constexpr int ZONE_TABLE_FIRST_YEAR = 1970;
inline constexpr int ZONE_TABLE_LAST_YEAR = 2100;

// The result of converting one local time.
struct UtcTime {
    std::int64_t utc_ns = 0;
    // The local time was in a repeated hour (converted with the earlier
    // offset, the first occurrence), in a skipped hour (moved forward by the
    // offset change), or outside the table.
    bool uncertain = false;
};

// The offsets of one IANA zone over the years of the table.
class ZoneTable {
public:
    // A table for UTC: every local time is that UTC time.
    ZoneTable() noexcept = default;

    // Builds the table of the zone `zone_name` (an IANA name such as
    // "Europe/Berlin"). Allocates and reads the time zone database. Fails
    // with NOT_FOUND for a name the database does not have.
    [[nodiscard]] static Result<ZoneTable> build(
        std::string_view zone_name) noexcept;

    // Converts `local_ns`, nanoseconds since 1970-01-01 on the zone's local
    // clock, to UTC (configuration schemas contract 4.6).
    SCADA_RT [[nodiscard]] UtcTime to_utc(std::int64_t local_ns) const noexcept;

private:
    // One interval of a constant offset, from `begin_utc_ns` (inclusive) to
    // the next interval's begin.
    struct Interval {
        std::int64_t begin_utc_ns = 0;
        std::int64_t offset_ns = 0;
    };

    std::vector<Interval> intervals_;
    // The end of the table's last year (UTC).
    std::int64_t end_utc_ns_ = 0;
};

}  // namespace scada::driver

#endif  // SCADA_DRIVER_ZONE_TABLE_H
