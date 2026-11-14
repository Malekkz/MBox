// Unit tests for the header region as an attaching process reads it
// (contract, sections 5 and 11.2): what a valid header gives, and that a
// header that is not valid is refused whatever is wrong with it.

#include "header_view.h"

#include "attach_fixture.h"
#include "layout.h"
#include "shared_memory.h"
#include "tag_set_samples.h"
#include "temp_database.h"

#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/handle.h"
#include "scada/tag/header_builder.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace scada::tag;
using scada::Fd;
using scada::Result;
using scada::StatusCode;
using scada::tag::testing::AlignedImage;
using scada::tag::testing::open_directory;

constexpr std::uint64_t GENERATION = 7;
constexpr std::size_t GLOBAL = sizeof(RegionHeader);
// A layout version this library does not have.
constexpr std::uint32_t OTHER_LAYOUT_VERSION = 2;
// An offset that is not a multiple of 8.
constexpr std::uint64_t ODD_OFFSET = 196;
// The step between two offsets that are multiples of 8.
constexpr std::size_t STEP = 8;
// A multiple of 8 so large that any table added to it wraps around.
constexpr std::uint64_t WRAPPING_OFFSET = ~std::uint64_t{0} - 7;
constexpr std::uint32_t NOT_A_POWER_OF_TWO = 1025;
// A value of QueueEntry.kind that is no queue kind.
constexpr std::uint16_t NO_QUEUE_KIND = 5;
// A value of GlobalHeader.state that is no state.
constexpr std::uint32_t NO_STATE = 9;
// The number of tags of the sample tag set, and one less.
constexpr std::uint32_t SAMPLE_TAGS = 5;

// The header of `tag_set` as scada-tagd publishes it: ready and RUNNING.
std::vector<std::byte> published(const TagSet& tag_set) {
    std::string detail;
    Result<HeaderImage> image =
        build_header(tag_set, {.generation = GENERATION}, detail);
    EXPECT_TRUE(image.ok()) << detail;
    if (!image.ok()) {
        return {};
    }
    std::vector<std::byte> bytes = std::move(image).value().bytes;
    copy_to(bytes, offsetof(RegionHeader, ready), std::uint32_t{1});
    copy_to(bytes, GLOBAL + offsetof(GlobalHeader, state),
            to_raw(GenerationState::RUNNING));
    return bytes;
}

// Reads the header of the sample tag set after `damage` changed its bytes.
Result<HeaderContents> read_damaged(
    const std::function<void(std::span<std::byte>)>& damage) {
    AlignedImage image(published(samples::sample()));
    damage(image.bytes());
    std::string detail;
    Result<HeaderContents> contents =
        read_header(image.bytes(), GENERATION, detail);
    EXPECT_EQ(contents.ok(), detail.empty()) << detail;
    return contents;
}

// The GlobalHeader of the image `bytes`.
GlobalHeader global_of(std::span<std::byte> bytes) {
    return copy_from<GlobalHeader>(bytes, GLOBAL);
}

template <typename Value>
void put(std::span<std::byte> bytes, std::size_t offset, Value value) {
    copy_to(bytes, offset, value);
}

// The values of `member` of every entry of `entries`, in order.
template <typename Entry, typename Value>
std::vector<Value> column(const std::vector<Entry>& entries,
                          Value Entry::*member) {
    std::vector<Value> values;
    values.reserve(entries.size());
    for (const Entry& entry : entries) {
        values.push_back(entry.*member);
    }
    return values;
}

using Names = std::vector<std::string>;
using Numbers = std::vector<std::uint32_t>;
using Indexes = std::vector<std::uint16_t>;

TEST(ReadHeader, GivesTheCountsOfTheTagSet) {
    const Result<HeaderContents> read = read_damaged([](auto) {});
    ASSERT_TRUE(read.ok());
    const HeaderContents& contents = read.value();
    EXPECT_EQ(contents.generation, GENERATION);
    EXPECT_EQ(contents.tag_count, SAMPLE_TAGS);
    EXPECT_EQ(contents.heartbeat_check_ms, DEFAULT_HEARTBEAT_CHECK_MS);
}

