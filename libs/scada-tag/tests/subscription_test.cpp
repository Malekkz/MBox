// Unit tests for the change rings as owners and consumers use them
// (contract, sections 6.1, 8 and 13): the entry a write appends,
// subscriptions with their gaps, scada_tag_notify, and wait sets. The side
// of scada-tagd is a GenerationHost in the test process; the processes are
// Attachments in it, so a consumer reads the ring through a read-only
// mapping of another "process" as a real one does.
//
// Every wait has a limit; the times compared with the 100 ms slice are
// medians, so that one late wake-up on a busy machine fails nothing.

#include "subscription.h"
#include "attach_fixture.h"
#include "attachment.h"
#include "change_ring.h"
#include "layout.h"
#include "shared_memory.h"
#include "wait_set.h"

#include "scada/common/clock.h"
#include "scada/common/mapped_region.h"
#include "scada/common/result.h"
#include "scada/tag/handle.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <poll.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
using Attached = std::unique_ptr<Attachment>;
using Sub = std::unique_ptr<Subscription>;

constexpr std::uint32_t RING = MIN_CHANGE_RING_CAPACITY;
// Tags of the process `bulk`: half the ring, so that a change of every tag
// fits twice (section 8.3).
constexpr std::size_t BULK_TAGS = RING / 2;
constexpr std::size_t MULTI_OWNERS = 10;
constexpr double LEVEL = 42.5;
constexpr std::int64_t DEVICE_TIME = 1'700'000'000'000'000'000;
// A wait that should end long before this has failed.
constexpr int WAIT_LIMIT_MS = 10'000;
constexpr std::int64_t NANOSECONDS_PER_MILLISECOND = 1'000'000;
constexpr std::int64_t SLICE_NS =
    std::int64_t{SCADA_WAIT_SLICE_MS} * NANOSECONDS_PER_MILLISECOND;
constexpr std::int64_t WAIT_LIMIT_NS =
    std::int64_t{WAIT_LIMIT_MS} * NANOSECONDS_PER_MILLISECOND;
constexpr std::size_t WAKE_TRIALS = 21;
// How long a test lets a consumer thread block before it writes.
constexpr std::chrono::milliseconds BLOCK_TIME{2};
// Writes of the test with a fast consumer, and how far the writer may run
// ahead of it: well inside the ring.
constexpr std::uint64_t FAST_WRITES = 50'000;
constexpr std::uint64_t MAX_AHEAD = RING / 2;
constexpr std::uint64_t NOTIFY_EVERY = 64;
constexpr std::uint64_t BEAT_EVERY = NOTIFY_EVERY * NOTIFY_EVERY;
constexpr std::uint8_t NO_TYPE = 9;
// Longer than the heartbeat timeout of every process of the fixture.
constexpr std::int64_t LONG_AGO_NS = 10'000 * NANOSECONDS_PER_MILLISECOND;
constexpr int ROUNDS = 3;

scada_value uint_value(std::uint64_t number,
                       std::uint8_t quality = SCADA_QUALITY_GOOD) {
    scada_value value{};
    value.type = SCADA_TYPE_UINT64;
    value.quality = quality;
    value.value = number;
    return value;
}

scada_value level_value(double level) {
    scada_value value{};
    value.type = SCADA_TYPE_FLOAT64;
    value.quality = SCADA_QUALITY_GOOD;
    value.value = std::bit_cast<std::uint64_t>(level);
    return value;
}

double level_of(const scada_change& change) {
    return std::bit_cast<double>(change.value);
}

std::string bulk_tag(std::size_t index) {
    return "bulk.t" + std::to_string(index);
}
std::string multi_owner(std::size_t index) {
    return "multi.w" + std::to_string(index);
}

// The return code of `sub.next` without waiting.
int next_code(Subscription& sub) {
    scada_change change{};
    return sub.next(change, 0);
}

// True if `sub` has a change now.
bool has_next(Subscription& sub) {
    return next_code(sub) == SCADA_OK;
}

// The next change of `sub` without waiting. Without one: a failed
// expectation, and a change of no handle.
scada_change take(Subscription& sub) {
    scada_change change{};
    const int code = sub.next(change, 0);
    if (code != SCADA_OK) {
        ADD_FAILURE() << "no change, return code " << code;
        change = scada_change{};
        change.handle = SCADA_INVALID_HANDLE;
    }
    return change;
}

// `sub` gives the levels `first` to `last`, one change each, in order.
::testing::AssertionResult takes_levels(Subscription& sub, std::uint32_t first,
                                        std::uint32_t last, double sign) {
    for (std::uint32_t i = first; i <= last; ++i) {
        scada_change change{};
        if (sub.next(change, 0) != SCADA_OK) {
            return ::testing::AssertionFailure() << "no change " << i;
        }
        if (level_of(change) != sign * i) {
            return ::testing::AssertionFailure()
                   << "change " << i << " is " << level_of(change);
        }
    }
    return ::testing::AssertionSuccess();
}

// `owner` writes the levels `first` to `last` of `handle`, multiplied by
// `sign`.
::testing::AssertionResult writes_levels(Attachment& owner,
                                         std::uint32_t handle,
                                         std::uint32_t first,
                                         std::uint32_t last, double sign) {
    for (std::uint32_t i = first; i <= last; ++i) {
        if (owner.write(handle, level_value(sign * i)) != SCADA_OK) {
            return ::testing::AssertionFailure() << "write " << i;
        }
    }
    return ::testing::AssertionSuccess();
}

