// Unit tests for the retain file (contract, section 12; decision D-45): the
// format, its checks, the atomic write and the lookup of one owner's values.

#include "retain_file.h"
#include "crc32.h"
#include "temp_database.h"

#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scada::tag;
using scada::Fd;
using scada::Result;
using scada::StatusCode;
using scada::tag::testing::TempDatabase;

constexpr std::uint64_t PI_BITS = 0x4009'21FB'5444'2D18ULL;
constexpr std::int64_t SOURCE_TIME = 1'700'000'000'000'000'000;
constexpr std::int64_t CREATED_TIME = 1'600'000'000'000'000'000;
constexpr std::int64_t WRITTEN_TIME = 1'650'000'000'000'000'000;
constexpr std::uint64_t GENERATION = 7;
constexpr std::int64_t BOOT_TIME = 123'456'789'012;
// Ages and clocks of the age tests, in ns.
constexpr std::int64_t AN_HOUR = 3'600'000'000'000;
constexpr std::int64_t A_SECOND = 1'000'000'000;
constexpr std::uint8_t HASH_BYTE = 0xA5;
constexpr std::size_t BITS_PER_BYTE = 8;
constexpr std::uint32_t LOW_BYTE = 0xFFU;
// Times of three files of the precedence test, and values in them.
constexpr std::int64_t OWN_WRITTEN = 10;
constexpr std::int64_t OLDER_WRITTEN = 20;
constexpr std::int64_t NEWER_WRITTEN = 30;
constexpr std::uint64_t OLDER_A = 100;
constexpr std::uint64_t OLDER_C = 300;
constexpr std::uint64_t NEWER_C = 333;
constexpr std::uint64_t OTHER_A = 5;

// One damaged byte behind a valid check sum, and what it damages.
struct Damage {
    std::size_t offset = 0;
    std::uint8_t value = 0;
    const char* what = "";
};
// Offsets in the file of `sample()`: the header, then the first record
// ("a.flag", BOOL) at RETAIN_HEADER_BYTES.
constexpr std::size_t FIRST = RETAIN_HEADER_BYTES;
constexpr std::array<Damage, 22> DAMAGES{{
    {8, 3, "format version"},
    {12, 5, "record count beyond the file"},
    {12, 3, "record count below the records"},
    {40, 4, "unknown flag"},
    {44, 1, "reserved field"},
    {FIRST, 5, "STRING type"},
    {FIRST, 0, "no type"},
    {FIRST + 2, 2, "quality BAD"},
    {FIRST + 2, 3, "a quality that is none"},
    {FIRST + 3, 4, "an unknown record flag"},
    {FIRST + 3, 2, "SOURCE_NOT_FINAL without ADOPTED"},
    {FIRST + 4, 0x10, "NOT_INIT"},
    {FIRST + 4, 0x20, "OWNER_DOWN"},
    {FIRST + 5, 0x04, "RESTORED"},
    {FIRST + 5, 0x10, "a flag bit that is no flag"},
    {FIRST + 6, 1, "reserved byte after the flags"},
    {FIRST + 7, 1, "last reserved byte of a record"},
    {FIRST + 8, 2, "BOOL of 2"},
    {FIRST + 24, 'A', "name with an upper-case letter"},
    {FIRST + 24 + 6, 1, "padding byte"},
    {FIRST + 1, 0, "empty name"},
    {FIRST + 24, 'q', "first name after the second"},
}};
// The same for a file of format version 1 (`sample_v1()`).
constexpr std::size_t FIRST_V1 = RETAIN_HEADER_BYTES_V1;
constexpr std::array<Damage, 5> DAMAGES_V1{{
    {40, 2, "CLOCK_SYNCED, which version 1 does not have"},
    {FIRST_V1 + 2, 1, "first reserved byte of a record"},
    {FIRST_V1 + 4, 0x08, "flags, which version 1 does not have"},
    {FIRST_V1 + 7, 1, "last reserved byte of a record"},
    {FIRST_V1 + 8, 2, "BOOL of 2"},
}};

// True if `left` and `right` hold the same values.
bool same_values(const RetainFile& left, const RetainFile& right) {
    return left.values == right.values;
}

// The value `loaded` has for wanted tag `index`; 0 if it has none.
std::uint64_t value_at(const LoadedValues& loaded, std::size_t index) {
    return loaded.values.at(index).value_or(RetainedValue{}).value;
}

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> bytes;
    for (const char character : text) {
        bytes.push_back(static_cast<std::byte>(character));
    }
    return bytes;
}

