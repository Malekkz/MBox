// Unit tests for the side of scada-tagd in retained values and retired
// generations (contract, sections 11.3 and 12; decision D-45), through
// GenerationHost: when a retain file is written and with what, the FINAL
// file of an owner that left a retired generation, and the removal of
// that generation, also with an observer still in it.

#include "attach_fixture.h"
#include "attachment.h"
#include "layout.h"
#include "queue.h"
#include "region_file.h"
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

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Later than the once-a-second look of PeerRegions.
constexpr std::int64_t LOOK_AGAIN_NS = 2'000'000'000;

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
    // `count` events posted by `producer` to events.io are accepted.
    static ::testing::AssertionResult posts_events(Attachment& producer,
                                                   std::uint64_t count) {
        std::unique_ptr<Queue> events;
        if (Queue::open(producer, "events.io", events) != SCADA_OK) {
            return ::testing::AssertionFailure() << "cannot open events.io";
        }
        for (std::uint64_t i = 0; i < count; ++i) {
            const scada_event_record record = system_event();
            if (const int code =
                    events->post(std::as_bytes(std::span(&record, 1)));
                code != SCADA_OK) {
                return ::testing::AssertionFailure()
                       << "post " << i << " returned " << code;
            }
        }
        return ::testing::AssertionSuccess();
    }

    // In the current generation, `web` attaches first and finds nothing
    // skipped, and the first event `io` posts has id 0.
    [[nodiscard]] ::testing::AssertionResult first_event_has_id_zero() {
        const Attached reader = attached("web");
        const Attached feeder = attached("io");
        if (reader == nullptr || feeder == nullptr) {
            return ::testing::AssertionFailure() << "no attach";
        }
        std::unique_ptr<Queue> taker;
        if (Queue::open(*reader, "events.io", taker) != SCADA_OK) {
            return ::testing::AssertionFailure() << "cannot open events.io";
        }
        if (taker->stats().skipped != 0) {
            return ::testing::AssertionFailure() << "records skipped";
        }
        if (const ::testing::AssertionResult posted = posts_events(*feeder, 1);
            !posted) {
            return posted;
        }
        static_cast<void>(reader->peers().refresh(
            reader->directory(), scada::monotonic_ns() + LOOK_AGAIN_NS));
        scada_event_record taken{};
        if (const int code =
                taker->next(std::as_writable_bytes(std::span(&taken, 1)), 0);
            code != SCADA_OK) {
            return ::testing::AssertionFailure() << "next returned " << code;
        }
        if (taken.seq != 0) {
            return ::testing::AssertionFailure() << "id " << taken.seq;
        }
        return ::testing::AssertionSuccess();
    }

    static scada_event_record system_event() {
        scada_event_record record{};
        record.kind = SCADA_EVENT_SYSTEM;
        record.source_handle = SCADA_INVALID_HANDLE;
        return record;
    }

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
        report.retain_files_removed.insert(report.retain_files_removed.end(),
                                           written.retain_files_removed.begin(),
                                           written.retain_files_removed.end());
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

// Decision D-45: a value the owner stored BAD is disowned; it leaves the
// file, and the tag starts again BAD with NOT_INIT.
TEST_F(Keeper, AValueTheOwnerMarkedBadIsRemovedFromTheFile) {
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    const std::int64_t now = scada::monotonic_ns();
    write(*feeder, "plant.mode", SCADA_TYPE_INT64, 2);
    write(*feeder, "plant.level", SCADA_TYPE_FLOAT64, VALUE_5);
    ASSERT_EQ(round(now).retain_written.size(), 1U);
    write(*feeder, "plant.mode", SCADA_TYPE_INT64, VALUE_9, SCADA_QUALITY_BAD);
    // plant.mode is RETAIN_NOW: written at the next check.
    EXPECT_EQ(round(now + 1).retain_written.size(), 1U);
    const RetainFile file = file_of("io");
    EXPECT_FALSE(value_in(file, "plant.mode").has_value());
    EXPECT_EQ(value_in(file, "plant.level"), VALUE_5);
}

