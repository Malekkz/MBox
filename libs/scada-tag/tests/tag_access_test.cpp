// Unit tests for the tag functions (contract, sections 5.1, 6, 7 and 13):
// resolve, info, read and write as two processes see them, the overlay of
// the owner state, and what a reader makes of a record that is not valid.
// The side of scada-tagd is a GenerationHost in the test process; the
// processes `io` (the owner) and `web` (a reader) are Attachments in it.

#include "attach_fixture.h"
#include "attachment.h"
#include "clock_check.h"
#include "layout.h"
#include "shared_memory.h"

#include "scada/common/clock.h"
#include "scada/common/mapped_region.h"
#include "scada/common/result.h"
#include "scada/tag/handle.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace scada::tag;
using scada::MappedRegion;
using scada::Result;
using scada::tag::testing::OWNER_IO;
using scada::tag::testing::PROCESS_IO;
using scada::tag::testing::SLOT_LABEL;
using scada::tag::testing::SLOT_LEVEL;
using scada::tag::testing::SLOT_NOTE;
using scada::tag::testing::SLOT_RUNNING;
using scada::tag::testing::STRING_SLOT_LABEL;
using scada::tag::testing::STRING_SLOT_NOTE;

using Attached = std::unique_ptr<Attachment>;

constexpr std::string_view NOTE_TEXT = "pump 1 tripped";
constexpr std::string_view LABEL_TEXT = "P1";
constexpr double LEVEL = 42.5;
constexpr double OTHER_LEVEL = -7.25;
constexpr std::int64_t DEVICE_TIME = 1'700'000'000'000'000'000;
constexpr std::int64_t NANOSECONDS_PER_SECOND = 1'000'000'000;
// Longer than the heartbeat timeout of every process of the fixture.
constexpr std::int64_t LONG_AGO_NS = 10 * NANOSECONDS_PER_SECOND;
constexpr std::uint32_t ODD_SEQ = 7;
// Values that are no enumerator and no flag.
constexpr std::uint8_t NO_QUALITY = 3;
constexpr std::uint8_t NO_TYPE = 9;
constexpr std::uint16_t NO_FLAG = 0x1000;
constexpr std::uint64_t NO_BOOL = 2;
// What a test puts into its output to see that a failed call left it.
constexpr std::uint64_t UNTOUCHED = 0xABCD;
constexpr std::size_t UNTOUCHED_LENGTH = 99;
constexpr std::size_t SMALL_BUFFER = 3;
// A handle of an owner and of a slot the generation does not have.
constexpr std::uint32_t NO_OWNER = 5;
constexpr std::uint32_t NO_SLOT = 4;

// Writes of the tests that run a writer against readers.
constexpr std::uint64_t RACE_WRITES = 100'000;
constexpr std::uint64_t STRING_RACE_WRITES = 20'000;
constexpr std::size_t READER_THREADS = 3;
constexpr std::uint64_t HEARTBEAT_WRITES = 256;
// The quality flags a racing writer derives from its counter: every flag
// but OWNER_DOWN, which a read adds.
constexpr std::uint64_t RACE_FLAGS = 0x0FDF;
constexpr std::uint64_t QUALITIES = 3;
// The racing writer makes texts of 0 to 250 bytes.
constexpr std::uint64_t TEXT_LENGTHS = SCADA_STRING_MAX + 1;
constexpr std::uint64_t LETTERS = 26;

scada_value value_of(TagType type, std::uint64_t pattern) {
    scada_value value{};
    value.type = to_raw(type);
    value.quality = to_raw(Quality::GOOD);
    value.value = pattern;
    return value;
}

scada_value level_value(double level) {
    return value_of(TagType::FLOAT64, std::bit_cast<std::uint64_t>(level));
}

class TagAccess : public scada::tag::testing::AttachFixture {
protected:
    void SetUp() override {
        AttachFixture::SetUp();
        start_host();
        io_ = attached("io");
        web_ = attached("web");
        ASSERT_NE(io_, nullptr);
        ASSERT_NE(web_, nullptr);
    }

    [[nodiscard]] Attached attached(std::string_view process) {
        std::string detail;
        Result<Attached> result = Attachment::attach(
            process,
            {.base_dir = base().string(), .retain_dir = {}, .tagd_uid = uid()},
            detail);
        EXPECT_TRUE(result.ok()) << result.status().message() << ": " << detail;
        return result.ok() ? std::move(result).value() : nullptr;
    }

    // The owner and a reader. A test may reset them.
    [[nodiscard]] Attached& io() { return io_; }
    [[nodiscard]] Attached& web() { return web_; }

    // The handle of `name` as `process` resolves it, or INVALID_HANDLE
    // with a failed expectation.
    [[nodiscard]] static std::uint32_t handle_of(Attachment& process,
                                                 std::string_view name) {
        std::uint32_t handle = INVALID_HANDLE;
        EXPECT_EQ(process.resolve(name, handle), SCADA_OK) << name;
        return handle;
    }

    // The record of slot `slot` in the `tags` region `tags`.
    [[nodiscard]] static TagRecord& record(const MappedRegion& tags,
                                           std::uint32_t slot) {
        return shared_at<TagRecord>(
            tags.bytes(), sizeof(RegionHeader) + (slot * sizeof(TagRecord)));
    }

    // The header of `proc.io`, as its owner has it.
    [[nodiscard]] static ProcHeader& proc_of(const MappedRegion& proc) {
        return shared_at<ProcHeader>(proc.bytes(), sizeof(RegionHeader));
    }

    // Makes the heartbeat of `io` older than its timeout.
    void stop_heartbeat_of_io() const {
        const MappedRegion proc = map_file("proc.io");
        store_shared(proc_of(proc).heartbeat_mono_ns,
                     scada::monotonic_ns() - LONG_AGO_NS,
                     std::memory_order_release);
    }

    // Sets the `down` flag of `io` in the process table, as scada-tagd does.
    void mark_io_down(std::uint32_t down) {
        const MappedRegion header = map_file("header");
        store_shared(
            shared_at<ProcessEntry>(
                header.bytes(),
                web_->header().contents().processes.at(PROCESS_IO).entry_offset)
                .down,
            down, std::memory_order_release);
    }

    // Removes the regions of `io` from the generation directory.
    void remove_regions_of_io() const {
        for (const char* name : {"tags.io", "strings.io", "proc.io"}) {
            EXPECT_TRUE(std::filesystem::remove(generation_dir() / name))
                << name;
        }
    }

    // Every tag function refuses `handle`, which is no handle of this
    // generation.
    static void expect_refused(Attachment& process, std::uint32_t handle) {
        expect_read_refused(process, handle);
        scada_tag_info_t info{};
        std::array<char, SCADA_STRING_MAX> buffer{};
        std::size_t length = 0;
        EXPECT_EQ(process.info(handle, info), SCADA_E_INVALID) << handle;
        EXPECT_EQ(process.write(handle, level_value(LEVEL)), SCADA_E_INVALID)
            << handle;
        EXPECT_EQ(process.read_string(handle, buffer, length, nullptr),
                  SCADA_E_INVALID);
        EXPECT_EQ(process.write_string(handle, "x", 0, 0, 0), SCADA_E_INVALID);
    }