TEST(ReadHeader, GivesTheProcessesOfTheTagSetInNameOrder) {
    const Result<HeaderContents> read = read_damaged([](auto) {});
    ASSERT_TRUE(read.ok());
    const HeaderContents& contents = read.value();
    EXPECT_EQ(column(contents.processes, &ProcessInfo::name),
              (Names{"control", "io", "journal"}));
    EXPECT_EQ(
        column(contents.processes, &ProcessInfo::uid),
        (Numbers{samples::UID_CONTROL, samples::UID_IO, samples::UID_JOURNAL}));
    EXPECT_EQ(column(contents.processes, &ProcessInfo::realtime),
              (std::vector<bool>{true, true, false}));
    EXPECT_EQ(
        column(contents.processes, &ProcessInfo::heartbeat_timeout_ms),
        (Numbers{samples::REALTIME_TIMEOUT_MS, samples::REALTIME_TIMEOUT_MS,
                 DEFAULT_HEARTBEAT_TIMEOUT_MS}));
}

TEST(ReadHeader, GivesTheOwnersOfTheTagSetInNameOrder) {
    const Result<HeaderContents> read = read_damaged([](auto) {});
    ASSERT_TRUE(read.ok());
    const std::vector<OwnerInfo>& owners = read.value().owners;
    EXPECT_EQ(column(owners, &OwnerInfo::name),
              (Names{"control.fast", "control.slow", "io"}));
    EXPECT_EQ(column(owners, &OwnerInfo::process_index), (Indexes{0, 0, 1}));
    EXPECT_EQ(column(owners, &OwnerInfo::tag_count), (Numbers{1, 2, 2}));
    EXPECT_EQ(column(owners, &OwnerInfo::string_slots), (Numbers{0, 1, 0}));
    EXPECT_EQ(
        column(owners, &OwnerInfo::change_ring_capacity),
        (Numbers{DEFAULT_CHANGE_RING_CAPACITY, DEFAULT_CHANGE_RING_CAPACITY,
                 samples::IO_RING_CAPACITY}));
}

TEST(ReadHeader, GivesTheQueuesOfTheTagSetInNameOrder) {
    const Result<HeaderContents> read = read_damaged([](auto) {});
    ASSERT_TRUE(read.ok());
    const std::vector<QueueInfo>& queues = read.value().queues;
    EXPECT_EQ(column(queues, &QueueInfo::name),
              (Names{"cmd.io.control.fast", "events.control.fast",
                     "result.io.control.fast"}));
    EXPECT_EQ(column(queues, &QueueInfo::kind),
              (std::vector<QueueKind>{QueueKind::CMD, QueueKind::EVENTS,
                                      QueueKind::RESULT}));
    EXPECT_EQ(column(queues, &QueueInfo::producer_process), (Indexes{0, 0, 1}));
    EXPECT_EQ(column(queues, &QueueInfo::consumer_process), (Indexes{1, 2, 0}));
    EXPECT_EQ(column(queues, &QueueInfo::capacity),
              (Numbers{DEFAULT_QUEUE_CAPACITY, samples::EVENT_QUEUE_CAPACITY,
                       DEFAULT_QUEUE_CAPACITY}));
}

TEST(ReadHeader, GivesTheSameContentsForAnyOrderOfTheTagSet) {
    AlignedImage first(published(samples::sample()));
    AlignedImage second(published(samples::reordered(samples::sample(), 3)));
    std::string detail;
    const Result<HeaderContents> one =
        read_header(first.bytes(), GENERATION, detail);
    const Result<HeaderContents> other =
        read_header(second.bytes(), GENERATION, detail);
    ASSERT_TRUE(one.ok() && other.ok()) << detail;
    EXPECT_EQ(one.value().config_hash, other.value().config_hash);
    EXPECT_EQ(one.value().owners[1].name, other.value().owners[1].name);
}

TEST(ReadHeader, AcceptsATagSetWithoutOwnersTagsAndQueues) {
    // Its empty tables start where the file ends.
    TagSet set;
    set.processes = {{.name = "p", .uid = samples::UID_OTHER}};
    AlignedImage image(published(set));
    std::string detail;
    const Result<HeaderContents> contents =
        read_header(image.bytes(), GENERATION, detail);
    ASSERT_TRUE(contents.ok()) << detail;
    EXPECT_TRUE(contents.value().owners.empty());
    EXPECT_TRUE(contents.value().queues.empty());
    EXPECT_EQ(contents.value().tag_count, 0U);
}