// `sub` gives one change of quality `quality` for each of `handles`, in
// order, and then nothing.
::testing::AssertionResult takes_qualities(
    Subscription& sub, std::span<const std::uint32_t> handles,
    std::uint8_t quality) {
    for (const std::uint32_t handle : handles) {
        scada_change change{};
        if (sub.next(change, 0) != SCADA_OK) {
            return ::testing::AssertionFailure() << "no change of " << handle;
        }
        if (change.handle != handle || change.quality != quality) {
            return ::testing::AssertionFailure()
                   << "change of " << change.handle << " with quality "
                   << int{change.quality} << ", not of " << handle;
        }
    }
    if (has_next(sub)) {
        return ::testing::AssertionFailure() << "a change too many";
    }
    return ::testing::AssertionSuccess();
}

// Only `subs[written]` of `subs` has a change, once.
::testing::AssertionResult only_one_has_a_change(std::span<const Sub> subs,
                                                 std::size_t written) {
    for (std::size_t i = 0; i < subs.size(); ++i) {
        if (has_next(*subs[i]) != (i == written)) {
            return ::testing::AssertionFailure()
                   << "ring " << i << " after a write to ring " << written;
        }
    }
    return ::testing::AssertionSuccess();
}

// What the consumer of the test with a fast consumer found.
struct FastCounts {
    std::uint64_t out_of_order = 0;
    std::uint64_t gaps = 0;
    bool failed = false;
};

// Takes the levels 1 to FAST_WRITES from `sub` and counts what is out of
// order. `consumed` is the last level taken.
void consume_levels(Subscription& sub, std::atomic<std::uint64_t>& consumed,
                    FastCounts& counts) {
    std::uint64_t expected = 1;
    while (expected <= FAST_WRITES) {
        scada_change change{};
        const int code = sub.next(change, WAIT_LIMIT_MS);
        if (code == SCADA_E_GAP) {
            ++counts.gaps;
            continue;
        }
        if (code != SCADA_OK) {
            counts.failed = true;
            consumed.store(FAST_WRITES, std::memory_order_release);
            return;
        }
        // In range: whole numbers up to FAST_WRITES.
        const auto number = static_cast<std::uint64_t>(level_of(change));
        if (number != expected) {
            ++counts.out_of_order;
            expected = number;
        }
        ++expected;
        consumed.store(number, std::memory_order_release);
    }
}

// Writes the levels 1 to FAST_WRITES to `handle`, notifying every
// NOTIFY_EVERY writes, and waits whenever the consumer is MAX_AHEAD behind.
::testing::AssertionResult write_paced(
    Attachment& owner, std::uint32_t handle,
    const std::atomic<std::uint64_t>& consumed) {
    for (std::uint64_t i = 1; i <= FAST_WRITES; ++i) {
        while (i - consumed.load(std::memory_order_acquire) > MAX_AHEAD) {
            static_cast<void>(owner.notify());
            std::this_thread::yield();
        }
        // In range: whole numbers far below 2^53.
        if (owner.write(handle, level_value(static_cast<double>(i))) !=
            SCADA_OK) {
            return ::testing::AssertionFailure() << "write " << i;
        }
        if (i % NOTIFY_EVERY == 0 && owner.notify() != SCADA_OK) {
            return ::testing::AssertionFailure() << "notify after " << i;
        }
        if (i % BEAT_EVERY == 0) {
            static_cast<void>(owner.heartbeat());
        }
    }
    return owner.notify() == SCADA_OK
               ? ::testing::AssertionSuccess()
               : ::testing::AssertionFailure() << "the last notify";
}

class Changes : public scada::tag::testing::AttachFixture {
protected:
    // scada-tagd; `io` with one owner and the tags of AttachFixture; `bulk`
    // with BULK_TAGS tags; `multi` with MULTI_OWNERS owners of one tag
    // each; and `web` without tags, the consumer.
    [[nodiscard]] static TagSet changes_tag_set() {
        TagSet set = tag_set();
        set.queues.clear();
        set.processes.push_back({.name = "bulk", .uid = uid()});
        set.processes.push_back({.name = "multi", .uid = uid()});
        set.owners.push_back(
            {.name = "bulk", .process = "bulk", .change_ring_capacity = RING});
        for (std::size_t i = 0; i < BULK_TAGS; ++i) {
            set.tags.push_back({.name = bulk_tag(i),
                                .owner = "bulk",
                                .type = TagType::UINT64});
        }
        for (std::size_t k = 0; k < MULTI_OWNERS; ++k) {
            set.owners.push_back({.name = multi_owner(k),
                                  .process = "multi",
                                  .change_ring_capacity = RING});
            set.tags.push_back({.name = multi_owner(k) + ".t",
                                .owner = multi_owner(k),
                                .type = TagType::UINT64});
        }
        return set;
    }