constexpr std::uint16_t flag(QualityFlag which) {
    return to_raw(which);
}

// A file of version 2 with every field set.
RetainFile sample() {
    RetainFile file;
    file.source = {GENERATION, CREATED_TIME};
    file.written_time_ns = WRITTEN_TIME;
    file.final = true;
    file.config_hash.fill(HASH_BYTE);
    file.written_boottime_ns = BOOT_TIME;
    for (std::size_t i = 0; i < file.boot_id.size(); ++i) {
        file.boot_id.at(i) = static_cast<std::uint8_t>(i + 1);
    }
    file.clock_synced = true;
    file.values = {
        {"a.flag", TagType::BOOL, 1, SOURCE_TIME, Quality::GOOD,
         flag(QualityFlag::SUBSTITUTED)},
        {"plant.mode", TagType::INT64,
         std::bit_cast<std::uint64_t>(std::int64_t{-3}), SOURCE_TIME + 1,
         Quality::UNCERTAIN, 0},
        {"plant.setpoint", TagType::FLOAT64, PI_BITS, SOURCE_TIME + 2,
         Quality::GOOD,
         static_cast<std::uint16_t>(flag(QualityFlag::STALE) |
                                    flag(QualityFlag::LOCAL_TIME) |
                                    flag(QualityFlag::OVERFLOW))},
        {"z", TagType::UINT64, ~std::uint64_t{0}, 0, Quality::UNCERTAIN,
         static_cast<std::uint16_t>(retainable_flags(QUALITY_FLAGS_MASK)), true,
         true},
    };
    file.values.at(1).adopted = true;  // plant.mode, from a FINAL file
    return file;
}

// `sample()` as version 1 can hold it: UNCERTAIN, no flags, no stamp.
RetainFile sample_v1() {
    RetainFile file = sample();
    file.written_boottime_ns = 0;
    file.boot_id = {};
    file.clock_synced = false;
    for (RetainedValue& value : file.values) {
        value.quality = Quality::UNCERTAIN;
        value.quality_flags = 0;
        value.adopted = false;
        value.source_not_final = false;
    }
    return file;
}

std::vector<std::byte> encoded_v1(const RetainFile& file) {
    std::string detail;
    Result<std::vector<std::byte>> bytes = encode_retain_file_v1(file, detail);
    EXPECT_TRUE(bytes.ok()) << detail;
    return bytes.ok() ? bytes.value() : std::vector<std::byte>{};
}

std::vector<std::byte> encoded(const RetainFile& file) {
    std::string detail;
    Result<std::vector<std::byte>> bytes = encode_retain_file(file, detail);
    EXPECT_TRUE(bytes.ok()) << detail;
    return bytes.ok() ? bytes.value() : std::vector<std::byte>{};
}

// Puts a new check sum on `bytes`, so that a test reaches the check after it.
void reseal(std::vector<std::byte>& bytes) {
    const std::size_t body = bytes.size() - RETAIN_CRC_BYTES;
    const std::uint32_t crc = crc32(std::span(bytes).first(body));
    for (std::size_t i = 0; i < RETAIN_CRC_BYTES; ++i) {
        bytes.at(body + i) =
            static_cast<std::byte>((crc >> (BITS_PER_BYTE * i)) & LOW_BYTE);
    }
}

StatusCode decode_code(const std::vector<std::byte>& bytes) {
    std::string detail;
    return decode_retain_file(bytes, detail).status().code();
}