TEST(ReadHeader, SlotTypesFollowTheSlotTableAndTheDirectory) {
    AlignedImage image(published(samples::sample()));
    std::string detail;
    const Result<HeaderContents> contents =
        read_header(image.bytes(), GENERATION, detail);
    ASSERT_TRUE(contents.ok()) << detail;
    // control.slow: plant.pump1.label, plant.pump1.mode.
    EXPECT_EQ(slot_type(image.bytes(), contents.value(), 1, 0),
              TagType::STRING);
    EXPECT_EQ(slot_type(image.bytes(), contents.value(), 1, 1), TagType::INT64);
    // io: plant.pump1.running, plant.pump1.speed.
    EXPECT_EQ(slot_type(image.bytes(), contents.value(), 2, 0), TagType::BOOL);
    EXPECT_EQ(slot_type(image.bytes(), contents.value(), 2, 1),
              TagType::FLOAT64);
    // No such slot, no such owner.
    EXPECT_EQ(slot_type(image.bytes(), contents.value(), 2, 2), std::nullopt);
    EXPECT_EQ(slot_type(image.bytes(), contents.value(), 3, 0), std::nullopt);
}

TEST(ReadHeader, ASlotThatPointsOutsideTheDirectoryHasNoType) {
    AlignedImage image(published(samples::sample()));
    std::string detail;
    const Result<HeaderContents> contents =
        read_header(image.bytes(), GENERATION, detail);
    ASSERT_TRUE(contents.ok()) << detail;
    const std::size_t table = contents.value().owners[2].slot_table_offset;
    put(image.bytes(), table, contents.value().tag_count);
    EXPECT_EQ(slot_type(image.bytes(), contents.value(), 2, 0), std::nullopt);
    // A directory entry of another slot: its handle does not match.
    put(image.bytes(), table, std::uint32_t{0});
    put(image.bytes(), table + sizeof(std::uint32_t), std::uint32_t{0});
    EXPECT_FALSE(slot_type(image.bytes(), contents.value(), 2, 0).has_value() &&
                 slot_type(image.bytes(), contents.value(), 2, 1).has_value());
}

TEST(ReadHeader, ADirectoryEntryWithNoTypeHasNoType) {
    AlignedImage image(published(samples::sample()));
    std::string detail;
    const Result<HeaderContents> contents =
        read_header(image.bytes(), GENERATION, detail);
    ASSERT_TRUE(contents.ok()) << detail;
    for (std::size_t i = 0; i < contents.value().tag_count; ++i) {
        put(image.bytes(),
            contents.value().directory_offset + (i * sizeof(DirectoryEntry)) +
                offsetof(DirectoryEntry, type),
            std::uint8_t{0});
    }
    EXPECT_EQ(slot_type(image.bytes(), contents.value(), 2, 0), std::nullopt);
}

TEST(ReadHeader, Conformance12AnotherLayoutVersionIsRefused) {
    const Result<HeaderContents> read = read_damaged([](auto bytes) {
        put(bytes, offsetof(RegionHeader, layout_version),
            OTHER_LAYOUT_VERSION);
    });
    EXPECT_EQ(read.status().code(), StatusCode::UNSUPPORTED);
}

TEST(ReadHeader, AnotherMagicIsRefused) {
    const Result<HeaderContents> read = read_damaged([](auto bytes) {
        put(bytes, offsetof(RegionHeader, magic), REGION_MAGIC + 1);
    });
    EXPECT_EQ(read.status().code(), StatusCode::UNSUPPORTED);
}

TEST(ReadHeader, TheLayoutVersionIsJudgedBeforeReady) {
    const Result<HeaderContents> read = read_damaged([](auto bytes) {
        put(bytes, offsetof(RegionHeader, layout_version),
            OTHER_LAYOUT_VERSION);
        put(bytes, offsetof(RegionHeader, ready), std::uint32_t{0});
    });
    EXPECT_EQ(read.status().code(), StatusCode::UNSUPPORTED);
}

TEST(ReadHeader, AHeaderThatIsNotReadyIsRefused) {
    const Result<HeaderContents> read = read_damaged([](auto bytes) {
        put(bytes, offsetof(RegionHeader, ready), std::uint32_t{0});
    });
    EXPECT_EQ(read.status().code(), StatusCode::UNAVAILABLE);
}

