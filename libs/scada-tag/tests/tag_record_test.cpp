// Unit tests for the sequence lock of a tag record (contract, sections
// 6.1, 6.2 and 7), on records in plain memory. What the library makes of a
// record it has read is tested in tag_access_test.cpp.

#include "tag_record.h"

#include "layout.h"
#include "shared_memory.h"

#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace {

using namespace scada::tag;

constexpr std::uint64_t FIRST_VALUE = 41;
constexpr std::uint64_t SECOND_VALUE = 42;
constexpr std::int64_t SOURCE_TIME = 1'700'000'000'000'000'000;
constexpr std::int64_t UPDATE_TIME = 1'700'000'000'500'000'000;
constexpr std::int64_t UPDATE_MONO = 123'456'789;
constexpr std::uint32_t LARGEST_EVEN_SEQ = 0xFFFF'FFFEU;
constexpr std::uint32_t ODD_SEQ = 5;
// What a test puts into its output to see that a failed read left it.
constexpr std::uint64_t UNTOUCHED = 0xABCD;
constexpr std::uint16_t UNTOUCHED_LENGTH = 7;
constexpr std::uint64_t STRING_SLOT = 3;

RecordUpdate update_with(std::uint64_t value) {
    RecordUpdate update;
    update.quality = to_raw(Quality::GOOD);
    update.quality_flags = to_raw(QualityFlag::TEST);
    update.value = value;
    update.source_time_ns = SOURCE_TIME;
    update.update_time_ns = UPDATE_TIME;
    update.update_mono_ns = UPDATE_MONO;
    return update;
}

std::uint32_t seq_of(TagRecord& record) {
    return load_shared(record.seq, std::memory_order_relaxed);
}

// `text` as the bytes a string slot stores.
std::array<std::uint8_t, STRING_SLOT_DATA_SIZE> stored(std::string_view text) {
    std::array<std::uint8_t, STRING_SLOT_DATA_SIZE> bytes{};
    std::ranges::transform(
        text.substr(0, bytes.size()), bytes.begin(),
        [](char character) { return static_cast<std::uint8_t>(character); });
    return bytes;
}

// Writes `text` to `slot` of `record` with `update`.
bool write_text(TagRecord& record, StringSlot& slot, std::string_view text,
                const RecordUpdate& update) {
    const std::array<std::uint8_t, STRING_SLOT_DATA_SIZE> bytes = stored(text);
    return write_string_record(record, slot,
                               std::span<const std::uint8_t>(bytes).first(
                                   std::min(text.size(), bytes.size())),
                               update);
}

// The text a read found.
std::string text_of(const StringCopy& copy) {
    std::string text;
    for (std::size_t i = 0; i < copy.len && i < copy.data.size(); ++i) {
        text.push_back(static_cast<char>(copy.data.at(i)));
    }
    return text;
}

TEST(TagRecordLock, AReadGivesWhatWasWritten) {
    TagRecord record;
    record.type = to_raw(TagType::UINT64);
    EXPECT_TRUE(write_record(record, update_with(FIRST_VALUE)));

    RecordCopy copy;
    ASSERT_TRUE(read_record(record, copy));
    EXPECT_EQ(copy.type, to_raw(TagType::UINT64));
    EXPECT_EQ(copy.quality, to_raw(Quality::GOOD));
    EXPECT_EQ(copy.quality_flags, to_raw(QualityFlag::TEST));
    EXPECT_EQ(copy.value, FIRST_VALUE);
    EXPECT_EQ(copy.source_time_ns, SOURCE_TIME);
    EXPECT_EQ(copy.update_time_ns, UPDATE_TIME);
    EXPECT_EQ(copy.update_mono_ns, UPDATE_MONO);
    EXPECT_EQ(copy.change_count, 1U);
}

TEST(TagRecordLock, AWriteAddsTwoToTheSequenceNumberAndLeavesItEven) {
    TagRecord record;
    static_cast<void>(write_record(record, update_with(FIRST_VALUE)));
    EXPECT_EQ(seq_of(record), 2U);
    static_cast<void>(write_record(record, update_with(FIRST_VALUE)));
    EXPECT_EQ(seq_of(record), 4U);
}

TEST(TagRecordLock, TheSequenceNumberWrapsAround) {
    TagRecord record;
    record.seq = LARGEST_EVEN_SEQ;
    static_cast<void>(write_record(record, update_with(FIRST_VALUE)));
    EXPECT_EQ(seq_of(record), 0U);
    RecordCopy copy;
    ASSERT_TRUE(read_record(record, copy));
    EXPECT_EQ(copy.value, FIRST_VALUE);
}