TEST(Crc32, MatchesTheCheckValueOfTheStandard) {
    EXPECT_EQ(crc32(bytes_of("123456789")), 0xCBF4'3926U);
    EXPECT_EQ(crc32({}), 0U);
}

TEST(RetainFile, RoundTripsEveryField) {
    const RetainFile file = sample();
    const std::vector<std::byte> bytes = encoded(file);
    // Header, four records of 24 bytes and their padded names, check sum.
    constexpr std::size_t NAMES = 8 + 16 + 16 + 8;
    EXPECT_EQ(bytes.size(), RETAIN_HEADER_BYTES +
                                (std::size_t{4} * RETAIN_RECORD_FIXED_BYTES) +
                                NAMES + RETAIN_CRC_BYTES);
    std::string detail;
    const Result<RetainFile> decoded = decode_retain_file(bytes, detail);
    ASSERT_TRUE(decoded.ok()) << detail;
    EXPECT_EQ(decoded.value().source, file.source);
    EXPECT_EQ(decoded.value().written_time_ns, WRITTEN_TIME);
    EXPECT_TRUE(decoded.value().final);
    EXPECT_EQ(decoded.value().config_hash, file.config_hash);
    EXPECT_EQ(decoded.value().version, RETAIN_FORMAT_VERSION);
    EXPECT_EQ(decoded.value().written_boottime_ns, BOOT_TIME);
    EXPECT_EQ(decoded.value().boot_id, file.boot_id);
    EXPECT_TRUE(decoded.value().clock_synced);
    EXPECT_TRUE(same_values(decoded.value(), file));
}

TEST(RetainFile, AFileOfVersion1IsReadAsUncertainWithoutFlags) {
    const RetainFile file = sample_v1();
    const std::vector<std::byte> bytes = encoded_v1(file);
    constexpr std::size_t NAMES = 8 + 16 + 16 + 8;
    EXPECT_EQ(bytes.size(), RETAIN_HEADER_BYTES_V1 +
                                (std::size_t{4} * RETAIN_RECORD_FIXED_BYTES) +
                                NAMES + RETAIN_CRC_BYTES);
    std::string detail;
    const Result<RetainFile> decoded = decode_retain_file(bytes, detail);
    ASSERT_TRUE(decoded.ok()) << detail;
    EXPECT_EQ(decoded.value().version, RETAIN_FORMAT_VERSION_1);
    EXPECT_EQ(decoded.value().source, file.source);
    EXPECT_TRUE(decoded.value().final);
    EXPECT_EQ(decoded.value().written_boottime_ns, 0);
    EXPECT_EQ(decoded.value().boot_id, BootId{});
    EXPECT_FALSE(decoded.value().clock_synced);
    EXPECT_TRUE(same_values(decoded.value(), file));
}

TEST(RetainFile, EveryRecordOfVersion1IsUncertainWithoutFlags) {
    std::string detail;
    const Result<RetainFile> decoded =
        decode_retain_file(encoded_v1(sample_v1()), detail);
    ASSERT_TRUE(decoded.ok()) << detail;
    ASSERT_EQ(decoded.value().values.size(), 4U);
    EXPECT_TRUE(std::ranges::all_of(
        decoded.value().values, [](const RetainedValue& value) {
            return value.quality == Quality::UNCERTAIN &&
                   value.quality_flags == 0;
        }));
}

TEST(RetainFile, TheWriterWritesVersion2) {
    // A file encoded for writing is version 2 whatever version it was read
    // from.
    std::string detail;
    Result<RetainFile> decoded =
        decode_retain_file(encoded_v1(sample_v1()), detail);
    ASSERT_TRUE(decoded.ok()) << detail;
    const std::vector<std::byte> again = encoded(decoded.value());
    ASSERT_GE(again.size(), 12U);
    EXPECT_EQ(std::to_integer<int>(again.at(8)), 2);
    const Result<RetainFile> read = decode_retain_file(again, detail);
    ASSERT_TRUE(read.ok()) << detail;
    EXPECT_EQ(read.value().version, RETAIN_FORMAT_VERSION);
}

TEST(RetainFile, AnEmptyFileIsValid) {
    const RetainFile file;
    const std::vector<std::byte> bytes = encoded(file);
    EXPECT_EQ(bytes.size(), RETAIN_HEADER_BYTES + RETAIN_CRC_BYTES);
    EXPECT_EQ(decode_code(bytes), StatusCode::OK);
}

TEST(RetainFile, EncodeRefusesWhatTheFormatCannotHold) {
    std::string detail;
    RetainFile file = sample();
    file.values[0].type = TagType::STRING;
    EXPECT_EQ(encode_retain_file(file, detail).status().code(),
              StatusCode::INVALID_ARGUMENT);
    file = sample();
    file.values[0].value = 2;  // a BOOL
    EXPECT_EQ(encode_retain_file(file, detail).status().code(),
              StatusCode::INVALID_ARGUMENT);
    file = sample();
    std::swap(file.values[0], file.values[1]);
    EXPECT_EQ(encode_retain_file(file, detail).status().code(),
              StatusCode::INVALID_ARGUMENT);
    file = sample();
    file.values[1].name = file.values[0].name;
    EXPECT_EQ(encode_retain_file(file, detail).status().code(),
              StatusCode::INVALID_ARGUMENT);
    file = sample();
    file.values[0].name = "Upper";
    EXPECT_EQ(encode_retain_file(file, detail).status().code(),
              StatusCode::INVALID_ARGUMENT);
}

// Decision D-59: a quality other than GOOD or UNCERTAIN, a bit that is no
// flag, and the flags that describe a record, not a value.
TEST(RetainFile, EncodeRefusesAQualityOrFlagAFileCannotHold) {
    std::string detail;
    RetainFile file = sample();
    file.values[0].quality = Quality::BAD;
    EXPECT_EQ(encode_retain_file(file, detail).status().code(),
              StatusCode::INVALID_ARGUMENT);
    for (const std::uint16_t refused :
         {flag(QualityFlag::NOT_INIT), flag(QualityFlag::OWNER_DOWN),
          flag(QualityFlag::RESTORED), std::uint16_t{1U << 12U},
          std::uint16_t{0x8000}}) {
        file = sample();
        file.values[1].quality_flags = refused;
        EXPECT_EQ(encode_retain_file(file, detail).status().code(),
                  StatusCode::INVALID_ARGUMENT)
            << refused;
    }
}

TEST(RetainFile, EncodeRefusesSourceNotFinalWithoutAdopted) {
    std::string detail;
    RetainFile file = sample();
    file.values[0].source_not_final = true;  // not adopted
    EXPECT_EQ(encode_retain_file(file, detail).status().code(),
              StatusCode::INVALID_ARGUMENT);
    file = sample_v1();
    file.values[0].adopted = true;
    EXPECT_EQ(encode_retain_file_v1(file, detail).status().code(),
              StatusCode::INVALID_ARGUMENT);
}

TEST(RetainFile, Version1EncodeRefusesWhatOnlyVersion2Holds) {
    std::string detail;
    EXPECT_TRUE(encode_retain_file_v1(sample_v1(), detail).ok()) << detail;
    RetainFile file = sample_v1();
    file.values[0].quality = Quality::GOOD;
    EXPECT_EQ(encode_retain_file_v1(file, detail).status().code(),
              StatusCode::INVALID_ARGUMENT);
    file = sample_v1();
    file.values[0].quality_flags = flag(QualityFlag::SUBSTITUTED);
    EXPECT_EQ(encode_retain_file_v1(file, detail).status().code(),
              StatusCode::INVALID_ARGUMENT);
    file = sample_v1();
    file.clock_synced = true;
    EXPECT_EQ(encode_retain_file_v1(file, detail).status().code(),
              StatusCode::INVALID_ARGUMENT);
    file = sample_v1();
    file.written_boottime_ns = 1;
    EXPECT_EQ(encode_retain_file_v1(file, detail).status().code(),
              StatusCode::INVALID_ARGUMENT);
    file = sample_v1();
    file.boot_id.at(0) = 1;
    EXPECT_EQ(encode_retain_file_v1(file, detail).status().code(),
              StatusCode::INVALID_ARGUMENT);
}

// Every single flipped bit of `bytes` makes the file rejected.
::testing::AssertionResult every_flip_rejected(
    const std::vector<std::byte>& bytes) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        for (std::size_t bit = 0; bit < BITS_PER_BYTE; ++bit) {
            std::vector<std::byte> damaged = bytes;
            damaged[i] ^= static_cast<std::byte>(1U << bit);
            if (decode_code(damaged) != StatusCode::DATA_CORRUPT) {
                return ::testing::AssertionFailure()
                       << "byte " << i << " bit " << bit;
            }
        }
    }
    return ::testing::AssertionSuccess();
}