    void SetUp() override {
        AttachFixture::SetUp();
        start_host(changes_tag_set());
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

    [[nodiscard]] Attached& io() { return io_; }
    [[nodiscard]] Attached& web() { return web_; }

    [[nodiscard]] static std::uint32_t handle_of(Attachment& process,
                                                 std::string_view name) {
        std::uint32_t handle = INVALID_HANDLE;
        EXPECT_EQ(process.resolve(name, handle), SCADA_OK) << name;
        return handle;
    }

    // A subscription of `consumer` to `owner`, or null with a failed
    // expectation.
    [[nodiscard]] static Sub subscribe(Attachment& consumer,
                                       std::string_view owner) {
        Sub sub;
        EXPECT_EQ(Subscription::open(consumer, owner, sub), SCADA_OK) << owner;
        return sub;
    }

    // The level of `io` written as `value`.
    void write_level(const scada_value& value) {
        ASSERT_EQ(io_->write(handle_of(*io_, "plant.level"), value), SCADA_OK);
    }

private:
    Attached io_;
    Attached web_;
};

TEST_F(Changes, AWriteThatChangesAppendsOneEntry) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    EXPECT_EQ(next_code(*sub), SCADA_E_TIMEOUT);
    write_level(level_value(LEVEL));
    EXPECT_EQ(take(*sub).handle, handle_of(*io(), "plant.level"));
    EXPECT_EQ(next_code(*sub), SCADA_E_TIMEOUT);
    EXPECT_EQ(sub->stats().received, 1U);
}

TEST_F(Changes, AWriteThatChangesNothingAppendsNoEntry) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    write_level(level_value(LEVEL));
    EXPECT_TRUE(has_next(*sub));
    // The same value, quality and flags again: no change.
    write_level(level_value(LEVEL));
    EXPECT_FALSE(has_next(*sub));
    // Another quality only is a change.
    scada_value uncertain = level_value(LEVEL);
    uncertain.quality = SCADA_QUALITY_UNCERTAIN;
    write_level(uncertain);
    EXPECT_EQ(take(*sub).quality, SCADA_QUALITY_UNCERTAIN);
    EXPECT_EQ(sub->stats().gaps, 0U);
}

TEST_F(Changes, AChangeCarriesTheFieldsOfTheRecord) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    scada_value value = level_value(LEVEL);
    value.quality = SCADA_QUALITY_UNCERTAIN;
    value.quality_flags = SCADA_FLAG_OUT_OF_RANGE;
    value.source_time_ns = DEVICE_TIME;
    write_level(value);

    const scada_change change = take(*sub);
    scada_value read{};
    ASSERT_EQ(io()->read(handle_of(*io(), "plant.level"), read), SCADA_OK);
    EXPECT_EQ(change.type, SCADA_TYPE_FLOAT64);
    EXPECT_EQ(change.quality, SCADA_QUALITY_UNCERTAIN);
    EXPECT_EQ(change.quality_flags, SCADA_FLAG_OUT_OF_RANGE);
    EXPECT_EQ(level_of(change), LEVEL);
    EXPECT_EQ(change.source_time_ns, DEVICE_TIME);
    // Section 8: the same instant as in the record.
    EXPECT_EQ(change.update_mono_ns, read.update_mono_ns);
}

// Without a device time the entry holds the source time the record got,
// and LOCAL_TIME.
TEST_F(Changes, AChangeWithoutADeviceTimeCarriesTheStoredOne) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    write_level(level_value(LEVEL));
    const scada_change change = take(*sub);
    scada_value read{};
    ASSERT_EQ(io()->read(handle_of(*io(), "plant.level"), read), SCADA_OK);
    EXPECT_EQ(change.quality_flags, SCADA_FLAG_LOCAL_TIME);
    EXPECT_EQ(change.source_time_ns, read.source_time_ns);
}

TEST_F(Changes, AStringChangeCarriesItsSlotNumber) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    const std::uint32_t note = handle_of(*io(), "plant.note");
    ASSERT_EQ(io()->write_string(note, "pump 1 tripped", SCADA_QUALITY_GOOD, 0,
                                 DEVICE_TIME),
              SCADA_OK);
    const scada_change change = take(*sub);
    EXPECT_EQ(change.handle, note);
    EXPECT_EQ(change.type, SCADA_TYPE_STRING);
    EXPECT_EQ(change.value, scada::tag::testing::STRING_SLOT_NOTE);
}

TEST_F(Changes, AStringChangesWhenItsTextChanges) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    const std::uint32_t note = handle_of(*io(), "plant.note");
    for (const std::string_view text :
         {"pump 1 tripped", "pump 1 tripped", "pump 1 running"}) {
        ASSERT_EQ(
            io()->write_string(note, text, SCADA_QUALITY_GOOD, 0, DEVICE_TIME),
            SCADA_OK);
    }
    EXPECT_TRUE(has_next(*sub));
    EXPECT_TRUE(has_next(*sub));
    EXPECT_FALSE(has_next(*sub));
}

TEST_F(Changes, WriteManyAppendsAnEntryForEachTagItChanged) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    const std::array handles{handle_of(*io(), "plant.level"),
                             handle_of(*io(), "plant.running")};
    scada_value running{};
    running.type = SCADA_TYPE_BOOL;
    running.value = 1;
    ASSERT_EQ(
        io()->write_many(handles, std::array{level_value(LEVEL), running}),
        SCADA_OK);
    const scada_change first = take(*sub);
    const scada_change second = take(*sub);
    EXPECT_EQ(first.handle, handles[0]);
    EXPECT_EQ(second.handle, handles[1]);
    // One reading of the clock for the whole call.
    EXPECT_EQ(first.update_mono_ns, second.update_mono_ns);

    // Only the level changes the second time.
    ASSERT_EQ(
        io()->write_many(handles, std::array{level_value(-LEVEL), running}),
        SCADA_OK);
    EXPECT_EQ(take(*sub).handle, handles[0]);
    EXPECT_FALSE(has_next(*sub));
}