TEST(TagRecordLock, TheChangeCountCountsChangesOfValueQualityAndFlagsOnly) {
    TagRecord record;
    RecordUpdate update = update_with(FIRST_VALUE);
    EXPECT_TRUE(write_record(record, update));
    // The same again, with other times: no change.
    update.source_time_ns += 1;
    update.update_time_ns += 1;
    update.update_mono_ns += 1;
    EXPECT_FALSE(write_record(record, update));
    RecordCopy copy;
    ASSERT_TRUE(read_record(record, copy));
    EXPECT_EQ(copy.change_count, 1U);
    EXPECT_EQ(copy.update_mono_ns, UPDATE_MONO + 1);

    update.value = SECOND_VALUE;
    EXPECT_TRUE(write_record(record, update));
    update.quality = to_raw(Quality::UNCERTAIN);
    EXPECT_TRUE(write_record(record, update));
    update.quality_flags = to_raw(QualityFlag::STALE);
    EXPECT_TRUE(write_record(record, update));
    ASSERT_TRUE(read_record(record, copy));
    EXPECT_EQ(copy.change_count, 4U);
}

TEST(TagRecordLock, AReadOfARecordLeftMidWriteGivesUpAndLeavesTheOutput) {
    TagRecord record;
    static_cast<void>(write_record(record, update_with(FIRST_VALUE)));
    record.seq = ODD_SEQ;

    RecordCopy copy;
    copy.value = UNTOUCHED;
    EXPECT_FALSE(read_record(record, copy));
    EXPECT_EQ(copy.value, UNTOUCHED);

    // The owner finishes the write: the next read succeeds.
    record.seq = ODD_SEQ + 1;
    ASSERT_TRUE(read_record(record, copy));
    EXPECT_EQ(copy.value, FIRST_VALUE);
}

TEST(TagRecordLock, AStringIsReadWithTheRecordOfTheSameWrite) {
    TagRecord record;
    record.type = to_raw(TagType::STRING);
    StringSlot slot;
    EXPECT_TRUE(write_text(record, slot, "open", update_with(STRING_SLOT)));

    RecordCopy copy;
    StringCopy text;
    ASSERT_TRUE(read_string_record(record, slot, copy, text));
    EXPECT_EQ(text_of(text), "open");
    EXPECT_EQ(copy.value, STRING_SLOT);
    EXPECT_EQ(copy.change_count, 1U);
    EXPECT_EQ(seq_of(record), 2U);
}

TEST(TagRecordLock, AStringCountsAsChangedWhenItsTextDiffers) {
    TagRecord record;
    StringSlot slot;
    const RecordUpdate update = update_with(STRING_SLOT);
    EXPECT_TRUE(write_text(record, slot, "open", update));
    EXPECT_FALSE(write_text(record, slot, "open", update));
    EXPECT_TRUE(write_text(record, slot, "shut", update));  // same length
    EXPECT_TRUE(write_text(record, slot, "shu", update));   // shorter
    EXPECT_TRUE(write_text(record, slot, "", update));
    EXPECT_FALSE(write_text(record, slot, "", update));

    RecordCopy copy;
    StringCopy text;
    ASSERT_TRUE(read_string_record(record, slot, copy, text));
    EXPECT_EQ(text.len, 0U);
    EXPECT_EQ(copy.change_count, 4U);
}

TEST(TagRecordLock, AStringOfTheFullSlotIsWrittenAndRead) {
    TagRecord record;
    StringSlot slot;
    const std::string longest(STRING_SLOT_DATA_SIZE, 'x');
    EXPECT_TRUE(write_text(record, slot, longest, update_with(STRING_SLOT)));
    RecordCopy copy;
    StringCopy text;
    ASSERT_TRUE(read_string_record(record, slot, copy, text));
    EXPECT_EQ(text_of(text), longest);
}

TEST(TagRecordLock, AStringReadOfARecordLeftMidWriteLeavesTheOutput) {
    TagRecord record;
    StringSlot slot;
    static_cast<void>(
        write_text(record, slot, "open", update_with(STRING_SLOT)));
    record.seq = ODD_SEQ;

    RecordCopy copy;
    copy.value = UNTOUCHED;
    StringCopy text;
    text.len = UNTOUCHED_LENGTH;
    EXPECT_FALSE(read_string_record(record, slot, copy, text));
    EXPECT_EQ(copy.value, UNTOUCHED);
    EXPECT_EQ(text.len, UNTOUCHED_LENGTH);
}

// A length that is no length is copied as it is: the caller checks it.
TEST(TagRecordLock, AStringReadDoesNotTrustTheStoredLength) {
    TagRecord record;
    StringSlot slot;
    slot.len = STRING_SLOT_DATA_SIZE + 1;
    RecordCopy copy;
    StringCopy text;
    ASSERT_TRUE(read_string_record(record, slot, copy, text));
    EXPECT_EQ(text.len, STRING_SLOT_DATA_SIZE + 1);
}

}  // namespace
