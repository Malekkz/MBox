// Unit tests for the side of scada-tagd in retained values and retired
// generations (contract, sections 11.3 and 12; decision D-45), through
// GenerationHost: when a retain file is written and with what, the FINAL
// file of an owner that left a retired generation, and the removal of
// that generation, also with an observer still in it.

#include "attach_fixture.h"
#include "attachment.h"
#include "layout.h"
#include "retain_file.h"
#include "shared_memory.h"
#include "temp_database.h"

#include "scada/common/clock.h"
#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/generation_host.h"
#include "scada/tag/handle.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
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

constexpr std::int64_t NS_PER_MS = 1'000'000;
constexpr std::int64_t INTERVAL_NS =
    std::int64_t{DEFAULT_RETAIN_INTERVAL_MS} * NS_PER_MS;
constexpr std::int64_t RETIRED_TIMEOUT_NS =
    std::int64_t{RETIRED_GENERATION_TIMEOUT_MS} * NS_PER_MS;
constexpr mode_t RETAIN_DIR_MODE = 02750;
// Values the tests write and expect back; only their difference matters.
constexpr std::uint64_t VALUE_5 = 5;
constexpr std::uint64_t VALUE_6 = 6;
constexpr std::uint64_t VALUE_7 = 7;
constexpr std::uint64_t VALUE_8 = 8;
constexpr std::uint64_t VALUE_9 = 9;

class Keeper : public scada::tag::testing::AttachFixture {
protected:
    void SetUp() override {
        AttachFixture::SetUp();
        retain_ = root() / "retain";
        ASSERT_EQ(
            ::mkdir(retain_.c_str(), scada::tag::testing::PRIVATE_DIR_MODE), 0);
        ASSERT_EQ(::chmod(retain_.c_str(), RETAIN_DIR_MODE), 0);
        use_retain_dir(retain_.string());
    }

    // `io` owns plant.level (retain), plant.mode (retain_now) and
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

    // `retained_set` with one tag more: another config hash, so a host
    // started with it builds the next generation.
    [[nodiscard]] static TagSet changed_set() {
        TagSet set = retained_set();
        set.tags.push_back(
            {.name = "plant.extra", .owner = "io", .type = TagType::BOOL});
        return set;
    }

    [[nodiscard]] AttachOptions options() const {
        return {.base_dir = base().string(),
                .retain_dir = retain_.string(),
                .tagd_uid = uid()};
    }

    [[nodiscard]] Attached attached(std::string_view process) const {
        std::string detail;
        Result<Attached> result =
            Attachment::attach(process, options(), detail);
        EXPECT_TRUE(result.ok()) << result.status().message() << ": " << detail;
        return result.ok() ? std::move(result).value() : nullptr;
    }

    [[nodiscard]] Attached observer() const {
        std::string detail;
        Result<Attached> result = Attachment::observe(options(), detail);
        EXPECT_TRUE(result.ok()) << result.status().message() << ": " << detail;
        return result.ok() ? std::move(result).value() : nullptr;
    }

    static void write(Attachment& feeder, std::string_view name,
                      std::uint8_t type, std::uint64_t value,
                      std::uint8_t quality = SCADA_QUALITY_GOOD) {
        std::uint32_t handle = SCADA_INVALID_HANDLE;
        ASSERT_EQ(feeder.resolve(name, handle), SCADA_OK);
        scada_value written{};
        written.type = type;
        written.quality = quality;
        written.value = value;
        written.source_time_ns = static_cast<std::int64_t>(value) + 1;
        ASSERT_EQ(feeder.write(handle, written), SCADA_OK);
    }

    [[nodiscard]] static std::uint64_t read(Attachment& process,
                                            std::string_view name) {
        std::uint32_t handle = SCADA_INVALID_HANDLE;
        EXPECT_EQ(process.resolve(name, handle), SCADA_OK);
        scada_value value{};
        EXPECT_EQ(process.read(handle, value), SCADA_OK);
        return value.value;
    }

    // True if `owner` has a valid retain file.
    [[nodiscard]] bool has_file(std::string_view owner) const {
        const Fd directory = scada::tag::testing::open_directory(retain_);
        std::string detail;
        return read_retain_file(directory.get(), retain_file_name(owner), uid(),
                                detail)
            .ok();
    }