// Conformance test 13: a consumer that keeps up sees every entry in order.
// The writer and the consumer run in two threads; the writer notifies, and
// waits when it is MAX_AHEAD entries ahead, as a ring sized for its load
// keeps a consumer from falling behind.
TEST_F(Changes, Conformance13AFastConsumerSeesEveryEntryInOrder) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    std::atomic<std::uint64_t> consumed{0};
    FastCounts counts;
    {
        const std::jthread consumer(
            [&] { consume_levels(*sub, consumed, counts); });
        EXPECT_TRUE(
            write_paced(*io(), handle_of(*io(), "plant.level"), consumed));
    }
    EXPECT_FALSE(counts.failed);
    EXPECT_EQ(counts.out_of_order, 0U);
    EXPECT_EQ(counts.gaps, 0U);
    EXPECT_EQ(sub->stats().received, FAST_WRITES);
}

// Conformance test 13: a consumer one entry less than the ring behind
// still reads every entry (section 8.2: the slot of the next write is not
// one to read from).
TEST_F(Changes, Conformance13AConsumerJustInsideTheRingGetsNoGap) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    constexpr std::uint32_t MOST_BEHIND = RING - 1;
    ASSERT_TRUE(writes_levels(*io(), handle_of(*io(), "plant.level"), 1,
                              MOST_BEHIND, 1));
    EXPECT_TRUE(takes_levels(*sub, 1, MOST_BEHIND, 1));
    EXPECT_EQ(sub->stats().gaps, 0U);
}

// Conformance test 13: a slow consumer gets SCADA_E_GAP exactly when it is
// overrun, once, and resynchronises without an old value replacing a newer
// one (section 8.3).
TEST_F(Changes, Conformance13ASlowConsumerGetsAGapExactlyWhenOverrun) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    ASSERT_TRUE(
        writes_levels(*io(), handle_of(*io(), "plant.level"), 1, RING, -1));
    EXPECT_EQ(next_code(*sub), SCADA_E_GAP);
    EXPECT_EQ(sub->stats().gaps, 1U);
    EXPECT_EQ(sub->stats().lost, RING);
    EXPECT_EQ(sub->stats().last_gap, SCADA_GAP_OVERRUN);
    // The cursor is at the end: nothing older comes afterwards.
    EXPECT_EQ(next_code(*sub), SCADA_E_TIMEOUT);

    // Resynchronise: read the record; a change delivered later is newer.
    scada_value read{};
    ASSERT_EQ(web()->read(handle_of(*web(), "plant.level"), read), SCADA_OK);
    EXPECT_EQ(std::bit_cast<double>(read.value), -static_cast<double>(RING));
    write_level(level_value(LEVEL));
    const scada_change change = take(*sub);
    EXPECT_EQ(level_of(change), LEVEL);
    EXPECT_GE(change.update_mono_ns, read.update_mono_ns);
    EXPECT_EQ(sub->stats().gaps, 1U);
}

// `owner` gives every tag of `handles` the quality `quality` in one call
// and notifies; `sub` then gets one change of each.
::testing::AssertionResult avalanche(Attachment& owner,
                                     std::span<const std::uint32_t> handles,
                                     std::uint8_t quality, Subscription& sub) {
    const std::vector<scada_value> values(handles.size(),
                                          uint_value(0, quality));
    if (owner.write_many(handles, values) != SCADA_OK ||
        owner.notify() != SCADA_OK) {
        return ::testing::AssertionFailure() << "write or notify failed";
    }
    return takes_qualities(sub, handles, quality);
}

// Conformance test 14: every tag of an owner changes quality in one cycle,
// twice; a consumer that reads after each cycle gets no gap.
TEST_F(Changes, Conformance14AnAvalancheGivesNoGapToAConsumerThatKeepsUp) {
    const Attached bulk = attached("bulk");
    ASSERT_NE(bulk, nullptr);
    const Sub sub = subscribe(*web(), "bulk");
    ASSERT_NE(sub, nullptr);
    std::vector<std::uint32_t> handles;
    for (std::size_t i = 0; i < BULK_TAGS; ++i) {
        handles.push_back(handle_of(*bulk, bulk_tag(i)));
    }
    EXPECT_TRUE(avalanche(*bulk, handles, SCADA_QUALITY_UNCERTAIN, *sub));
    EXPECT_TRUE(avalanche(*bulk, handles, SCADA_QUALITY_BAD, *sub));
    EXPECT_EQ(sub->stats().gaps, 0U);
    EXPECT_EQ(sub->stats().received, 2 * BULK_TAGS);
}

// `sub` gives SCADA_E_GAP once, then nothing, and has seen `epoch`.
::testing::AssertionResult gives_one_gap(Subscription& sub,
                                         std::uint32_t epoch) {
    const int first = next_code(sub);
    const int second = next_code(sub);
    if (first != SCADA_E_GAP || second != SCADA_E_TIMEOUT ||
        sub.stats().gaps != 1 || sub.stats().epoch != epoch ||
        sub.stats().last_gap != SCADA_GAP_RESTART) {
        return ::testing::AssertionFailure()
               << "codes " << first << " and " << second << ", "
               << sub.stats().gaps << " gaps, epoch " << sub.stats().epoch;
    }
    return ::testing::AssertionSuccess();
}