TEST(ReadHeader, ARegionTooSmallForTheHeadersIsRefused) {
    AlignedImage image(published(samples::sample()));
    std::string detail;
    const Result<HeaderContents> contents =
        read_header(image.bytes().first(GLOBAL + sizeof(GlobalHeader) - 1),
                    GENERATION, detail);
    EXPECT_EQ(contents.status().code(), StatusCode::DATA_CORRUPT);
    const Result<HeaderContents> empty = read_header({}, GENERATION, detail);
    EXPECT_EQ(empty.status().code(), StatusCode::DATA_CORRUPT);
}

// One way to damage a header that must be refused as DATA_CORRUPT.
struct Damage {
    const char* name;
    std::function<void(std::span<std::byte>)> apply;
};

// The offset of entry `index` of a table that starts at `table`.
template <typename Entry>
std::size_t entry_at(std::uint64_t table, std::size_t index) {
    return static_cast<std::size_t>(table) + (index * sizeof(Entry));
}

const std::vector<Damage>& damages() {
    static const std::vector<Damage> DAMAGES = {
        {"another kind",
         [](auto bytes) {
             put(bytes, offsetof(RegionHeader, kind), to_raw(RegionKind::PROC));
         }},
        {"another generation",
         [](auto bytes) {
             put(bytes, offsetof(RegionHeader, generation), GENERATION + 1);
         }},
        {"another size",
         [](auto bytes) {
             put(bytes, offsetof(RegionHeader, size_bytes),
                 std::uint64_t{bytes.size() + STEP});
         }},
        {"a queue id",
         [](auto bytes) {
             put(bytes, offsetof(RegionHeader, queue_id), std::uint16_t{0});
         }},
        {"no process",
         [](auto bytes) {
             put(bytes, GLOBAL + offsetof(GlobalHeader, process_count),
                 std::uint16_t{0});
         }},
        {"too many processes",
         [](auto bytes) {
             put(bytes, GLOBAL + offsetof(GlobalHeader, process_count),
                 std::uint16_t{MAX_PROCESSES + 1});
         }},
        {"too many owners",
         [](auto bytes) {
             put(bytes, GLOBAL + offsetof(GlobalHeader, owner_count),
                 std::uint16_t{MAX_OWNERS + 1});
         }},
        {"too many queues",
         [](auto bytes) {
             put(bytes, GLOBAL + offsetof(GlobalHeader, queue_count),
                 std::uint16_t{MAX_QUEUES + 1});
         }},
        {"too many tags",
         [](auto bytes) {
             put(bytes, GLOBAL + offsetof(GlobalHeader, tag_count),
                 std::uint32_t{MAX_TAGS + 1});
         }},
        {"another tag count",
         [](auto bytes) {
             put(bytes, GLOBAL + offsetof(GlobalHeader, tag_count),
                 SAMPLE_TAGS - 1);
         }},
        {"no check interval",
         [](auto bytes) {
             put(bytes, GLOBAL + offsetof(GlobalHeader, heartbeat_check_ms),
                 std::uint32_t{0});
         }},
        {"process table outside",
         [](auto bytes) {
             put(bytes, GLOBAL + offsetof(GlobalHeader, process_table_offset),
                 std::uint64_t{bytes.size() - STEP});
         }},
        {"owner table at an odd offset",
         [](auto bytes) {
             put(bytes, GLOBAL + offsetof(GlobalHeader, owner_table_offset),
                 ODD_OFFSET);
         }},
        {"queue table beyond the end",
         [](auto bytes) {
             put(bytes, GLOBAL + offsetof(GlobalHeader, queue_table_offset),
                 std::uint64_t{bytes.size() + STEP});
         }},
        {"directory offset that wraps",
         [](auto bytes) {
             put(bytes, GLOBAL + offsetof(GlobalHeader, directory_offset),
                 WRAPPING_OFFSET);
         }},
        {"name pool larger than the region",
         [](auto bytes) {
             put(bytes, GLOBAL + offsetof(GlobalHeader, name_pool_size),
                 ~std::uint64_t{0});
         }},
        {"process name without terminator",
         [](auto bytes) {
             const std::size_t entry = entry_at<ProcessEntry>(
                 global_of(bytes).process_table_offset, 1);
             for (std::size_t i = 0; i < NAME_FIELD_SIZE; ++i) {
                 put(bytes, entry + i, 'a');
             }
         }},
        {"process name with bytes after the terminator",
         [](auto bytes) {
             const std::size_t entry = entry_at<ProcessEntry>(
                 global_of(bytes).process_table_offset, 1);
             put(bytes, entry + NAME_FIELD_SIZE - 1, 'x');
         }},
        {"process name outside the grammar",
         [](auto bytes) {
             const std::size_t entry = entry_at<ProcessEntry>(
                 global_of(bytes).process_table_offset, 1);
             put(bytes, entry, '/');
         }},
        {"process without a heartbeat timeout",
         [](auto bytes) {
             const std::size_t entry = entry_at<ProcessEntry>(
                 global_of(bytes).process_table_offset, 0);
             put(bytes, entry + offsetof(ProcessEntry, heartbeat_timeout_ms),
                 std::uint32_t{0});
         }},
        {"process with an unknown flag",
         [](auto bytes) {
             const std::size_t entry = entry_at<ProcessEntry>(
                 global_of(bytes).process_table_offset, 0);
             put(bytes, entry + offsetof(ProcessEntry, flags),
                 std::uint32_t{2});
         }},
        {"owner of a process that does not exist",
         [](auto bytes) {
             const std::size_t entry =
                 entry_at<OwnerEntry>(global_of(bytes).owner_table_offset, 0);
             put(bytes, entry + offsetof(OwnerEntry, process_index),
                 std::uint16_t{3});
         }},
        {"owner with another uid than its process",
         [](auto bytes) {
             const std::size_t entry =
                 entry_at<OwnerEntry>(global_of(bytes).owner_table_offset, 0);
             put(bytes, entry + offsetof(OwnerEntry, uid), samples::UID_IO);
         }},
        {"ring capacity that is no power of two",
         [](auto bytes) {
             const std::size_t entry =
                 entry_at<OwnerEntry>(global_of(bytes).owner_table_offset, 2);
             put(bytes, entry + offsetof(OwnerEntry, change_ring_capacity),
                 NOT_A_POWER_OF_TWO);
         }},
        {"ring capacity below the limit",
         [](auto bytes) {
             const std::size_t entry =
                 entry_at<OwnerEntry>(global_of(bytes).owner_table_offset, 2);
             put(bytes, entry + offsetof(OwnerEntry, change_ring_capacity),
                 MIN_CHANGE_RING_CAPACITY / 2);
         }},
        {"more string slots than tags",
         [](auto bytes) {
             const std::size_t entry =
                 entry_at<OwnerEntry>(global_of(bytes).owner_table_offset, 2);
             put(bytes, entry + offsetof(OwnerEntry, string_slots),
                 std::uint32_t{3});
         }},
        {"slot table outside the region",
         [](auto bytes) {
             const std::size_t entry =
                 entry_at<OwnerEntry>(global_of(bytes).owner_table_offset, 2);
             put(bytes, entry + offsetof(OwnerEntry, slot_table_offset),
                 std::uint64_t{bytes.size()});
         }},
        {"queue with another id than its position",
         [](auto bytes) {
             const std::size_t entry =
                 entry_at<QueueEntry>(global_of(bytes).queue_table_offset, 1);
             put(bytes, entry + offsetof(QueueEntry, queue_id),
                 std::uint16_t{2});
         }},
        {"queue of no kind",
         [](auto bytes) {
             const std::size_t entry =
                 entry_at<QueueEntry>(global_of(bytes).queue_table_offset, 1);
             put(bytes, entry + offsetof(QueueEntry, kind), NO_QUEUE_KIND);
         }},
        {"queue with the record size of another kind",
         [](auto bytes) {
             const std::size_t entry =
                 entry_at<QueueEntry>(global_of(bytes).queue_table_offset, 1);
             put(bytes, entry + offsetof(QueueEntry, record_size),
                 std::uint32_t{sizeof(WriteResult)});
         }},
        {"queue capacity that is no power of two",
         [](auto bytes) {
             const std::size_t entry =
                 entry_at<QueueEntry>(global_of(bytes).queue_table_offset, 1);
             put(bytes, entry + offsetof(QueueEntry, capacity),
                 std::uint32_t{0});
         }},
        {"queue with a consumer that does not exist",
         [](auto bytes) {
             const std::size_t entry =
                 entry_at<QueueEntry>(global_of(bytes).queue_table_offset, 1);
             put(bytes, entry + offsetof(QueueEntry, consumer_process),
                 std::uint16_t{3});
         }},
    };
    return DAMAGES;
}