TEST_F(Keeper, AfterGoodThenBadTheTagStartsAgainBadAndNotInit) {
    start_host(retained_set());
    Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    write(*feeder, "plant.mode", SCADA_TYPE_INT64, 2);
    write(*feeder, "plant.mode", SCADA_TYPE_INT64, 2, SCADA_QUALITY_BAD);
    drop_host();
    start_host(changed_set());
    feeder.reset();
    ASSERT_EQ(two_rounds(scada::monotonic_ns()).retired.size(), 1U);
    const Attached again = attached("io");
    ASSERT_NE(again, nullptr);
    std::uint32_t handle = SCADA_INVALID_HANDLE;
    ASSERT_EQ(again->resolve("plant.mode", handle), SCADA_OK);
    scada_value value{};
    ASSERT_EQ(again->read(handle, value), SCADA_OK);
    EXPECT_EQ(value.quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(value.quality_flags, SCADA_FLAG_NOT_INIT);
}

// Re-check of PR #69: a restore that failed leaves the records NOT_INIT;
// that says nothing about the values on file, which must survive.
TEST_F(Keeper, AFailedRestoreDoesNotEraseTheValuesOnFile) {
    RetainFile kept;
    kept.values = {{"plant.level", TagType::FLOAT64, VALUE_5, 0},
                   {"plant.mode", TagType::INT64, 2, 0}};
    {
        const Fd directory = scada::tag::testing::open_directory(retain());
        std::string detail;
        ASSERT_TRUE(
            write_retain_file(directory.get(), "io", kept, detail).ok());
    }
    start_host(retained_set());
    AttachOptions unusable = options();
    unusable.retain_dir = (root() / "no-such-directory").string();
    std::string detail;
    Result<Attached> feeder = Attachment::attach("io", unusable, detail);
    ASSERT_TRUE(feeder.ok()) << detail;
    ASSERT_EQ(feeder.value()->status().directory_unusable, 1U);
    const std::int64_t now = scada::monotonic_ns();
    static_cast<void>(round(now));
    static_cast<void>(round(now + INTERVAL_NS));
    static_cast<void>(host().flush_retained());
    static_cast<void>(host().wait_retained_written());
    const RetainFile file = file_of("io");
    EXPECT_EQ(value_in(file, "plant.mode"), 2U);
    EXPECT_EQ(value_in(file, "plant.level"), VALUE_5);
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

// Review of PR #69, blocking 1: a generation finished at start must not
// write its FINAL file over newer values of a later generation that is
// still there.
TEST_F(Keeper, AFinalFileOfAnOlderGenerationNeverReplacesNewerValues) {
    start_host(retained_set());
    Attached first = attached("io");  // generation 1
    ASSERT_NE(first, nullptr);
    write(*first, "plant.mode", SCADA_TYPE_INT64, VALUE_6);
    drop_host();
    start_host(changed_set());  // generation 2; io is still in 1
    // A second io in generation 2; its wait for generation 1 runs out.
    const Attached second = attached("io");
    ASSERT_NE(second, nullptr);
    write(*second, "plant.mode", SCADA_TYPE_INT64, VALUE_9);
    static_cast<void>(round(scada::monotonic_ns()));
    ASSERT_EQ(value_in(file_of("io"), "plant.mode"), VALUE_9);
    // scada-tagd stops, the process of generation 1 ends, and scada-tagd
    // starts with a third tag set while the one of generation 2 runs.
    drop_host();
    first.reset();
    TagSet third = changed_set();
    third.tags.push_back(
        {.name = "plant.third", .owner = "io", .type = TagType::BOOL});
    start_host(third);
    EXPECT_FALSE(std::filesystem::exists(generation_dir(1)));
    EXPECT_EQ(value_in(file_of("io"), "plant.mode"), VALUE_9);
}

class ProcLock : public scada::tag::testing::TempDatabase {};

TEST_F(ProcLock, OnlyAMissingRegionOrAFreeLockReadsAsNotHeld) {
    const Fd directory = scada::tag::testing::open_directory(base());
    EXPECT_FALSE(proc_lock_held(directory.get(), "proc.none"));
    scada::tag::testing::touch(base() / "proc.free");
    EXPECT_FALSE(proc_lock_held(directory.get(), "proc.free"));
    const Fd held = scada::tag::testing::open_file(base() / "proc.free");
    ASSERT_EQ(::flock(held.get(), LOCK_EX | LOCK_NB), 0);
    EXPECT_TRUE(proc_lock_held(directory.get(), "proc.free"));
    // An error that is not "no such file" leaves it unknown: held.
    scada::tag::testing::touch(base() / "file");
    EXPECT_TRUE(proc_lock_held(directory.get(), "file/proc.x"));
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

// Issue #65 (decision D-54): records left in a queue of a retired
// generation are counted when scada-tagd removes it.
TEST_F(Keeper, RecordsLeftInARemovedGenerationAreCountedPerQueue) {
    start_host(retained_set());
    // The consumer first: the producer looks for its region at attach.
    Attached reader = attached("web");
    Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    ASSERT_NE(reader, nullptr);
    constexpr std::uint64_t POSTED = 3;
    ASSERT_TRUE(posts_events(*feeder, POSTED));
    drop_host();
    start_host(changed_set());
    feeder.reset();
    reader.reset();
    const CheckReport report = two_rounds(scada::monotonic_ns());
    ASSERT_EQ(report.retired.size(), 1U);
    const RetiredGeneration& retired = report.retired.at(0);
    ASSERT_TRUE(retired.removed);
    ASSERT_EQ(retired.unconsumed.size(), 1U);
    const UnconsumedQueue& left = retired.unconsumed.at(0);
    EXPECT_EQ(left.queue, "events.io");
    EXPECT_TRUE(left.events);
    EXPECT_TRUE(left.known);
    EXPECT_EQ(left.first, 0U);
    EXPECT_EQ(left.end, POSTED);
    EXPECT_EQ(left.records, POSTED);
}

// Review of PR #82 (T5): a retired generation whose header cannot be read
// is reported with one entry, its queues and their records unknown.
TEST_F(Keeper, AGenerationWithoutAReadableHeaderIsReportedUnknown) {
    start_host(retained_set());
    drop_host();
    std::filesystem::remove(generation_dir(1) / "header");
    start_host(changed_set());
    const CheckReport report = two_rounds(scada::monotonic_ns());
    ASSERT_EQ(report.retired.size(), 1U);
    const RetiredGeneration& retired = report.retired.at(0);
    ASSERT_TRUE(retired.removed);
    ASSERT_EQ(retired.unconsumed.size(), 1U);
    const UnconsumedQueue& left = retired.unconsumed.at(0);
    EXPECT_TRUE(left.queue.empty());
    EXPECT_TRUE(left.events);
    EXPECT_FALSE(left.known);
}

// Issue #65 (decision D-54): nothing of a queue is carried into the next
// generation. Through two changes in a row, a consumer that attaches first
// creates its region with the producer's `write_seq` 0, and event ids start
// at 0 again.
TEST_F(Keeper, EventIdsStartAtZeroAfterEachOfTwoGenerationChanges) {
    const std::vector<TagSet> sets = {retained_set(), changed_set(),
                                      retained_set()};
    for (std::size_t round = 0; round < sets.size(); ++round) {
        if (round > 0) {
            drop_host();
        }
        start_host(sets.at(round));
        ASSERT_EQ(host().generation(), round + 1);
        EXPECT_TRUE(first_event_has_id_zero()) << "round " << round;
    }
}

}  // namespace

// Decision D-59: format version 2 keeps quality and flags.
namespace {

constexpr std::int64_t FIXED_SOURCE_TIME = 1'000;

// Writes `value` to `name` with `quality` and `flags` and a fixed source
// time, so that only quality and flags differ between two writes.
void write_flagged(Attachment& feeder, std::string_view name,
                   std::uint64_t value, std::uint8_t quality,
                   std::uint16_t flags) {
    std::uint32_t handle = SCADA_INVALID_HANDLE;
    ASSERT_EQ(feeder.resolve(name, handle), SCADA_OK);
    scada_value written{};
    written.type = SCADA_TYPE_INT64;
    written.quality = quality;
    written.quality_flags = flags;
    written.value = value;
    written.source_time_ns = FIXED_SOURCE_TIME;
    ASSERT_EQ(feeder.write(handle, written), SCADA_OK);
}

TEST_F(Keeper, AChangeOfQualityOrFlagsAloneIsWritten) {
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    const std::int64_t now = scada::monotonic_ns();
    write_flagged(*feeder, "plant.mode", 2, SCADA_QUALITY_GOOD, 0);
    ASSERT_EQ(round(now).retain_written.size(), 1U);
    RetainFile file = file_of("io");
    EXPECT_EQ(file.version, RETAIN_FORMAT_VERSION);
    ASSERT_NE(find_retained(file, "plant.mode"), nullptr);
    EXPECT_EQ(find_retained(file, "plant.mode")->quality, Quality::GOOD);
    EXPECT_EQ(find_retained(file, "plant.mode")->quality_flags, 0U);
    // The stamp of this boot (decision D-59, point 2).
    const RetainClockSample clock = retain_clock_now();
    EXPECT_EQ(file.boot_id, clock.boot_id);
    EXPECT_GT(file.written_boottime_ns, 0);
    EXPECT_LE(file.written_boottime_ns, clock.boottime_ns);

    // SUBSTITUTED alone: plant.mode is RETAIN_NOW, so the next check.
    write_flagged(*feeder, "plant.mode", 2, SCADA_QUALITY_GOOD,
                  SCADA_FLAG_SUBSTITUTED);
    ASSERT_EQ(round(now + 1).retain_written.size(), 1U);
    file = file_of("io");
    EXPECT_EQ(find_retained(file, "plant.mode")->quality_flags,
              SCADA_FLAG_SUBSTITUTED);
    // The quality alone.
    write_flagged(*feeder, "plant.mode", 2, SCADA_QUALITY_UNCERTAIN,
                  SCADA_FLAG_SUBSTITUTED);
    ASSERT_EQ(round(now + 2).retain_written.size(), 1U);
    EXPECT_EQ(find_retained(file_of("io"), "plant.mode")->quality,
              Quality::UNCERTAIN);
    // No change: no write.
    EXPECT_TRUE(round(now + 3).retain_written.empty());
}

TEST_F(Keeper, ARestoredValueNotWrittenSinceKeepsTheQualityOnFile) {
    RetainFile stored;
    stored.values = {{"plant.mode", TagType::INT64, VALUE_7, FIXED_SOURCE_TIME,
                      Quality::GOOD, to_raw(QualityFlag::SUBSTITUTED)}};
    {
        const Fd directory = scada::tag::testing::open_directory(retain());
        std::string detail;
        ASSERT_TRUE(
            write_retain_file(directory.get(), "io", stored, detail).ok())
            << detail;
    }
    start_host(retained_set());
    const Attached feeder = attached("io");
    ASSERT_NE(feeder, nullptr);
    ASSERT_EQ(feeder->status().values_restored, 1U);
    const std::int64_t now = scada::monotonic_ns();
    // The restored record (UNCERTAIN, RESTORED, SUBSTITUTED) is no change.
    EXPECT_TRUE(round(now).retain_written.empty());
    // A write of another tag writes the file again; plant.mode keeps what
    // the owner stored, not what the restore made of it.
    write(*feeder, "plant.level", SCADA_TYPE_FLOAT64, VALUE_5);
    ASSERT_EQ(round(now + INTERVAL_NS).retain_written.size(), 1U);
    const RetainFile file = file_of("io");
    ASSERT_NE(find_retained(file, "plant.mode"), nullptr);
    EXPECT_EQ(*find_retained(file, "plant.mode"), stored.values.at(0));
    EXPECT_EQ(value_in(file, "plant.level"), VALUE_5);
}

}  // namespace

namespace {

// Decision D-59, point 6: generation 1 has a hung owner that still holds
// its lock, generation 2 holds that owner's newer value, and generation 3
// has no region of it yet. At the 30-s timeout of generation 1 nothing of
// it may replace the value of generation 2, which is on file marked FINAL.
TEST_F(Keeper,
       AtTheTimeoutAHungOwnerNeverOverwritesTheValueOfANewerGeneration) {
    start_host(retained_set());  // generation 1
    const Attached hung = attached("io");
    ASSERT_NE(hung, nullptr);
    write(*hung, "plant.mode", SCADA_TYPE_INT64, VALUE_5);
    drop_host();
    start_host(changed_set());  // generation 2
    {
        // Its wait for the FINAL file of generation 1 runs out: the owner
        // there never leaves.
        const Attached newer = attached("io");
        ASSERT_NE(newer, nullptr);
        write(*newer, "plant.mode", SCADA_TYPE_INT64, VALUE_6);
    }
    drop_host();
    TagSet third = changed_set();
    third.tags.push_back(
        {.name = "plant.third", .owner = "io", .type = TagType::BOOL});
    // Generation 3; 1 and 2 are retired. io has left generation 2: the
    // first checks write its FINAL file with VALUE_6 and remove it.
    start_host(third);
    ASSERT_EQ(host().generation(), 3U);
    const std::int64_t now = scada::monotonic_ns();
    CheckReport report = two_rounds(now);
    ASSERT_EQ(report.retired.size(), 1U);
    EXPECT_EQ(report.retired[0].generation, 2U);
    RetainFile file = file_of("io");
    ASSERT_TRUE(file.final);
    ASSERT_EQ(file.source.generation, 2U);
    ASSERT_EQ(value_in(file, "plant.mode"), VALUE_6);

    // Generation 1 times out with io still attached.
    report = two_rounds(now + RETIRED_TIMEOUT_NS);
    ASSERT_EQ(report.retired.size(), 1U);
    EXPECT_EQ(report.retired[0].generation, 1U);
    EXPECT_TRUE(report.retired[0].timed_out);
    file = file_of("io");
    EXPECT_EQ(value_in(file, "plant.mode"), VALUE_6);
    EXPECT_TRUE(file.final);
    EXPECT_EQ(file.source.generation, 2U);
}

}  // namespace

// Decision D-59, point 4: a tag that moved to another owner keeps its value
// when the new owner attaches late.
namespace {

// The tag set of `Keeper::retained_set` with every tag moved from `io` to
// `web`, and `io` no longer an owner.
TagSet moved_to_web(TagSet set) {
    set.owners = {{.name = "web",
                   .process = "web",
                   .change_ring_capacity = MIN_CHANGE_RING_CAPACITY}};
    for (auto& tag : set.tags) {
        tag.owner = "web";
    }
    return set;
}

class MovedKeeper : public Keeper {
protected:
    // Generation 1 with `former`, attached as io, having written `value`
    // to plant.level; then generation 2 with the tags moved to web.
    [[nodiscard]] Attached former_wrote(std::uint64_t value) {
        start_host(retained_set());
        Attached former = attached("io");
        if (former != nullptr) {
            write(*former, "plant.level", SCADA_TYPE_FLOAT64, value);
        }
        drop_host();
        start_host(moved_to_web(retained_set()));
        return former;
    }

    // What the restore of `process` did for plant.level.
    [[nodiscard]] static scada_tag_retain_info_t level_info(
        Attachment& process) {
        std::uint32_t handle = SCADA_INVALID_HANDLE;
        scada_tag_retain_info_t info{};
        EXPECT_EQ(process.resolve("plant.level", handle), SCADA_OK);
        EXPECT_EQ(process.retain_info(handle, info), SCADA_OK);
        return info;
    }

    // True if web.bin or io.bin holds `value` for plant.level.
    [[nodiscard]] bool value_somewhere(std::uint64_t value) const {
        const bool web_has =
            has_file("web") && value_in(file_of("web"), "plant.level") == value;
        const bool io_has =
            has_file("io") && value_in(file_of("io"), "plant.level") == value;
        return web_has || io_has;
    }

    // Rounds at `now + first` and the `count - 1` ns after it; fails at the
    // first after which no file holds `value`.
    [[nodiscard]] ::testing::AssertionResult rounds_keep(std::int64_t now,
                                                         int first, int count,
                                                         std::uint64_t value) {
        for (int i = first; i < first + count; ++i) {
            static_cast<void>(round(now + i));
            if (!value_somewhere(value)) {
                return ::testing::AssertionFailure() << "after round " << i;
            }
        }
        return ::testing::AssertionSuccess();
    }
};

constexpr int ROUNDS = 6;

// (a) The old generation is removed and its FINAL file is used: no wait.
TEST_F(MovedKeeper, ATagAttachedAfterTheOldGenerationWasRemovedTakesItsFinal) {
    Attached former = former_wrote(VALUE_7);
    ASSERT_NE(former, nullptr);
    former.reset();  // io leaves generation 1
    const std::int64_t now = scada::monotonic_ns();
    ASSERT_EQ(two_rounds(now).retired.size(), 1U);
    ASSERT_FALSE(std::filesystem::exists(generation_dir(1)));
    // io is in no generation any more. Its value of the moved tag goes to
    // the file of web, which has not attached, before io.bin is removed.
    ASSERT_TRUE(rounds_keep(now, 2, ROUNDS, VALUE_7));
    EXPECT_FALSE(has_file("io"));
    EXPECT_EQ(value_in(file_of("web"), "plant.level"), VALUE_7);

    const std::int64_t start = scada::monotonic_ns();
    const Attached web = attached("web");
    ASSERT_NE(web, nullptr);
    EXPECT_LT(scada::monotonic_ns() - start, RETAIN_FINAL_WAIT_NS / 2);
    EXPECT_EQ(web->status().final_waits_expired, 0U);
    EXPECT_EQ(read(*web, "plant.level"), VALUE_7);
    const scada_tag_retain_info_t info = level_info(*web);
    EXPECT_EQ(info.origin, SCADA_RETAIN_OWN_PERIODIC);
    EXPECT_EQ(info.final_wait_expired, 0);
}

// (b) The wait runs out while the former owner is still in the old
// generation: the periodic file is used, and the expired wait counted.
TEST_F(MovedKeeper, ATagWhoseFormerOwnerStaysTakesThePeriodicFileAfterTheWait) {
    start_host(retained_set());
    const Attached former = attached("io");
    ASSERT_NE(former, nullptr);
    write(*former, "plant.level", SCADA_TYPE_FLOAT64, VALUE_7);
    // The first write of an owner is at once: the periodic file.
    ASSERT_EQ(round(scada::monotonic_ns()).retain_written.size(), 1U);
    // A later value that only a FINAL file would hold.
    write(*former, "plant.level", SCADA_TYPE_FLOAT64, VALUE_8);
    drop_host();
    start_host(moved_to_web(retained_set()));

    const Attached web = attached("web");  // io holds on: the wait runs out
    ASSERT_NE(web, nullptr);
    EXPECT_EQ(web->status().final_waits_expired, 1U);
    EXPECT_EQ(read(*web, "plant.level"), VALUE_7);
    const scada_tag_retain_info_t info = level_info(*web);
    EXPECT_EQ(info.origin, SCADA_RETAIN_OTHER_OWNER);
    EXPECT_EQ(info.final_wait_expired, 1);
}

// (c) The file of the new owner is on disk before the file of the former
// owner, which is in no generation any more, is unlinked: at no point is
// the value on file nowhere.
TEST_F(MovedKeeper, TheNewOwnersFileIsWrittenBeforeTheFormerOwnersIsRemoved) {
    Attached former = former_wrote(VALUE_7);
    ASSERT_NE(former, nullptr);
    former.reset();
    // The FINAL file of io; then web attaches, before the removal of
    // generation 1, and restores from it.
    const std::int64_t now = scada::monotonic_ns();
    static_cast<void>(round(now));
    ASSERT_TRUE(file_of("io").final);
    const Attached web = attached("web");
    ASSERT_NE(web, nullptr);
    ASSERT_EQ(read(*web, "plant.level"), VALUE_7);
    ASSERT_TRUE(rounds_keep(now, 1, ROUNDS, VALUE_7));
    EXPECT_FALSE(has_file("io"));
    EXPECT_EQ(value_in(file_of("web"), "plant.level"), VALUE_7);
}

}  // namespace
