// The TagWriter of a channel worker and the writes of 9.1 against a real
// scada-tagd, in this process attached as `drv-test` (driver interface
// contract 9.1; decision D-142; delta 5 of the review of PR 150): a STRING
// write waits for the notify, a point without a tag is skipped, a refused
// write is counted, and 9.1 by owner slot keeps every value as stored, with
// SUBSTITUTED on a kept substitute (D-145).

#include "tagd_fixture.h"

#include "scada/common/status.h"
#include "scada/driver/compiled.h"
#include "scada/driver/sample_processing.h"
#include "scada/driver/tag_value.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/types.h"
#include "scada/test/temp_directory.h"
#include "tag_writer.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::test::driver_it {
namespace {

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers): the test data

using scada::driver::ProcessedSample;
using scada::driver::TagValue;
using scada::driver::TagWrite;
using scada::driver::detail::PointHandles;
using scada::driver::detail::PointSeeds;
using scada::driver::detail::TagWriter;
using scada::driver::detail::write_owner_not_initialised;
using scada::test::tagd::RunningTagd;

constexpr mode_t BASE_DIR_MODE = 02750;

ProcessedSample good(TagValue value) {
    ProcessedSample sample;
    sample.write_tag = true;
    sample.tag = TagWrite{.value = value, .quality = tag::Quality::GOOD};
    return sample;
}

class TagWriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::optional<scada::test::TempDirectory> temp =
            scada::test::TempDirectory::create("scada-tag-writer-it-");
        if (!temp.has_value()) {
            FAIL() << "no temporary directory";
        }
        temp_.emplace(std::move(*temp));
        const std::filesystem::path base = temp_->path() / "base";
        std::filesystem::create_directory(base);
        ASSERT_EQ(::chmod(base.c_str(), BASE_DIR_MODE), 0);
        const std::string uid = std::to_string(::geteuid());
        const std::string path = (temp_->path() / "tag-set.json").string();
        std::ofstream file(path);
        file << R"({"format": 1, "heartbeat_check_ms": 50, "processes": [)"
             << R"({"name": "tagd", "uid": )" << uid << "}, "
             << R"({"name": "drv-test", "uid": )" << uid
             << R"(, "heartbeat_timeout_ms": 1500}, )"
             << R"({"name": "other", "uid": )" << uid
             << R"(, "heartbeat_timeout_ms": 1500}], "owners": [)"
             << R"({"name": "drv-test", "process": "drv-test", )"
             << R"("change_ring_capacity": 1024}, )"
             << R"({"name": "other", "process": "other", )"
             << R"("change_ring_capacity": 1024}], "queues": [], "tags": [)"
             << R"({"name": "d1.v", "owner": "drv-test", "type": "int64"}, )"
             << R"({"name": "d1.s", "owner": "drv-test", "type": "string"}, )"
             << R"({"name": "o.v", "owner": "other", "type": "int64"}]})"
             << "\n";
        file.close();
        std::optional<RunningTagd> tagd =
            scada::test::tagd::ready_tagd(path, base.string());
        if (!tagd.has_value()) {
            FAIL() << "scada-tagd did not start";
        }
        tagd_.emplace(std::move(*tagd));
        const scada_attach_opts options{.base_dir = base.c_str(),
                                        .retain_dir = nullptr,
                                        .tagd_uid = std::int64_t{::geteuid()}};
        ASSERT_EQ(scada_tag_attach_opts("drv-test", &options, &ctx_), SCADA_OK);
        for (auto [name, out] :
             {std::pair{"d1.v", &number_}, std::pair{"d1.s", &text_},
              std::pair{"o.v", &theirs_}}) {
            ASSERT_EQ(scada_tag_resolve(ctx_, name, out), SCADA_OK) << name;
        }
        scada_tag_info_t info{};
        ASSERT_EQ(scada_tag_info(ctx_, number_, &info), SCADA_OK);
        owner_ = info.owner_index;
        ASSERT_EQ(scada_tag_info(ctx_, theirs_, &info), SCADA_OK);
        other_ = info.owner_index;
    }

    void TearDown() override {
        if (ctx_ != nullptr) {
            scada_tag_detach(ctx_);
        }
        tagd_.reset();
        temp_.reset();
    }

    // The writer of device 0's points: d1.v, a point without a tag, d1.s
    // and a tag of another process, with room for `strings` STRING writes
    // a step.
    TagWriter writer(std::size_t strings = 1) {
        return TagWriter(ctx_,
                         PointHandles{.handles = {number_, SCADA_INVALID_HANDLE,
                                                  text_, theirs_},
                                      .first = {0}},
                         strings);
    }

    [[nodiscard]] std::string text() const {
        std::array<char, SCADA_STRING_MAX> buffer{};
        std::size_t length = 0;
        if (scada_tag_read_string(ctx_, text_, buffer.data(), buffer.size(),
                                  &length, nullptr) != SCADA_OK) {
            return "<unreadable>";
        }
        return {buffer.data(), length};
    }

    [[nodiscard]] scada_value value(std::uint32_t tag) const {
        scada_value out{};
        (void)scada_tag_read(ctx_, tag, &out);  // a failed read gives BAD 0
        return out;
    }

    [[nodiscard]] scada_value number() const { return value(number_); }
    [[nodiscard]] scada_value text_value() const { return value(text_); }
    [[nodiscard]] Status not_initialise_ours() const {
        return write_owner_not_initialised(ctx_, owner_);
    }
    [[nodiscard]] Status not_initialise_theirs() const {
        return write_owner_not_initialised(ctx_, other_);
    }

