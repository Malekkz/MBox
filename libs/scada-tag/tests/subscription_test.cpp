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
constexpr std::size_t WAKE_TRIALS = 21;
// Writes of the test with a fast consumer, and how far the writer may run
// ahead of it: well inside the ring.
constexpr std::uint64_t FAST_WRITES = 50'000;
constexpr std::uint64_t MAX_AHEAD = RING / 2;
constexpr std::uint64_t NOTIFY_EVERY = 64;
constexpr std::uint8_t NO_TYPE = 9;

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

std::string bulk_tag(std::size_t i) {
    return "bulk.t" + std::to_string(i);
}
std::string multi_owner(std::size_t k) {
    return "multi.w" + std::to_string(k);
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

    // The next change of `sub` without waiting, or nothing.
    [[nodiscard]] static std::optional<scada_change> next_now(
        Subscription& sub) {
        scada_change change{};
        if (sub.next(change, 0) != SCADA_OK) {
            return std::nullopt;
        }
        return change;
    }

private:
    Attached io_;
    Attached web_;
};

TEST_F(Changes, AWriteThatChangesAppendsOneEntryAndOneThatDoesNotNone) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    const std::uint32_t level = handle_of(*io(), "plant.level");
    scada_change change{};
    EXPECT_EQ(sub->next(change, 0), SCADA_E_TIMEOUT);

    ASSERT_EQ(io()->write(level, level_value(LEVEL)), SCADA_OK);
    ASSERT_EQ(sub->next(change, 0), SCADA_OK);
    EXPECT_EQ(change.handle, level);
    EXPECT_EQ(sub->next(change, 0), SCADA_E_TIMEOUT);

    // The same value, quality and flags again: no change.
    ASSERT_EQ(io()->write(level, level_value(LEVEL)), SCADA_OK);
    EXPECT_EQ(sub->next(change, 0), SCADA_E_TIMEOUT);
    // Another quality only is a change.
    scada_value uncertain = level_value(LEVEL);
    uncertain.quality = SCADA_QUALITY_UNCERTAIN;
    ASSERT_EQ(io()->write(level, uncertain), SCADA_OK);
    ASSERT_EQ(sub->next(change, 0), SCADA_OK);
    EXPECT_EQ(change.quality, SCADA_QUALITY_UNCERTAIN);
    EXPECT_EQ(sub->stats().received, 2U);
    EXPECT_EQ(sub->stats().gaps, 0U);
}

TEST_F(Changes, AChangeCarriesTheFieldsOfTheRecord) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    const std::uint32_t level = handle_of(*io(), "plant.level");
    scada_value value = level_value(LEVEL);
    value.quality = SCADA_QUALITY_UNCERTAIN;
    value.quality_flags = SCADA_FLAG_OUT_OF_RANGE;
    value.source_time_ns = DEVICE_TIME;
    ASSERT_EQ(io()->write(level, value), SCADA_OK);

    scada_change change{};
    ASSERT_EQ(sub->next(change, 0), SCADA_OK);
    scada_value read{};
    ASSERT_EQ(io()->read(level, read), SCADA_OK);
    EXPECT_EQ(change.handle, level);
    EXPECT_EQ(change.type, SCADA_TYPE_FLOAT64);
    EXPECT_EQ(change.quality, SCADA_QUALITY_UNCERTAIN);
    EXPECT_EQ(change.quality_flags, SCADA_FLAG_OUT_OF_RANGE);
    EXPECT_EQ(std::bit_cast<double>(change.value), LEVEL);
    EXPECT_EQ(change.source_time_ns, DEVICE_TIME);
    // Section 8: the same instant as in the record.
    EXPECT_EQ(change.update_mono_ns, read.update_mono_ns);

    // Without a device time: the stored source time and LOCAL_TIME.
    ASSERT_EQ(io()->write(level, level_value(-LEVEL)), SCADA_OK);
    ASSERT_EQ(sub->next(change, 0), SCADA_OK);
    ASSERT_EQ(io()->read(level, read), SCADA_OK);
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
    scada_change change{};
    ASSERT_EQ(sub->next(change, 0), SCADA_OK);
    EXPECT_EQ(change.handle, note);
    EXPECT_EQ(change.type, SCADA_TYPE_STRING);
    EXPECT_EQ(change.value, scada::tag::testing::STRING_SLOT_NOTE);
    // The same text again is no change; another text is.
    ASSERT_EQ(io()->write_string(note, "pump 1 tripped", SCADA_QUALITY_GOOD, 0,
                                 DEVICE_TIME),
              SCADA_OK);
    EXPECT_EQ(sub->next(change, 0), SCADA_E_TIMEOUT);
    ASSERT_EQ(io()->write_string(note, "pump 1 running", SCADA_QUALITY_GOOD, 0,
                                 DEVICE_TIME),
              SCADA_OK);
    EXPECT_EQ(sub->next(change, 0), SCADA_OK);
}

