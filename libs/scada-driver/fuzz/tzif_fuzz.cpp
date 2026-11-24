// Fuzz target for the zone file reader (TZif, RFC 9636) and the POSIX TZ
// string parser: zone files are outside input (decision D-130). The input
// is read as a TZ string and as a zone file; a file that is accepted builds
// a table, which converts local times at the ends of int64 and inside the
// table. A file the table accepts and the reader refuses aborts, and so
// does a table that breaks the properties of `check_zone`.

#include "../src/tzif.h"
#include "scada/driver/zone_table.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <string>

namespace {

constexpr std::int64_t NS_PER_S = 1'000'000'000;
constexpr std::int64_t DAY = 86'400;
constexpr std::int64_t FIRST_S = 2 * DAY;                   // 1970-01-03
constexpr std::int64_t LAST_S = 4'133'894'400 - (2 * DAY);  // 2100-12-30
constexpr std::size_t MAX_CHECKED = 64;

void check_round_trips(const scada::driver::detail::TzifZone& zone,
                       const scada::driver::ZoneTable& table);

// Properties of an accepted file and its table: transitions strictly
// increasing and offsets within RFC 9636's bounds; inside the table every
// conversion moves a local time by an offset within those bounds; and an
// instant between two transitions, written as local time with the offset
// the file gives it, converts back to itself (or, in a repeated hour, to an
// earlier instant, marked uncertain).
void check_zone(const scada::driver::detail::TzifZone& zone,
                const scada::driver::ZoneTable& table) {
    using scada::driver::detail::TZIF_MAX_OFFSET_S;
    using scada::driver::detail::TZIF_MIN_OFFSET_S;
    const auto& times = zone.transitions_s;
    if (zone.offsets_s.size() != times.size()) {
        std::abort();
    }
    for (std::size_t index = 0; index < times.size(); ++index) {
        if ((index > 0 && times[index] <= times[index - 1]) ||
            zone.offsets_s[index] < TZIF_MIN_OFFSET_S ||
            zone.offsets_s[index] > TZIF_MAX_OFFSET_S) {
            std::abort();
        }
    }
    check_round_trips(zone, table);
}

// Instants between two transitions inside the table, written as local
// time with the offset the file gives them, convert back (see check_zone).
void check_round_trips(const scada::driver::detail::TzifZone& zone,
                       const scada::driver::ZoneTable& table) {
    using scada::driver::detail::TZIF_MAX_OFFSET_S;
    using scada::driver::detail::TZIF_MIN_OFFSET_S;
    const auto& times = zone.transitions_s;
    const std::size_t count = std::min(times.size(), MAX_CHECKED);
    for (std::size_t index = 0; index + 1 < count; ++index) {
        // Only transitions inside the table: a file may hold any instant,
        // and the distance to the next one must not overflow.
        if (times[index] < FIRST_S || times[index] >= LAST_S) {
            continue;
        }
        for (const std::int64_t delta :
             {std::int64_t{0}, std::int64_t{1},
              (times[index + 1] - times[index]) / 2}) {
            const std::int64_t instant = times[index] + delta;
            if (instant < FIRST_S || instant >= LAST_S ||
                instant >= times[index + 1]) {
                continue;
            }
            const std::int64_t local = instant + zone.offsets_s[index];
            const scada::driver::UtcTime back = table.to_utc(local * NS_PER_S);
            const std::int64_t moved = local * NS_PER_S - back.utc_ns;
            if (moved < std::int64_t{TZIF_MIN_OFFSET_S} * NS_PER_S ||
                moved > std::int64_t{TZIF_MAX_OFFSET_S} * NS_PER_S) {
                std::abort();
            }
            if (back.uncertain ? back.utc_ns > instant * NS_PER_S
                               : back.utc_ns != instant * NS_PER_S) {
                std::abort();
            }
        }
    }
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::span<const std::byte> bytes =
        std::as_bytes(std::span(data, size));
    std::string text;
    text.reserve(size);
    for (const std::byte byte : bytes) {
        text.push_back(static_cast<char>(byte));
    }
    (void)scada::driver::detail::parse_tz_string(text);
    const auto table = scada::driver::ZoneTable::from_tzif(bytes);
    if (!table.ok()) {
        return 0;
    }
    constexpr std::int64_t SECOND = 1'000'000'000;
    constexpr std::int64_t YEAR_2000_S = 946'684'800;
    constexpr std::int64_t DAY_S = 86'400;
    const std::array<std::int64_t, 5> probes{
        std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max(), 0, YEAR_2000_S * SECOND,
        (YEAR_2000_S + (180 * DAY_S)) * SECOND};
    for (const std::int64_t local : probes) {
        (void)table.value().to_utc(local);
    }
    const auto zone = scada::driver::detail::parse_tzif(bytes);
    if (!zone.ok()) {
        std::abort();  // from_tzif accepted what parse_tzif refuses
    }
    check_zone(zone.value(), table.value());
    return 0;
}
