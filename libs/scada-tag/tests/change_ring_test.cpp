// Unit tests for the change ring protocol (contract, sections 8.1 and 8.2)
// and for the wake mechanism under it (section 8.4): a futex wait on a
// read-only shared mapping that a wake through a writable mapping of the
// same file ends. That is the experiment of issue P1-07; the result is in
// docs/decisions.md, D-32.

#include "change_ring.h"
#include "futex.h"
#include "layout.h"
#include "shared_memory.h"
#include "temp_database.h"

#include "scada/common/clock.h"
#include "scada/common/fd.h"
#include "scada/common/mapped_region.h"
#include "scada/common/result.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace scada::tag;
using scada::Fd;
using scada::MapAccess;
using scada::MappedRegion;
using scada::Result;

constexpr std::uint32_t CAPACITY = MIN_CHANGE_RING_CAPACITY;
constexpr std::int64_t NANOSECONDS_PER_MILLISECOND = 1'000'000;
// Long against any wake on a working machine; a test that waits this long
// has failed.
constexpr std::int64_t WAIT_LIMIT_NS = 10'000 * NANOSECONDS_PER_MILLISECOND;
constexpr std::int64_t SHORT_WAIT_NS = 20 * NANOSECONDS_PER_MILLISECOND;
constexpr std::size_t FILE_SIZE = 4096;
constexpr std::size_t WORDS = 10;
constexpr std::uint64_t ROUNDS_OF_THE_RING = 3;
// A cursor ahead of a ring that holds one entry.
constexpr std::uint64_t AHEAD = 5;
// Two values of a word, the one stored and another.
constexpr std::uint32_t STORED = 7;
constexpr std::uint32_t NOT_STORED = 6;

// A `changes` region of CAPACITY entries in private memory.
class Ring {
public:
    Ring() : blocks_(size() / sizeof(Block)) {
        ring_header(bytes()).capacity = CAPACITY;
    }

    [[nodiscard]] std::span<std::byte> bytes() {
        return std::as_writable_bytes(std::span<Block>(blocks_));
    }

    [[nodiscard]] static constexpr std::size_t size() {
        return change_ring_entries_offset() + (CAPACITY * sizeof(ChangeEntry));
    }

private:
    struct alignas(REGION_ALIGNMENT) Block {
        std::array<std::byte, REGION_ALIGNMENT> bytes{};
    };
    std::vector<Block> blocks_;
};

static_assert(Ring::size() % REGION_ALIGNMENT == 0);

ChangeEntry entry_of(std::uint64_t number) {
    ChangeEntry entry;
    entry.update_mono_ns = static_cast<std::int64_t>(number);
    entry.handle = static_cast<std::uint32_t>(number);
    entry.type = to_raw(TagType::UINT64);
    entry.value = number;
    entry.source_time_ns = static_cast<std::int64_t>(number) + 1;
    return entry;
}

// Appends entries `first` to `last - 1` to `ring`.
void append_entries(Ring& ring, std::uint64_t first, std::uint64_t last) {
    for (std::uint64_t i = first; i < last; ++i) {
        append_change(ring.bytes(), CAPACITY, entry_of(i));
    }
}

// Reads entries `first` to `last - 1` of `ring` from the cursor `first`,
// each whole and in order.
::testing::AssertionResult reads_entries(Ring& ring, std::uint64_t first,
                                         std::uint64_t last) {
    std::uint64_t cursor = first;
    for (std::uint64_t i = first; i < last; ++i) {
        ChangeEntry out;
        std::uint64_t written = 0;
        if (read_change(ring.bytes(), CAPACITY, cursor, out, written) !=
            RingRead::ENTRY) {
            return ::testing::AssertionFailure() << "no entry " << i;
        }
        const ChangeEntry expected = entry_of(i);
        if (out.value != i || out.handle != expected.handle ||
            out.source_time_ns != expected.source_time_ns || cursor != i + 1) {
            return ::testing::AssertionFailure() << "entry " << i << " wrong";
        }
    }
    return ::testing::AssertionSuccess();
}