    // A read refuses `handle` and leaves no good value in its output.
    static void expect_read_refused(Attachment& process, std::uint32_t handle) {
        scada_value value = level_value(LEVEL);
        value.source_time_ns = DEVICE_TIME;
        EXPECT_EQ(process.read(handle, value), SCADA_E_INVALID) << handle;
        EXPECT_EQ(value.quality, SCADA_QUALITY_BAD);
        EXPECT_EQ(value.source_time_ns, 0);
    }

    // `value` is LEVEL as the test AnotherProcessReadsWhatTheOwnerWrote
    // wrote it.
    static void expect_written_level(const scada_value& value) {
        EXPECT_EQ(value.type, SCADA_TYPE_FLOAT64);
        EXPECT_EQ(std::bit_cast<double>(value.value), LEVEL);
        EXPECT_EQ(value.quality, SCADA_QUALITY_UNCERTAIN);
        EXPECT_EQ(value.quality_flags, SCADA_FLAG_OUT_OF_RANGE);
        EXPECT_EQ(value.source_time_ns, DEVICE_TIME);
        EXPECT_EQ(value.change_count, 1U);
    }

    // The update times of `value` lie between `before_time` and
    // `before_mono`, read before the write, and now.
    static void expect_updated_since(const scada_value& value,
                                     std::int64_t before_time,
                                     std::int64_t before_mono) {
        EXPECT_GE(value.update_time_ns, before_time);
        EXPECT_LE(value.update_time_ns, scada::realtime_ns());
        EXPECT_GE(value.update_mono_ns, before_mono);
        EXPECT_GE(value.age_ns, 0);
        EXPECT_LE(value.age_ns, scada::monotonic_ns() - value.update_mono_ns);
    }

    // `reader` reads the tag plant.note as the test
    // AStringIsWrittenAndReadWithItsQualityAndTimes wrote it.
    static void expect_note(Attachment& reader) {
        std::array<char, SCADA_STRING_MAX> buffer{};
        std::size_t length = 0;
        scada_value meta{};
        ASSERT_EQ(reader.read_string(handle_of(reader, "plant.note"), buffer,
                                     length, &meta),
                  SCADA_OK);
        EXPECT_EQ(std::string_view(buffer.data(), length), NOTE_TEXT);
        expect_note_meta(meta);
    }

    // `meta` is what that test wrote with the text of plant.note.
    static void expect_note_meta(const scada_value& meta) {
        EXPECT_EQ(meta.type, SCADA_TYPE_STRING);
        EXPECT_EQ(meta.value, STRING_SLOT_NOTE);
        EXPECT_EQ(meta.quality, SCADA_QUALITY_UNCERTAIN);
        EXPECT_EQ(meta.quality_flags, SCADA_FLAG_SUBSTITUTED);
        EXPECT_EQ(meta.source_time_ns, DEVICE_TIME);
        EXPECT_EQ(meta.change_count, 1U);
    }

    // `reader` reads the tag plant.label as that test wrote it.
    static void expect_label(Attachment& reader) {
        std::array<char, SCADA_STRING_MAX> buffer{};
        std::size_t length = 0;
        ASSERT_EQ(reader.read_string(handle_of(reader, "plant.label"), buffer,
                                     length, nullptr),
                  SCADA_OK);
        EXPECT_EQ(std::string_view(buffer.data(), length), LABEL_TEXT);
    }

    // The heartbeat of `io`, at every HEARTBEAT_WRITES-th write `count` of a
    // long loop: on a slow machine the loop takes longer than the
    // heartbeat timeout, and the owner must not read as down meanwhile.
    void beat_now_and_then(std::uint64_t count) {
        if (count % HEARTBEAT_WRITES == 0) {
            static_cast<void>(io_->heartbeat());
        }
    }

private:
    Attached io_;
    Attached web_;
};

TEST_F(TagAccess, ResolveGivesTheHandleOfOwnerAndSlot) {
    EXPECT_EQ(handle_of(*io(), "plant.level"),
              pack_handle(OWNER_IO, SLOT_LEVEL));
    EXPECT_EQ(handle_of(*web(), "plant.level"),
              pack_handle(OWNER_IO, SLOT_LEVEL));
    EXPECT_EQ(handle_of(*web(), "plant.label"),
              pack_handle(OWNER_IO, SLOT_LABEL));
    EXPECT_EQ(handle_of(*web(), "plant.note"),
              pack_handle(OWNER_IO, SLOT_NOTE));
    EXPECT_EQ(handle_of(*web(), "plant.running"),
              pack_handle(OWNER_IO, SLOT_RUNNING));
}

TEST_F(TagAccess, ResolveRefusesNamesThatAreNoTag) {
    std::uint32_t handle = 0;
    EXPECT_EQ(web()->resolve("plant.pressure", handle), SCADA_E_NOT_FOUND);
    EXPECT_EQ(handle, INVALID_HANDLE);
    // A prefix of a name and a name with something after it.
    EXPECT_EQ(web()->resolve("plant.leve", handle), SCADA_E_NOT_FOUND);
    EXPECT_EQ(web()->resolve("plant.level2", handle), SCADA_E_NOT_FOUND);

    handle = 0;
    EXPECT_EQ(web()->resolve("", handle), SCADA_E_INVALID);
    EXPECT_EQ(handle, INVALID_HANDLE);
    EXPECT_EQ(web()->resolve("Plant.Level", handle), SCADA_E_INVALID);
    EXPECT_EQ(web()->resolve(std::string(SCADA_TAG_NAME_MAX + 1, 'a'), handle),
              SCADA_E_INVALID);
    EXPECT_EQ(web()->resolve(std::string(SCADA_TAG_NAME_MAX, 'a'), handle),
              SCADA_E_NOT_FOUND);
}

TEST_F(TagAccess, InfoGivesNameTypeFlagsAndOwner) {
    scada_tag_info_t info{};
    ASSERT_EQ(web()->info(handle_of(*web(), "plant.level"), info), SCADA_OK);
    EXPECT_EQ(std::string_view(std::data(info.name)), "plant.level");
    EXPECT_EQ(std::string_view(std::data(info.owner)), "io");
    EXPECT_EQ(info.type, SCADA_TYPE_FLOAT64);
    EXPECT_EQ(info.flags, 0);
    EXPECT_EQ(info.owned, 0);
    EXPECT_EQ(info.owner_index, OWNER_IO);
    EXPECT_EQ(info.process_index, PROCESS_IO);
    EXPECT_EQ(info.reserved0, 0);

    ASSERT_EQ(io()->info(handle_of(*io(), "plant.note"), info), SCADA_OK);
    EXPECT_EQ(std::string_view(std::data(info.name)), "plant.note");
    EXPECT_EQ(info.type, SCADA_TYPE_STRING);
    EXPECT_EQ(info.owned, 1);
}

