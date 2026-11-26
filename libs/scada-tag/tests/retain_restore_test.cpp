// Unit tests for the restore of retained values at attach (contract,
// section 12; decision D-45): a new `tags` region gets the values of its
// owner's RETAIN tags, a reused one keeps its own, a damaged file is
// ignored, and an attach after a generation change waits, with a bound,
// for the FINAL file of the generation the owner left.

#include "attach_fixture.h"
#include "attachment.h"
#include "header_view.h"
#include "layout.h"
#include "retain_file.h"
#include "temp_database.h"

#include "scada/common/clock.h"
#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/tag_set.h"
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
#include <functional>
#include <initializer_list>
#include <ios>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace scada::tag;
using scada::Fd;
using scada::Result;
using Attached = std::unique_ptr<Attachment>;

constexpr std::uint64_t LEVEL_BITS = 0x4045'0000'0000'0000ULL;  // 42.0
constexpr std::int64_t SOURCE_TIME = 1'700'000'000'000'000'000;
constexpr mode_t RETAIN_DIR_MODE = 02750;
constexpr mode_t WRITABLE_BY_OTHERS_MODE = 02752;
constexpr std::uint64_t COUNT_ON_FILE = 9;
// A value no field of a status holds, to see that the call clears it.
constexpr std::uint32_t NOT_CLEARED = 7;
constexpr std::int64_t AN_HOUR = 3'600'000'000'000;
constexpr std::uint8_t OTHER_BOOT_BYTE = 0xEE;

// The retain information of `name` through the attachment.
scada_tag_retain_info_t info_of(Attachment& attachment, std::string_view name) {
    std::uint32_t handle = SCADA_INVALID_HANDLE;
    EXPECT_EQ(attachment.resolve(name, handle), SCADA_OK);
    scada_tag_retain_info_t info{};
    EXPECT_EQ(attachment.retain_info(handle, info), SCADA_OK);
    return info;
}

class Restore : public scada::tag::testing::AttachFixture {
protected:
    void SetUp() override {
        AttachFixture::SetUp();
        retain_ = root() / "retain";
        ASSERT_EQ(
            ::mkdir(retain_.c_str(), scada::tag::testing::PRIVATE_DIR_MODE), 0);
        ASSERT_EQ(::chmod(retain_.c_str(), RETAIN_DIR_MODE), 0);
    }

    // `io` owns plant.level (retained), plant.mode (retained) and
    // plant.count (not retained).
    [[nodiscard]] static TagSet retained_set() {
        TagSet set = tag_set();
        set.tags = {
            {.name = "plant.level",
             .owner = "io",
             .type = TagType::FLOAT64,
             .retain = true},
            {.name = "plant.mode",
             .owner = "io",
             .type = TagType::INT64,
             .retain = true,
             .retain_now = true},
            {.name = "plant.count", .owner = "io", .type = TagType::UINT64}};
        return set;
    }

    [[nodiscard]] Attached attached(std::string_view process) {
        std::string detail;
        Result<Attached> result =
            Attachment::attach(process,
                               {.base_dir = base().string(),
                                .retain_dir = retain_.string(),
                                .tagd_uid = uid()},
                               detail);
        EXPECT_TRUE(result.ok()) << result.status().message() << ": " << detail;
        return result.ok() ? std::move(result).value() : nullptr;
    }

    // As `attached`, with the age bounds `bounds` (section 12, 1.1).
    [[nodiscard]] Attached attached_with_bounds(
        std::string_view process,
        std::map<std::string, std::int64_t, std::less<>> bounds) {
        std::string detail;
        Result<Attached> result =
            Attachment::attach(process,
                               {.base_dir = base().string(),
                                .retain_dir = retain_.string(),
                                .tagd_uid = uid(),
                                .retain_bounds = std::move(bounds)},
                               detail);
        EXPECT_TRUE(result.ok()) << result.status().message() << ": " << detail;
        return result.ok() ? std::move(result).value() : nullptr;
    }

    // `values_file()` written `age` ago in this boot, so that its values
    // have a known age of at least `age`.
    void write_file_of_age(std::int64_t age) const {
        const RetainClockSample now = retain_clock_now();
        RetainFile file = values_file();
        file.written_time_ns = now.realtime_ns - age;
        file.written_boottime_ns = now.boottime_ns - age;
        file.boot_id = now.boot_id;
        write_file("io", file);
    }

    void write_file(std::string_view owner, const RetainFile& file) const {
        const Fd directory = scada::tag::testing::open_directory(retain_);
        std::string detail;
        ASSERT_TRUE(
            write_retain_file(directory.get(), owner, file, detail).ok())
            << detail;
    }

    [[nodiscard]] static RetainFile values_file() {
        RetainFile file;
        file.values = {
            {"plant.count", TagType::UINT64, COUNT_ON_FILE,
             SOURCE_TIME},  // not retained
            {"plant.level", TagType::FLOAT64, LEVEL_BITS, SOURCE_TIME},
            {"plant.mode", TagType::INT64, 3, SOURCE_TIME + 1},
        };
        return file;
    }

    // The value of `name` read through the attachment.
    [[nodiscard]] static scada_value read(Attachment& attachment,
                                          std::string_view name) {
        std::uint32_t handle = SCADA_INVALID_HANDLE;
        EXPECT_EQ(attachment.resolve(name, handle), SCADA_OK);
        scada_value value{};
        EXPECT_EQ(attachment.read(handle, value), SCADA_OK);
        return value;
    }

    [[nodiscard]] const std::filesystem::path& retain() const {
        return retain_;
    }

private:
    std::filesystem::path retain_;
};