// What `read_change` gives at `cursor` of `ring`, with the `write_seq` it
// reports in `written`.
RingRead read_at(Ring& ring, std::uint64_t cursor, std::uint64_t& written) {
    ChangeEntry out;
    return read_change(ring.bytes(), CAPACITY, cursor, out, written);
}

TEST(ChangeRing, EntriesComeOutInOrderAndThenNothing) {
    Ring ring;
    constexpr std::uint64_t ENTRIES = ROUNDS_OF_THE_RING * CAPACITY;
    for (std::uint64_t i = 0; i < ENTRIES; ++i) {
        append_entries(ring, i, i + 1);
        ASSERT_TRUE(reads_entries(ring, i, i + 1));
        std::uint64_t written = 0;
        ASSERT_EQ(read_at(ring, i + 1, written), RingRead::NOTHING) << i;
        ASSERT_EQ(written, i + 1);
    }
    EXPECT_EQ(ring_write_seq(ring.bytes()), ENTRIES);
}

// Conformance test 13, the part of the ring: a consumer one entry less
// than the capacity behind still reads every entry. One entry more is a
// gap (section 8.2: `w - cursor >= capacity`), because the owner may be
// writing the slot of the cursor while `write_seq` shows that position.
TEST(ChangeRing, AConsumerJustInsideTheRingReadsEveryEntry) {
    Ring ring;
    append_entries(ring, 0, CAPACITY - 1);
    EXPECT_TRUE(reads_entries(ring, 0, CAPACITY - 1));
}

TEST(ChangeRing, AGapIsReportedExactlyWhenTheCursorIsOverrun) {
    Ring overrun;
    append_entries(overrun, 0, CAPACITY);
    std::uint64_t written = 0;
    EXPECT_EQ(read_at(overrun, 0, written), RingRead::GAP);
    EXPECT_EQ(written, std::uint64_t{CAPACITY});
    // The oldest entry still there.
    EXPECT_TRUE(reads_entries(overrun, 1, CAPACITY));
}

// A `write_seq` behind the cursor is in no valid ring; it is not followed.
TEST(ChangeRing, AWriteSeqBehindTheCursorIsAGap) {
    Ring ring;
    append_change(ring.bytes(), CAPACITY, entry_of(0));
    std::uint64_t written = 0;
    EXPECT_EQ(read_at(ring, AHEAD, written), RingRead::GAP);
}