private:
    std::optional<scada::test::TempDirectory> temp_;
    std::optional<RunningTagd> tagd_;
    scada_ctx* ctx_ = nullptr;
    std::uint32_t number_ = SCADA_INVALID_HANDLE;
    std::uint32_t text_ = SCADA_INVALID_HANDLE;
    std::uint32_t theirs_ = SCADA_INVALID_HANDLE;
    std::uint16_t owner_ = 0;
    std::uint16_t other_ = 0;
};

TEST_F(TagWriterTest, AStringWaitsForTheNotifyAndAPointWithoutATagIsSkipped) {
    TagWriter tags = writer();
    tags.write_point(0, 0, good(scada::driver::int64_value(7)));
    tags.write_point(0, 1, good(scada::driver::int64_value(9)));  // no tag
    tags.write_point(0, 2,
                     good(TagValue{.type = tag::TagType::STRING, .text = "a"}));
    // A second write of the same tag in one step replaces the first.
    tags.write_point(
        0, 2, good(TagValue{.type = tag::TagType::STRING, .text = "abc"}));
    EXPECT_EQ(std::bit_cast<std::int64_t>(number().value), 7);
    EXPECT_NE(text(), "abc");  // not before the notify
    tags.notify();
    EXPECT_EQ(text(), "abc");
    EXPECT_EQ(text_value().quality, SCADA_QUALITY_GOOD);
    EXPECT_EQ(tags.refused(), 0U);
}

TEST_F(TagWriterTest, AWriteTheLibraryRefusesIsCounted) {
    TagWriter tags = writer();
    tags.write_point(0, 3, good(scada::driver::int64_value(5)));  // not ours
    EXPECT_EQ(tags.refused(), 1U);
}

TEST_F(TagWriterTest, AStringPastTheReservedRoomIsRefused) {
    // The room is reserved at construction, so that a write never
    // allocates: a STRING write past it is refused and counted, and the
    // tag keeps what it had.
    TagWriter tags = writer(0);
    tags.write_point(
        0, 2, good(TagValue{.type = tag::TagType::STRING, .text = "lost"}));
    tags.notify();
    EXPECT_EQ(tags.refused(), 1U);
    EXPECT_NE(text(), "lost");
}