    // The retain file of `owner`; an empty one, with a failed expectation,
    // if there is no valid one.
    [[nodiscard]] RetainFile file_of(std::string_view owner) const {
        const Fd directory = scada::tag::testing::open_directory(retain_);
        std::string detail;
        Result<RetainFile> file = read_retain_file(
            directory.get(), retain_file_name(owner), uid(), detail);
        EXPECT_TRUE(file.ok()) << detail;
        return file.ok() ? std::move(file).value() : RetainFile{};
    }

    [[nodiscard]] static std::optional<std::uint64_t> value_in(
        const RetainFile& file, std::string_view name) {
        const RetainedValue* value = find_retained(file, name);
        return value != nullptr ? std::optional<std::uint64_t>(value->value)
                                : std::nullopt;
    }

    [[nodiscard]] const std::filesystem::path& retain() const {
        return retain_;
    }

    // One check at `now`, then the files the writer thread finished for it,
    // as one report: the writes of a check are reported at once.
    [[nodiscard]] CheckReport round(std::int64_t now) {
        CheckReport report = host().check(now);
        const CheckReport written = host().wait_retained_written();
        report.retain_written.insert(report.retain_written.end(),
                                     written.retain_written.begin(),
                                     written.retain_written.end());
        report.retain_problems.insert(report.retain_problems.end(),
                                      written.retain_problems.begin(),
                                      written.retain_problems.end());
        return report;
    }

    // Two rounds, at `now` and 1 ns later: a generation whose owners left is
    // removed at the check after the one that wrote their FINAL files.
    [[nodiscard]] CheckReport two_rounds(std::int64_t now) {
        CheckReport report = round(now);
        const CheckReport second = round(now + 1);
        report.retain_written.insert(report.retain_written.end(),
                                     second.retain_written.begin(),
                                     second.retain_written.end());
        report.retired.insert(report.retired.end(), second.retired.begin(),
                              second.retired.end());
        return report;
    }

private:
    std::filesystem::path retain_;
};

TEST_F(Keeper, ARetainNowChangeIsWrittenAtTheNextCheck) {
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    const std::int64_t now = scada::monotonic_ns();
    // Nothing on file and nothing written yet: no file.
    EXPECT_TRUE(round(now).retain_written.empty());
    EXPECT_FALSE(has_file("io"));

    write(*feeder, "plant.mode", SCADA_TYPE_INT64, 2);
    const CheckReport report = round(now + 1);
    ASSERT_EQ(report.retain_written.size(), 1U);
    EXPECT_EQ(report.retain_written[0].owner, "io");
    EXPECT_FALSE(report.retain_written[0].final);
    const RetainFile file = file_of("io");
    EXPECT_EQ(value_in(file, "plant.mode"), 2U);
    EXPECT_EQ(find_retained(file, "plant.mode")->source_time_ns, 3);
    EXPECT_FALSE(value_in(file, "plant.count").has_value());
    EXPECT_FALSE(file.final);
    EXPECT_EQ(file.source.generation, 1U);
    // No change: no write.
    EXPECT_TRUE(round(now + 2).retain_written.empty());
}

TEST_F(Keeper, AnOtherChangeWaitsForTheRetainInterval) {
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    const std::int64_t now = scada::monotonic_ns();
    write(*feeder, "plant.level", SCADA_TYPE_FLOAT64, VALUE_5);
    // The first write of an owner is not held back.
    EXPECT_EQ(round(now).retain_written.size(), 1U);
    write(*feeder, "plant.level", SCADA_TYPE_FLOAT64, VALUE_6);
    EXPECT_TRUE(round(now + INTERVAL_NS - 1).retain_written.empty());
    EXPECT_EQ(value_in(file_of("io"), "plant.level"), VALUE_5);
    EXPECT_EQ(round(now + INTERVAL_NS).retain_written.size(), 1U);
    EXPECT_EQ(value_in(file_of("io"), "plant.level"), VALUE_6);
}

TEST_F(Keeper, ARecordWithoutAValueLeavesTheValueOnFile) {
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    const std::int64_t now = scada::monotonic_ns();
    write(*feeder, "plant.mode", SCADA_TYPE_INT64, 2);
    ASSERT_EQ(round(now).retain_written.size(), 1U);
    write(*feeder, "plant.mode", SCADA_TYPE_INT64, VALUE_9, SCADA_QUALITY_BAD);
    EXPECT_TRUE(round(now + INTERVAL_NS).retain_written.empty());
    EXPECT_EQ(value_in(file_of("io"), "plant.mode"), 2U);
}

