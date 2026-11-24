// Zone files are read beneath the zone directory (decision D-130; coding
// standard 10). Each case runs with openat2 (RESOLVE_BENEATH) and with the
// fallback that resolves the path in user space.

#include "../src/zone_file.h"

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/driver/zone_table.h"
#include "tzif_builder.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
#include <vector>

namespace scada::driver {
namespace {

using detail::BeneathMethod;
using detail::read_zone_file_beneath;

constexpr std::int32_t PLUS_ONE_S = 3600;
constexpr std::int64_t JUL01_1200_NS = 1'782'907'200'000'000'000;  // 2026

// A zone directory with one zone file, an alias of it inside the directory,
// links that lead out of it, a FIFO and a subdirectory; removed again when
// the test ends.
class ZoneDirectory {
public:
    ZoneDirectory()
        : base_(std::filesystem::temp_directory_path() /
                ("scada-zone-file-test-" + std::to_string(::getpid()))),
          zones_(base_ / "zones") {
        std::filesystem::remove_all(base_);
        std::filesystem::create_directories(zones_ / "Area");
        test::TzifSpec spec;
        spec.offsets = {PLUS_ONE_S};
        spec.footer = "<+01>-1";
        bytes_ = test::build_tzif(spec);
        write(zones_ / "Area" / "City", bytes_);
        write(base_ / "outside", bytes_);
        std::filesystem::create_directory_symlink("Area", zones_ / "Link");
        std::filesystem::create_symlink("Area/City", zones_ / "Alias");
        std::filesystem::create_symlink("../Area/City", zones_ / "Area" / "Up");
        std::filesystem::create_symlink("../outside", zones_ / "Escape");
        std::filesystem::create_symlink(base_ / "outside", zones_ / "Absolute");
        std::filesystem::create_directory_symlink("..", zones_ / "Parent");
        fifo_made_ =
            ::mkfifo((zones_ / "Fifo").c_str(), S_IRUSR | S_IWUSR) == 0;
    }
    ~ZoneDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(base_, ignored);
    }
    ZoneDirectory(const ZoneDirectory&) = delete;
    ZoneDirectory& operator=(const ZoneDirectory&) = delete;
    ZoneDirectory(ZoneDirectory&&) = delete;
    ZoneDirectory& operator=(ZoneDirectory&&) = delete;

    [[nodiscard]] Result<std::vector<std::byte>> read(
        const char* name, BeneathMethod method) const {
        return read_zone_file_beneath(zones_.string(), name, method);
    }
    [[nodiscard]] const std::filesystem::path& zones() const { return zones_; }
    [[nodiscard]] const std::vector<std::byte>& bytes() const { return bytes_; }
    [[nodiscard]] bool fifo_made() const { return fifo_made_; }

private:
    static void write(const std::filesystem::path& path,
                      const std::vector<std::byte>& bytes) {
        std::string text;
        text.reserve(bytes.size());
        for (const std::byte byte : bytes) {
            text.push_back(static_cast<char>(byte));
        }
        std::ofstream file(path, std::ios::binary);
        file << text;
    }

    std::filesystem::path base_;
    std::filesystem::path zones_;
    std::vector<std::byte> bytes_;
    bool fifo_made_ = false;
};

constexpr std::array<BeneathMethod, 2> METHODS{BeneathMethod::AUTOMATIC,
                                               BeneathMethod::RESOLVED};

TEST(ZoneFile, FileAndLinksInsideTheDirectoryAreRead) {
    const ZoneDirectory directory;
    for (const BeneathMethod method : METHODS) {
        for (const char* name :
             {"Area/City", "Alias", "Area/Up", "Link/City"}) {
            const auto bytes = directory.read(name, method);
            ASSERT_TRUE(bytes.ok()) << name;
            EXPECT_EQ(bytes.value(), directory.bytes()) << name;
        }
    }
}

TEST(ZoneFile, LinksThatLeadOutAreRefused) {
    const ZoneDirectory directory;
    for (const BeneathMethod method : METHODS) {
        for (const char* name : {"Escape", "Absolute", "Parent/outside"}) {
            EXPECT_EQ(directory.read(name, method).status().code(),
                      StatusCode::PERMISSION_DENIED)
                << name;
        }
    }
}

TEST(ZoneFile, AFifoOrADirectoryIsNotAZoneFileAndNeverBlocks) {
    const ZoneDirectory directory;
    ASSERT_TRUE(directory.fifo_made());
    for (const BeneathMethod method : METHODS) {
        EXPECT_EQ(directory.read("Fifo", method).status().code(),
                  StatusCode::DATA_CORRUPT);
        EXPECT_EQ(directory.read("Area", method).status().code(),
                  StatusCode::DATA_CORRUPT);
        EXPECT_EQ(directory.read("Area/Town", method).status().code(),
                  StatusCode::NOT_FOUND);
    }
}

TEST(ZoneFile, TheDirectoryMustBeAnAbsolutePath) {
    for (const BeneathMethod method : METHODS) {
        EXPECT_EQ(read_zone_file_beneath("zones", "Area/City", method)
                      .status()
                      .code(),
                  StatusCode::INVALID_ARGUMENT);
        EXPECT_EQ(
            read_zone_file_beneath("", "Area/City", method).status().code(),
            StatusCode::INVALID_ARGUMENT);
    }
}

TEST(ZoneFile, BuildReadsTheDirectoryOfTzdir) {
    const ZoneDirectory directory;
    // NOLINTBEGIN(concurrency-mt-unsafe): the unit tests run on one thread
    ::setenv("TZDIR", directory.zones().c_str(), 1);
    const auto alias = ZoneTable::build("Alias");
    const auto escape = ZoneTable::build("Escape");
    ::setenv("TZDIR", "zones", 1);
    const auto relative = ZoneTable::build("Alias");
    ::unsetenv("TZDIR");
    // NOLINTEND(concurrency-mt-unsafe)
    ASSERT_TRUE(alias.ok());
    const UtcTime noon = alias.value().to_utc(JUL01_1200_NS);
    EXPECT_FALSE(noon.uncertain);
    EXPECT_EQ(noon.utc_ns,
              JUL01_1200_NS - (std::int64_t{PLUS_ONE_S} * 1'000'000'000));
    EXPECT_EQ(escape.status().code(), StatusCode::PERMISSION_DENIED);
    EXPECT_EQ(relative.status().code(), StatusCode::INVALID_ARGUMENT);
}

TEST(ZoneNames, NamesTheSystemChoosesAreNotZoneNames) {
    for (const char* name : {"localtime", "posixrules", "posix/Europe/Berlin",
                             "right/Europe/Berlin", "posix", "right"}) {
        EXPECT_FALSE(is_zone_name(name)) << name;
        EXPECT_EQ(ZoneTable::build(name).status().code(),
                  StatusCode::INVALID_ARGUMENT)
            << name;
    }
    for (const char* name : {"Europe/Berlin", "Etc/UTC", "Posix/x", "rightx"}) {
        EXPECT_TRUE(is_zone_name(name)) << name;
    }
}

}  // namespace
}  // namespace scada::driver