// Conformance test 7, the part of the ring: an owner that attaches again
// increments the epoch, and every consumer gets SCADA_E_GAP once.
TEST_F(Changes, Conformance07AnOwnerRestartGivesEveryConsumerOneGap) {
    const std::array subs{subscribe(*web(), "io"), subscribe(*web(), "io")};
    ASSERT_TRUE(subs[0] != nullptr && subs[1] != nullptr);
    const std::uint32_t epoch = subs[0]->stats().epoch;

    io().reset();
    io() = attached("io");
    ASSERT_NE(io(), nullptr);

    for (const Sub& sub : subs) {
        EXPECT_TRUE(gives_one_gap(*sub, epoch + 1));
    }
    // The restarted owner's writes arrive.
    write_level(level_value(LEVEL));
    EXPECT_TRUE(has_next(*subs[0]) && has_next(*subs[1]));
}

// `sub` saw the owner's process change state `changes` times, sees it down
// now if `down`, and gave the reason of that change for its last gap.
::testing::AssertionResult saw_owner(Subscription& sub, std::uint32_t changes,
                                     bool down) {
    const scada_sub_stats_t stats = sub.stats();
    std::uint8_t reason = down ? SCADA_GAP_OWNER_DOWN : SCADA_GAP_OWNER_BACK;
    if (changes == 0) {
        reason = SCADA_GAP_NONE;
    }
    if (stats.owner_changes != changes || (stats.owner_down != 0) != down ||
        stats.last_gap != reason) {
        return ::testing::AssertionFailure()
               << stats.owner_changes << " owner changes, owner_down "
               << int{stats.owner_down} << ", last gap " << int{stats.last_gap};
    }
    return ::testing::AssertionSuccess();
}

// The tag plant.level, as `reader` reads it, has OWNER_DOWN.
bool reads_as_down(Attachment& reader, std::uint32_t handle) {
    scada_value value{};
    return reader.read(handle, value) == SCADA_OK &&
           has_flag(value.quality_flags, QualityFlag::OWNER_DOWN);
}

// Issue #42, point 2: an owner that stops writes no change; the consumer of
// its ring gets a gap when it is seen down, reads the tags as OWNER_DOWN,
// and gets one more gap when the owner is back.
TEST_F(Changes, AnOwnerThatStopsGivesAGapAndAnotherWhenItIsBack) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    const std::uint32_t level = handle_of(*web(), "plant.level");
    write_level(level_value(LEVEL));
    EXPECT_TRUE(has_next(*sub));

    io().reset();  // a clean stop: down at once
    EXPECT_EQ(next_code(*sub), SCADA_E_GAP);
    EXPECT_EQ(next_code(*sub), SCADA_E_TIMEOUT);
    EXPECT_TRUE(saw_owner(*sub, 1, true));
    EXPECT_TRUE(reads_as_down(*web(), level));

    io() = attached("io");
    ASSERT_NE(io(), nullptr);
    // One gap for the return; the epoch it incremented is part of it.
    EXPECT_EQ(next_code(*sub), SCADA_E_GAP);
    EXPECT_EQ(next_code(*sub), SCADA_E_TIMEOUT);
    EXPECT_TRUE(saw_owner(*sub, 2, false));
    EXPECT_EQ(sub->stats().gaps, 2U);
}

// An owner whose heartbeat stopped may still have threads that write. Its
// changes then come with the overlay of a read: BAD and OWNER_DOWN.
TEST_F(Changes, AChangeOfAnOwnerThatIsDownComesAsBad) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    {
        const MappedRegion proc = map_file("proc.io");
        store_shared(shared_at<ProcHeader>(proc.bytes(), sizeof(RegionHeader))
                         .heartbeat_mono_ns,
                     scada::monotonic_ns() - LONG_AGO_NS,
                     std::memory_order_release);
    }
    EXPECT_EQ(next_code(*sub), SCADA_E_GAP);
    write_level(level_value(LEVEL));
    const scada_change down = take(*sub);
    EXPECT_EQ(down.quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(down.quality_flags,
              SCADA_FLAG_OWNER_DOWN | SCADA_FLAG_LOCAL_TIME);
    EXPECT_EQ(level_of(down), LEVEL);

    EXPECT_TRUE(io()->heartbeat());
    EXPECT_EQ(next_code(*sub), SCADA_E_GAP);
    write_level(level_value(-LEVEL));
    EXPECT_EQ(take(*sub).quality, SCADA_QUALITY_GOOD);
    EXPECT_TRUE(saw_owner(*sub, 2, false));
}

// An owner that is down when the subscription opens is the starting point:
// no gap until it comes up.
TEST_F(Changes, AnOwnerDownAtOpenGivesNoGapUntilItComesUp) {
    io().reset();
    const Sub after = subscribe(*web(), "io");
    ASSERT_NE(after, nullptr);
    EXPECT_TRUE(saw_owner(*after, 0, true));
    EXPECT_EQ(next_code(*after), SCADA_E_TIMEOUT);
}

// `wait` of a consumer blocked in another thread returns when the owner
// stops, without a write.
template <typename Wait>
::testing::AssertionResult wakes_when_the_owner_stops(Attached& owner,
                                                      const Wait& wait) {
    int code = SCADA_E_INVALID;
    {
        const std::jthread consumer([&] { code = wait(); });
        std::this_thread::sleep_for(BLOCK_TIME);
        owner.reset();
    }
    if (code != SCADA_OK) {
        return ::testing::AssertionFailure() << "return code " << code;
    }
    return ::testing::AssertionSuccess();
}