// Every prefix of `bytes`, and `bytes` with a byte more, is rejected.
::testing::AssertionResult every_cut_rejected(
    const std::vector<std::byte>& bytes) {
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        const std::vector<std::byte> cut(
            bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size));
        if (decode_code(cut) != StatusCode::DATA_CORRUPT) {
            return ::testing::AssertionFailure() << "cut to " << size;
        }
    }
    std::vector<std::byte> longer = bytes;
    longer.push_back(std::byte{0});
    if (decode_code(longer) != StatusCode::DATA_CORRUPT) {
        return ::testing::AssertionFailure() << "a byte more";
    }
    return ::testing::AssertionSuccess();
}

TEST(RetainFile, EveryFlippedBitIsRejected) {
    EXPECT_TRUE(every_flip_rejected(encoded(sample())));
    EXPECT_TRUE(every_flip_rejected(encoded_v1(sample_v1())));
}

TEST(RetainFile, EveryTruncationIsRejected) {
    EXPECT_TRUE(every_cut_rejected(encoded(sample())));
    EXPECT_TRUE(every_cut_rejected(encoded_v1(sample_v1())));
}

TEST(RetainFile, AVersion2HeaderCutToTheLengthOfVersion1IsRejected) {
    // A version 2 header whose last 24 bytes are missing, sealed again.
    std::vector<std::byte> bytes = encoded(RetainFile{});
    bytes.erase(bytes.begin() + RETAIN_HEADER_BYTES_V1,
                bytes.begin() + RETAIN_HEADER_BYTES);
    reseal(bytes);
    EXPECT_EQ(decode_code(bytes), StatusCode::DATA_CORRUPT);
}