TEST_F(Keeper, FlushWritesWhatChangedSinceTheLastWrite) {
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    write(*feeder, "plant.level", SCADA_TYPE_FLOAT64, VALUE_5);
    ASSERT_EQ(round(scada::monotonic_ns()).retain_written.size(), 1U);
    write(*feeder, "plant.level", SCADA_TYPE_FLOAT64, VALUE_7);
    // Not checked since: the flush reads the generation itself.
    EXPECT_EQ(host().flush_retained().retain_written.size(), 1U);
    EXPECT_EQ(value_in(file_of("io"), "plant.level"), VALUE_7);
    EXPECT_TRUE(host().flush_retained().retain_written.empty());
}

TEST_F(Keeper, AFileThatFailsItsChecksIsReportedOnceAndReplaced) {
    scada::tag::testing::touch(retain() / "io.bin");
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    const std::int64_t now = scada::monotonic_ns();
    const CheckReport first = round(now);
    ASSERT_EQ(first.retain_problems.size(), 1U);
    EXPECT_EQ(first.retain_problems[0].subject, "io.bin");
    EXPECT_TRUE(round(now + 1).retain_problems.empty());
    write(*feeder, "plant.mode", SCADA_TYPE_INT64, 2);
    EXPECT_EQ(round(now + 2).retain_written.size(), 1U);
    EXPECT_TRUE(has_file("io"));
}

TEST_F(Keeper, StaleTemporaryFilesAreRemovedAtStart) {
    scada::tag::testing::touch(retain() / ".tmp.io.bin.123");
    start_host(retained_set());
    EXPECT_TRUE(scada::tag::testing::names_in(retain()).empty());
}

TEST_F(Keeper, ARetainDirectoryWithTheWrongModeIsRefused) {
    ASSERT_EQ(::chmod(retain().c_str(), 0770), 0);
    std::string detail;
    EXPECT_EQ(GenerationHost::start(
                  retained_set(),
                  {.base_dir = base().string(),
                   .tagd_uid = uid(),
                   .retain_dir = retain().string(),
                   .retain_interval_ms = DEFAULT_RETAIN_INTERVAL_MS},
                  detail)
                  .status()
                  .code(),
              scada::StatusCode::PERMISSION_DENIED);
}

// Conformance test 21, the side of scada-tagd: a value changed right before
// the change reaches the new generation.
TEST_F(Keeper,
       AnOwnerThatLeavesARetiredGenerationGetsItsFinalFileThenItIsRemoved) {
    start_host(retained_set());
    Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    write(*feeder, "plant.mode", SCADA_TYPE_INT64, 2);
    ASSERT_EQ(round(scada::monotonic_ns()).retain_written.size(), 1U);

    // scada-tagd restarts with a changed tag set while io runs.
    drop_host();
    start_host(changed_set());
    ASSERT_EQ(host().generation(), 2U);
    // Changed after the last write: only the FINAL file has it.
    write(*feeder, "plant.mode", SCADA_TYPE_INT64, 3);
    write(*feeder, "plant.level", SCADA_TYPE_FLOAT64, VALUE_8);
    const std::int64_t now = scada::monotonic_ns();
    CheckReport report = round(now);
    EXPECT_TRUE(report.retired.empty());  // io is still in it
    EXPECT_TRUE(std::filesystem::exists(generation_dir(1)));
    EXPECT_FALSE(file_of("io").final);

    feeder.reset();  // detaches: state STOPPING, lock released
    report = round(now + 1);
    ASSERT_EQ(report.retain_written.size(), 1U);
    EXPECT_TRUE(report.retain_written[0].final);
    // Removed once the FINAL file is on disk, at the next check.
    EXPECT_TRUE(report.retired.empty());
    report = round(now + 2);
    ASSERT_EQ(report.retired.size(), 1U);
    EXPECT_EQ(report.retired[0].generation, 1U);
    EXPECT_TRUE(report.retired[0].removed);
    EXPECT_FALSE(report.retired[0].timed_out);
    EXPECT_FALSE(std::filesystem::exists(generation_dir(1)));
    const RetainFile file = file_of("io");
    EXPECT_TRUE(file.final);
    EXPECT_EQ(file.source.generation, 1U);
    EXPECT_EQ(value_in(file, "plant.mode"), 3U);
    EXPECT_EQ(value_in(file, "plant.level"), VALUE_8);

    // io attaches to generation 2 and gets them back.
    const Attached again = attached("io");
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->status().final_waits_expired, 0U);
    EXPECT_EQ(read(*again, "plant.mode"), 3U);
    EXPECT_EQ(read(*again, "plant.level"), VALUE_8);
    // The restored values are what is on file: nothing to write.
    EXPECT_TRUE(round(now + 3).retain_written.empty());
}