TEST_F(TagAccess, InfoGivesTheFlagsOfTheTagSet) {
    io().reset();
    web().reset();
    drop_host();
    TagSet set = tag_set();
    set.tags.at(0).retain = true;
    set.tags.at(0).retain_now = true;
    set.tags.at(0).writable_by_command = true;
    start_host(set);
    const Attached reader = attached("web");
    ASSERT_NE(reader, nullptr);
    scada_tag_info_t info{};
    ASSERT_EQ(reader->info(handle_of(*reader, "plant.level"), info), SCADA_OK);
    EXPECT_EQ(info.flags, SCADA_TAG_RETAIN | SCADA_TAG_WRITABLE_BY_COMMAND |
                              SCADA_TAG_RETAIN_NOW);
}

TEST_F(TagAccess, AHandleOfNoTagIsRefusedEverywhere) {
    expect_refused(*io(), INVALID_HANDLE);
    expect_refused(*io(), (NO_OWNER << HANDLE_SLOT_BITS) | SLOT_LEVEL);
    expect_refused(*io(),
                   (std::uint32_t{OWNER_IO} << HANDLE_SLOT_BITS) | NO_SLOT);
}

TEST_F(TagAccess, ATagNeverWrittenReadsAsBadAndNotInitialised) {
    scada_value value{};
    ASSERT_EQ(web()->read(handle_of(*web(), "plant.level"), value), SCADA_OK);
    EXPECT_EQ(value.type, SCADA_TYPE_FLOAT64);
    EXPECT_EQ(value.quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(value.quality_flags, SCADA_FLAG_NOT_INIT);
    EXPECT_EQ(value.value, 0U);
    EXPECT_EQ(value.change_count, 0U);
    EXPECT_EQ(value.source_time_ns, 0);
    EXPECT_EQ(value.update_time_ns, 0);
    EXPECT_EQ(value.update_mono_ns, 0);
}

TEST_F(TagAccess, AnotherProcessReadsWhatTheOwnerWrote) {
    const std::int64_t before_time = scada::realtime_ns();
    const std::int64_t before_mono = scada::monotonic_ns();
    scada_value written = level_value(LEVEL);
    written.quality = SCADA_QUALITY_UNCERTAIN;
    written.quality_flags = SCADA_FLAG_OUT_OF_RANGE;
    written.source_time_ns = DEVICE_TIME;
    // These are ignored by a write.
    written.update_time_ns = 1;
    written.update_mono_ns = 1;
    written.change_count = UNTOUCHED_LENGTH;
    ASSERT_EQ(io()->write(handle_of(*io(), "plant.level"), written), SCADA_OK);

    scada_value value{};
    ASSERT_EQ(web()->read(handle_of(*web(), "plant.level"), value), SCADA_OK);
    expect_written_level(value);
    expect_updated_since(value, before_time, before_mono);
    // The owner reads its own tag the same way.
    value = {};
    ASSERT_EQ(io()->read(handle_of(*io(), "plant.level"), value), SCADA_OK);
    expect_written_level(value);
    expect_updated_since(value, before_time, before_mono);
}

TEST_F(TagAccess, AWriteWithoutADeviceTimeUsesTheUpdateTime) {
    const std::uint32_t level = handle_of(*io(), "plant.level");
    ASSERT_EQ(io()->write(level, level_value(LEVEL)), SCADA_OK);
    scada_value value{};
    ASSERT_EQ(io()->read(level, value), SCADA_OK);
    EXPECT_EQ(value.source_time_ns, value.update_time_ns);
    EXPECT_EQ(value.quality_flags, SCADA_FLAG_LOCAL_TIME);
    EXPECT_EQ(value.quality, SCADA_QUALITY_GOOD);
}

TEST_F(TagAccess, TheChangeCountFollowsValueQualityAndFlags) {
    const std::uint32_t level = handle_of(*io(), "plant.level");
    scada_value written = level_value(LEVEL);
    written.source_time_ns = DEVICE_TIME;
    ASSERT_EQ(io()->write(level, written), SCADA_OK);
    written.source_time_ns = DEVICE_TIME + 1;
    ASSERT_EQ(io()->write(level, written), SCADA_OK);
    scada_value value{};
    ASSERT_EQ(io()->read(level, value), SCADA_OK);
    EXPECT_EQ(value.change_count, 1U);
    EXPECT_EQ(value.source_time_ns, DEVICE_TIME + 1);

    ASSERT_EQ(io()->write(level, level_value(OTHER_LEVEL)), SCADA_OK);
    ASSERT_EQ(io()->read(level, value), SCADA_OK);
    // The value changed, and LOCAL_TIME came with the missing device time.
    EXPECT_EQ(value.change_count, 2U);
}

TEST_F(TagAccess, EveryTypeIsWrittenAndRead) {
    const std::uint32_t running = handle_of(*io(), "plant.running");
    ASSERT_EQ(io()->write(running, value_of(TagType::BOOL, 1)), SCADA_OK);
    scada_value value{};
    ASSERT_EQ(web()->read(handle_of(*web(), "plant.running"), value), SCADA_OK);
    EXPECT_EQ(value.type, SCADA_TYPE_BOOL);
    EXPECT_EQ(value.value, 1U);

    // A STRING tag read as a value gives the number of its string slot.
    ASSERT_EQ(web()->read(handle_of(*web(), "plant.note"), value), SCADA_OK);
    EXPECT_EQ(value.type, SCADA_TYPE_STRING);
    EXPECT_EQ(value.value, STRING_SLOT_NOTE);
    ASSERT_EQ(web()->read(handle_of(*web(), "plant.label"), value), SCADA_OK);
    EXPECT_EQ(value.value, STRING_SLOT_LABEL);
}

TEST_F(TagAccess, Conformance08AProcessThatIsNotTheOwnerCannotWrite) {
    const std::uint32_t level = handle_of(*web(), "plant.level");
    EXPECT_EQ(web()->write(level, level_value(LEVEL)), SCADA_E_NOT_OWNER);
    const std::array<std::uint32_t, 1> handles{level};
    const std::array<scada_value, 1> values{level_value(LEVEL)};
    EXPECT_EQ(web()->write_many(handles, values), SCADA_E_NOT_OWNER);
    EXPECT_EQ(
        web()->write_string(handle_of(*web(), "plant.note"), "x", 0, 0, 0),
        SCADA_E_NOT_OWNER);

    // Nothing was written: the record is as attach left it.
    scada_value value{};
    ASSERT_EQ(io()->read(handle_of(*io(), "plant.level"), value), SCADA_OK);
    EXPECT_EQ(value.quality_flags, SCADA_FLAG_NOT_INIT);
    EXPECT_EQ(value.change_count, 0U);
}

TEST_F(TagAccess, AWriteOfTheWrongTypeIsRefused) {
    const std::uint32_t level = handle_of(*io(), "plant.level");
    EXPECT_EQ(io()->write(level, value_of(TagType::INT64, 1)), SCADA_E_TYPE);
    scada_value no_type = level_value(LEVEL);
    no_type.type = NO_TYPE;
    EXPECT_EQ(io()->write(level, no_type), SCADA_E_TYPE);
    // A STRING tag is written with write_string.
    EXPECT_EQ(io()->write(handle_of(*io(), "plant.note"),
                          value_of(TagType::STRING, 1)),
              SCADA_E_TYPE);
    scada_value value{};
    ASSERT_EQ(io()->read(level, value), SCADA_OK);
    EXPECT_EQ(value.change_count, 0U);
}

TEST_F(TagAccess, AWriteOfAnInvalidValueIsRefused) {
    const std::uint32_t level = handle_of(*io(), "plant.level");
    const std::uint32_t running = handle_of(*io(), "plant.running");
    scada_value bad = level_value(LEVEL);
    bad.quality = NO_QUALITY;
    EXPECT_EQ(io()->write(level, bad), SCADA_E_INVALID);
    bad = level_value(LEVEL);
    bad.quality_flags = NO_FLAG;
    EXPECT_EQ(io()->write(level, bad), SCADA_E_INVALID);
    EXPECT_EQ(io()->write(running, value_of(TagType::BOOL, NO_BOOL)),
              SCADA_E_INVALID);

    scada_value value{};
    ASSERT_EQ(io()->read(level, value), SCADA_OK);
    EXPECT_EQ(value.change_count, 0U);
    ASSERT_EQ(io()->read(running, value), SCADA_OK);
    EXPECT_EQ(value.change_count, 0U);
}

TEST_F(TagAccess, ReadManyGivesAStatusForEachHandle) {
    const std::uint32_t level = handle_of(*web(), "plant.level");
    const std::uint32_t running = handle_of(*web(), "plant.running");
    ASSERT_EQ(io()->write(level, level_value(LEVEL)), SCADA_OK);
    ASSERT_EQ(io()->write(running, value_of(TagType::BOOL, 1)), SCADA_OK);

    const std::array<std::uint32_t, 3> handles{level, INVALID_HANDLE, running};
    std::array<scada_value, 3> values{};
    values.at(1).value = UNTOUCHED;
    std::array<int, 3> status{-1, -1, -1};
    EXPECT_EQ(web()->read_many(handles, values, status), SCADA_E_INVALID);
    EXPECT_EQ(status,
              (std::array<int, 3>{SCADA_OK, SCADA_E_INVALID, SCADA_OK}));
    EXPECT_EQ(std::bit_cast<double>(values.at(0).value), LEVEL);
    // The handle that is none leaves no value behind.
    EXPECT_EQ(values.at(1).value, 0U);
    EXPECT_EQ(values.at(1).quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(values.at(2).value, 1U);
    // One reading of the clock: tags written together have ages that
    // differ as their update times do.
    EXPECT_EQ(values.at(0).age_ns - values.at(2).age_ns,
              values.at(2).update_mono_ns - values.at(0).update_mono_ns);

    // Without a status array, and without any handle.
    const std::span<const std::uint32_t> good =
        std::span<const std::uint32_t>(handles).first(1);
    EXPECT_EQ(
        web()->read_many(good, std::span<scada_value>(values).first(1), {}),
        SCADA_OK);
    EXPECT_EQ(web()->read_many({}, {}, {}), SCADA_OK);
}

TEST_F(TagAccess, WriteManyWritesAllWithOneTimeOrNothing) {
    const std::uint32_t level = handle_of(*io(), "plant.level");
    const std::uint32_t running = handle_of(*io(), "plant.running");
    const std::array<std::uint32_t, 2> handles{level, running};
    std::array<scada_value, 2> values{level_value(LEVEL),
                                      value_of(TagType::BOOL, NO_BOOL)};
    EXPECT_EQ(io()->write_many(handles, values), SCADA_E_INVALID);
    scada_value value{};
    ASSERT_EQ(io()->read(level, value), SCADA_OK);
    EXPECT_EQ(value.change_count, 0U) << "the first of the pair was written";

    values.at(1) = value_of(TagType::BOOL, 1);
    ASSERT_EQ(io()->write_many(handles, values), SCADA_OK);
    std::array<scada_value, 2> read{};
    ASSERT_EQ(io()->read_many(handles, read, {}), SCADA_OK);
    EXPECT_EQ(std::bit_cast<double>(read.at(0).value), LEVEL);
    EXPECT_EQ(read.at(1).value, 1U);
    EXPECT_EQ(read.at(0).update_mono_ns, read.at(1).update_mono_ns);
    EXPECT_EQ(read.at(0).update_time_ns, read.at(1).update_time_ns);
    EXPECT_EQ(io()->write_many({}, {}), SCADA_OK);
}

TEST_F(TagAccess, Conformance06AnOwnerThatStopsCleanlyReadsAsDownAtOnce) {
    const std::uint32_t level = handle_of(*web(), "plant.level");
    ASSERT_EQ(io()->write(handle_of(*io(), "plant.level"), level_value(LEVEL)),
              SCADA_OK);
    scada_value value{};
    ASSERT_EQ(web()->read(level, value), SCADA_OK);
    EXPECT_EQ(value.quality, SCADA_QUALITY_GOOD);

    io().reset();  // detach
    ASSERT_EQ(web()->read(level, value), SCADA_OK);
    EXPECT_EQ(value.quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(value.quality_flags,
              SCADA_FLAG_OWNER_DOWN | SCADA_FLAG_LOCAL_TIME);
    // The last value is still there; the record was not changed.
    EXPECT_EQ(std::bit_cast<double>(value.value), LEVEL);
    EXPECT_EQ(value.change_count, 1U);

    // The owner comes back: readers recover without resolving again.
    io() = attached("io");
    ASSERT_NE(io(), nullptr);
    ASSERT_EQ(web()->read(level, value), SCADA_OK);
    EXPECT_EQ(value.quality, SCADA_QUALITY_GOOD);
    EXPECT_EQ(value.quality_flags, SCADA_FLAG_LOCAL_TIME);
}

TEST_F(TagAccess, AnOwnerWhoseHeartbeatStoppedReadsAsDown) {
    const std::uint32_t level = handle_of(*web(), "plant.level");
    ASSERT_EQ(io()->write(handle_of(*io(), "plant.level"), level_value(LEVEL)),
              SCADA_OK);
    stop_heartbeat_of_io();
    scada_value value{};
    ASSERT_EQ(web()->read(level, value), SCADA_OK);
    EXPECT_EQ(value.quality, SCADA_QUALITY_BAD);
    EXPECT_TRUE(has_flag(value.quality_flags, QualityFlag::OWNER_DOWN));
    EXPECT_EQ(std::bit_cast<double>(value.value), LEVEL);
    // The process reads its own tags the same way.
    ASSERT_EQ(io()->read(handle_of(*io(), "plant.level"), value), SCADA_OK);
    EXPECT_TRUE(has_flag(value.quality_flags, QualityFlag::OWNER_DOWN));

    // The next heartbeat brings it back.
    EXPECT_EQ(io()->heartbeat(), SCADA_OK);
    ASSERT_EQ(web()->read(level, value), SCADA_OK);
    EXPECT_EQ(value.quality, SCADA_QUALITY_GOOD);
    EXPECT_FALSE(has_flag(value.quality_flags, QualityFlag::OWNER_DOWN));
}

// The library judges the owner itself (contract 10.1). The flag of
// scada-tagd lags behind by a check interval, and one left by a scada-tagd
// that stopped must not keep a running owner down.
TEST_F(TagAccess, TheDownFlagOfScadaTagdDoesNotChangeARead) {
    const std::uint32_t level = handle_of(*web(), "plant.level");
    ASSERT_EQ(io()->write(handle_of(*io(), "plant.level"), level_value(LEVEL)),
              SCADA_OK);
    mark_io_down(1);
    scada_value value{};
    ASSERT_EQ(web()->read(level, value), SCADA_OK);
    EXPECT_EQ(value.quality, SCADA_QUALITY_GOOD);
    EXPECT_FALSE(has_flag(value.quality_flags, QualityFlag::OWNER_DOWN));
}

TEST_F(TagAccess, ATagThatWasNotResolvedHereReadsAsOwnerDown) {
    // `web` takes the handle from elsewhere: nothing of `io` is mapped.
    const std::uint32_t level = handle_of(*io(), "plant.level");
    ASSERT_EQ(io()->write(level, level_value(LEVEL)), SCADA_OK);
    scada_value value{};
    ASSERT_EQ(web()->read(level, value), SCADA_OK);
    EXPECT_EQ(value.type, SCADA_TYPE_FLOAT64);
    EXPECT_EQ(value.quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(value.quality_flags, SCADA_FLAG_OWNER_DOWN);
    EXPECT_EQ(value.value, 0U);
    EXPECT_EQ(value.update_mono_ns, 0);

    ASSERT_EQ(handle_of(*web(), "plant.level"), level);
    ASSERT_EQ(web()->read(level, value), SCADA_OK);
    EXPECT_EQ(value.quality, SCADA_QUALITY_GOOD);
    EXPECT_EQ(std::bit_cast<double>(value.value), LEVEL);
}

TEST_F(TagAccess, ATagWhoseRegionsAreMissingReadsAsOwnerDown) {
    // As if `io` had never attached: its regions are not there.
    io().reset();
    remove_regions_of_io();
    const std::uint32_t level = handle_of(*web(), "plant.level");
    scada_value value{};
    ASSERT_EQ(web()->read(level, value), SCADA_OK);
    EXPECT_EQ(value.quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(value.quality_flags, SCADA_FLAG_OWNER_DOWN);
    std::array<char, SCADA_STRING_MAX> buffer{};
    std::size_t length = UNTOUCHED_LENGTH;
    scada_value meta{};
    ASSERT_EQ(web()->read_string(handle_of(*web(), "plant.note"), buffer,
                                 length, &meta),
              SCADA_OK);
    EXPECT_EQ(length, 0U);
    EXPECT_EQ(meta.type, SCADA_TYPE_STRING);
    EXPECT_EQ(meta.quality_flags, SCADA_FLAG_OWNER_DOWN);
}

TEST_F(TagAccess, ARecordLeftMidWriteIsBusyWhileItsOwnerIsUp) {
    const std::uint32_t level = handle_of(*web(), "plant.level");
    ASSERT_EQ(io()->write(handle_of(*io(), "plant.level"), level_value(LEVEL)),
              SCADA_OK);
    const MappedRegion tags = map_file("tags.io");
    store_shared(record(tags, SLOT_LEVEL).seq, ODD_SEQ,
                 std::memory_order_release);

    scada_value value{};
    value.value = UNTOUCHED;
    EXPECT_EQ(web()->read(level, value), SCADA_E_BUSY);
    EXPECT_EQ(value.value, UNTOUCHED);
    const std::array<std::uint32_t, 1> handles{level};
    std::array<scada_value, 1> values{};
    std::array<int, 1> status{};
    EXPECT_EQ(web()->read_many(handles, values, status), SCADA_E_BUSY);
    EXPECT_EQ(status.at(0), SCADA_E_BUSY);
}

TEST_F(TagAccess, Conformance05ARecordLeftMidWriteByADeadOwnerReadsAsDown) {
    const std::uint32_t level = handle_of(*web(), "plant.level");
    const std::uint32_t note = handle_of(*web(), "plant.note");
    ASSERT_EQ(io()->write(handle_of(*io(), "plant.level"), level_value(LEVEL)),
              SCADA_OK);
    ASSERT_EQ(
        io()->write_string(handle_of(*io(), "plant.note"), "text", 0, 0, 0),
        SCADA_OK);
    const MappedRegion tags = map_file("tags.io");
    store_shared(record(tags, SLOT_LEVEL).seq, ODD_SEQ,
                 std::memory_order_release);
    store_shared(record(tags, SLOT_NOTE).seq, ODD_SEQ,
                 std::memory_order_release);
    stop_heartbeat_of_io();

    // No value: what the owner left is half a write.
    scada_value value{};
    ASSERT_EQ(web()->read(level, value), SCADA_OK);
    EXPECT_EQ(value.type, SCADA_TYPE_FLOAT64);
    EXPECT_EQ(value.quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(value.quality_flags, SCADA_FLAG_OWNER_DOWN);
    EXPECT_EQ(value.value, 0U);

    std::array<char, SCADA_STRING_MAX> buffer{};
    std::size_t length = UNTOUCHED_LENGTH;
    scada_value meta{};
    ASSERT_EQ(web()->read_string(note, buffer, length, &meta), SCADA_OK);
    EXPECT_EQ(length, 0U);
    EXPECT_EQ(meta.quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(meta.quality_flags, SCADA_FLAG_OWNER_DOWN);
}

// `reader` refuses the record of `handle` and leaves no good value in its
// output: a caller that keeps what it has must not keep a good value for
// ever.
void expect_refused_record(Attachment& reader, std::uint32_t handle,
                           std::uint8_t type) {
    scada_value value{};
    value.value = UNTOUCHED;
    value.quality = SCADA_QUALITY_GOOD;
    EXPECT_EQ(reader.read(handle, value), SCADA_E_LAYOUT);
    EXPECT_EQ(value.type, type);
    EXPECT_EQ(value.quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(value.quality_flags, 0);
    EXPECT_EQ(value.value, 0U);
}

TEST_F(TagAccess, ARecordWithAnInvalidQualityFlagOrTypeIsRefused) {
    const std::uint32_t level = handle_of(*web(), "plant.level");
    ASSERT_EQ(io()->write(handle_of(*io(), "plant.level"), level_value(LEVEL)),
              SCADA_OK);
    const MappedRegion tags = map_file("tags.io");
    // The owner is the only writer of these fields; the test stands in
    // for an owner with a defect.
    record(tags, SLOT_LEVEL).quality = NO_QUALITY;
    expect_refused_record(*web(), level, SCADA_TYPE_FLOAT64);
    record(tags, SLOT_LEVEL).quality = to_raw(Quality::GOOD);
    record(tags, SLOT_LEVEL).quality_flags = NO_FLAG;
    expect_refused_record(*web(), level, SCADA_TYPE_FLOAT64);
    // OWNER_DOWN is laid over a value by a read and never stored.
    record(tags, SLOT_LEVEL).quality_flags = SCADA_FLAG_OWNER_DOWN;
    expect_refused_record(*web(), level, SCADA_TYPE_FLOAT64);
    record(tags, SLOT_LEVEL).quality_flags = 0;
    record(tags, SLOT_LEVEL).type = to_raw(TagType::INT64);
    expect_refused_record(*web(), level, SCADA_TYPE_FLOAT64);
    record(tags, SLOT_LEVEL).type = to_raw(TagType::FLOAT64);
    scada_value value{};
    EXPECT_EQ(web()->read(level, value), SCADA_OK);
    EXPECT_EQ(std::bit_cast<double>(value.value), LEVEL);
}

TEST_F(TagAccess, ARecordWithAnInvalidValueIsRefused) {
    const std::uint32_t running = handle_of(*web(), "plant.running");
    const std::uint32_t note = handle_of(*web(), "plant.note");
    const MappedRegion tags = map_file("tags.io");
    record(tags, SLOT_RUNNING).quality_flags = 0;
    record(tags, SLOT_RUNNING).value = NO_BOOL;
    expect_refused_record(*web(), running, SCADA_TYPE_BOOL);
    // A STRING record that names another string slot.
    record(tags, SLOT_NOTE).quality_flags = 0;
    record(tags, SLOT_NOTE).value = STRING_SLOT_LABEL;
    expect_refused_record(*web(), note, SCADA_TYPE_STRING);
    std::array<char, SCADA_STRING_MAX> buffer{};
    std::size_t length = UNTOUCHED_LENGTH;
    scada_value meta{};
    meta.quality = SCADA_QUALITY_GOOD;
    EXPECT_EQ(web()->read_string(note, buffer, length, &meta), SCADA_E_LAYOUT);
    EXPECT_EQ(length, 0U);
    EXPECT_EQ(meta.quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(meta.type, SCADA_TYPE_STRING);
}

TEST_F(TagAccess, OwnerDownCannotBeWritten) {
    const std::uint32_t level = handle_of(*io(), "plant.level");
    scada_value down = level_value(LEVEL);
    down.quality = SCADA_QUALITY_BAD;
    down.quality_flags = SCADA_FLAG_OWNER_DOWN | SCADA_FLAG_STALE;
    EXPECT_EQ(io()->write(level, down), SCADA_E_INVALID);
    const std::array<std::uint32_t, 1> handles{level};
    const std::array<scada_value, 1> values{down};
    EXPECT_EQ(io()->write_many(handles, values), SCADA_E_INVALID);
    EXPECT_EQ(io()->write_string(handle_of(*io(), "plant.note"), "x",
                                 SCADA_QUALITY_BAD, SCADA_FLAG_OWNER_DOWN, 0),
              SCADA_E_INVALID);
    scada_value value{};
    ASSERT_EQ(io()->read(level, value), SCADA_OK);
    EXPECT_EQ(value.change_count, 0U);
}

TEST_F(TagAccess, AStringIsWrittenAndReadWithItsQualityAndTimes) {
    const std::uint32_t note = handle_of(*io(), "plant.note");
    ASSERT_EQ(io()->write_string(note, NOTE_TEXT, SCADA_QUALITY_UNCERTAIN,
                                 SCADA_FLAG_SUBSTITUTED, DEVICE_TIME),
              SCADA_OK);
    // The other string tag is not touched.
    ASSERT_EQ(io()->write_string(handle_of(*io(), "plant.label"), LABEL_TEXT, 0,
                                 0, 0),
              SCADA_OK);
    expect_note(*web());
    expect_label(*web());
    expect_note(*io());
    expect_label(*io());
}

TEST_F(TagAccess, AStringOfTheLargestLengthAndAnEmptyOneAreWritten) {
    const std::uint32_t note = handle_of(*io(), "plant.note");
    const std::string longest(SCADA_STRING_MAX, 'z');
    ASSERT_EQ(io()->write_string(note, longest, 0, 0, 0), SCADA_OK);
    std::array<char, SCADA_STRING_MAX> buffer{};
    std::size_t length = 0;
    ASSERT_EQ(io()->read_string(note, buffer, length, nullptr), SCADA_OK);
    EXPECT_EQ(std::string_view(buffer.data(), length), longest);

    ASSERT_EQ(io()->write_string(note, "", 0, 0, 0), SCADA_OK);
    ASSERT_EQ(io()->read_string(note, buffer, length, nullptr), SCADA_OK);
    EXPECT_EQ(length, 0U);
    // An empty buffer is enough for an empty text.
    ASSERT_EQ(io()->read_string(note, {}, length, nullptr), SCADA_OK);

    EXPECT_EQ(io()->write_string(note, std::string(SCADA_STRING_MAX + 1, 'z'),
                                 0, 0, 0),
              SCADA_E_INVALID);
}

TEST_F(TagAccess, ABufferTooSmallForTheStringGetsItsLengthOnly) {
    const std::uint32_t note = handle_of(*io(), "plant.note");
    ASSERT_EQ(io()->write_string(note, "four", 0, 0, 0), SCADA_OK);
    std::array<char, SMALL_BUFFER> buffer{'a', 'b', 'c'};
    std::size_t length = 0;
    scada_value meta{};
    meta.value = UNTOUCHED;
    EXPECT_EQ(io()->read_string(note, buffer, length, &meta), SCADA_E_INVALID);
    EXPECT_EQ(length, 4U);
    EXPECT_EQ(std::string_view(buffer.data(), buffer.size()), "abc");
    EXPECT_EQ(meta.value, UNTOUCHED);
}

TEST_F(TagAccess, AStringFunctionOnATagThatIsNoStringIsRefused) {
    const std::uint32_t level = handle_of(*io(), "plant.level");
    std::array<char, SCADA_STRING_MAX> buffer{};
    std::size_t length = 0;
    EXPECT_EQ(io()->read_string(level, buffer, length, nullptr), SCADA_E_TYPE);
    EXPECT_EQ(io()->write_string(level, "x", 0, 0, 0), SCADA_E_TYPE);
}

TEST_F(TagAccess, AStringWithAnInvalidQualityIsRefused) {
    const std::uint32_t note = handle_of(*io(), "plant.note");
    EXPECT_EQ(io()->write_string(note, "x", NO_QUALITY, 0, 0), SCADA_E_INVALID);
    EXPECT_EQ(io()->write_string(note, "x", 0, NO_FLAG, 0), SCADA_E_INVALID);
}

TEST_F(TagAccess, AStringOfWellFormedUtf8IsWritten) {
    const std::uint32_t note = handle_of(*io(), "plant.note");
    // One to four bytes for a character, and the characters next to the
    // surrogates.
    const std::array<std::string_view, 5> well_formed{
        "plain", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80",
        "\xED\x9F\xBF\xEE\x80\x80"};
    for (const std::string_view text : well_formed) {
        EXPECT_EQ(io()->write_string(note, text, 0, 0, 0), SCADA_OK) << text;
    }
    std::array<char, SCADA_STRING_MAX> buffer{};
    std::size_t length = 0;
    ASSERT_EQ(io()->read_string(note, buffer, length, nullptr), SCADA_OK);
    EXPECT_EQ(std::string_view(buffer.data(), length), well_formed.back());
}

TEST_F(TagAccess, AStringThatIsNotUtf8IsRefused) {
    const std::uint32_t note = handle_of(*io(), "plant.note");
    ASSERT_EQ(io()->write_string(note, "kept", 0, 0, 0), SCADA_OK);
    // A lone continuation byte, a truncated sequence, an overlong form, a
    // surrogate, a value above U+10FFFF and bytes that never occur.
    const std::array<std::string_view, 9> malformed{
        "\x80",         "a\xC3",        "\xE2\x82",         "\xC0\xAF",
        "\xE0\x80\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xFF",
        "\xF0\x9F\x98"};
    for (const std::string_view text : malformed) {
        EXPECT_EQ(io()->write_string(note, text, 0, 0, 0), SCADA_E_INVALID);
    }
    std::array<char, SCADA_STRING_MAX> buffer{};
    std::size_t length = 0;
    ASSERT_EQ(io()->read_string(note, buffer, length, nullptr), SCADA_OK);
    EXPECT_EQ(std::string_view(buffer.data(), length), "kept");
}

TEST_F(TagAccess, AStoredLengthBeyondTheSlotIsRefused) {
    const std::uint32_t note = handle_of(*web(), "plant.note");
    ASSERT_EQ(io()->write_string(handle_of(*io(), "plant.note"), "x", 0, 0, 0),
              SCADA_OK);
    const MappedRegion strings = map_file("strings.io");
    shared_at<StringSlot>(
        strings.bytes(),
        sizeof(RegionHeader) + (STRING_SLOT_NOTE * sizeof(StringSlot)))
        .len = SCADA_STRING_MAX + 1;
    std::array<char, SCADA_STRING_MAX> buffer{};
    std::size_t length = UNTOUCHED_LENGTH;
    EXPECT_EQ(web()->read_string(note, buffer, length, nullptr),
              SCADA_E_LAYOUT);
    EXPECT_EQ(length, 0U);
}

TEST_F(TagAccess, ARestartedOwnerKeepsItsValuesAndReadersTheirHandles) {
    const std::uint32_t level = handle_of(*web(), "plant.level");
    ASSERT_EQ(io()->write(handle_of(*io(), "plant.level"), level_value(LEVEL)),
              SCADA_OK);
    io().reset();
    io() = attached("io");
    ASSERT_NE(io(), nullptr);
    scada_value value{};
    ASSERT_EQ(web()->read(level, value), SCADA_OK);
    EXPECT_EQ(std::bit_cast<double>(value.value), LEVEL);
    EXPECT_EQ(value.quality, SCADA_QUALITY_GOOD);
    ASSERT_EQ(
        io()->write(handle_of(*io(), "plant.level"), level_value(OTHER_LEVEL)),
        SCADA_OK);
    ASSERT_EQ(web()->read(level, value), SCADA_OK);
    EXPECT_EQ(std::bit_cast<double>(value.value), OTHER_LEVEL);
    EXPECT_EQ(value.change_count, 2U);
}

// What one reader of a race found.
struct RaceCounts {
    std::uint64_t reads = 0;
    std::uint64_t busy = 0;
    std::uint64_t errors = 0;     // a return code neither OK nor BUSY
    std::uint64_t torn = 0;       // fields that do not belong to one write
    std::uint64_t backwards = 0;  // a value older than one read before
    bool saw_last = false;        // the last read gave the last write
};

// The fields a racing writer derives from the number `count` of its write.
scada_value derived_value(std::uint64_t count) {
    scada_value value{};
    value.type = SCADA_TYPE_FLOAT64;
    value.value = count;
    // In range: less than QUALITIES and within RACE_FLAGS.
    value.quality = static_cast<std::uint8_t>(count % QUALITIES);
    value.quality_flags = static_cast<std::uint16_t>(count & RACE_FLAGS);
    // In range: the number stays far below 2^63.
    value.source_time_ns = DEVICE_TIME + static_cast<std::int64_t>(count);
    return value;
}

// True if the fields of `value` belong to the write whose number is its
// value. That number is also the change count: every write changes the
// value.
bool is_one_write(const scada_value& value) {
    const scada_value expected = derived_value(value.value);
    return value.value == 0 ||
           (value.quality == expected.quality &&
            value.quality_flags == expected.quality_flags &&
            value.source_time_ns == expected.source_time_ns &&
            value.change_count == value.value &&
            value.type == SCADA_TYPE_FLOAT64);
}

// A reader of the value race: reads `handle` through `reader` until `done`
// is set, and once more after that.
void read_values_until(Attachment& reader, std::uint32_t handle,
                       const std::atomic<bool>& done, RaceCounts& counts) {
    std::uint64_t last = 0;
    std::int64_t last_mono = 0;
    for (bool last_round = false; !last_round;) {
        // Acquire: pairs with the release store of the writer.
        last_round = done.load(std::memory_order_acquire);
        scada_value value{};
        const int code = reader.read(handle, value);
        ++counts.reads;
        if (code != SCADA_OK) {
            ++(code == SCADA_E_BUSY ? counts.busy : counts.errors);
            continue;
        }
        if (!is_one_write(value)) {
            ++counts.torn;
        }
        if (value.value < last || value.update_mono_ns < last_mono) {
            ++counts.backwards;
        }
        last = value.value;
        last_mono = value.update_mono_ns;
    }
    counts.saw_last = last == RACE_WRITES;
}

// The text a racing writer derives from the number `count` of its write:
// `count % 251` letters, all the same.
std::string derived_text(std::uint64_t count) {
    // In range: below LETTERS.
    const char letter =
        static_cast<char>('a' + static_cast<char>(count % LETTERS));
    std::string text(count % TEXT_LENGTHS, letter);
    return text;
}

// True if `text` and `meta` belong to one write of the string race.
bool is_one_string_write(std::string_view text, const scada_value& meta) {
    if (meta.change_count == 0) {
        return true;  // not written yet
    }
    // The number of the write, from its source time.
    const auto count =
        static_cast<std::uint64_t>(meta.source_time_ns - DEVICE_TIME);
    return text == derived_text(count) &&
           meta.quality_flags == (count & RACE_FLAGS) &&
           meta.quality == count % QUALITIES;
}

// A reader of the string race: reads the text of `handle` through `reader`
// until `done` is set, and once more after that.
void read_texts_until(Attachment& reader, std::uint32_t handle,
                      const std::atomic<bool>& done, RaceCounts& counts) {
    std::array<char, SCADA_STRING_MAX> buffer{};
    for (bool last_round = false; !last_round;) {
        // Acquire: pairs with the release store of the writer.
        last_round = done.load(std::memory_order_acquire);
        std::size_t length = 0;
        scada_value meta{};
        const int code = reader.read_string(handle, buffer, length, &meta);
        ++counts.reads;
        if (code != SCADA_OK) {
            ++(code == SCADA_E_BUSY ? counts.busy : counts.errors);
            continue;
        }
        if (!is_one_string_write(std::string_view(buffer.data(), length),
                                 meta)) {
            ++counts.torn;
        }
    }
}

// No reader of a race saw anything but whole writes.
void expect_whole_writes(std::span<const RaceCounts> counts) {
    for (const RaceCounts& count : counts) {
        EXPECT_GT(count.reads, 0U);
        EXPECT_EQ(count.torn, 0U);
        EXPECT_EQ(count.backwards, 0U);
        EXPECT_EQ(count.errors, 0U);
    }
}

// Conformance test 3: values whose fields are derived from one another, a
// writer at the maximum rate, readers in other threads through the
// read-only mapping of another process and through the owner's own.
TEST_F(TagAccess, Conformance03AReaderNeverSeesFieldsOfTwoWrites) {
    const std::uint32_t own = handle_of(*io(), "plant.level");
    const std::uint32_t other = handle_of(*web(), "plant.level");
    std::atomic<bool> done{false};
    std::array<RaceCounts, READER_THREADS + 1> counts{};
    {
        std::vector<std::jthread> readers;
        readers.reserve(counts.size());
        for (std::size_t i = 0; i < READER_THREADS; ++i) {
            readers.emplace_back(read_values_until, std::ref(*web()), other,
                                 std::cref(done), std::ref(counts.at(i)));
        }
        readers.emplace_back(read_values_until, std::ref(*io()), own,
                             std::cref(done),
                             std::ref(counts.at(READER_THREADS)));
        // The owner thread.
        std::uint64_t failed = 0;
        for (std::uint64_t count = 1; count <= RACE_WRITES; ++count) {
            if (io()->write(own, derived_value(count)) != SCADA_OK) {
                ++failed;
            }
            beat_now_and_then(count);
        }
        EXPECT_EQ(failed, 0U);
        // Release: the readers load with acquire.
        done.store(true, std::memory_order_release);
    }
    expect_whole_writes(counts);
    std::uint64_t reads = 0;
    std::uint64_t busy = 0;
    for (const RaceCounts& count : counts) {
        EXPECT_TRUE(count.saw_last);
        reads += count.reads;
        busy += count.busy;
    }
    RecordProperty("reads", std::to_string(reads));
    RecordProperty("busy", std::to_string(busy));
}

// Conformance test 23: a string is never paired with the quality or times
// of another write.
TEST_F(TagAccess, Conformance23AStringIsReadWithTheQualityOfItsWrite) {
    const std::uint32_t own = handle_of(*io(), "plant.note");
    const std::uint32_t other = handle_of(*web(), "plant.note");
    std::atomic<bool> done{false};
    std::array<RaceCounts, READER_THREADS> counts{};
    {
        std::vector<std::jthread> readers;
        readers.reserve(counts.size());
        for (RaceCounts& count : counts) {
            readers.emplace_back(read_texts_until, std::ref(*web()), other,
                                 std::cref(done), std::ref(count));
        }
        std::uint64_t failed = 0;
        for (std::uint64_t count = 1; count <= STRING_RACE_WRITES; ++count) {
            const scada_value fields = derived_value(count);
            if (io()->write_string(own, derived_text(count), fields.quality,
                                   fields.quality_flags,
                                   fields.source_time_ns) != SCADA_OK) {
                ++failed;
            }
            beat_now_and_then(count);
        }
        EXPECT_EQ(failed, 0U);
        // Release: the readers load with acquire.
        done.store(true, std::memory_order_release);
    }
    expect_whole_writes(counts);
}

// A raw system call that takes this long, in ns.
constexpr std::int64_t SYSTEM_CALL_NS = 300;

TEST(ClockCheck, AClockSourceTheVdsoReadsIsNoSystemCall) {
    for (const std::string_view source :
         {"tsc", "tsc-early", "kvm-clock", "hyperv_clocksource_tsc_page",
          "arch_sys_counter"}) {
        EXPECT_FALSE(clock_reads_enter_kernel(source, 1, SYSTEM_CALL_NS))
            << source;
        EXPECT_FALSE(clock_reads_enter_kernel(
            source, SYSTEM_CALL_NS / CLOCK_READ_SPEEDUP, SYSTEM_CALL_NS))
            << source;
    }
    // A machine that does not name its clock source is judged by time.
    EXPECT_FALSE(clock_reads_enter_kernel(
        "", SYSTEM_CALL_NS / CLOCK_READ_SPEEDUP, SYSTEM_CALL_NS));
    EXPECT_TRUE(clock_reads_enter_kernel(
        "", (SYSTEM_CALL_NS / CLOCK_READ_SPEEDUP) + 1, SYSTEM_CALL_NS));
}

TEST(ClockCheck, AnotherClockSourceIsASystemCall) {
    for (const std::string_view source :
         {"hpet", "acpi_pm", "jiffies", "refined-jiffies", "tsc2"}) {
        EXPECT_TRUE(clock_reads_enter_kernel(source, 1, SYSTEM_CALL_NS))
            << source;
        EXPECT_TRUE(clock_reads_enter_kernel(source, 1, std::nullopt))
            << source;
    }
}

TEST(ClockCheck, AReadNotClearlyFasterThanTheSystemCallIsOne) {
    // The ARM timer erratum: the name stays, the vDSO falls back to the
    // system call, and on a fast core both take well under 200 ns.
    EXPECT_TRUE(clock_reads_enter_kernel("arch_sys_counter", 120, 110));
    EXPECT_TRUE(clock_reads_enter_kernel("arch_sys_counter", 60, 110));
    // A slow core: a vDSO read above 200 ns is no system call when the
    // system call takes far longer.
    EXPECT_FALSE(clock_reads_enter_kernel("arch_sys_counter", 250, 1'500));
}

TEST(ClockCheck, WithoutTheSystemCallOnlyTheNameDecides) {
    EXPECT_FALSE(clock_reads_enter_kernel("tsc", 10'000, std::nullopt));
    EXPECT_FALSE(clock_reads_enter_kernel("", 10'000, std::nullopt));
}

TEST(ClockCheck, NoTimeOverflows) {
    constexpr std::int64_t MOST = std::numeric_limits<std::int64_t>::max();
    EXPECT_TRUE(clock_reads_enter_kernel("tsc", MOST, MOST));
    EXPECT_FALSE(clock_reads_enter_kernel("tsc", 0, MOST));
}

TEST(ClockCheck, ASystemCallTimedAgainstItselfIsOne) {
    // What a machine with the vDSO clock disabled measures: the library
    // read is the system call. Takes about 0.1 ms.
    const std::optional<std::int64_t> first = time_system_call_read();
    const std::optional<std::int64_t> second = time_system_call_read();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_TRUE(clock_reads_enter_kernel("arch_sys_counter", first.value_or(0),
                                         second));
}

TEST_F(TagAccess, TheStatusReportsTheClockCheck) {
    // The status reports the result of the check at attach, kept for a
    // minute. Two attachments are not compared: each times the clocks, and
    // in an instrumented build the library read can sit near the limit
    // (D-34).
    const bool first = io()->status().clock_syscall;
    EXPECT_EQ(io()->status().clock_syscall, first);
}

}  // namespace