// Damage that comes with a valid check sum, as a bug in a writer would make.
TEST(RetainFile, FieldChecksHoldBehindAValidCheckSum) {
    const std::vector<std::byte> bytes = encoded(sample());
    for (const Damage& damage : DAMAGES) {
        std::vector<std::byte> copy = bytes;
        copy.at(damage.offset) = static_cast<std::byte>(damage.value);
        reseal(copy);
        EXPECT_EQ(decode_code(copy), StatusCode::DATA_CORRUPT) << damage.what;
    }
    const std::vector<std::byte> old_format = encoded_v1(sample_v1());
    for (const Damage& damage : DAMAGES_V1) {
        std::vector<std::byte> copy = old_format;
        copy.at(damage.offset) = static_cast<std::byte>(damage.value);
        reseal(copy);
        EXPECT_EQ(decode_code(copy), StatusCode::DATA_CORRUPT) << damage.what;
    }
}

TEST(RetainFile, RestoreCarriesTheFlagsThatDescribeTheValue) {
    // Decision D-59: STALE and COMM_FAIL describe the live link.
    EXPECT_EQ(RETAIN_CARRIED_FLAGS,
              flag(QualityFlag::SUBSTITUTED) | flag(QualityFlag::BLOCKED) |
                  flag(QualityFlag::TEST) | flag(QualityFlag::OUT_OF_RANGE) |
                  flag(QualityFlag::OVERFLOW) | flag(QualityFlag::LOCAL_TIME) |
                  flag(QualityFlag::TIME_UNCERTAIN));
    EXPECT_EQ(RETAIN_CARRIED_FLAGS & flag(QualityFlag::STALE), 0);
    EXPECT_EQ(RETAIN_CARRIED_FLAGS & flag(QualityFlag::COMM_FAIL), 0);
    EXPECT_EQ(retainable_flags(0xFFFFU),
              QUALITY_FLAGS_MASK & ~(flag(QualityFlag::NOT_INIT) |
                                     flag(QualityFlag::OWNER_DOWN) |
                                     flag(QualityFlag::RESTORED)));
}

// The age rule of decision D-59.
TEST(RetainAge, InTheSameBootTheBootClockGivesTheAge) {
    RetainStamp stamp;
    stamp.boot_id.fill(1);
    stamp.written_boottime_ns = A_SECOND;
    stamp.written_time_ns = WRITTEN_TIME;
    RetainClockSample now;
    now.boot_id = stamp.boot_id;
    now.boottime_ns = A_SECOND + AN_HOUR;
    // The wall clock is not used in the same boot, even if it went back.
    now.realtime_ns = 0;
    EXPECT_EQ(retained_age_ns(stamp, now), AN_HOUR);
    now.boottime_ns = 0;  // before the write: no age
    EXPECT_FALSE(retained_age_ns(stamp, now).has_value());
}

TEST(RetainAge, InAnotherBootOnlyASynchronisedClockGivesTheAge) {
    RetainStamp stamp;
    stamp.boot_id.fill(1);
    stamp.written_boottime_ns = AN_HOUR;
    stamp.written_time_ns = WRITTEN_TIME;
    stamp.clock_synced = true;
    RetainClockSample now;
    now.boot_id.fill(2);
    now.boottime_ns = A_SECOND;
    now.realtime_ns = WRITTEN_TIME + AN_HOUR;
    now.clock_synced = true;
    EXPECT_EQ(retained_age_ns(stamp, now), AN_HOUR);
    now.clock_synced = false;  // not synchronised now
    EXPECT_FALSE(retained_age_ns(stamp, now).has_value());
    now.clock_synced = true;
    stamp.clock_synced = false;  // not synchronised at the write
    EXPECT_FALSE(retained_age_ns(stamp, now).has_value());
    stamp.clock_synced = true;
    now.realtime_ns = WRITTEN_TIME - A_SECOND;  // a negative difference
    EXPECT_FALSE(retained_age_ns(stamp, now).has_value());
}

