// Fuzz target for the zone file reader (TZif, RFC 9636) and the POSIX TZ
// string parser: zone files are outside input (decision D-130). The input
// is read as a TZ string and as a zone file; a file that is accepted builds
// a table, which converts local times at the ends of int64 and inside the
// table. A file the table accepts and the reader refuses aborts.

#include "../src/tzif.h"
#include "scada/driver/zone_table.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <string>

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
    return 0;
}