TEST_F(Restore, ANewTagsRegionGetsTheRetainedValuesOfItsRetainTags) {
    write_file("io", values_file());
    start_host(retained_set());
    const std::int64_t before = scada::monotonic_ns();
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_EQ(feeder->status().values_restored, 2U);
    EXPECT_EQ(feeder->status().final_waits_expired, 0U);

    const scada_value level = read(*feeder, "plant.level");
    EXPECT_EQ(level.value, LEVEL_BITS);
    EXPECT_EQ(level.quality, SCADA_QUALITY_UNCERTAIN);
    EXPECT_EQ(level.quality_flags, SCADA_FLAG_RESTORED);
    EXPECT_EQ(level.source_time_ns, SOURCE_TIME);
    EXPECT_GE(level.update_mono_ns, before);
    const scada_value mode = read(*feeder, "plant.mode");
    EXPECT_EQ(mode.value, 3U);
    EXPECT_EQ(mode.source_time_ns, SOURCE_TIME + 1);
    // Not RETAIN: the initial state, although the file has a value.
    const scada_value count = read(*feeder, "plant.count");
    EXPECT_EQ(count.quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(count.quality_flags, SCADA_FLAG_NOT_INIT);
    EXPECT_EQ(count.value, 0U);
}

// Contract 1.1, section 2 (#151): the plain owner `io` beside the owner
// `io.scan` of the same process, a tag set that 1.0 refused. Each owner has
// its own region and retain file, and the plain owner keeps the file it had
// when its process had no other owner.
TEST_F(Restore, APlainOwnerBesideAWriterOwnerRestoresFromItsOwnFile) {
    constexpr std::uint64_t FLOW_ON_FILE = 17;
    constexpr std::uint64_t FLOW_WRITTEN = 18;
    TagSet set = retained_set();
    set.owners.push_back({.name = "io.scan",
                          .process = "io",
                          .change_ring_capacity = MIN_CHANGE_RING_CAPACITY});
    set.tags.push_back({.name = "plant.flow",
                        .owner = "io.scan",
                        .type = TagType::UINT64,
                        .retain = true});
    write_file("io", values_file());
    RetainFile scan_file;
    scan_file.values = {
        {"plant.flow", TagType::UINT64, FLOW_ON_FILE, SOURCE_TIME}};
    write_file("io.scan", scan_file);
    start_host(set);

    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_EQ(feeder->status().values_restored, 3U);
    EXPECT_TRUE(std::filesystem::exists(generation_dir() / "tags.io"));
    EXPECT_TRUE(std::filesystem::exists(generation_dir() / "tags.io.scan"));
    const scada_value level = read(*feeder, "plant.level");
    EXPECT_EQ(level.value, LEVEL_BITS);
    EXPECT_EQ(level.quality_flags, SCADA_FLAG_RESTORED);
    EXPECT_EQ(info_of(*feeder, "plant.level").origin,
              SCADA_RETAIN_OWN_PERIODIC);
    const scada_value flow = read(*feeder, "plant.flow");
    EXPECT_EQ(flow.value, FLOW_ON_FILE);
    EXPECT_EQ(flow.quality_flags, SCADA_FLAG_RESTORED);
    EXPECT_EQ(info_of(*feeder, "plant.flow").origin, SCADA_RETAIN_OWN_PERIODIC);

    // Each owner writes its own tag (one thread per owner, contract 13.2;
    // here both are the test's thread).
    std::uint32_t handle = SCADA_INVALID_HANDLE;
    ASSERT_EQ(feeder->resolve("plant.flow", handle), SCADA_OK);
    scada_value value{};
    value.type = SCADA_TYPE_UINT64;
    value.quality = SCADA_QUALITY_GOOD;
    value.value = FLOW_WRITTEN;
    EXPECT_EQ(feeder->write(handle, value), SCADA_OK);
    EXPECT_EQ(read(*feeder, "plant.flow").value, FLOW_WRITTEN);
    ASSERT_EQ(feeder->resolve("plant.mode", handle), SCADA_OK);
    value.type = SCADA_TYPE_INT64;
    value.value = 4;
    EXPECT_EQ(feeder->write(handle, value), SCADA_OK);
    EXPECT_EQ(read(*feeder, "plant.mode").quality, SCADA_QUALITY_GOOD);
}

TEST_F(Restore, AWriteOfTheOwnerClearsRestored) {
    write_file("io", values_file());
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    std::uint32_t handle = SCADA_INVALID_HANDLE;
    ASSERT_EQ(feeder->resolve("plant.mode", handle), SCADA_OK);
    scada_value value{};
    value.type = SCADA_TYPE_INT64;
    value.quality = SCADA_QUALITY_GOOD;
    value.value = 4;
    ASSERT_EQ(feeder->write(handle, value), SCADA_OK);
    const scada_value mode = read(*feeder, "plant.mode");
    EXPECT_EQ(mode.quality, SCADA_QUALITY_GOOD);
    EXPECT_EQ(mode.quality_flags & SCADA_FLAG_RESTORED, 0);
}

TEST_F(Restore, AReusedRegionKeepsItsValues) {
    start_host(retained_set());
    {
        const Attached feeder = attached("io");
        ASSERT_NE(feeder, nullptr);
        EXPECT_EQ(feeder->status().values_restored, 0U);
    }
    write_file("io", values_file());
    const Attached again = attached("io");
    ASSERT_NE(again, nullptr);
    EXPECT_GT(again->status().regions_reused, 0U);
    EXPECT_EQ(again->status().values_restored, 0U);
    EXPECT_EQ(read(*again, "plant.level").quality_flags, SCADA_FLAG_NOT_INIT);
}

TEST_F(Restore,
       Conformance25ACorruptOrTruncatedFileIsIgnoredAndAttachSucceeds) {
    const Fd directory = scada::tag::testing::open_directory(retain());
    std::string detail;
    Result<std::vector<std::byte>> bytes =
        encode_retain_file(values_file(), detail);
    ASSERT_TRUE(bytes.ok());
    for (const std::size_t cut :
         {std::size_t{0}, std::size_t{10}, bytes.value().size() - 1}) {
        {
            std::ofstream out(retain() / "io.bin",
                              std::ios::binary | std::ios::trunc);
            out.write(std::bit_cast<const char*>(bytes.value().data()),
                      static_cast<std::streamsize>(cut));
        }
        start_host(retained_set());
        const Attached feeder = attached("io");
        ASSERT_NE(feeder, nullptr) << cut;
        EXPECT_EQ(feeder->status().values_restored, 0U) << cut;
        EXPECT_EQ(read(*feeder, "plant.level").quality_flags,
                  SCADA_FLAG_NOT_INIT);
        drop_host();
        std::filesystem::remove_all(generation_dir());
        std::filesystem::remove(base() / "current");
    }
}

TEST_F(Restore, AFileNotOwnedByTagdIsNotTrusted) {
    write_file("io", values_file());
    // The file belongs to the user of the test; read as if scada-tagd were
    // another user, it is refused (section 12).
    const Fd directory = scada::tag::testing::open_directory(retain());
    const std::vector<WantedValue> wanted = {{"plant.level", TagType::FLOAT64}};
    const LoadedValues loaded =
        load_retained(directory.get(), other_uid(), "io", wanted);
    EXPECT_FALSE(loaded.values[0].has_value());
    EXPECT_EQ(loaded.rejected.size(), 1U);
}

TEST_F(Restore, AMovedTagIsFoundInTheFileOfItsFormerOwner) {
    RetainFile former;
    former.values = {
        {"plant.level", TagType::FLOAT64, LEVEL_BITS, SOURCE_TIME}};
    write_file("io.old", former);
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_EQ(feeder->status().values_restored, 1U);
    EXPECT_EQ(read(*feeder, "plant.level").value, LEVEL_BITS);
}

// The generation change of section 11.3: generation 1 is RETIRING and lists
// `io`; the attach to generation 2 waits for the FINAL file of generation 1.
class RestoreAfterChange : public Restore {
protected:
    void SetUp() override {
        Restore::SetUp();
        start_host(retained_set());
        drop_host();
        TagSet changed = retained_set();
        changed.tags.push_back(
            {.name = "plant.extra", .owner = "io", .type = TagType::BOOL});
        start_host(changed);
        ASSERT_EQ(host().generation(), 2U);
        const Fd directory =
            scada::tag::testing::open_directory(generation_dir(1));
        std::string detail;
        Result<HeaderView> old =
            HeaderView::open(directory.get(), 1, uid(), detail);
        ASSERT_TRUE(old.ok()) << detail;
        ASSERT_EQ(old.value().state(), GenerationState::RETIRING);
        old_ = {1, old.value().contents().created_time_ns};
    }

    [[nodiscard]] RetainFile final_file() const {
        RetainFile file = values_file();
        file.final = true;
        file.source = old_generation();
        return file;
    }

    [[nodiscard]] const GenerationIdentity& old_generation() const {
        return old_;
    }

private:
    GenerationIdentity old_;
};

TEST_F(RestoreAfterChange, AFinalFileOfTheRetiringGenerationIsTakenAtOnce) {
    write_file("io", final_file());
    const std::int64_t start = scada::monotonic_ns();
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_LT(scada::monotonic_ns() - start, RETAIN_FINAL_WAIT_NS / 2);
    EXPECT_EQ(feeder->status().final_waits_expired, 0U);
    EXPECT_EQ(feeder->status().values_restored, 2U);
}

TEST_F(RestoreAfterChange, AttachWaitsForTheFinalFileAndTakesIt) {
    RetainFile periodic = values_file();
    periodic.values.at(2).value = 1;  // plant.mode before the last change
    write_file("io", periodic);
    // scada-tagd writes the FINAL file while the attach waits: once the new
    // `tags` region exists under its temporary name, which is while its
    // records are initialised and the retained values loaded.
    const std::jthread writer([this] {
        const std::int64_t deadline =
            scada::monotonic_ns() + RETAIN_FINAL_WAIT_NS;
        bool seen = false;
        while (!seen && scada::monotonic_ns() < deadline) {
            for (const std::string& name :
                 scada::tag::testing::names_in(generation_dir(2))) {
                seen = seen || name.starts_with(".tmp.tags.io.");
            }
            std::this_thread::yield();
        }
        write_file("io", final_file());
    });
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_EQ(feeder->status().final_waits_expired, 0U);
    EXPECT_EQ(read(*feeder, "plant.mode").value, 3U);
}

TEST_F(RestoreAfterChange, TheWaitIsBoundedAndTakesTheFileAsItIs) {
    RetainFile stale = final_file();
    stale.source.created_time_ns += 1;  // FINAL of another generation 1
    stale.values.at(2).value = 1;
    write_file("io", stale);
    const std::int64_t start = scada::monotonic_ns();
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    const std::int64_t waited = scada::monotonic_ns() - start;
    EXPECT_GE(waited, RETAIN_FINAL_WAIT_NS);
    EXPECT_LT(waited, RETAIN_FINAL_WAIT_NS + 2'000'000'000);
    EXPECT_EQ(feeder->status().final_waits_expired, 1U);
    EXPECT_EQ(read(*feeder, "plant.mode").value, 1U);
}

// Review of PR #69, point 8: one deadline for the whole attach, however
// many former owners it waits for.
TEST_F(Restore, TheFinalWaitForTwoFormerOwnersEndsAtOneDeadline) {
    TagSet split = retained_set();
    // plant.mode moves from the owner `web` to `io`.
    split.owners.push_back({.name = "web",
                            .process = "web",
                            .change_ring_capacity = MIN_CHANGE_RING_CAPACITY});
    split.tags.at(1).owner = "web";
    start_host(split);
    drop_host();
    start_host(retained_set());
    ASSERT_EQ(host().generation(), 2U);
    RetainFile stale = values_file();
    stale.final = true;
    stale.source = {1, 1};  // FINAL of another generation 1: never the one
    write_file("io", stale);
    write_file("web", stale);
    const std::int64_t start = scada::monotonic_ns();
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    const std::int64_t waited = scada::monotonic_ns() - start;
    EXPECT_GE(waited, RETAIN_FINAL_WAIT_NS);
    EXPECT_LT(waited, RETAIN_FINAL_WAIT_NS + (RETAIN_FINAL_WAIT_NS / 2));
    EXPECT_EQ(feeder->status().final_waits_expired, 1U);
}

// Review of PR #69, point 7: a retain directory that cannot be used is
// counted, and attach still succeeds, without retained values.
TEST_F(Restore, ARetainDirectoryWithTheWrongModeIsCountedAndAttachSucceeds) {
    write_file("io", values_file());
    ASSERT_EQ(::chmod(retain().c_str(), WRITABLE_BY_OTHERS_MODE), 0);
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_EQ(feeder->status().directory_unusable, 1U);
    EXPECT_EQ(feeder->status().values_restored, 0U);
}

TEST_F(Restore, ARetainDirectoryThatIsASymbolicLinkIsCounted) {
    write_file("io", values_file());
    const std::filesystem::path link = root() / "retain-link";
    std::filesystem::create_directory_symlink(retain(), link);
    start_host(retained_set());
    std::string detail;
    Result<Attached> result = Attachment::attach("io",
                                                 {.base_dir = base().string(),
                                                  .retain_dir = link.string(),
                                                  .tagd_uid = uid()},
                                                 detail);
    ASSERT_TRUE(result.ok()) << detail;
    EXPECT_EQ(result.value()->status().directory_unusable, 1U);
    EXPECT_EQ(result.value()->status().values_restored, 0U);
}

TEST_F(RestoreAfterChange, AnExpiredWaitIsToldForEachTagThatNeededTheFile) {
    RetainFile stale = final_file();
    stale.source.created_time_ns += 1;  // FINAL of another generation 1
    write_file("io", stale);
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    ASSERT_EQ(feeder->status().final_waits_expired, 1U);
    const scada_tag_retain_info_t level = info_of(*feeder, "plant.level");
    EXPECT_EQ(level.final_wait_expired, 1);
    EXPECT_EQ(level.origin, SCADA_RETAIN_OWN_FINAL);
    const scada_tag_retain_info_t mode = info_of(*feeder, "plant.mode");
    EXPECT_EQ(mode.final_wait_expired, 1);
    EXPECT_EQ(mode.origin, SCADA_RETAIN_OWN_FINAL);
    // plant.extra is no RETAIN tag: nothing waited for it.
    const scada_tag_retain_info_t extra = info_of(*feeder, "plant.extra");
    EXPECT_EQ(extra.final_wait_expired, 0);
    EXPECT_EQ(extra.origin, SCADA_RETAIN_NONE);
}

TEST_F(RestoreAfterChange, AFinalFileThatCameInTimeIsNoExpiredWait) {
    write_file("io", final_file());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    const scada_tag_retain_info_t info = info_of(*feeder, "plant.mode");
    EXPECT_EQ(info.origin, SCADA_RETAIN_OWN_FINAL);
    EXPECT_EQ(info.final_wait_expired, 0);
}

TEST_F(RestoreAfterChange, AnOwnerTheRetiringGenerationDoesNotListDoesNotWait) {
    std::filesystem::remove_all(generation_dir(1));
    write_file("io", values_file());
    const std::int64_t start = scada::monotonic_ns();
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_LT(scada::monotonic_ns() - start, RETAIN_FINAL_WAIT_NS / 2);
    EXPECT_EQ(feeder->status().values_restored, 2U);
}

}  // namespace

namespace {

TEST_F(Restore, TheCInterfaceReportsWhatAttachDidWithRetainedValues) {
    write_file("io", values_file());
    scada::tag::testing::touch(retain() / "broken.bin");
    start_host(retained_set());
    const std::string base_dir = base().string();
    const std::string retain_dir = retain().string();
    const scada_attach_opts opts{.base_dir = base_dir.c_str(),
                                 .retain_dir = retain_dir.c_str(),
                                 .tagd_uid = uid()};
    scada_ctx* ctx = nullptr;
    ASSERT_EQ(scada_tag_attach_opts("io", &opts, &ctx), SCADA_OK);
    scada_retain_status_t status{};
    status.directory_unusable = NOT_CLEARED;
    ASSERT_EQ(scada_tag_retain_status(ctx, &status), SCADA_OK);
    EXPECT_EQ(status.values_restored, 2U);
    // The own file serves both tags: the broken one is not looked at.
    EXPECT_EQ(status.files_rejected, 0U);
    EXPECT_EQ(status.final_waits_expired, 0U);
    EXPECT_EQ(status.directory_unusable, 0U);
    EXPECT_EQ(scada_tag_retain_status(ctx, nullptr), SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_retain_status(nullptr, &status), SCADA_E_INVALID);
    scada_tag_detach(ctx);

    // A tag the own file lacks makes attach read the other files.
    std::filesystem::remove(retain() / "io.bin");
    drop_host();
    std::filesystem::remove_all(generation_dir());
    std::filesystem::remove(base() / "current");
    start_host(retained_set());
    ASSERT_EQ(scada_tag_attach_opts("io", &opts, &ctx), SCADA_OK);
    ASSERT_EQ(scada_tag_retain_status(ctx, &status), SCADA_OK);
    EXPECT_EQ(status.values_restored, 0U);
    EXPECT_EQ(status.files_rejected, 1U);
    scada_tag_detach(ctx);
}

}  // namespace

// Decision D-59: format version 2, the flags a restored value keeps, and
// what the restore of each tag reports.
namespace {

constexpr std::uint16_t flags_of(std::initializer_list<QualityFlag> flags) {
    std::uint16_t all = 0;
    for (const QualityFlag flag : flags) {
        all = static_cast<std::uint16_t>(all | to_raw(flag));
    }
    return all;
}

TEST_F(Restore, ARestoredValueKeepsTheFlagsThatDescribeItAndIsNeverGood) {
    RetainFile file = values_file();
    file.values.at(1).quality = Quality::GOOD;  // plant.level
    file.values.at(1).quality_flags = flags_of(
        {QualityFlag::SUBSTITUTED, QualityFlag::STALE, QualityFlag::COMM_FAIL,
         QualityFlag::LOCAL_TIME, QualityFlag::TEST});
    file.values.at(2).quality = Quality::GOOD;  // plant.mode
    write_file("io", file);
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    const scada_value level = read(*feeder, "plant.level");
    EXPECT_EQ(level.quality, SCADA_QUALITY_UNCERTAIN);
    // STALE and COMM_FAIL describe the live link and are not carried.
    EXPECT_EQ(level.quality_flags,
              flags_of({QualityFlag::RESTORED, QualityFlag::SUBSTITUTED,
                        QualityFlag::LOCAL_TIME, QualityFlag::TEST}));
    const scada_value mode = read(*feeder, "plant.mode");
    EXPECT_EQ(mode.quality, SCADA_QUALITY_UNCERTAIN);
    EXPECT_EQ(mode.quality_flags, SCADA_FLAG_RESTORED);
    // The stored quality is there for diagnosis.
    EXPECT_EQ(info_of(*feeder, "plant.level").stored_quality,
              SCADA_QUALITY_GOOD);
}

TEST_F(Restore, EveryFlagThatDescribesTheValueIsCarried) {
    RetainFile file = values_file();
    file.values.at(1).quality_flags = retainable_flags(QUALITY_FLAGS_MASK);
    write_file("io", file);
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_EQ(read(*feeder, "plant.level").quality_flags,
              to_raw(QualityFlag::RESTORED) | RETAIN_CARRIED_FLAGS);
}

TEST_F(Restore, AFileOfVersion1IsRestoredAsBeforeWithAnUnknownAge) {
    const Fd directory = scada::tag::testing::open_directory(retain());
    std::string detail;
    const Result<std::vector<std::byte>> bytes =
        encode_retain_file_v1(values_file(), detail);
    ASSERT_TRUE(bytes.ok()) << detail;
    {
        std::ofstream out(retain() / "io.bin",
                          std::ios::binary | std::ios::trunc);
        out.write(std::bit_cast<const char*>(bytes.value().data()),
                  static_cast<std::streamsize>(bytes.value().size()));
    }
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_EQ(feeder->status().values_restored, 2U);
    EXPECT_EQ(feeder->status().ages_unknown, 2U);
    const scada_value level = read(*feeder, "plant.level");
    EXPECT_EQ(level.value, LEVEL_BITS);
    EXPECT_EQ(level.quality, SCADA_QUALITY_UNCERTAIN);
    EXPECT_EQ(level.quality_flags, SCADA_FLAG_RESTORED);
    const scada_tag_retain_info_t info = info_of(*feeder, "plant.level");
    EXPECT_EQ(info.age_known, 0);
    EXPECT_EQ(info.age_ns, 0);
    EXPECT_EQ(info.stored_quality, SCADA_QUALITY_UNCERTAIN);
}

TEST_F(Restore, TheInformationTellsOriginWriteTimeAndAgeInTheSameBoot) {
    const RetainClockSample now = retain_clock_now();
    // A write half the time since boot ago: a boot time the clock had.
    const std::int64_t age = now.boottime_ns / 2;
    ASSERT_GT(age, 0);
    RetainFile file = values_file();
    file.written_time_ns = now.realtime_ns - age;
    file.written_boottime_ns = now.boottime_ns - age;
    file.boot_id = now.boot_id;
    write_file("io", file);
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_EQ(feeder->status().ages_unknown, 0U);
    const scada_tag_retain_info_t level = info_of(*feeder, "plant.level");
    EXPECT_EQ(level.origin, SCADA_RETAIN_OWN_PERIODIC);
    EXPECT_EQ(level.written_time_ns, file.written_time_ns);
    EXPECT_EQ(level.age_known, 1);
    EXPECT_GE(level.age_ns, age);
    EXPECT_LT(level.age_ns, age + AN_HOUR);
    EXPECT_EQ(level.stored_quality, SCADA_QUALITY_UNCERTAIN);
    EXPECT_EQ(level.final_wait_expired, 0);
    // Not RETAIN: not restored.
    const scada_tag_retain_info_t count = info_of(*feeder, "plant.count");
    EXPECT_EQ(count.origin, SCADA_RETAIN_NONE);
    EXPECT_EQ(count.written_time_ns, 0);
    EXPECT_EQ(count.age_known, 0);
    EXPECT_EQ(count.stored_quality, SCADA_QUALITY_BAD);
}

TEST_F(Restore, AnotherBootWithoutASynchronisedClockGivesAnUnknownAge) {
    RetainFile file = values_file();
    file.written_time_ns = scada::realtime_ns() - AN_HOUR;
    file.written_boottime_ns = 1;
    file.boot_id.fill(OTHER_BOOT_BYTE);  // another boot
    file.clock_synced = false;
    write_file("io", file);
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_EQ(feeder->status().values_restored, 2U);
    EXPECT_EQ(feeder->status().ages_unknown, 2U);
    EXPECT_EQ(info_of(*feeder, "plant.mode").age_known, 0);
    // The value is restored all the same (decision D-59: attach never
    // refuses a value on its age).
    EXPECT_EQ(read(*feeder, "plant.mode").value, 3U);
}

TEST_F(Restore, AMovedTagHasTheOriginOfAnotherOwner) {
    RetainFile former;
    former.values = {
        {"plant.level", TagType::FLOAT64, LEVEL_BITS, SOURCE_TIME}};
    former.final = true;
    write_file("io.old", former);
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_EQ(info_of(*feeder, "plant.level").origin, SCADA_RETAIN_OTHER_OWNER);
    // From a FINAL file: no FINAL wait is missing.
    EXPECT_EQ(info_of(*feeder, "plant.level").final_wait_expired, 0);
    // Nothing on file for plant.mode.
    EXPECT_EQ(info_of(*feeder, "plant.mode").origin, SCADA_RETAIN_NONE);
}

// Review D3 of #93: a value taken from another owner's file that is not
// FINAL reports an expired FINAL wait, although no wait happened (#90).
TEST_F(Restore, AMovedTagFromAFileThatIsNotFinalReportsAnExpiredWait) {
    RetainFile former;
    former.values = {
        {"plant.level", TagType::FLOAT64, LEVEL_BITS, SOURCE_TIME}};
    write_file("io.old", former);
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    EXPECT_EQ(feeder->status().final_waits_expired, 0U);  // none ran out
    const scada_tag_retain_info_t info = info_of(*feeder, "plant.level");
    EXPECT_EQ(info.origin, SCADA_RETAIN_OTHER_OWNER);
    EXPECT_EQ(info.final_wait_expired, 1);
}

TEST_F(Restore, AReusedRegionReportsNoRestore) {
    write_file("io", values_file());
    start_host(retained_set());
    { const Attached first = attached("io"); }
    const Attached again = attached("io");
    ASSERT_NE(again, nullptr);
    EXPECT_GT(again->status().regions_reused, 0U);
    EXPECT_EQ(info_of(*again, "plant.level").origin, SCADA_RETAIN_NONE);
}

TEST_F(Restore, TheCInterfaceGivesTheInformationOfOwnTagsOnly) {
    write_file("io", values_file());
    start_host(retained_set());
    const std::string base_dir = base().string();
    const std::string retain_dir = retain().string();
    const scada_attach_opts opts{.base_dir = base_dir.c_str(),
                                 .retain_dir = retain_dir.c_str(),
                                 .tagd_uid = uid()};
    scada_ctx* ctx = nullptr;
    ASSERT_EQ(scada_tag_attach_opts("io", &opts, &ctx), SCADA_OK);
    std::uint32_t handle = SCADA_INVALID_HANDLE;
    ASSERT_EQ(scada_tag_resolve(ctx, "plant.mode", &handle), SCADA_OK);
    scada_tag_retain_info_t info{};
    ASSERT_EQ(scada_tag_retain_info(ctx, handle, &info), SCADA_OK);
    EXPECT_EQ(info.origin, SCADA_RETAIN_OWN_PERIODIC);
    scada_retain_status_t status{};
    status.bounds_unused = NOT_CLEARED;
    ASSERT_EQ(scada_tag_retain_status(ctx, &status), SCADA_OK);
    EXPECT_EQ(status.bounds_unused, 0U);  // no bounds passed

    // Errors zero the output.
    info.origin = SCADA_RETAIN_OTHER_OWNER;
    EXPECT_EQ(scada_tag_retain_info(ctx, SCADA_INVALID_HANDLE, &info),
              SCADA_E_INVALID);
    EXPECT_EQ(info.origin, SCADA_RETAIN_NONE);
    EXPECT_EQ(scada_tag_retain_info(ctx, scada_tag_handle(0, 1000), &info),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_retain_info(ctx, handle, nullptr), SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_retain_info(nullptr, handle, &info), SCADA_E_INVALID);

    // An observer owns nothing.
    scada_ctx* observer = nullptr;
    ASSERT_EQ(scada_tag_observe_opts(&opts, &observer), SCADA_OK);
    std::uint32_t observed = SCADA_INVALID_HANDLE;
    ASSERT_EQ(scada_tag_resolve(observer, "plant.mode", &observed), SCADA_OK);
    EXPECT_EQ(scada_tag_retain_info(observer, observed, &info),
              SCADA_E_NOT_OWNER);
    scada_tag_detach(observer);
    scada_tag_detach(ctx);
}

// The age bound of section 12 (contract 1.1, #89). The file is written a
// known time ago in this boot: half the time since boot, at least 2 ns.
class RestoreWithBound : public Restore {
protected:
    void SetUp() override {
        Restore::SetUp();
        age_ = retain_clock_now().boottime_ns / 2;
        ASSERT_GE(age_, 2);
    }

    [[nodiscard]] std::int64_t age() const { return age_; }

private:
    std::int64_t age_ = 0;
};

TEST_F(RestoreWithBound, AValueOlderThanTheBoundIsRestoredBadWithItsValue) {
    write_file_of_age(age());
    start_host(retained_set());
    const Attached feeder =
        attached_with_bounds("io", {{"plant.level", age() / 2}});
    ASSERT_NE(feeder, nullptr);
    EXPECT_EQ(feeder->status().values_restored, 2U);
    const scada_value level = read(*feeder, "plant.level");
    EXPECT_EQ(level.quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(level.quality_flags, SCADA_FLAG_RESTORED);
    EXPECT_EQ(level.value, LEVEL_BITS);
    EXPECT_EQ(level.source_time_ns, SOURCE_TIME);
    const scada_tag_retain_info_t info = info_of(*feeder, "plant.level");
    EXPECT_EQ(info.age_known, 1);
    EXPECT_GE(info.age_ns, age());
    EXPECT_EQ(info.stored_quality, SCADA_QUALITY_UNCERTAIN);
    EXPECT_EQ(info.origin, SCADA_RETAIN_OWN_PERIODIC);
    // A tag without a bound is restored as before.
    const scada_value mode = read(*feeder, "plant.mode");
    EXPECT_EQ(mode.quality, SCADA_QUALITY_UNCERTAIN);
    EXPECT_EQ(mode.quality_flags, SCADA_FLAG_RESTORED);
}

TEST_F(RestoreWithBound, AValueWithinTheBoundIsRestoredUncertain) {
    write_file_of_age(age());
    start_host(retained_set());
    const Attached feeder = attached_with_bounds(
        "io", {{"plant.level", age() + AN_HOUR}, {"plant.mode", AN_HOUR}});
    ASSERT_NE(feeder, nullptr);
    for (const std::string_view name : {"plant.level", "plant.mode"}) {
        const scada_value value = read(*feeder, name);
        EXPECT_EQ(value.quality, SCADA_QUALITY_UNCERTAIN) << name;
        EXPECT_EQ(value.quality_flags, SCADA_FLAG_RESTORED) << name;
    }
    EXPECT_EQ(read(*feeder, "plant.level").value, LEVEL_BITS);
}

TEST_F(Restore, AValueOfUnknownAgeIsRestoredBadUnderABound) {
    RetainFile file = values_file();
    file.written_time_ns = scada::realtime_ns() - AN_HOUR;
    file.written_boottime_ns = 1;
    file.boot_id.fill(OTHER_BOOT_BYTE);  // another boot
    file.clock_synced = false;
    write_file("io", file);
    start_host(retained_set());
    // A bound far longer than any age: an unknown age still exceeds it.
    constexpr std::int64_t TEN_YEARS = AN_HOUR * 24 * 3650;
    const Attached feeder =
        attached_with_bounds("io", {{"plant.mode", TEN_YEARS}});
    ASSERT_NE(feeder, nullptr);
    EXPECT_EQ(feeder->status().ages_unknown, 2U);
    const scada_value mode = read(*feeder, "plant.mode");
    EXPECT_EQ(mode.quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(mode.quality_flags, SCADA_FLAG_RESTORED);
    EXPECT_EQ(mode.value, 3U);
    EXPECT_EQ(mode.source_time_ns, SOURCE_TIME + 1);
    EXPECT_EQ(info_of(*feeder, "plant.mode").age_known, 0);
    // Without a bound, an unknown age is restored UNCERTAIN (D-59).
    EXPECT_EQ(read(*feeder, "plant.level").quality, SCADA_QUALITY_UNCERTAIN);
}

TEST_F(RestoreWithBound, ARestoreBeyondTheBoundKeepsTheCarriedFlags) {
    const RetainClockSample now = retain_clock_now();
    RetainFile file = values_file();
    file.written_time_ns = now.realtime_ns - age();
    file.written_boottime_ns = now.boottime_ns - age();
    file.boot_id = now.boot_id;
    for (RetainedValue& value : file.values) {
        value.quality_flags = static_cast<std::uint16_t>(
            SCADA_FLAG_SUBSTITUTED | SCADA_FLAG_STALE);
    }
    write_file("io", file);
    start_host(retained_set());
    const Attached feeder = attached_with_bounds("io", {{"plant.level", 1}});
    ASSERT_NE(feeder, nullptr);
    const scada_value level = read(*feeder, "plant.level");
    EXPECT_EQ(level.quality, SCADA_QUALITY_BAD);
    // SUBSTITUTED describes the value and is carried; STALE is not.
    EXPECT_EQ(level.quality_flags,
              SCADA_FLAG_RESTORED | SCADA_FLAG_SUBSTITUTED);
}

TEST_F(RestoreWithBound, ABoundOfATagThatIsNotRetainedOrUnknownIsNotUsed) {
    write_file_of_age(age());
    start_host(retained_set());
    const Attached feeder =
        attached_with_bounds("io", {{"plant.count", 1}, {"no.such.tag", 1}});
    ASSERT_NE(feeder, nullptr);
    // Both are counted: an unused bound is not silent (section 13).
    EXPECT_EQ(feeder->status().bounds_unused, 2U);
    EXPECT_EQ(read(*feeder, "plant.level").quality, SCADA_QUALITY_UNCERTAIN);
    const scada_value count = read(*feeder, "plant.count");
    EXPECT_EQ(count.quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(count.quality_flags, SCADA_FLAG_NOT_INIT);
}

TEST_F(RestoreWithBound, AReusedRegionRestoresNothingSoTheBoundWaits) {
    start_host(retained_set());
    {
        const Attached feeder = attached("io");
        ASSERT_NE(feeder, nullptr);
    }
    write_file_of_age(age());
    const Attached again = attached_with_bounds("io", {{"plant.level", 1}});
    ASSERT_NE(again, nullptr);
    EXPECT_GT(again->status().regions_reused, 0U);
    EXPECT_EQ(again->status().values_restored, 0U);
    EXPECT_EQ(read(*again, "plant.level").quality_flags, SCADA_FLAG_NOT_INIT);
}

// The C interface of the bound (section 13), with the options of the test.
class RestoreWithBoundThroughC : public RestoreWithBound {
protected:
    void SetUp() override {
        RestoreWithBound::SetUp();
        write_file_of_age(age());
        start_host(retained_set());
        base_dir_ = base().string();
        retain_dir_ = retain().string();
    }

    [[nodiscard]] scada_attach_opts opts() const {
        return {.base_dir = base_dir_.c_str(),
                .retain_dir = retain_dir_.c_str(),
                .tagd_uid = uid()};
    }

    // Expects `bounds` refused with SCADA_E_INVALID and `*out` set to NULL.
    void expect_bounds_refused(const scada_retain_bound* bounds,
                               std::size_t n) const {
        auto* ctx = std::bit_cast<scada_ctx*>(std::uintptr_t{1});
        const scada_attach_opts options = opts();
        EXPECT_EQ(scada_tag_attach_retain("io", &options, bounds, n, &ctx),
                  SCADA_E_INVALID);
        EXPECT_EQ(ctx, nullptr);
    }

    // Expects the one bound `name`, `max_age_ns` refused.
    void expect_bound_refused(const char* name, std::int64_t max_age_ns) const {
        const std::array<scada_retain_bound, 1> bound{
            {{.tag_name = name, .max_age_ns = max_age_ns}}};
        expect_bounds_refused(bound.data(), 1);
    }

    // Reads plant.level through `ctx`.
    [[nodiscard]] static scada_value level_of(scada_ctx* ctx) {
        std::uint32_t handle = SCADA_INVALID_HANDLE;
        EXPECT_EQ(scada_tag_resolve(ctx, "plant.level", &handle), SCADA_OK);
        scada_value level{};
        EXPECT_EQ(scada_tag_read(ctx, handle, &level), SCADA_OK);
        return level;
    }

    // `bounds_unused` of `ctx`.
    [[nodiscard]] static std::uint32_t unused_of(scada_ctx* ctx) {
        scada_retain_status_t status{};
        EXPECT_EQ(scada_tag_retain_status(ctx, &status), SCADA_OK);
        return status.bounds_unused;
    }

private:
    std::string base_dir_;
    std::string retain_dir_;
};

TEST_F(RestoreWithBoundThroughC, BoundsThatAreNotValidAreRefused) {
    expect_bounds_refused(nullptr, 1);
    const std::array<scada_retain_bound, 1> one{
        {{.tag_name = "plant.level", .max_age_ns = 1}}};
    expect_bounds_refused(one.data(), MAX_TAGS + 1);
    expect_bound_refused(nullptr, 1);
    expect_bound_refused("plant.level", 0);
    expect_bound_refused("plant.level", -1);
    // Names that are no tag names: upper case, empty, a space, and 128 bytes
    // or more (only 128 bytes are read, so the NUL after them is not seen).
    const std::string too_long(MAX_TAG_NAME_LENGTH + 1, 'a');
    const std::string longer(MAX_TAG_NAME_LENGTH + 50, 'a');
    for (const char* name :
         {"Plant.Level", "", "plant level", too_long.c_str(), longer.c_str()}) {
        expect_bound_refused(name, 1);
    }
    const std::array<scada_retain_bound, 2> twice{
        {{.tag_name = "plant.level", .max_age_ns = 1},
         {.tag_name = "plant.level", .max_age_ns = 2}}};
    expect_bounds_refused(twice.data(), 2);
    const scada_attach_opts options = opts();
    EXPECT_EQ(scada_tag_attach_retain("io", &options, one.data(), 1, nullptr),
              SCADA_E_INVALID);
    // Nothing was attached: no region of io exists.
    EXPECT_FALSE(std::filesystem::exists(generation_dir() / "tags.io"));
}

TEST_F(RestoreWithBoundThroughC, BoundsAreAppliedCountedAndNotKept) {
    // A name of 127 bytes is a tag name: valid, and unused here.
    const std::string longest(MAX_TAG_NAME_LENGTH, 'a');
    const std::array<scada_retain_bound, 2> used_and_unused{
        {{.tag_name = "plant.level", .max_age_ns = 1},
         {.tag_name = longest.c_str(), .max_age_ns = 1}}};
    const scada_attach_opts options = opts();
    scada_ctx* ctx = nullptr;
    ASSERT_EQ(scada_tag_attach_retain("io", &options, used_and_unused.data(), 2,
                                      &ctx),
              SCADA_OK);
    EXPECT_EQ(unused_of(ctx), 1U);
    const scada_value restored = level_of(ctx);
    EXPECT_EQ(restored.quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(restored.quality_flags, SCADA_FLAG_RESTORED);
    EXPECT_EQ(restored.value, LEVEL_BITS);
    scada_tag_detach(ctx);

    // No bounds: as scada_tag_attach_opts. The region is reused, so the
    // value restored BAD by the attach before stays as it is (section 13).
    ASSERT_EQ(scada_tag_attach_retain("io", &options, nullptr, 0, &ctx),
              SCADA_OK);
    const scada_value kept = level_of(ctx);
    EXPECT_EQ(kept.quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(kept.quality_flags, SCADA_FLAG_RESTORED);
    EXPECT_EQ(unused_of(ctx), 0U);
    scada_tag_detach(ctx);
}

}  // namespace