TEST(RetainAge, AnUnknownBootIdIsNeverTheSameBoot) {
    // A file of version 1 has no boot id and no CLOCK_SYNCED.
    RetainStamp stamp;
    stamp.written_time_ns = WRITTEN_TIME;
    RetainClockSample now;
    now.boottime_ns = AN_HOUR;
    now.realtime_ns = WRITTEN_TIME + A_SECOND;
    now.clock_synced = true;
    EXPECT_FALSE(retained_age_ns(stamp, now).has_value());
    // Neither side knows its boot: not the same boot either.
    stamp.clock_synced = true;
    EXPECT_EQ(retained_age_ns(stamp, now), A_SECOND);
}

// Committee review of PR #91, B2: the stamp of a file that holds values of
// several sources.
TEST(RetainAge, TheOldestStampOfOneBootTakesTheEarliestTimes) {
    RetainStamp first;
    first.boot_id.fill(1);
    first.written_time_ns = WRITTEN_TIME;
    first.written_boottime_ns = AN_HOUR;
    first.clock_synced = true;
    RetainStamp second = first;
    second.written_time_ns = WRITTEN_TIME + A_SECOND;
    second.written_boottime_ns = A_SECOND;
    second.clock_synced = false;
    const std::array stamps{first, second};
    const RetainStamp oldest = oldest_stamp(stamps);
    EXPECT_EQ(oldest.boot_id, first.boot_id);
    EXPECT_EQ(oldest.written_time_ns, WRITTEN_TIME);
    EXPECT_EQ(oldest.written_boottime_ns, A_SECOND);
    EXPECT_FALSE(oldest.clock_synced);
}

TEST(RetainAge, TheOldestStampOfTwoBootsLeavesOnlyTheWallClock) {
    RetainStamp first;
    first.boot_id.fill(1);
    first.written_time_ns = WRITTEN_TIME + A_SECOND;
    first.clock_synced = true;
    RetainStamp second = first;
    second.boot_id.fill(2);
    second.written_time_ns = WRITTEN_TIME;
    std::array stamps{first, second};
    RetainStamp oldest = oldest_stamp(stamps);
    EXPECT_EQ(oldest.boot_id, BootId{});
    EXPECT_EQ(oldest.written_boottime_ns, 0);
    EXPECT_EQ(oldest.written_time_ns, WRITTEN_TIME);
    EXPECT_TRUE(oldest.clock_synced);
    // One of them not synchronised: no age at all.
    stamps.at(1).clock_synced = false;
    oldest = oldest_stamp(stamps);
    RetainClockSample now;
    now.realtime_ns = WRITTEN_TIME + AN_HOUR;
    now.clock_synced = true;
    EXPECT_FALSE(retained_age_ns(oldest, now).has_value());
}

TEST(RetainAge, TheClockOfThisBootIsRead) {
    const RetainClockSample first = retain_clock_now();
    const RetainClockSample second = retain_clock_now();
    EXPECT_NE(first.boot_id, BootId{});  // Linux has a boot id
    EXPECT_EQ(first.boot_id, second.boot_id);
    EXPECT_GT(first.boottime_ns, 0);
    EXPECT_GE(second.boottime_ns, first.boottime_ns);
    RetainStamp stamp;
    stamp.boot_id = first.boot_id;
    stamp.written_boottime_ns = first.boottime_ns;
    EXPECT_GE(retained_age_ns(stamp, second).value_or(-1), 0);
}

TEST(RetainFile, FileNames) {
    EXPECT_EQ(retain_file_name("control.slow"), "control.slow.bin");
    EXPECT_EQ(owner_of_retain_file("control.slow.bin"), "control.slow");
    EXPECT_FALSE(owner_of_retain_file(".bin").has_value());
    EXPECT_FALSE(owner_of_retain_file("Control.bin").has_value());
    EXPECT_FALSE(owner_of_retain_file("control").has_value());
    EXPECT_FALSE(owner_of_retain_file(".tmp.a.bin.12").has_value());
    EXPECT_FALSE(
        owner_of_retain_file(std::string(32, 'a') + ".bin").has_value());
}