TEST_F(Changes, WriteManyAppendsAnEntryForEachTagItChanged) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    const std::array handles{handle_of(*io(), "plant.level"),
                             handle_of(*io(), "plant.running")};
    scada_value running{};
    running.type = SCADA_TYPE_BOOL;
    running.value = 1;
    const std::array values{level_value(LEVEL), running};
    ASSERT_EQ(io()->write_many(handles, values), SCADA_OK);
    const std::optional<scada_change> first = next_now(*sub);
    const std::optional<scada_change> second = next_now(*sub);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->handle, handles[0]);
    EXPECT_EQ(second->handle, handles[1]);
    // One reading of the clock for the whole call.
    EXPECT_EQ(first->update_mono_ns, second->update_mono_ns);

    // Only the level changes the second time.
    const std::array changed{level_value(-LEVEL), running};
    ASSERT_EQ(io()->write_many(handles, changed), SCADA_OK);
    const std::optional<scada_change> third = next_now(*sub);
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(third->handle, handles[0]);
    EXPECT_FALSE(next_now(*sub).has_value());
}

// Conformance test 13: a consumer that keeps up sees every entry in order.
// The writer and the consumer run in two threads; the writer notifies, and
// waits when it is MAX_AHEAD entries ahead, as a ring sized for its load
// keeps a consumer from falling behind.
TEST_F(Changes, Conformance13AFastConsumerSeesEveryEntryInOrder) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    const std::uint32_t level = handle_of(*io(), "plant.level");
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<bool> consumer_failed{false};
    std::uint64_t out_of_order = 0;
    std::uint64_t gaps = 0;
    std::jthread consumer([&] {
        std::uint64_t expected = 1;
        while (expected <= FAST_WRITES) {
            scada_change change{};
            const int code = sub->next(change, WAIT_LIMIT_MS);
            if (code == SCADA_E_GAP) {
                ++gaps;
                continue;
            }
            if (code != SCADA_OK) {
                consumer_failed.store(true);
                return;
            }
            // In range: whole numbers up to FAST_WRITES.
            const auto number =
                static_cast<std::uint64_t>(std::bit_cast<double>(change.value));
            if (number != expected) {
                ++out_of_order;
                expected = number;
            }
            ++expected;
            consumed.store(number, std::memory_order_release);
        }
    });
    for (std::uint64_t i = 1; i <= FAST_WRITES; ++i) {
        while (i - consumed.load(std::memory_order_acquire) > MAX_AHEAD &&
               !consumer_failed.load()) {
            ASSERT_EQ(io()->notify(), SCADA_OK);
            std::this_thread::yield();
        }
        ASSERT_EQ(io()->write(level, level_value(static_cast<double>(i))),
                  SCADA_OK);
        if (i % NOTIFY_EVERY == 0) {
            ASSERT_EQ(io()->notify(), SCADA_OK);
        }
        if (i % (NOTIFY_EVERY * NOTIFY_EVERY) == 0) {
            static_cast<void>(io()->heartbeat());
        }
    }
    ASSERT_EQ(io()->notify(), SCADA_OK);
    consumer.join();
    EXPECT_FALSE(consumer_failed.load());
    EXPECT_EQ(out_of_order, 0U);
    EXPECT_EQ(gaps, 0U);
    EXPECT_EQ(sub->stats().received, FAST_WRITES);
}