TEST_F(Changes, ABlockedConsumerLearnsThatTheOwnerStopped) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    EXPECT_TRUE(wakes_when_the_owner_stops(io(), [&] {
        scada_change change{};
        return sub->next(change, WAIT_LIMIT_MS) == SCADA_E_GAP
                   ? SCADA_OK
                   : SCADA_E_INVALID;
    }));
}

TEST_F(Changes, AWaitSetWakesWhenTheOwnerStops) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    WaitSet set(*web());
    ASSERT_EQ(set.add_sub(*sub), SCADA_OK);
    EXPECT_TRUE(wakes_when_the_owner_stops(
        io(), [&] { return set.wait(WAIT_LIMIT_MS); }));
    EXPECT_EQ(next_code(*sub), SCADA_E_GAP);
}

// A ring that is not there when the subscription opens is no error; once
// it is there, the consumer gets one gap and then the changes.
TEST_F(Changes, ARingThatIsMissingAtOpenGivesAGapWhenItAppears) {
    const Sub sub = subscribe(*web(), "bulk");
    ASSERT_NE(sub, nullptr);
    EXPECT_EQ(next_code(*sub), SCADA_E_TIMEOUT);

    const Attached bulk = attached("bulk");
    ASSERT_NE(bulk, nullptr);
    scada_change change{};
    // The library looks for the region again at most once a second.
    EXPECT_EQ(sub->next(change, WAIT_LIMIT_MS), SCADA_E_GAP);
    // The process of `bulk` came up with its ring: the owner's reason.
    EXPECT_EQ(sub->stats().last_gap, SCADA_GAP_OWNER_BACK);
    EXPECT_EQ(next_code(*sub), SCADA_E_TIMEOUT);
    ASSERT_EQ(bulk->write(handle_of(*bulk, bulk_tag(0)), uint_value(1)),
              SCADA_OK);
    EXPECT_EQ(take(*sub).value, 1U);
}

// A change entry is input: one that is not a valid change of a tag of the
// owner is reported, counted and skipped.
TEST_F(Changes, AnEntryThatIsNotValidIsReportedAndSkipped) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    write_level(level_value(LEVEL));
    write_level(level_value(-LEVEL));
    {
        const MappedRegion ring = map_file("changes.io");
        auto& entry =
            shared_at<ChangeEntry>(ring.bytes(), change_ring_entries_offset());
        store_shared(entry.type, NO_TYPE, std::memory_order_relaxed);
    }
    EXPECT_EQ(next_code(*sub), SCADA_E_LAYOUT);
    EXPECT_EQ(sub->stats().invalid, 1U);
    EXPECT_EQ(level_of(take(*sub)), -LEVEL);
}

TEST_F(Changes, ARingWithAnotherCapacityThanTheOwnerTableIsRefused) {
    {
        const MappedRegion ring = map_file("changes.io");
        store_shared(ring_header(ring.bytes()).capacity, 2 * RING,
                     std::memory_order_relaxed);
    }
    Sub sub;
    EXPECT_EQ(Subscription::open(*web(), "io", sub), SCADA_E_LAYOUT);
    EXPECT_EQ(sub, nullptr);
}

TEST_F(Changes, OpenRefusesAnOwnerThatIsNotThere) {
    Sub sub;
    EXPECT_EQ(Subscription::open(*web(), "nobody", sub), SCADA_E_NOT_FOUND);
    EXPECT_EQ(Subscription::open(*web(), "Not A Name", sub), SCADA_E_INVALID);
    EXPECT_EQ(Subscription::open(*web(), "", sub), SCADA_E_INVALID);
    EXPECT_EQ(sub, nullptr);
}

TEST_F(Changes, AProcessSubscribesToItsOwnRing) {
    const Sub sub = subscribe(*io(), "io");
    ASSERT_NE(sub, nullptr);
    write_level(level_value(LEVEL));
    EXPECT_TRUE(has_next(*sub));
}

TEST_F(Changes, ANegativeTimeoutIsRefused) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    scada_change change{};
    EXPECT_EQ(sub->next(change, -1), SCADA_E_INVALID);
}

// The time from a write of `handle` by `owner` to the return of a consumer
// blocked in `sub.next` in another thread, or WAIT_LIMIT_NS if it did not
// return with the change. `notify` says whether the writer notifies.
std::int64_t wake_time_ns(Attachment& owner, std::uint32_t handle,
                          std::uint64_t value, bool notify, Subscription& sub) {
    std::atomic<bool> started{false};
    std::int64_t woke = 0;
    std::int64_t written = 0;
    {
        const std::jthread consumer([&] {
            started.store(true);
            scada_change change{};
            if (sub.next(change, WAIT_LIMIT_MS) == SCADA_OK) {
                woke = scada::monotonic_ns();
            }
        });
        while (!started.load()) {
            std::this_thread::yield();
        }
        // Time for the consumer to block; if it has not, it finds the change
        // before it waits, which only makes this trial faster.
        std::this_thread::sleep_for(BLOCK_TIME);
        written = scada::monotonic_ns();
        EXPECT_EQ(owner.write(handle, uint_value(value)), SCADA_OK);
        if (notify) {
            EXPECT_EQ(owner.notify(), SCADA_OK);
        }
    }
    return woke == 0 ? WAIT_LIMIT_NS : woke - written;
}

// The median of WAKE_TRIALS times of `wake_time_ns`.
std::int64_t median_wake_ns(Attachment& owner, std::uint32_t handle,
                            bool notify, Subscription& sub) {
    std::vector<std::int64_t> times;
    for (std::size_t trial = 0; trial < WAKE_TRIALS; ++trial) {
        times.push_back(wake_time_ns(owner, handle, trial + 1, notify, sub));
    }
    std::ranges::sort(times);
    return times[times.size() / 2];
}