class RetainDirectory : public TempDatabase {
protected:
    void SetUp() override {
        TempDatabase::SetUp();
        directory_ = scada::tag::testing::open_directory(base());
    }

    [[nodiscard]] int dir() const { return directory_.get(); }

    void write(std::string_view owner, const RetainFile& file) {
        std::string detail;
        ASSERT_TRUE(write_retain_file(dir(), owner, file, detail).ok())
            << detail;
    }

private:
    Fd directory_;
};

TEST_F(RetainDirectory, WritesAtomicallyWithMode0640UnderUmask077) {
    const mode_t old = ::umask(scada::tag::testing::UMASK_077);
    write("ctl", sample());
    ::umask(old);
    EXPECT_EQ(scada::tag::testing::names_in(base()),
              std::vector<std::string>{"ctl.bin"});
    EXPECT_EQ(scada::tag::testing::mode_of(base() / "ctl.bin"), 0640U);
    std::string detail;
    const Result<RetainFile> read =
        read_retain_file(dir(), "ctl.bin", uid(), detail);
    ASSERT_TRUE(read.ok()) << detail;
    EXPECT_EQ(read.value().values.size(), 4U);

    // A second write replaces the first.
    RetainFile smaller = sample();
    smaller.values.resize(1);
    write("ctl", smaller);
    ASSERT_TRUE(read_retain_file(dir(), "ctl.bin", uid(), detail).ok());
    EXPECT_EQ(
        read_retain_file(dir(), "ctl.bin", uid(), detail).value().values.size(),
        1U);
    EXPECT_EQ(scada::tag::testing::names_in(base()),
              std::vector<std::string>{"ctl.bin"});
}

TEST_F(RetainDirectory, ReadChecksOwnerTypeAndPresence) {
    std::string detail;
    EXPECT_EQ(
        read_retain_file(dir(), "none.bin", uid(), detail).status().code(),
        StatusCode::NOT_FOUND);
    write("ctl", sample());
    EXPECT_EQ(
        read_retain_file(dir(), "ctl.bin", other_uid(), detail).status().code(),
        StatusCode::PERMISSION_DENIED);
    std::filesystem::create_symlink("ctl.bin", base() / "link.bin");
    EXPECT_EQ(
        read_retain_file(dir(), "link.bin", uid(), detail).status().code(),
        StatusCode::DATA_CORRUPT);
    std::filesystem::create_directory(base() / "dir.bin");
    EXPECT_EQ(read_retain_file(dir(), "dir.bin", uid(), detail).status().code(),
              StatusCode::DATA_CORRUPT);
    {
        std::ofstream truncated(base() / "cut.bin", std::ios::binary);
        truncated << "SCADARTN";
    }
    EXPECT_EQ(read_retain_file(dir(), "cut.bin", uid(), detail).status().code(),
              StatusCode::DATA_CORRUPT);
}

TEST_F(RetainDirectory, StaleTemporariesAreRemovedAndNothingElse) {
    write("ctl", sample());
    scada::tag::testing::touch(base() / ".tmp.ctl.bin.4242");
    scada::tag::testing::touch(base() / ".tmp.ctl.bin.x");
    scada::tag::testing::touch(base() / "notes.txt");
    EXPECT_EQ(remove_retain_temporaries(dir()), 1U);
    EXPECT_EQ(
        scada::tag::testing::names_in(base()),
        (std::vector<std::string>{".tmp.ctl.bin.x", "ctl.bin", "notes.txt"}));
}

