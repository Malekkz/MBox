// Unit tests for the retain file (contract, section 12; decision D-44): the
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
#include <unistd.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scada::tag;
using scada::Fd;
using scada::Result;
using scada::Status;
using scada::StatusCode;
using scada::tag::testing::TempDatabase;

constexpr std::uint64_t PI_BITS = 0x4009'21FB'5444'2D18ULL;
constexpr std::int64_t SOURCE_TIME = 1'700'000'000'000'000'000;
constexpr std::int64_t CREATED_TIME = 1'600'000'000'000'000'000;
constexpr std::int64_t WRITTEN_TIME = 1'650'000'000'000'000'000;
constexpr std::uint64_t GENERATION = 7;
constexpr std::uint8_t HASH_BYTE = 0xA5;

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> bytes;
    for (const char c : text) {
        bytes.push_back(static_cast<std::byte>(c));
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
        bytes.at(body + i) = static_cast<std::byte>((crc >> (8 * i)) & 0xFFU);
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
    EXPECT_EQ(bytes.size(), RETAIN_HEADER_BYTES + (4 * 24) + 8 + 16 + 16 + 8 +
                                RETAIN_CRC_BYTES);
    std::string detail;
    const Result<RetainFile> decoded = decode_retain_file(bytes, detail);
    ASSERT_TRUE(decoded.ok()) << detail;
    EXPECT_EQ(decoded.value().source, file.source);
    EXPECT_EQ(decoded.value().written_time_ns, WRITTEN_TIME);
    EXPECT_TRUE(decoded.value().final);
    EXPECT_EQ(decoded.value().config_hash, file.config_hash);
    ASSERT_EQ(decoded.value().values.size(), file.values.size());
    for (std::size_t i = 0; i < file.values.size(); ++i) {
        EXPECT_EQ(decoded.value().values[i].name, file.values[i].name);
        EXPECT_EQ(decoded.value().values[i].type, file.values[i].type);
        EXPECT_EQ(decoded.value().values[i].value, file.values[i].value);
        EXPECT_EQ(decoded.value().values[i].source_time_ns,
                  file.values[i].source_time_ns);
    }
}

TEST(RetainFile, AnEmptyFileIsValid) {
    RetainFile file;
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
        for (unsigned bit = 0; bit < 8; ++bit) {
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
    const auto damaged = [&](std::size_t offset, std::byte value) {
        std::vector<std::byte> copy = bytes;
        copy.at(offset) = value;
        reseal(copy);
        return decode_code(copy);
    };
    EXPECT_EQ(damaged(8, std::byte{2}), StatusCode::DATA_CORRUPT);   // version
    EXPECT_EQ(damaged(12, std::byte{5}), StatusCode::DATA_CORRUPT);  // count
    EXPECT_EQ(damaged(12, std::byte{3}), StatusCode::DATA_CORRUPT);  // count
    EXPECT_EQ(damaged(40, std::byte{2}), StatusCode::DATA_CORRUPT);  // flag
    EXPECT_EQ(damaged(44, std::byte{1}), StatusCode::DATA_CORRUPT);  // reserved
    const std::size_t first = RETAIN_HEADER_BYTES;
    EXPECT_EQ(damaged(first, std::byte{5}),
              StatusCode::DATA_CORRUPT);  // STRING
    EXPECT_EQ(damaged(first, std::byte{0}),
              StatusCode::DATA_CORRUPT);  // no type
    EXPECT_EQ(damaged(first + 2, std::byte{1}), StatusCode::DATA_CORRUPT);
    EXPECT_EQ(damaged(first + 8, std::byte{2}),
              StatusCode::DATA_CORRUPT);  // BOOL 2
    EXPECT_EQ(damaged(first + 24, std::byte{'A'}), StatusCode::DATA_CORRUPT);
    EXPECT_EQ(damaged(first + 24 + 6, std::byte{1}), StatusCode::DATA_CORRUPT);
    EXPECT_EQ(damaged(first + 1, std::byte{0}),
              StatusCode::DATA_CORRUPT);  // no name
    // The first name made to sort after the second.
    EXPECT_EQ(damaged(first + 24, std::byte{'q'}), StatusCode::DATA_CORRUPT);
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
    own.written_time_ns = 10;
    own.values = {{"t.a", TagType::INT64, 1, 0}, {"t.b", TagType::INT64, 2, 0}};
    write("ctl", own);
    RetainFile older;
    older.written_time_ns = 20;
    older.values = {{"t.a", TagType::INT64, 100, 0},
                    {"t.c", TagType::INT64, 300, 0}};
    write("old", older);
    RetainFile newer;
    newer.written_time_ns = 30;
    newer.values = {{"t.c", TagType::INT64, 333, 0},
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
    EXPECT_EQ(loaded.values[0]->value, 1U);      // own file, not the other
    EXPECT_FALSE(loaded.values[1].has_value());  // another type
    ASSERT_TRUE(loaded.values[2].has_value());
    EXPECT_EQ(loaded.values[2]->value, 333U);    // the newest other file
    EXPECT_FALSE(loaded.values[3].has_value());  // another type
    EXPECT_FALSE(loaded.values[4].has_value());
    ASSERT_TRUE(loaded.own.has_value());
    ASSERT_EQ(loaded.rejected.size(), 1U);
    EXPECT_EQ(loaded.rejected[0].file, "broken.bin");
}

TEST_F(RetainDirectory, ACorruptOwnFileIsRejectedAndOthersStillServe) {
    scada::tag::testing::touch(base() / "ctl.bin");
    RetainFile other;
    other.values = {{"t.a", TagType::INT64, 5, 0}};
    write("other", other);
    const std::vector<WantedValue> wanted = {{"t.a", TagType::INT64}};
    const LoadedValues loaded = load_retained(dir(), uid(), "ctl", wanted);
    EXPECT_FALSE(loaded.own.has_value());
    ASSERT_EQ(loaded.rejected.size(), 1U);
    ASSERT_TRUE(loaded.values[0].has_value());
    EXPECT_EQ(loaded.values[0]->value, 5U);
}

}  // namespace