TEST_F(Keeper, AFinalFileStaysMarkedFinalWhenLaterValuesAreWritten) {
    start_host(retained_set());
    Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    drop_host();
    start_host(changed_set());
    feeder.reset();
    const std::int64_t now = scada::monotonic_ns();
    ASSERT_EQ(two_rounds(now).retired.size(), 1U);
    const Attached again = attached("io");
    ASSERT_NE(again, nullptr);
    write(*again, "plant.mode", SCADA_TYPE_INT64, 4);
    ASSERT_EQ(round(now + 1).retain_written.size(), 1U);
    const RetainFile file = file_of("io");
    EXPECT_TRUE(file.final);
    EXPECT_EQ(file.source.generation, 1U);
    EXPECT_EQ(value_in(file, "plant.mode"), 4U);
}

TEST_F(Keeper,
       ARetiredGenerationIsRemovedAfterTheTimeoutWithAProcessStillInIt) {
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    write(*feeder, "plant.mode", SCADA_TYPE_INT64, VALUE_5);
    drop_host();
    start_host(changed_set());
    const std::int64_t now = scada::monotonic_ns();
    // io keeps its heartbeat fresh by the clock of the check.
    EXPECT_TRUE(round(now).retired.empty());
    const CheckReport report = round(now + RETIRED_TIMEOUT_NS);
    ASSERT_EQ(report.retired.size(), 1U);
    EXPECT_TRUE(report.retired[0].removed);
    EXPECT_TRUE(report.retired[0].timed_out);
    EXPECT_FALSE(std::filesystem::exists(generation_dir(1)));
    EXPECT_EQ(value_in(file_of("io"), "plant.mode"), VALUE_5);
    // The process still has its mappings and is told to move on.
    EXPECT_EQ(feeder->heartbeat(), SCADA_E_RETIRING);
    EXPECT_EQ(read(*feeder, "plant.mode"), VALUE_5);
}

TEST_F(Keeper, ARetiredGenerationLeftBeforeTheStartIsFinishedAtTheFirstCheck) {
    start_host(retained_set());
    {
        const Attached feeder = attached("io");
        ASSERT_NE(feeder, nullptr);
        write(*feeder, "plant.mode", SCADA_TYPE_INT64, VALUE_6);
    }
    // scada-tagd stopped before it read the change.
    drop_host();
    start_host(changed_set());
    const CheckReport report = two_rounds(scada::monotonic_ns());
    ASSERT_EQ(report.retired.size(), 1U);
    EXPECT_FALSE(std::filesystem::exists(generation_dir(1)));
    const RetainFile file = file_of("io");
    EXPECT_TRUE(file.final);
    EXPECT_EQ(value_in(file, "plant.mode"), VALUE_6);
}

TEST_F(Keeper,
       AnOlderGenerationWithoutLiveProcessIsFinishedAtStartBeforeItIsRemoved) {
    start_host(retained_set());
    Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    write(*feeder, "plant.mode", SCADA_TYPE_INT64, VALUE_6);
    // Generation 2 replaces 1 while io is in it; scada-tagd stops at once.
    drop_host();
    start_host(changed_set());
    drop_host();
    feeder.reset();
    // Generation 3: generation 1 is neither current nor alive.
    TagSet third = changed_set();
    third.tags.push_back(
        {.name = "plant.third", .owner = "io", .type = TagType::BOOL});
    start_host(third);
    ASSERT_EQ(host().generation(), 3U);
    EXPECT_FALSE(std::filesystem::exists(generation_dir(1)));
    const RetainFile file = file_of("io");
    EXPECT_TRUE(file.final);
    EXPECT_EQ(file.source.generation, 1U);
    EXPECT_EQ(value_in(file, "plant.mode"), VALUE_6);
    // The write at start is reported by the first check, which also
    // finishes generation 2.
    const CheckReport report = two_rounds(scada::monotonic_ns());
    EXPECT_FALSE(report.retain_written.empty());
    ASSERT_EQ(report.retired.size(), 1U);
    EXPECT_EQ(report.retired[0].generation, 2U);
}

