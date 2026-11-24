// Every zone of the system's database, read by ZoneTable, against the C
// library's own reading of the same files (`localtime_r` with TZ set), an
// independent implementation (decision D-130). For each zone: every change
// of offset from 1970 to 2100 is found by a scan of the C library's offsets
// and refined to the second; local times around each change (before,
// inside a repeated or skipped interval, after) and one in every quarter
// are converted both ways. The expected answer is found by a full search:
// every offset the zone uses is tried, and a local time no offset reaches
// is moved forward by the change.
//
// Runs single-threaded: it sets TZ, which the C library reads globally.

#include "scada/driver/zone_table.h"

#include "tzif_builder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <ios>
#include <set>
#include <string>
#include <vector>

namespace scada::driver {
namespace {

constexpr std::int64_t SECOND = 1'000'000'000;
constexpr std::int64_t HOUR_S = 3600;
constexpr std::int64_t DAY_S = 86'400;
constexpr std::int64_t SCAN_STEP_S = DAY_S;
// The footers below have intervals of an hour or two.
constexpr std::int64_t SHORT_SCAN_STEP_S = HOUR_S / 2;
constexpr std::int64_t SEARCH_S = 27 * HOUR_S;  // more than any offset change
constexpr std::int64_t QUARTER_S = std::int64_t{91} * DAY_S;
constexpr std::int64_t FIRST_S = 2 * DAY_S;               // 1970-01-03
constexpr std::int64_t LAST_S = 4'133'894'400 - FIRST_S;  // 2100-12-30
constexpr std::size_t MIN_ZONES = 300;
constexpr std::size_t REPORTED_FAILURES = 20;

std::filesystem::path zone_directory() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test
    const char* tzdir = std::getenv("TZDIR");
    return tzdir != nullptr && *tzdir != '\0' ? tzdir : SCADA_ZONEINFO_DIR;
}

bool is_tzif(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::string magic(4, '\0');
    file.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    return file && magic == "TZif";
}

// Every zone file under the directory, by its name, without the `posix/`
// and `right/` copies (the second have leap seconds and are refused).
std::vector<std::string> zone_names(const std::filesystem::path& root) {
    std::vector<std::string> out;
    for (auto entry = std::filesystem::recursive_directory_iterator(root);
         entry != std::filesystem::recursive_directory_iterator(); ++entry) {
        const std::string name = entry->path().filename().string();
        if (entry->is_directory() && (name == "posix" || name == "right")) {
            entry.disable_recursion_pending();
            continue;
        }
        if (entry->is_regular_file() && name != "posixrules" &&
            name != "localtime" && is_tzif(entry->path())) {
            out.push_back(entry->path().lexically_relative(root).string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Sets TZ: `:name` for a zone file, or a POSIX TZ string as it stands.
void use_zone(const std::string& tz_value) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test
    ::setenv("TZ", tz_value.c_str(), 1);
    ::tzset();  // NOLINT(concurrency-mt-unsafe): single-threaded test
}

// The C library's offset at a UT second.
std::int64_t offset_at(std::int64_t utc_s) {
    const auto time = static_cast<std::time_t>(utc_s);
    std::tm local{};
    ::localtime_r(&time, &local);
    return local.tm_gmtoff;
}

// The second at which the offset changes from `before`, between `low` (still
// `before`) and `high` (not).
std::int64_t change_between(std::int64_t low, std::int64_t high,
                            std::int64_t before) {
    while (high - low > 1) {
        const std::int64_t middle = low + ((high - low) / 2);
        if (offset_at(middle) == before) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return high;
}

struct Expected {
    std::int64_t utc_s = 0;
    bool uncertain = false;
};

// The answer for a local time by a full search over the C library: every
// offset the zone uses anywhere (`zone_offsets`) is tried.
Expected expected_for(std::int64_t local_s,
                      const std::set<std::int64_t>& zone_offsets) {
    std::vector<std::int64_t> matches;
    for (const std::int64_t offset : zone_offsets) {
        if (offset_at(local_s - offset) == offset) {
            matches.push_back(local_s - offset);
        }
    }
    if (!matches.empty()) {
        return Expected{
            .utc_s = *std::min_element(matches.begin(), matches.end()),
            .uncertain = matches.size() > 1};
    }
    // Skipped: the change lies between local - max and local - min of the
    // offsets near it; the offset before the change moves the local time
    // forward.
    std::set<std::int64_t> offsets;
    for (std::int64_t at = local_s - SEARCH_S; at <= local_s + SEARCH_S;
         at += HOUR_S) {
        offsets.insert(offset_at(at));
    }
    const std::int64_t low = local_s - *offsets.rbegin();
    const std::int64_t change =
        change_between(low, local_s - *offsets.begin(), offset_at(low));
    return Expected{.utc_s = local_s - offset_at(change - 1),
                    .uncertain = true};
}

// The local times to check in the current zone, and every offset it uses.
struct Probes {
    std::vector<std::int64_t> locals;
    std::set<std::int64_t> offsets;
};

// `step` is the scan's step: an offset in force for less than a step can be
// missed, so a zone with such intervals needs a shorter one.
Probes probes_of_zone(std::int64_t step) {
    Probes probes;
    std::int64_t previous = offset_at(FIRST_S);
    probes.offsets.insert(previous);
    for (std::int64_t at = FIRST_S; at < LAST_S; at += step) {
        const std::int64_t offset = offset_at(at + step);
        if (offset == previous) {
            continue;
        }
        const std::int64_t change = change_between(at, at + step, previous);
        for (const std::int64_t base : {change + previous, change + offset}) {
            for (const std::int64_t delta :
                 {-HOUR_S - 1, std::int64_t{-1}, std::int64_t{0},
                  std::int64_t{1}, HOUR_S / 4, HOUR_S / 2, HOUR_S - 1, HOUR_S,
                  HOUR_S + 1}) {
                probes.locals.push_back(base + delta);
            }
        }
        previous = offset;
        probes.offsets.insert(offset);
    }
    for (std::int64_t at = FIRST_S; at < LAST_S; at += QUARTER_S) {
        probes.locals.push_back(at);
    }
    return probes;
}

struct Tally {
    std::size_t checked = 0;
    std::size_t failures = 0;
};

// Checks one zone (`tz_value` as TZ takes it) against the C library, adding to
// `tally`.
void check_zone(const std::string& tz_value, const ZoneTable& table,
                Tally& tally, std::int64_t step = SCAN_STEP_S) {
    use_zone(tz_value);
    const std::string& name = tz_value;
    const Probes probes = probes_of_zone(step);
    for (const std::int64_t local_s : probes.locals) {
        if (local_s < FIRST_S || local_s >= LAST_S) {
            continue;
        }
        const Expected want = expected_for(local_s, probes.offsets);
        const UtcTime got = table.to_utc(local_s * SECOND);
        ++tally.checked;
        const bool same = got.utc_ns == want.utc_s * SECOND &&
                          got.uncertain == want.uncertain;
        if (!same && ++tally.failures <= REPORTED_FAILURES) {
            ADD_FAILURE() << name << " local " << local_s << ": got "
                          << got.utc_ns / SECOND << (got.uncertain ? "?" : "")
                          << ", want " << want.utc_s
                          << (want.uncertain ? "?" : "");
        }
    }
}

TEST(ZoneTableAllZones, AgreesWithTheCLibraryInEveryZone) {
    const std::filesystem::path root = zone_directory();
    const std::vector<std::string> zones = zone_names(root);
    ASSERT_GT(zones.size(), MIN_ZONES) << "zone directory " << root;
    Tally tally;
    for (const std::string& name : zones) {
        const auto table = ZoneTable::build(name);
        ASSERT_TRUE(table.ok()) << name << ": " << table.status().message();
        check_zone(":" + name, table.value(), tally);
    }
    ::unsetenv("TZ");  // NOLINT(concurrency-mt-unsafe): single-threaded test
    EXPECT_EQ(tally.failures, 0U)
        << "of " << tally.checked << " local times in " << zones.size()
        << " zones";
    RecordProperty("local_times", static_cast<int>(tally.checked));
    RecordProperty("zones", static_cast<int>(zones.size()));
}

TEST(ZoneTableAllZones, DegenerateFooterRulesAgreeWithTheCLibrary) {
    // Rules no zone of the database uses today, read by the C library from
    // the same TZ string: a start and an end at one instant, a start and an
    // end on one day in that order and the other, times outside 0 to 24 h.
    // Daylight time all year (`EST5EDT,0/0,J365/25`, RFC 9636 3.3.1) is
    // checked in zone_table_test.cpp against the RFC instead: glibc 2.39
    // gives standard time from 00:00 UTC on 1 January to the rule's start.
    const std::vector<std::string> footers{
        "EST5EDT,M3.2.0/2,M3.2.0/3", "<-03>3<-02>,J60/0,J60/0",
        "<-03>3<-02>,J60/1,J60/0", "<-03>3<-02>,M3.5.0/-2,M10.5.0/-1",
        "<+01>-1<+02>,M3.5.0/167,M10.5.0/-167"};
    Tally tally;
    for (const std::string& footer : footers) {
        test::TzifSpec spec;
        spec.offsets = {0};
        spec.footer = footer;
        const auto table = ZoneTable::from_tzif(test::build_tzif(spec));
        ASSERT_TRUE(table.ok()) << footer << ": " << table.status().message();
        Tally one;
        check_zone(footer, table.value(), one, SHORT_SCAN_STEP_S);
        EXPECT_EQ(one.failures, 0U)
            << footer << ": of " << one.checked << " local times";
        tally.checked += one.checked;
    }
    ::unsetenv("TZ");  // NOLINT(concurrency-mt-unsafe): single-threaded test
    RecordProperty("local_times", static_cast<int>(tally.checked));
}

}  // namespace
}  // namespace scada::driver