TEST(ReadHeader, AHeaderThatIsNotValidIsRefusedWhateverIsWrong) {
    for (const Damage& damage : damages()) {
        const Result<HeaderContents> read = read_damaged(damage.apply);
        EXPECT_EQ(read.status().code(), StatusCode::DATA_CORRUPT)
            << damage.name;
    }
}

class HeaderFile : public scada::tag::testing::TempDatabase {
protected:
    // Writes `bytes` as the file `header` of the base directory, which
    // stands in for a generation directory.
    void write_header(const std::vector<std::byte>& bytes) const {
        std::ofstream file(base() / "header", std::ios::binary);
        for (const std::byte byte : bytes) {
            file.put(static_cast<char>(byte));
        }
    }

    [[nodiscard]] Result<HeaderView> open(std::uint32_t tagd_uid) {
        const Fd directory = open_directory(base());
        return HeaderView::open(directory.get(), GENERATION, tagd_uid, detail_);
    }

    [[nodiscard]] const std::string& detail() const { return detail_; }

private:
    std::string detail_;
};

TEST_F(HeaderFile, OpensAndShowsStateAndDownFlags) {
    std::vector<std::byte> bytes = published(samples::sample());
    const auto global = copy_from<GlobalHeader>(bytes, GLOBAL);
    // scada-tagd marked process 1 as down.
    copy_to(bytes,
            entry_at<ProcessEntry>(global.process_table_offset, 1) +
                offsetof(ProcessEntry, down),
            std::uint32_t{1});
    write_header(bytes);

    const Result<HeaderView> header = open(uid());
    ASSERT_TRUE(header.ok()) << detail();
    EXPECT_EQ(header.value().state(), GenerationState::RUNNING);
    EXPECT_EQ(header.value().find_process("io"), 1);
    EXPECT_EQ(header.value().find_process("journal"), 2);
    EXPECT_EQ(header.value().find_process("nobody"), std::nullopt);
    EXPECT_FALSE(header.value().marked_down(0));
    EXPECT_TRUE(header.value().marked_down(1));
    // No such process: down.
    EXPECT_TRUE(header.value().marked_down(3));
    EXPECT_EQ(header.value().slot_type(2, 1), TagType::FLOAT64);
}