TEST_F(Keeper, WithoutARetainDirectoryARetiredGenerationIsStillRemoved) {
    use_retain_dir({});
    start_host(retained_set());
    Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    drop_host();
    start_host(changed_set());
    const std::int64_t now = scada::monotonic_ns();
    EXPECT_TRUE(round(now).retired.empty());
    feeder.reset();
    EXPECT_EQ(round(now + 1).retired.size(), 1U);
    EXPECT_FALSE(std::filesystem::exists(generation_dir(1)));
    EXPECT_FALSE(has_file("io"));
}

// Contract 11.4: an observer holds no lock, so it does not keep a retired
// generation; it is told by scada_tag_status and observes again.
TEST_F(Keeper, AnObserverInARemovedGenerationIsToldAndObservesAgain) {
    start_host(retained_set());
    Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    write(*feeder, "plant.mode", SCADA_TYPE_INT64, VALUE_7);
    Attached watcher = observer();
    ASSERT_NE(watcher, nullptr);
    EXPECT_EQ(read(*watcher, "plant.mode"), VALUE_7);

    drop_host();
    start_host(changed_set());
    feeder.reset();
    ASSERT_EQ(two_rounds(scada::monotonic_ns()).retired.size(), 1U);
    ASSERT_FALSE(std::filesystem::exists(generation_dir(1)));

    // The header stays mapped: the observer sees RETIRING at once.
    EXPECT_EQ(watcher->status().state, GenerationState::RETIRING);
    // Its mapping of the removed region still reads.
    std::uint32_t handle = SCADA_INVALID_HANDLE;
    ASSERT_EQ(watcher->resolve("plant.mode", handle), SCADA_OK);
    scada_value value{};
    EXPECT_EQ(watcher->read(handle, value), SCADA_OK);
    watcher.reset();
    const Attached again = observer();
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->status().generation, 2U);
}

}  // namespace

namespace {

constexpr std::int64_t HEARTBEAT_STALE_NS = std::int64_t{4000} * NS_PER_MS;

TEST_F(Keeper, AHungOwnerIsNotTakenAsLeftBeforeTheTimeoutAndItsFileIsNotFinal) {
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    write(*feeder, "plant.mode", SCADA_TYPE_INT64, VALUE_5);
    drop_host();
    start_host(changed_set());
    const std::int64_t now = scada::monotonic_ns();
    // By the clock of the check its heartbeat is stale; it is RUNNING and
    // holds its lock, so it may write again.
    CheckReport report = round(now + HEARTBEAT_STALE_NS);
    EXPECT_TRUE(report.retired.empty());
    EXPECT_TRUE(std::ranges::none_of(
        report.retain_written,
        [](const RetainWrite& write) { return write.final; }));
    report = two_rounds(now + RETIRED_TIMEOUT_NS);
    ASSERT_EQ(report.retired.size(), 1U);
    EXPECT_TRUE(report.retired[0].timed_out);
    ASSERT_EQ(report.retired[0].still_attached.size(), 1U);
    EXPECT_EQ(report.retired[0].still_attached[0],
              "io (pid " + std::to_string(::getpid()) + ")");
    const RetainFile file = file_of("io");
    EXPECT_FALSE(file.final);
    EXPECT_EQ(value_in(file, "plant.mode"), VALUE_5);
}

TEST_F(Keeper, AnOwnerKilledMidWriteLeavesTheValueOnFile) {
    start_host(retained_set());
    Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    write(*feeder, "plant.mode", SCADA_TYPE_INT64, 2);
    ASSERT_EQ(round(scada::monotonic_ns()).retain_written.size(), 1U);
    drop_host();
    start_host(changed_set());
    write(*feeder, "plant.mode", SCADA_TYPE_INT64, 3);
    // The record of plant.mode (slot 1 in name order) is left mid-write.
    std::uint32_t handle = SCADA_INVALID_HANDLE;
    ASSERT_EQ(feeder->resolve("plant.mode", handle), SCADA_OK);
    auto& record = shared_at<TagRecord>(
        feeder->owners()[0].tags.memory.bytes(),
        sizeof(RegionHeader) +
            ((handle & MAX_TAGS_PER_OWNER) * sizeof(TagRecord)));
    store_shared(record.seq,
                 load_shared(record.seq, std::memory_order_relaxed) + 1,
                 std::memory_order_release);
    feeder.reset();
    const CheckReport report = two_rounds(scada::monotonic_ns());
    ASSERT_EQ(report.retired.size(), 1U);
    const RetainFile file = file_of("io");
    EXPECT_TRUE(file.final);
    EXPECT_EQ(value_in(file, "plant.mode"), 2U);
}

TEST_F(Keeper, NothingIsWrittenFromAnOlderGenerationOnceTheNewRegionExists) {
    start_host(retained_set());
    Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    write(*feeder, "plant.mode", SCADA_TYPE_INT64, 2);
    drop_host();
    start_host(changed_set());
    const std::int64_t now = scada::monotonic_ns();
    feeder.reset();
    // io attaches to generation 2 before scada-tagd looked: its wait runs
    // out, and it writes a newer value there.
    const Attached again = attached("io");
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->status().final_waits_expired, 1U);
    write(*again, "plant.mode", SCADA_TYPE_INT64, VALUE_9);
    const CheckReport report = two_rounds(now);
    ASSERT_EQ(report.retired.size(), 1U);
    const RetainFile file = file_of("io");
    EXPECT_FALSE(file.final);
    EXPECT_EQ(value_in(file, "plant.mode"), VALUE_9);
}