// Conformance test 24: with notify, a blocked consumer receives at once, in
// far less than the 100 ms slice.
TEST_F(Changes, Conformance24NotifyWakesABlockedConsumer) {
    const Attached bulk = attached("bulk");
    ASSERT_NE(bulk, nullptr);
    const Sub sub = subscribe(*web(), "bulk");
    ASSERT_NE(sub, nullptr);
    EXPECT_LT(median_wake_ns(*bulk, handle_of(*bulk, bulk_tag(0)), true, *sub),
              SLICE_NS / 4);
}

// Conformance test 24: without notify, a change still arrives within the
// 100 ms cap of a wait.
TEST_F(Changes, Conformance24WithoutNotifyAChangeArrivesWithinTheCap) {
    const Attached bulk = attached("bulk");
    ASSERT_NE(bulk, nullptr);
    const Sub sub = subscribe(*web(), "bulk");
    ASSERT_NE(sub, nullptr);
    EXPECT_LE(median_wake_ns(*bulk, handle_of(*bulk, bulk_tag(0)), false, *sub),
              2 * SLICE_NS);
}

// `set.wait` returns for a write by `owner` of `handle` with notify in
// another thread.
::testing::AssertionResult wakes_for_a_write(WaitSet& set, Attachment& owner,
                                             std::uint32_t handle,
                                             std::uint64_t value) {
    int code = SCADA_E_INVALID;
    {
        const std::jthread writer([&] {
            std::this_thread::sleep_for(BLOCK_TIME);
            EXPECT_EQ(owner.write(handle, uint_value(value)), SCADA_OK);
            EXPECT_EQ(owner.notify(), SCADA_OK);
        });
        code = set.wait(WAIT_LIMIT_MS);
    }
    if (code != SCADA_OK) {
        return ::testing::AssertionFailure() << "return code " << code;
    }
    return ::testing::AssertionSuccess();
}

// `set` wakes for a write of `handle` by `owner`, only `subs[written]` has
// a change, and then nothing is ready.
::testing::AssertionResult wakes_only_for_the_ring_written(
    WaitSet& set, Attachment& owner, std::uint32_t handle,
    std::span<const Sub> subs, std::size_t written) {
    ::testing::AssertionResult woke =
        wakes_for_a_write(set, owner, handle, written + 1);
    if (!woke) {
        return woke << " for ring " << written;
    }
    const ::testing::AssertionResult only =
        only_one_has_a_change(subs, written);
    if (!only) {
        return only;
    }
    if (set.wait(0) != SCADA_E_TIMEOUT) {
        return ::testing::AssertionFailure()
               << "still ready after ring " << written;
    }
    return ::testing::AssertionSuccess();
}

// Conformance test 24: a wait set over 10 rings wakes for any of them, and
// only the ring written has something.
TEST_F(Changes, Conformance24AWaitSetOverTenRingsWakesForAnyOfThem) {
    const Attached multi = attached("multi");
    ASSERT_NE(multi, nullptr);
    WaitSet set(*web());
    std::vector<Sub> subs;
    for (std::size_t k = 0; k < MULTI_OWNERS; ++k) {
        subs.push_back(subscribe(*web(), multi_owner(k)));
        ASSERT_TRUE(subs.back() != nullptr &&
                    set.add_sub(*subs.back()) == SCADA_OK)
            << k;
    }
    EXPECT_EQ(set.wait(0), SCADA_E_TIMEOUT);
    for (std::size_t k = 0; k < MULTI_OWNERS; ++k) {
        EXPECT_TRUE(wakes_only_for_the_ring_written(
            set, *multi, handle_of(*multi, multi_owner(k) + ".t"), subs, k));
    }
}

// `descriptor` becomes readable within the wait limit, and is read.
::testing::AssertionResult becomes_readable(int descriptor) {
    pollfd entry{.fd = descriptor, .events = POLLIN, .revents = 0};
    if (::poll(&entry, 1, WAIT_LIMIT_MS) != 1 ||
        (entry.revents & POLLIN) == 0) {
        return ::testing::AssertionFailure() << "not readable";
    }
    std::uint64_t count = 0;
    if (::read(descriptor, &count, sizeof(count)) !=
            static_cast<ssize_t>(sizeof(count)) ||
        count == 0) {
        return ::testing::AssertionFailure() << "nothing to read";
    }
    return ::testing::AssertionSuccess();
}

// True if `descriptor` is readable now.
bool readable_now(int descriptor) {
    pollfd entry{.fd = descriptor, .events = POLLIN, .revents = 0};
    return ::poll(&entry, 1, 0) == 1;
}

// `owner` writes the level `round` to `handle` and notifies; `descriptor`
// becomes readable and `sub` has that one change.
::testing::AssertionResult arrives_by_poll(Attachment& owner,
                                           std::uint32_t handle, int round,
                                           int descriptor, Subscription& sub) {
    if (owner.write(handle, level_value(round)) != SCADA_OK ||
        owner.notify() != SCADA_OK) {
        return ::testing::AssertionFailure() << "write or notify failed";
    }
    ::testing::AssertionResult readable = becomes_readable(descriptor);
    if (!readable) {
        return readable << " in round " << round;
    }
    scada_change change{};
    if (sub.next(change, 0) != SCADA_OK || level_of(change) != round ||
        has_next(sub)) {
        return ::testing::AssertionFailure() << "not one change " << round;
    }
    return ::testing::AssertionSuccess();
}