// Conformance test 13: a slow consumer gets SCADA_E_GAP exactly when it is
// overrun, once, and resynchronises without an old value replacing a newer
// one (section 8.3).
TEST_F(Changes, Conformance13ASlowConsumerGetsAGapExactlyWhenOverrun) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    const std::uint32_t level = handle_of(*io(), "plant.level");
    const std::uint32_t reader_level = handle_of(*web(), "plant.level");

    // One entry less than the ring behind: every entry is still read
    // (section 8.2; the slot of the next write is not one to read from).
    constexpr std::uint32_t MOST_BEHIND = RING - 1;
    for (std::uint32_t i = 1; i <= MOST_BEHIND; ++i) {
        ASSERT_EQ(io()->write(level, level_value(i)), SCADA_OK);
    }
    for (std::uint32_t i = 1; i <= MOST_BEHIND; ++i) {
        const std::optional<scada_change> change = next_now(*sub);
        ASSERT_TRUE(change.has_value()) << i;
        EXPECT_EQ(std::bit_cast<double>(change->value), i);
    }
    EXPECT_EQ(sub->stats().gaps, 0U);

    // One entry more: a gap, reported once.
    constexpr std::uint32_t OVER = RING;
    for (std::uint32_t i = 1; i <= OVER; ++i) {
        ASSERT_EQ(io()->write(level, level_value(-static_cast<double>(i))),
                  SCADA_OK);
    }
    scada_change change{};
    EXPECT_EQ(sub->next(change, 0), SCADA_E_GAP);
    EXPECT_EQ(sub->stats().gaps, 1U);
    EXPECT_EQ(sub->stats().lost, OVER);
    // The cursor is at the end: nothing older comes afterwards.
    EXPECT_EQ(sub->next(change, 0), SCADA_E_TIMEOUT);

    // Resynchronise: read the record; a change delivered later is newer.
    scada_value read{};
    ASSERT_EQ(web()->read(reader_level, read), SCADA_OK);
    EXPECT_EQ(std::bit_cast<double>(read.value), -static_cast<double>(OVER));
    ASSERT_EQ(io()->write(level, level_value(LEVEL)), SCADA_OK);
    ASSERT_EQ(sub->next(change, 0), SCADA_OK);
    EXPECT_EQ(std::bit_cast<double>(change.value), LEVEL);
    EXPECT_GE(change.update_mono_ns, read.update_mono_ns);
    EXPECT_EQ(sub->stats().gaps, 1U);
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
    for (const std::uint8_t quality : {std::uint8_t{SCADA_QUALITY_UNCERTAIN},
                                       std::uint8_t{SCADA_QUALITY_BAD}}) {
        const std::vector<scada_value> values(BULK_TAGS,
                                              uint_value(0, quality));
        ASSERT_EQ(bulk->write_many(handles, values), SCADA_OK);
        ASSERT_EQ(bulk->notify(), SCADA_OK);
        for (std::size_t i = 0; i < BULK_TAGS; ++i) {
            const std::optional<scada_change> change = next_now(*sub);
            ASSERT_TRUE(change.has_value()) << i;
            EXPECT_EQ(change->handle, handles[i]);
            EXPECT_EQ(change->quality, quality);
        }
        EXPECT_FALSE(next_now(*sub).has_value());
    }
    EXPECT_EQ(sub->stats().gaps, 0U);
    EXPECT_EQ(sub->stats().received, 2 * BULK_TAGS);
}

// Conformance test 7, the part of the ring: an owner that attaches again
// increments the epoch, and every consumer gets SCADA_E_GAP once.
TEST_F(Changes, Conformance07AnOwnerRestartGivesEveryConsumerOneGap) {
    const Sub first = subscribe(*web(), "io");
    const Sub second = subscribe(*web(), "io");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    const std::uint32_t epoch = first->stats().epoch;

    io().reset();
    io() = attached("io");
    ASSERT_NE(io(), nullptr);

    for (Subscription* sub : {first.get(), second.get()}) {
        scada_change change{};
        EXPECT_EQ(sub->next(change, 0), SCADA_E_GAP);
        EXPECT_EQ(sub->next(change, 0), SCADA_E_TIMEOUT);
        EXPECT_EQ(sub->stats().gaps, 1U);
        EXPECT_EQ(sub->stats().epoch, epoch + 1);
    }
    // The restarted owner's writes arrive.
    ASSERT_EQ(io()->write(handle_of(*io(), "plant.level"), level_value(LEVEL)),
              SCADA_OK);
    EXPECT_TRUE(next_now(*first).has_value());
    EXPECT_TRUE(next_now(*second).has_value());
}

// A ring that is not there when the subscription opens is no error; once
// it is there, the consumer gets one gap and then the changes.
TEST_F(Changes, ARingThatIsMissingAtOpenGivesAGapWhenItAppears) {
    const Sub sub = subscribe(*web(), "bulk");
    ASSERT_NE(sub, nullptr);
    scada_change change{};
    EXPECT_EQ(sub->next(change, 0), SCADA_E_TIMEOUT);

    const Attached bulk = attached("bulk");
    ASSERT_NE(bulk, nullptr);
    // The library looks for the region again at most once a second.
    EXPECT_EQ(sub->next(change, WAIT_LIMIT_MS), SCADA_E_GAP);
    EXPECT_EQ(sub->next(change, 0), SCADA_E_TIMEOUT);
    ASSERT_EQ(bulk->write(handle_of(*bulk, bulk_tag(0)), uint_value(1)),
              SCADA_OK);
    ASSERT_EQ(sub->next(change, 0), SCADA_OK);
    EXPECT_EQ(change.value, 1U);
}