TEST_F(Keeper, AMovedTagWaitsForTheFinalFileOfItsFormerOwner) {
    TagSet before = retained_set();
    before.owners.push_back({.name = "web",
                             .process = "web",
                             .change_ring_capacity = MIN_CHANGE_RING_CAPACITY});
    TagSet after = before;
    after.tags.at(0).owner = "web";  // plant.level moves from io to web
    start_host(before);
    Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    write(*feeder, "plant.level", SCADA_TYPE_FLOAT64, VALUE_8);
    drop_host();
    start_host(after);
    // web attaches while io is still in generation 1; io leaves and
    // scada-tagd writes its FINAL file meanwhile.
    Attached web;
    std::jthread attaching([&] { web = attached("web"); });
    const std::int64_t deadline = scada::monotonic_ns() + RETAIN_FINAL_WAIT_NS;
    while (scada::monotonic_ns() < deadline &&
           !std::filesystem::exists(generation_dir(2) / "proc.web")) {
        std::this_thread::yield();
    }
    feeder.reset();
    static_cast<void>(round(scada::monotonic_ns()));
    attaching.join();
    ASSERT_NE(web, nullptr);
    EXPECT_EQ(web->status().final_waits_expired, 0U);
    EXPECT_EQ(read(*web, "plant.level"), VALUE_8);
}

TEST_F(Keeper, FilesOfOwnersThatAreGoneAreRemovedOnceEveryOwnerHasOne) {
    RetainFile gone;
    gone.values = {{"plant.level", TagType::FLOAT64, 1, 0}};
    {
        const Fd directory = scada::tag::testing::open_directory(retain());
        std::string detail;
        ASSERT_TRUE(
            write_retain_file(directory.get(), "old.owner", gone, detail).ok());
    }
    start_host(retained_set());
    // io has a value (from old.owner) but no file: written at the first
    // check; old.owner is removed at the one after.
    const std::int64_t now = scada::monotonic_ns();
    CheckReport report = round(now);
    EXPECT_EQ(report.retain_written.size(), 1U);
    report = round(now + 1);
    EXPECT_EQ(report.retain_files_removed,
              std::vector<std::string>{"old.owner.bin"});
    EXPECT_EQ(scada::tag::testing::names_in(retain()),
              std::vector<std::string>{"io.bin"});
    EXPECT_EQ(value_in(file_of("io"), "plant.level"), 1U);
}

TEST_F(Keeper, TwoGenerationChangesInARowKeepTheLastValue) {
    start_host(retained_set());
    Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    write(*feeder, "plant.mode", SCADA_TYPE_INT64, 1);
    drop_host();
    start_host(changed_set());  // generation 2
    write(*feeder, "plant.mode", SCADA_TYPE_INT64, 2);
    drop_host();
    TagSet third = changed_set();
    third.tags.push_back(
        {.name = "plant.third", .owner = "io", .type = TagType::BOOL});
    start_host(third);  // generation 3; 1 and 2 are retired, io is in 1
    ASSERT_EQ(host().generation(), 3U);
    write(*feeder, "plant.mode", SCADA_TYPE_INT64, 3);
    feeder.reset();
    const CheckReport report = two_rounds(scada::monotonic_ns());
    EXPECT_EQ(report.retired.size(), 2U);
    const Attached again = attached("io");
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->status().final_waits_expired, 0U);
    EXPECT_EQ(read(*again, "plant.mode"), 3U);
}

}  // namespace