// A file in the temporary directory, mapped twice: read-write, as its owner
// has it, and read-only, as every other process has it.
class Futex : public scada::tag::testing::TempDatabase {
protected:
    void SetUp() override {
        TempDatabase::SetUp();
        const std::filesystem::path path = root() / "words";
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open has no other form
        const Fd file(::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
        ASSERT_TRUE(file.valid());
        ASSERT_EQ(::ftruncate(file.get(), FILE_SIZE), 0);
        Result<MappedRegion> writable =
            MappedRegion::map(file.get(), FILE_SIZE, MapAccess::READ_WRITE);
        Result<MappedRegion> readable =
            MappedRegion::map(file.get(), FILE_SIZE, MapAccess::READ_ONLY);
        ASSERT_TRUE(writable.ok());
        ASSERT_TRUE(readable.ok());
        writable_ = std::move(writable).value();
        readable_ = std::move(readable).value();
        ASSERT_NE(writable_.bytes().data(), readable_.bytes().data());
    }

    // Word `index` of the file as the owner and as a reader see it.
    [[nodiscard]] std::uint32_t& owner_word(std::size_t index) {
        return shared_at<std::uint32_t>(writable_.bytes(),
                                        index * sizeof(std::uint64_t));
    }
    [[nodiscard]] std::uint32_t& reader_word(std::size_t index) {
        return shared_at<std::uint32_t>(readable_.bytes(),
                                        index * sizeof(std::uint64_t));
    }

private:
    MappedRegion writable_;
    MappedRegion readable_;
};

// The experiment of issue P1-07: a waiter on the read-only mapping is woken
// by a wake on the other mapping, which another process would hold. The
// kernel keys a shared futex by file and offset, not by address.
TEST_F(Futex, AWaitOnAReadOnlyMappingIsWokenThroughAWritableOne) {
    std::atomic<bool> waiting{false};
    FutexWait result = FutexWait::FAILED;
    std::jthread waiter([&] {
        waiting.store(true);
        // The word changes before the wake, so a wait that started late
        // returns VALUE_CHANGED: either way it does not sleep to the limit.
        result = futex_wait(&reader_word(0), 0,
                            scada::monotonic_ns() + WAIT_LIMIT_NS);
    });
    while (!waiting.load()) {
        std::this_thread::yield();
    }
    const std::int64_t start = scada::monotonic_ns();
    store_shared(owner_word(0), 1U, std::memory_order_release);
    // Wakes until the waiter has returned: it may not have started its wait.
    std::atomic<bool> done{false};
    const std::jthread waker([&] {
        while (!done.load()) {
            EXPECT_TRUE(futex_wake_all(&owner_word(0)));
            std::this_thread::yield();
        }
    });
    waiter.join();
    done.store(true);
    EXPECT_TRUE(result == FutexWait::WOKEN ||
                result == FutexWait::VALUE_CHANGED);
    EXPECT_LT(scada::monotonic_ns() - start, WAIT_LIMIT_NS / 2);
    // And the value is visible through the read-only mapping.
    EXPECT_EQ(load_shared(reader_word(0), std::memory_order_acquire), 1U);
}

// A wait compares the word first: a wake that came before the wait is not
// lost, because the word changed.
TEST_F(Futex, AWaitOnAWordThatChangedReturnsAtOnce) {
    store_shared(owner_word(0), STORED, std::memory_order_release);
    EXPECT_EQ(futex_wait(&reader_word(0), NOT_STORED,
                         scada::monotonic_ns() + WAIT_LIMIT_NS),
              FutexWait::VALUE_CHANGED);
}

TEST_F(Futex, AWaitWithoutAWakeEndsAtItsDeadline) {
    const std::int64_t start = scada::monotonic_ns();
    EXPECT_EQ(futex_wait(&reader_word(0), 0, start + SHORT_WAIT_NS),
              FutexWait::TIMED_OUT);
    EXPECT_GE(scada::monotonic_ns() - start, SHORT_WAIT_NS);
    EXPECT_EQ(futex_wait(&reader_word(0), 0, start), FutexWait::TIMED_OUT);
}

// futex_waitv on words of a read-only mapping: a wake of any one of them
// ends the wait.
TEST_F(Futex, AWaitOnSeveralWordsIsWokenByAnyOfThem) {
    for (std::size_t woken = 0; woken < WORDS; ++woken) {
        std::array<FutexWaiter, WORDS> waiters{};
        for (std::size_t i = 0; i < WORDS; ++i) {
            waiters.at(i) = {.word = &reader_word(i),
                             .expected = load_shared(reader_word(i),
                                                     std::memory_order_acquire),
                             .process_private = false};
        }
        std::atomic<bool> done{false};
        FutexWait result = FutexWait::FAILED;
        std::jthread waiter([&] {
            result =
                futex_wait_any(waiters, scada::monotonic_ns() + WAIT_LIMIT_NS);
            done.store(true);
        });
        fetch_add_shared(owner_word(woken), 1U, std::memory_order_release);
        while (!done.load()) {
            EXPECT_TRUE(futex_wake_all(&owner_word(woken)));
            std::this_thread::yield();
        }
        waiter.join();
        EXPECT_TRUE(result == FutexWait::WOKEN ||
                    result == FutexWait::VALUE_CHANGED)
            << woken;
    }
}

TEST_F(Futex, AWaitOnSeveralWordsEndsAtItsDeadline) {
    std::array<FutexWaiter, 2> waiters{
        FutexWaiter{
            .word = &reader_word(0), .expected = 0, .process_private = false},
        FutexWaiter{
            .word = &reader_word(1), .expected = 0, .process_private = false}};
    EXPECT_EQ(futex_wait_any(waiters, scada::monotonic_ns() + SHORT_WAIT_NS),
              FutexWait::TIMED_OUT);
}

TEST(FutexWord, IsTheLowHalfOfACounter) {
    EXPECT_EQ(futex_word_of(0x1234'5678'9ABC'DEF0U), 0x9ABC'DEF0U);
}

}  // namespace