// Conformance test 24: the descriptor of a wait set works with poll.
TEST_F(Changes, Conformance24TheDescriptorOfAWaitSetWorksWithPoll) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    WaitSet set(*web());
    ASSERT_EQ(set.add_sub(*sub), SCADA_OK);
    int descriptor = -1;
    ASSERT_EQ(set.fd(descriptor), SCADA_OK);
    EXPECT_FALSE(readable_now(descriptor));

    const std::uint32_t level = handle_of(*io(), "plant.level");
    for (int round = 1; round <= ROUNDS; ++round) {
        EXPECT_TRUE(arrives_by_poll(*io(), level, round, descriptor, *sub));
    }
}

TEST_F(Changes, AWaitSetWithADescriptorTakesNoMoreSources) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    WaitSet set(*web());
    int descriptor = -1;
    ASSERT_EQ(set.fd(descriptor), SCADA_OK);
    int again = -1;
    ASSERT_EQ(set.fd(again), SCADA_OK);
    EXPECT_EQ(again, descriptor);
    EXPECT_EQ(set.add_sub(*sub), SCADA_E_INVALID);
    EXPECT_EQ(set.add_cycle(0), SCADA_E_INVALID);
}

// What was ready before the descriptor was asked for makes it readable.
TEST_F(Changes, ADescriptorIsReadableAtOnceForAChangeWrittenBefore) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    write_level(level_value(LEVEL));
    WaitSet set(*web());
    ASSERT_EQ(set.add_sub(*sub), SCADA_OK);
    int descriptor = -1;
    ASSERT_EQ(set.fd(descriptor), SCADA_OK);
    EXPECT_TRUE(becomes_readable(descriptor));
}

// `set.wait` returns for a notify of `owner` without a write.
::testing::AssertionResult wakes_for_a_cycle(WaitSet& set, Attachment& owner) {
    int code = SCADA_E_INVALID;
    {
        const std::jthread notifier([&] {
            std::this_thread::sleep_for(BLOCK_TIME);
            EXPECT_EQ(owner.notify(), SCADA_OK);
        });
        code = set.wait(WAIT_LIMIT_MS);
    }
    if (code != SCADA_OK) {
        return ::testing::AssertionFailure() << "return code " << code;
    }
    return ::testing::AssertionSuccess();
}

// Conformance test 24: a waiter on `cycle_seq` wakes on a cycle with no tag
// change.
TEST_F(Changes, Conformance24AWaiterOnTheCycleWakesWithoutATagChange) {
    const std::optional<std::uint16_t> process =
        web()->header().find_process("io");
    ASSERT_TRUE(process.has_value());
    WaitSet set(*web());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): checked by the assertion above
    ASSERT_EQ(set.add_cycle(*process), SCADA_OK);
    EXPECT_EQ(set.wait(0), SCADA_E_TIMEOUT);
    for (int cycle = 0; cycle < ROUNDS; ++cycle) {
        EXPECT_TRUE(wakes_for_a_cycle(set, *io())) << cycle;
        EXPECT_EQ(set.wait(0), SCADA_E_TIMEOUT) << cycle;
    }
}

TEST_F(Changes, AWaitSetRefusesWhatItCannotHold) {
    WaitSet set(*web());
    EXPECT_EQ(set.add_cycle(static_cast<std::uint16_t>(web()->process_count())),
              SCADA_E_INVALID);
    EXPECT_EQ(set.wait(-1), SCADA_E_INVALID);

    // A subscription is in one wait set at a time, and of the same context.
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    ASSERT_EQ(set.add_sub(*sub), SCADA_OK);
    EXPECT_EQ(set.add_sub(*sub), SCADA_E_INVALID);
    WaitSet other(*web());
    EXPECT_EQ(other.add_sub(*sub), SCADA_E_INVALID);
    WaitSet of_io(*io());
    const Sub web_sub = subscribe(*web(), "io");
    ASSERT_NE(web_sub, nullptr);
    EXPECT_EQ(of_io.add_sub(*web_sub), SCADA_E_INVALID);
}

TEST_F(Changes, AWaitSetHoldsAtMost127Sources) {
    WaitSet full(*web());
    std::size_t added = 0;
    while (added < MAX_WAIT_SET_SOURCES && full.add_cycle(0) == SCADA_OK) {
        ++added;
    }
    EXPECT_EQ(added, MAX_WAIT_SET_SOURCES);
    EXPECT_EQ(full.add_cycle(0), SCADA_E_INVALID);
}

// A subscription that is closed leaves its wait set; a wait set that is
// destroyed lets go of its subscriptions.
TEST_F(Changes, ASubscriptionAndAWaitSetCanBeDestroyedInEitherOrder) {
    auto set = std::make_unique<WaitSet>(*web());
    Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    ASSERT_EQ(set->add_sub(*sub), SCADA_OK);
    sub.reset();
    EXPECT_EQ(set->wait(0), SCADA_E_TIMEOUT);

    sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    ASSERT_EQ(set->add_sub(*sub), SCADA_OK);
    int descriptor = -1;
    ASSERT_EQ(set->fd(descriptor), SCADA_OK);
    set.reset();
    EXPECT_EQ(sub->wait_set(), nullptr);
    WaitSet again(*web());
    EXPECT_EQ(again.add_sub(*sub), SCADA_OK);
}

}  // namespace