// A change entry is input: one that is not a valid change of a tag of the
// owner is reported, counted and skipped.
TEST_F(Changes, AnEntryThatIsNotValidIsReportedAndSkipped) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    const std::uint32_t level = handle_of(*io(), "plant.level");
    ASSERT_EQ(io()->write(level, level_value(LEVEL)), SCADA_OK);
    ASSERT_EQ(io()->write(level, level_value(-LEVEL)), SCADA_OK);
    {
        const MappedRegion ring = map_file("changes.io");
        auto& entry =
            shared_at<ChangeEntry>(ring.bytes(), change_ring_entries_offset());
        store_shared(entry.type, NO_TYPE, std::memory_order_relaxed);
    }
    scada_change change{};
    EXPECT_EQ(sub->next(change, 0), SCADA_E_LAYOUT);
    EXPECT_EQ(sub->stats().invalid, 1U);
    ASSERT_EQ(sub->next(change, 0), SCADA_OK);
    EXPECT_EQ(std::bit_cast<double>(change.value), -LEVEL);
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
    ASSERT_EQ(io()->write(handle_of(*io(), "plant.level"), level_value(LEVEL)),
              SCADA_OK);
    EXPECT_TRUE(next_now(*sub).has_value());
}

TEST_F(Changes, ANegativeTimeoutIsRefused) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    scada_change change{};
    EXPECT_EQ(sub->next(change, -1), SCADA_E_INVALID);
}

// The median time from a write to the return of `wait`, a consumer blocked
// in another thread, over WAKE_TRIALS writes; `notify` says whether the
// writer calls scada_tag_notify.
template <typename Wait>
std::int64_t median_wake_ns(Attachment& owner, std::uint32_t handle,
                            bool notify, const Wait& wait) {
    std::vector<std::int64_t> times;
    for (std::size_t trial = 0; trial < WAKE_TRIALS; ++trial) {
        std::atomic<bool> started{false};
        std::int64_t woke = 0;
        std::jthread consumer([&] {
            started.store(true);
            if (wait()) {
                woke = scada::monotonic_ns();
            }
        });
        while (!started.load()) {
            std::this_thread::yield();
        }
        // Give the consumer time to block; if it has not, it finds the
        // change before it waits, which only makes this trial faster.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const std::int64_t written = scada::monotonic_ns();
        EXPECT_EQ(owner.write(handle, uint_value(trial + 1)), SCADA_OK);
        if (notify) {
            EXPECT_EQ(owner.notify(), SCADA_OK);
        }
        consumer.join();
        EXPECT_NE(woke, 0) << "trial " << trial;
        times.push_back(woke - written);
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
    const std::int64_t median =
        median_wake_ns(*bulk, handle_of(*bulk, bulk_tag(0)), true, [&] {
            scada_change change{};
            return sub->next(change, WAIT_LIMIT_MS) == SCADA_OK;
        });
    EXPECT_LT(median, SLICE_NS / 4);
}

// Conformance test 24: without notify, a change still arrives within the
// 100 ms cap of a wait.
TEST_F(Changes, Conformance24WithoutNotifyAChangeArrivesWithinTheCap) {
    const Attached bulk = attached("bulk");
    ASSERT_NE(bulk, nullptr);
    const Sub sub = subscribe(*web(), "bulk");
    ASSERT_NE(sub, nullptr);
    const std::int64_t median =
        median_wake_ns(*bulk, handle_of(*bulk, bulk_tag(0)), false, [&] {
            scada_change change{};
            return sub->next(change, WAIT_LIMIT_MS) == SCADA_OK;
        });
    EXPECT_LE(median, 2 * SLICE_NS);
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
        ASSERT_NE(subs.back(), nullptr);
        ASSERT_EQ(set.add_sub(*subs.back()), SCADA_OK);
    }
    EXPECT_EQ(set.wait(0), SCADA_E_TIMEOUT);
    for (std::size_t k = 0; k < MULTI_OWNERS; ++k) {
        const std::uint32_t handle = handle_of(*multi, multi_owner(k) + ".t");
        std::jthread writer([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            EXPECT_EQ(multi->write(handle, uint_value(k + 1)), SCADA_OK);
            EXPECT_EQ(multi->notify(), SCADA_OK);
        });
        const std::int64_t start = scada::monotonic_ns();
        ASSERT_EQ(set.wait(WAIT_LIMIT_MS), SCADA_OK) << k;
        EXPECT_LT(scada::monotonic_ns() - start,
                  WAIT_LIMIT_MS * NANOSECONDS_PER_MILLISECOND / 2);
        writer.join();
        for (std::size_t j = 0; j < MULTI_OWNERS; ++j) {
            EXPECT_EQ(next_now(*subs[j]).has_value(), j == k) << k << " " << j;
        }
        EXPECT_EQ(set.wait(0), SCADA_E_TIMEOUT);
    }
}

