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
constexpr std::array<Damage, 13> DAMAGES{{
    {8, 2, "format version"},
    {12, 5, "record count beyond the file"},
    {12, 3, "record count below the records"},
    {40, 2, "unknown flag"},
    {44, 1, "reserved field"},
    {FIRST, 5, "STRING type"},
    {FIRST, 0, "no type"},
    {FIRST + 2, 1, "reserved byte of a record"},
    {FIRST + 8, 2, "BOOL of 2"},
    {FIRST + 24, 'A', "name with an upper-case letter"},
    {FIRST + 24 + 6, 1, "padding byte"},
    {FIRST + 1, 0, "empty name"},
    {FIRST + 24, 'q', "first name after the second"},
}};

// True if `left` and `right` hold the same values.
bool same_values(const RetainFile& left, const RetainFile& right) {
    if (left.values.size() != right.values.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.values.size(); ++i) {
        const RetainedValue& one = left.values[i];
        const RetainedValue& other = right.values[i];
        if (one.name != other.name || one.type != other.type ||
            one.value != other.value ||
            one.source_time_ns != other.source_time_ns) {
            return false;
        }
    }
    return true;
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

RetainFile sample() {
    RetainFile file;
    file.source = {GENERATION, CREATED_TIME};
    file.written_time_ns = WRITTEN_TIME;
    file.final = true;
    file.config_hash.fill(HASH_BYTE);
    file.values = {
        {"a.flag", TagType::BOOL, 1, SOURCE_TIME},
        {"plant.mode", TagType::INT64,
         std::bit_cast<std::uint64_t>(std::int64_t{-3}), SOURCE_TIME + 1},
        {"plant.setpoint", TagType::FLOAT64, PI_BITS, SOURCE_TIME + 2},
        {"z", TagType::UINT64, ~std::uint64_t{0}, 0},
    };
    return file;
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
    EXPECT_TRUE(same_values(decoded.value(), file));
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

TEST(RetainFile, EveryFlippedBitIsRejected) {
    const std::vector<std::byte> bytes = encoded(sample());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        for (std::size_t bit = 0; bit < BITS_PER_BYTE; ++bit) {
            std::vector<std::byte> damaged = bytes;
            damaged[i] ^= static_cast<std::byte>(1U << bit);
            ASSERT_EQ(decode_code(damaged), StatusCode::DATA_CORRUPT)
                << "byte " << i << " bit " << bit;
        }
    }
}

TEST(RetainFile, EveryTruncationIsRejected) {
    const std::vector<std::byte> bytes = encoded(sample());
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        const std::vector<std::byte> cut(
            bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size));
        ASSERT_EQ(decode_code(cut), StatusCode::DATA_CORRUPT) << size;
    }
    std::vector<std::byte> longer = bytes;
    longer.push_back(std::byte{0});
    EXPECT_EQ(decode_code(longer), StatusCode::DATA_CORRUPT);
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