TEST_F(HeaderFile, AStateThatIsNoneIsReportedAsNone) {
    std::vector<std::byte> bytes = published(samples::sample());
    copy_to(bytes, GLOBAL + offsetof(GlobalHeader, state), NO_STATE);
    write_header(bytes);
    const Result<HeaderView> header = open(uid());
    ASSERT_TRUE(header.ok()) << detail();
    EXPECT_EQ(header.value().state(), std::nullopt);
}

TEST_F(HeaderFile, Conformance09AHeaderOfAnotherUserIsRefused) {
    write_header(published(samples::sample()));
    const Result<HeaderView> header = open(other_uid());
    EXPECT_EQ(header.status().code(), StatusCode::PERMISSION_DENIED);
    EXPECT_NE(detail().find("owned by user"), std::string::npos) << detail();
}

TEST_F(HeaderFile, AMissingHeaderIsNotFound) {
    const Result<HeaderView> header = open(uid());
    EXPECT_EQ(header.status().code(), StatusCode::NOT_FOUND);
}

TEST_F(HeaderFile, Conformance09ASymbolicLinkIsNotFollowed) {
    std::ofstream(root() / "elsewhere", std::ios::binary).put('x');
    std::filesystem::create_symlink(root() / "elsewhere", base() / "header");
    const Result<HeaderView> header = open(uid());
    EXPECT_EQ(header.status().code(), StatusCode::DATA_CORRUPT);
}

TEST_F(HeaderFile, ADirectoryOrAnEmptyFileIsRefused) {
    std::filesystem::create_directory(base() / "header");
    EXPECT_FALSE(open(uid()).ok());
    std::filesystem::remove(base() / "header");
    write_header({});
    EXPECT_EQ(open(uid()).status().code(), StatusCode::DATA_CORRUPT);
}

TEST_F(HeaderFile, AHeaderOfAnotherGenerationIsRefused) {
    write_header(published(samples::sample()));
    const Fd directory = open_directory(base());
    std::string why;
    const Result<HeaderView> header =
        HeaderView::open(directory.get(), GENERATION + 1, uid(), why);
    EXPECT_EQ(header.status().code(), StatusCode::DATA_CORRUPT);
}

}  // namespace