// Conformance test 24: the descriptor of a wait set works with poll.
TEST_F(Changes, Conformance24TheDescriptorOfAWaitSetWorksWithPoll) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    WaitSet set(*web());
    ASSERT_EQ(set.add_sub(*sub), SCADA_OK);
    int descriptor = -1;
    ASSERT_EQ(set.fd(descriptor), SCADA_OK);
    int again = -1;
    ASSERT_EQ(set.fd(again), SCADA_OK);
    EXPECT_EQ(again, descriptor);
    pollfd entry{.fd = descriptor, .events = POLLIN, .revents = 0};
    EXPECT_EQ(::poll(&entry, 1, 0), 0);

    const std::uint32_t level = handle_of(*io(), "plant.level");
    for (int round = 1; round <= 3; ++round) {
        ASSERT_EQ(io()->write(level, level_value(round)), SCADA_OK);
        ASSERT_EQ(io()->notify(), SCADA_OK);
        ASSERT_EQ(::poll(&entry, 1, WAIT_LIMIT_MS), 1) << round;
        EXPECT_NE(entry.revents & POLLIN, 0);
        std::uint64_t count = 0;
        ASSERT_EQ(::read(descriptor, &count, sizeof(count)),
                  static_cast<ssize_t>(sizeof(count)));
        EXPECT_GE(count, 1U);
        const std::optional<scada_change> change = next_now(*sub);
        ASSERT_TRUE(change.has_value());
        EXPECT_EQ(std::bit_cast<double>(change->value), round);
        EXPECT_FALSE(next_now(*sub).has_value());
    }
    // Sources cannot be added once the thread runs.
    const Sub other = subscribe(*web(), "io");
    ASSERT_NE(other, nullptr);
    EXPECT_EQ(set.add_sub(*other), SCADA_E_INVALID);
}

// What was ready before the descriptor was asked for makes it readable.
TEST_F(Changes, ADescriptorIsReadableAtOnceForAChangeWrittenBefore) {
    const Sub sub = subscribe(*web(), "io");
    ASSERT_NE(sub, nullptr);
    ASSERT_EQ(io()->write(handle_of(*io(), "plant.level"), level_value(LEVEL)),
              SCADA_OK);
    WaitSet set(*web());
    ASSERT_EQ(set.add_sub(*sub), SCADA_OK);
    int descriptor = -1;
    ASSERT_EQ(set.fd(descriptor), SCADA_OK);
    pollfd entry{.fd = descriptor, .events = POLLIN, .revents = 0};
    EXPECT_EQ(::poll(&entry, 1, WAIT_LIMIT_MS), 1);
}

// Conformance test 24: a waiter on `cycle_seq` wakes on a cycle with no tag
// change.
TEST_F(Changes, Conformance24AWaiterOnTheCycleWakesWithoutATagChange) {
    const std::optional<std::uint16_t> process =
        web()->header().find_process("io");
    ASSERT_TRUE(process.has_value());
    WaitSet set(*web());
    ASSERT_EQ(set.add_cycle(*process), SCADA_OK);
    EXPECT_EQ(set.wait(0), SCADA_E_TIMEOUT);
    for (int cycle = 0; cycle < 3; ++cycle) {
        std::jthread owner([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            EXPECT_EQ(io()->notify(), SCADA_OK);
        });
        EXPECT_EQ(set.wait(WAIT_LIMIT_MS), SCADA_OK) << cycle;
        owner.join();
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
    EXPECT_EQ(of_io.add_sub(*web_sub), SCADA_E_INVALID);

    // At most MAX_WAIT_SET_SOURCES sources.
    WaitSet full(*web());
    for (std::size_t i = 0; i < MAX_WAIT_SET_SOURCES; ++i) {
        ASSERT_EQ(full.add_cycle(0), SCADA_OK) << i;
    }
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