TEST_F(TagWriterTest, NineOneByOwnerSlotKeepsEveryValueAsStored) {
    TagWriter tags = writer();
    tags.write_point(0, 0, good(scada::driver::int64_value(7)));
    tags.write_point(
        0, 2, good(TagValue{.type = tag::TagType::STRING, .text = "kept"}));
    tags.notify();
    ASSERT_TRUE(not_initialise_ours().ok());
    const scada_value number = this->number();
    EXPECT_TRUE(number.quality == SCADA_QUALITY_BAD &&
                (number.quality_flags & SCADA_FLAG_NOT_INIT) != 0 &&
                std::bit_cast<std::int64_t>(number.value) == 7);
    EXPECT_EQ(text(), "kept");
    EXPECT_NE(text_value().quality_flags & SCADA_FLAG_NOT_INIT, 0);
    // Another process's owner is refused: the start would fail.
    EXPECT_FALSE(not_initialise_theirs().ok());
}

// The writes of 9.1 keep SUBSTITUTED with a kept substitute value, and
// only with one (D-145; review R1 of PR 157).
TEST_F(TagWriterTest, NineOneKeepsSubstitutedWithTheValue) {
    TagWriter tags = writer();
    ProcessedSample substitute = good(scada::driver::int64_value(4));
    substitute.tag.quality = tag::Quality::UNCERTAIN;
    substitute.tag.flags = tag::to_raw(tag::QualityFlag::SUBSTITUTED);
    tags.write_point(0, 0, substitute);
    tags.write_point(
        0, 2, good(TagValue{.type = tag::TagType::STRING, .text = "read"}));
    tags.notify();
    ASSERT_TRUE(not_initialise_ours().ok());
    const scada_value number = this->number();
    EXPECT_EQ(number.quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(std::bit_cast<std::int64_t>(number.value), 4);
    // LOCAL_TIME is the library's: the write gave no source time.
    EXPECT_EQ(number.quality_flags & ~SCADA_FLAG_LOCAL_TIME,
              SCADA_FLAG_NOT_INIT | SCADA_FLAG_SUBSTITUTED);
    EXPECT_EQ(text_value().quality_flags & SCADA_FLAG_SUBSTITUTED, 0);
}

// What TagWriter::seed hands over, by point of device 0.
class RecordedSeeds final : public PointSeeds {
public:
    struct Seed {
        std::uint32_t point = 0;
        TagValue stored{};
        std::string text;  // `stored.text` is borrowed only for the call
        bool substituted = false;
    };

    void seed_point(std::uint32_t device, std::uint32_t point,
                    const TagValue& stored,
                    bool substituted) noexcept override {
        if (device == 0) {
            seeds.push_back(Seed{.point = point,
                                 .stored = stored,
                                 .text = std::string(stored.text),
                                 .substituted = substituted});
        }
    }

    std::vector<Seed> seeds;
};

// The restart path (review R1 of PR 157): after 9.1, a tag that holds a
// substitute is seeded with SUBSTITUTED, and a tag that holds a value read
// from the device without it.
TEST_F(TagWriterTest, ASeedCarriesSubstitutedFromTheStoredValue) {
    TagWriter tags = writer();
    ProcessedSample substitute = good(scada::driver::int64_value(4));
    substitute.tag.quality = tag::Quality::UNCERTAIN;
    substitute.tag.flags = tag::to_raw(tag::QualityFlag::SUBSTITUTED);
    tags.write_point(0, 0, substitute);
    tags.write_point(
        0, 2, good(TagValue{.type = tag::TagType::STRING, .text = "read"}));
    tags.notify();
    ASSERT_TRUE(not_initialise_ours().ok());
    scada::driver::CompiledChannel channel;
    channel.devices.resize(1);
    channel.devices[0].points.resize(4);
    RecordedSeeds recorded;
    tags.seed(recorded, channel);
    // Point 1 has no tag; point 3's tag is another process's, read all the
    // same.
    ASSERT_EQ(recorded.seeds.size(), 3U);
    EXPECT_EQ(recorded.seeds[0].point, 0U);
    EXPECT_EQ(recorded.seeds[0].stored.i, 4);
    EXPECT_TRUE(recorded.seeds[0].substituted);
    EXPECT_EQ(recorded.seeds[1].point, 2U);
    EXPECT_EQ(recorded.seeds[1].text, "read");
    EXPECT_FALSE(recorded.seeds[1].substituted);
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

}  // namespace
}  // namespace scada::test::driver_it