TEST_F(RetainDirectory, LoadTakesTheOwnFileFirstThenTheNewestOther) {
    RetainFile own;
    own.written_time_ns = OWN_WRITTEN;
    own.values = {{"t.a", TagType::INT64, 1, 0}, {"t.b", TagType::INT64, 2, 0}};
    write("ctl", own);
    RetainFile older;
    older.written_time_ns = OLDER_WRITTEN;
    older.values = {{"t.a", TagType::INT64, OLDER_A, 0},
                    {"t.c", TagType::INT64, OLDER_C, 0}};
    write("old", older);
    RetainFile newer;
    newer.written_time_ns = NEWER_WRITTEN;
    newer.values = {{"t.c", TagType::INT64, NEWER_C, 0},
                    {"t.d", TagType::BOOL, 1, 0}};
    write("new", newer);
    scada::tag::testing::touch(base() / "broken.bin");

    const std::vector<WantedValue> wanted = {{"t.a", TagType::INT64},
                                             {"t.b", TagType::FLOAT64},
                                             {"t.c", TagType::INT64},
                                             {"t.d", TagType::INT64},
                                             {"t.e", TagType::INT64}};
    const LoadedValues loaded = load_retained(dir(), uid(), "ctl", wanted);
    ASSERT_EQ(loaded.values.size(), wanted.size());
    ASSERT_TRUE(loaded.values[0].has_value());
    EXPECT_EQ(value_at(loaded, 0), 1U);          // own file, not the other
    EXPECT_FALSE(loaded.values[1].has_value());  // another type
    ASSERT_TRUE(loaded.values[2].has_value());
    EXPECT_EQ(value_at(loaded, 2), NEWER_C);     // the newest other file
    EXPECT_FALSE(loaded.values[3].has_value());  // another type
    EXPECT_FALSE(loaded.values[4].has_value());
    ASSERT_TRUE(loaded.own.has_value());
    ASSERT_EQ(loaded.rejected.size(), 1U);
    EXPECT_EQ(loaded.rejected[0].file, "broken.bin");
    // Where each value came from (decision D-59).
    ASSERT_EQ(loaded.sources.size(), wanted.size());
    EXPECT_TRUE(loaded.sources[0].own);
    EXPECT_EQ(loaded.sources[0].stamp.written_time_ns, OWN_WRITTEN);
    EXPECT_FALSE(loaded.sources[2].own);
    EXPECT_EQ(loaded.sources[2].stamp.written_time_ns, NEWER_WRITTEN);
}

TEST_F(RetainDirectory, LoadKeepsQualityFlagsAndTheMarkOfTheFile) {
    write("ctl", sample());
    const std::vector<WantedValue> wanted = {
        {"a.flag", TagType::BOOL}, {"plant.setpoint", TagType::FLOAT64}};
    const LoadedValues loaded = load_retained(dir(), uid(), "ctl", wanted);
    ASSERT_TRUE(loaded.values[0].has_value());
    const RetainedValue first = loaded.values[0].value_or(RetainedValue{});
    EXPECT_EQ(first.quality, Quality::GOOD);
    EXPECT_EQ(first.quality_flags, flag(QualityFlag::SUBSTITUTED));
    EXPECT_TRUE(loaded.sources[0].own);
    EXPECT_TRUE(loaded.sources[0].final);
    EXPECT_EQ(loaded.sources[1].stamp, stamp_of(sample()));
}

TEST_F(RetainDirectory, ACorruptOwnFileIsRejectedAndOthersStillServe) {
    scada::tag::testing::touch(base() / "ctl.bin");
    RetainFile other;
    other.values = {{"t.a", TagType::INT64, OTHER_A, 0}};
    write("other", other);
    const std::vector<WantedValue> wanted = {{"t.a", TagType::INT64}};
    const LoadedValues loaded = load_retained(dir(), uid(), "ctl", wanted);
    EXPECT_FALSE(loaded.own.has_value());
    ASSERT_EQ(loaded.rejected.size(), 1U);
    ASSERT_TRUE(loaded.values[0].has_value());
    EXPECT_EQ(value_at(loaded, 0), OTHER_A);
}

}  // namespace

namespace {

// Review D1 of #93: a file that cannot be opened makes the look incomplete.
TEST_F(RetainDirectory, AnOtherFileThatCannotBeOpenedMakesTheLookIncomplete) {
    RetainFile other;
    other.values = {{"t.a", TagType::INT64, OTHER_A, 0}};
    write("other", other);
    const std::vector<WantedValue> wanted = {{"t.a", TagType::INT64}};
    EXPECT_TRUE(load_retained(dir(), uid(), "ctl", wanted).complete);
    std::filesystem::create_symlink("other.bin", base() / "link.bin");
    const LoadedValues loaded = load_retained(dir(), uid(), "ctl", wanted);
    EXPECT_FALSE(loaded.complete);
    ASSERT_EQ(loaded.rejected.size(), 1U);
    EXPECT_EQ(loaded.rejected[0].file, "link.bin");
}

}  // namespace
