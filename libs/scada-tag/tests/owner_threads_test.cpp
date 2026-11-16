// Conformance test 15 of the tag database contract (section 15): a process
// with several owner threads, each writing its own tags and posting to its
// own queue, passes tests 2 and 13. All of it runs in this one process, so
// the thread sanitizer build (the `tsan` job) sees every access of every
// thread: the owner threads, the readers, the consumers of the rings and
// of the queues, and the main thread that writes the heartbeats.
//
// The shape of test 2: readers read every tag while the owners write and
// never see the fields of two writes in one value, nor a value go back.
// The shape of test 13: a consumer that keeps up with a ring sees every
// entry in order; a consumer that does not read is overrun, gets
// SCADA_E_GAP once, and after reading the tags again sees nothing older.
// Each queue delivers every record once, in order.
//
// Every wait has a limit (DEADLINE); nothing sleeps.

#include "attach_fixture.h"

#include "scada/tag/scada_tag.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace scada::tag;
using namespace scada::tag::testing;
using Clock = std::chrono::steady_clock;

constexpr std::size_t OWNERS = 4;
constexpr std::size_t TAGS_PER_OWNER = 256;
// Every tag is written ROUNDS times: 16,384 changes per owner, 16 times
// the capacity of its ring, so that the consumer that does not read is
// overrun.
constexpr std::uint64_t ROUNDS = 64;
constexpr std::uint32_t RING = MIN_CHANGE_RING_CAPACITY;
static_assert(ROUNDS * TAGS_PER_OWNER > RING);
// A writer waits while it is this many changes ahead of the consumer that
// keeps up: that consumer is then never overrun, however slow the
// sanitizer makes it.
constexpr std::uint64_t LEAD = RING / 2;
// Small, so that the queues run full and the producers meet SCADA_E_FULL.
constexpr std::uint32_t QUEUE_CAPACITY = 8;
constexpr std::size_t READERS = 2;
constexpr std::size_t READ_BATCH = 64;
// The longest any thread waits for another; the sanitizer makes the
// threads up to twenty times slower.
constexpr std::chrono::seconds DEADLINE{120};
// The heartbeat period of the main thread: well below the default
// heartbeat timeout of 3,000 ms.
constexpr std::chrono::milliseconds BEAT_PERIOD{100};
constexpr int WAIT_MS = 100;
// Device times of the writes: BASE_TIME plus the round shifted past the
// slot, so that a value of another tag or round shows.
constexpr std::int64_t BASE_TIME = 1'700'000'000'000'000'000;
constexpr unsigned ROUND_SHIFT = 16;
constexpr std::uint8_t QUALITIES = 3;
constexpr std::uint16_t FLAG_MASK = 0x0F;

std::string owner_name(std::size_t owner) {
    return "multi.w" + std::to_string(owner);
}

std::string tag_name(std::size_t owner, std::size_t tag) {
    return owner_name(owner) + ".t" + std::to_string(tag);
}

std::string queue_name(std::size_t owner) {
    return "events." + owner_name(owner);
}

// The value of write `round` of the tag in `slot` of its owner. Every
// field is derived from the two, so a value made of two writes shows.
scada_value derived_value(std::uint64_t round, std::size_t slot) {
    scada_value value{};
    value.type = SCADA_TYPE_UINT64;
    value.value = round;
    // In range: less than QUALITIES and within FLAG_MASK.
    value.quality = static_cast<std::uint8_t>(round % QUALITIES);
    value.quality_flags = static_cast<std::uint16_t>(round & FLAG_MASK);
    // In range: both far below 2^ROUND_SHIFT and the sum far below 2^63.
    value.source_time_ns =
        BASE_TIME + static_cast<std::int64_t>((round << ROUND_SHIFT) + slot);
    return value;
}

// True if the fields of `value`, read from the tag in `slot`, belong to one
// write of that tag, or the tag was never written.
bool is_one_write(const scada_value& value, std::size_t slot) {
    if (value.value == 0) {
        return true;
    }
    const scada_value expected = derived_value(value.value, slot);
    return value.type == SCADA_TYPE_UINT64 &&
           value.quality == expected.quality &&
           value.quality_flags == expected.quality_flags &&
           value.source_time_ns == expected.source_time_ns &&
           value.change_count == value.value;
}

struct SubClose {
    void operator()(scada_sub* sub) const noexcept { scada_sub_close(sub); }
};
using Sub = std::unique_ptr<scada_sub, SubClose>;

struct QueueClose {
    void operator()(scada_queue* queue) const noexcept {
        scada_queue_close(queue);
    }
};
using Queue = std::unique_ptr<scada_queue, QueueClose>;

// Counts every thread fills in and the test reads once it is joined, but
// for `progress`, which the writer of the owner reads while it runs.
struct OwnerCounts {
    std::uint64_t write_failures = 0;
    std::uint64_t post_failures = 0;
    int post_failure = SCADA_OK;     // the code of the first one
    std::uint64_t full = 0;          // SCADA_E_FULL met by the producer
    std::uint64_t out_of_order = 0;  // ring entries not in write order
    std::uint64_t ring_errors = 0;   // gaps or errors of the fast consumer
    std::uint64_t received = 0;      // ring entries of the fast consumer
    std::uint64_t events = 0;        // queue records delivered
    std::uint64_t event_errors = 0;  // a record out of order or an error
    bool timed_out = false;
    // Ring entries the fast consumer has taken.
    std::atomic<std::uint64_t> progress{0};
};

struct ReaderCounts {
    std::uint64_t reads = 0;
    std::uint64_t busy = 0;
    std::uint64_t errors = 0;
    std::uint64_t torn = 0;
    std::uint64_t backwards = 0;
};

// Tells the main thread that a thread has finished.
class Finished {
public:
    void one_more() {
        {
            const std::scoped_lock lock(mutex_);
            ++count_;
        }
        changed_.notify_all();
    }

    // Waits up to `period` for `count` threads to have finished; true if
    // they have.
    bool wait_for(std::size_t count, std::chrono::milliseconds period) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, period, [&] { return count_ >= count; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t count_ = 0;
};

class OwnerThreadsBase : public AttachFixture {
protected:
    // The process `multi` with OWNERS owners of TAGS_PER_OWNER UINT64 tags and
    // one event queue each, consumed by `sink`, which also reads and
    // subscribes.
    [[nodiscard]] static TagSet owner_threads_tag_set() {
        TagSet set;
        set.processes = {{.name = "tagd",
                          .uid = uid(),
                          .heartbeat_timeout_ms = TAGD_TIMEOUT_MS},
                         {.name = "multi", .uid = uid()},
                         {.name = "sink", .uid = uid()}};
        for (std::size_t owner = 0; owner < OWNERS; ++owner) {
            set.owners.push_back({.name = owner_name(owner),
                                  .process = "multi",
                                  .change_ring_capacity = RING});
            set.queues.push_back({.name = queue_name(owner),
                                  .producer = "multi",
                                  .consumer = "sink",
                                  .capacity = QUEUE_CAPACITY});
            for (std::size_t tag = 0; tag < TAGS_PER_OWNER; ++tag) {
                set.tags.push_back({.name = tag_name(owner, tag),
                                    .owner = owner_name(owner),
                                    .type = TagType::UINT64});
            }
        }
        return set;
    }

    void SetUp() override {
        AttachFixture::SetUp();
        start_host(owner_threads_tag_set());
        // The consumer first: a producer that attaches before it would
        // judge it down until it looks for its region again, at most once
        // a second (section 11.2), and refuse the posts until then.
        sink_ = attach("sink");
        multi_ = attach("multi");
        ASSERT_NE(multi_, nullptr);
        ASSERT_NE(sink_, nullptr);
        for (std::size_t owner = 0; owner < OWNERS; ++owner) {
            for (std::size_t tag = 0; tag < TAGS_PER_OWNER; ++tag) {
                handles_.at(owner).at(tag) = resolve(multi_, owner, tag);
                // The same handle in every context (section 6.2); resolved
                // here so that `sink` may read the tag.
                EXPECT_EQ(resolve(sink_, owner, tag),
                          handles_.at(owner).at(tag));
            }
        }
    }

    void TearDown() override {
        scada_tag_detach(sink_);
        scada_tag_detach(multi_);
        AttachFixture::TearDown();
    }

    [[nodiscard]] scada_ctx* attach(const char* process) const {
        const std::string base_dir = base().string();
        const scada_attach_opts options{.base_dir = base_dir.c_str(),
                                        .retain_dir = nullptr,
                                        .tagd_uid = std::int64_t{uid()}};
        scada_ctx* ctx = nullptr;
        EXPECT_EQ(scada_tag_attach_opts(process, &options, &ctx), SCADA_OK)
            << process;
        return ctx;
    }

    [[nodiscard]] static std::uint32_t resolve(scada_ctx* ctx,
                                               std::size_t owner,
                                               std::size_t tag) {
        std::uint32_t handle = SCADA_INVALID_HANDLE;
        EXPECT_EQ(scada_tag_resolve(ctx, tag_name(owner, tag).c_str(), &handle),
                  SCADA_OK);
        return handle;
    }

    [[nodiscard]] scada_ctx* multi() const { return multi_; }
    [[nodiscard]] scada_ctx* sink() const { return sink_; }
    [[nodiscard]] std::span<const std::uint32_t> handles_of(
        std::size_t owner) const {
        return handles_.at(owner);
    }

private:
    scada_ctx* multi_ = nullptr;
    scada_ctx* sink_ = nullptr;
    std::array<std::array<std::uint32_t, TAGS_PER_OWNER>, OWNERS> handles_{};
};

// True while `deadline` has not passed; sets `timed_out` once it has.
bool before(Clock::time_point deadline, bool& timed_out) {
    if (Clock::now() < deadline) {
        return true;
    }
    timed_out = true;
    return false;
}

// Posts the event of `round` to `queue`, again while the queue is full.
void post_round(scada_queue* queue, std::size_t owner, std::uint64_t round,
                Clock::time_point deadline, OwnerCounts& counts) {
    scada_event_record record{};
    record.kind = SCADA_EVENT_SYSTEM;
    record.source_handle = SCADA_INVALID_HANDLE;
    record.payload_len = 2;
    // In range: both far below 256.
    record.payload[0] = static_cast<std::uint8_t>(owner);
    record.payload[1] = static_cast<std::uint8_t>(round);
    while (before(deadline, counts.timed_out)) {
        const int code = scada_queue_post(queue, &record, sizeof(record));
        if (code == SCADA_OK) {
            return;
        }
        if (code != SCADA_E_FULL) {
            if (counts.post_failures++ == 0) {
                counts.post_failure = code;
            }
            return;
        }
        ++counts.full;
        std::this_thread::yield();
    }
}

// The owner thread of `owner`: writes every tag ROUNDS times, round after
// round, notifies after each round and posts one event for it. It keeps at
// most LEAD changes ahead of the consumer that keeps up.
void write_owner(scada_ctx* ctx, std::size_t owner,
                 std::span<const std::uint32_t> handles, scada_queue* queue,
                 Clock::time_point deadline, OwnerCounts& counts) {
    std::uint64_t written = 0;
    for (std::uint64_t round = 1; round <= ROUNDS; ++round) {
        for (std::size_t slot = 0; slot < handles.size(); ++slot) {
            // Acquire: pairs with the release store of the consumer.
            while (written - counts.progress.load(std::memory_order_acquire) >=
                       LEAD &&
                   before(deadline, counts.timed_out)) {
                std::this_thread::yield();
            }
            if (counts.timed_out) {
                return;
            }
            const scada_value value = derived_value(round, slot);
            if (scada_tag_write(ctx, handles[slot], &value) != SCADA_OK) {
                ++counts.write_failures;
            }
            ++written;
        }
        static_cast<void>(scada_tag_notify(ctx));
        post_round(queue, owner, round, deadline, counts);
        static_cast<void>(scada_tag_notify(ctx));
    }
}

// The consumer of the ring of one owner that keeps up: every entry comes
// in write order, round after round and slot after slot.
void follow_ring(scada_sub* sub, std::span<const std::uint32_t> handles,
                 Clock::time_point deadline, OwnerCounts& counts) {
    const std::uint64_t expected = ROUNDS * handles.size();
    while (counts.received < expected && before(deadline, counts.timed_out)) {
        scada_change change{};
        const int code = scada_sub_next(sub, &change, WAIT_MS);
        if (code == SCADA_E_TIMEOUT) {
            continue;
        }
        if (code != SCADA_OK) {
            ++counts.ring_errors;
            continue;
        }
        const std::uint64_t round = (counts.received / handles.size()) + 1;
        const std::size_t slot = counts.received % handles.size();
        if (change.handle != handles[slot] || change.value != round) {
            ++counts.out_of_order;
        }
        ++counts.received;
        // Release: the writer loads with acquire.
        counts.progress.store(counts.received, std::memory_order_release);
    }
}

// The consumer of the queue of one owner: one record for each round, in
// order, with ids one after the other.
void follow_queue(scada_queue* queue, std::size_t owner,
                  Clock::time_point deadline, OwnerCounts& counts) {
    std::uint64_t first_seq = 0;
    while (counts.events < ROUNDS && before(deadline, counts.timed_out)) {
        scada_event_record record{};
        const int code =
            scada_queue_next(queue, &record, sizeof(record), WAIT_MS);
        if (code == SCADA_E_TIMEOUT) {
            continue;
        }
        if (code != SCADA_OK) {
            ++counts.event_errors;
            continue;
        }
        if (counts.events == 0) {
            first_seq = record.seq;
        }
        if (record.seq != first_seq + counts.events ||
            record.payload_len != 2 || record.payload[0] != owner ||
            record.payload[1] != counts.events + 1) {
            ++counts.event_errors;
        }
        ++counts.events;
        if (scada_queue_ack(queue) != SCADA_OK) {
            ++counts.event_errors;
        }
    }
}

// Judges one read of the tag in `slot`, whose value was `last` at the read
// before, and sets `last` to its value now.
void judge(int status, const scada_value& value, std::size_t slot,
           std::uint64_t& last, ReaderCounts& counts) {
    ++counts.reads;
    if (status != SCADA_OK) {
        ++(status == SCADA_E_BUSY ? counts.busy : counts.errors);
        return;
    }
    if (!is_one_write(value, slot)) {
        ++counts.torn;
    }
    if (value.value < last) {
        ++counts.backwards;
    }
    last = value.value;
}

using LastValues = std::array<std::uint64_t, TAGS_PER_OWNER>;

// Reads every tag of one owner once, READ_BATCH at a time, and judges every
// value.
void read_owner(scada_ctx* ctx, std::span<const std::uint32_t> handles,
                LastValues& last, ReaderCounts& counts) {
    std::array<scada_value, READ_BATCH> values{};
    std::array<int, READ_BATCH> status{};
    for (std::size_t first = 0; first < handles.size(); first += READ_BATCH) {
        const std::size_t count = std::min(READ_BATCH, handles.size() - first);
        static_cast<void>(
            scada_tag_read_many(ctx, handles.subspan(first, count).data(),
                                count, values.data(), status.data()));
        for (std::size_t i = 0; i < count; ++i) {
            judge(status.at(i), values.at(i), first + i, last.at(first + i),
                  counts);
        }
    }
}

using OwnerHandles = std::array<std::span<const std::uint32_t>, OWNERS>;

// One reader: reads every tag of every owner until `stop` is set.
void read_all(scada_ctx* ctx, const OwnerHandles& owners,
              const std::atomic<bool>& stop, ReaderCounts& counts) {
    std::array<LastValues, OWNERS> last{};
    // Acquire: pairs with the release store of the test.
    while (!stop.load(std::memory_order_acquire)) {
        for (std::size_t owner = 0; owner < OWNERS; ++owner) {
            read_owner(ctx, owners.at(owner), last.at(owner), counts);
        }
    }
}

// The consumer `sub` that did not read: one gap for the overrun, and the
// cursor at the end, so that no older entry follows it.
::testing::AssertionResult gives_one_overrun(scada_sub* sub) {
    scada_change change{};
    const int first = scada_sub_next(sub, &change, 0);
    const int second = scada_sub_next(sub, &change, 0);
    scada_sub_stats_t stats{};
    if (first != SCADA_E_GAP || second != SCADA_E_TIMEOUT ||
        scada_sub_stats(sub, &stats) != SCADA_OK || stats.gaps != 1 ||
        stats.last_gap != SCADA_GAP_OVERRUN || stats.lost == 0) {
        return ::testing::AssertionFailure()
               << "first=" << first << " second=" << second
               << " gaps=" << stats.gaps << " last_gap=" << int{stats.last_gap}
               << " lost=" << stats.lost;
    }
    return ::testing::AssertionSuccess();
}

// How many of the tags `handles` do not read as their last write: what the
// consumer that was overrun reads to resynchronise.
std::size_t tags_without_last_write(scada_ctx* ctx,
                                    std::span<const std::uint32_t> handles) {
    std::size_t wrong = 0;
    for (std::size_t slot = 0; slot < handles.size(); ++slot) {
        scada_value value{};
        if (scada_tag_read(ctx, handles[slot], &value) != SCADA_OK ||
            value.value != ROUNDS || !is_one_write(value, slot)) {
            ++wrong;
        }
    }
    return wrong;
}

// What one owner thread found: every write and post went through.
void expect_owner(const OwnerCounts& count) {
    EXPECT_FALSE(count.timed_out);
    EXPECT_EQ(count.write_failures, 0U);
    EXPECT_EQ(count.post_failures, 0U) << "first: " << count.post_failure;
}

// What the consumers of one owner's ring and queue found.
void expect_consumers(const OwnerCounts& count) {
    EXPECT_EQ(count.received, ROUNDS * TAGS_PER_OWNER);
    EXPECT_EQ(count.out_of_order, 0U);
    EXPECT_EQ(count.ring_errors, 0U);
    EXPECT_EQ(count.events, ROUNDS);
    EXPECT_EQ(count.event_errors, 0U);
}

// The readers saw only whole writes, each value at least as new as the one
// before. Records the reads of all of them.
void expect_readers(std::span<const ReaderCounts> reads) {
    std::uint64_t all_reads = 0;
    std::uint64_t busy = 0;
    for (const ReaderCounts& count : reads) {
        EXPECT_GT(count.reads, 0U);
        EXPECT_EQ(count.torn, 0U);
        EXPECT_EQ(count.backwards, 0U);
        EXPECT_EQ(count.errors, 0U);
        all_reads += count.reads;
        busy += count.busy;
    }
    ::testing::Test::RecordProperty("reads", std::to_string(all_reads));
    ::testing::Test::RecordProperty("busy", std::to_string(busy));
}

// The subscriptions and queue sides of the test, one of each per owner.
struct Sides {
    std::array<Sub, OWNERS> fast;
    std::array<Sub, OWNERS> slow;
    std::array<Queue, OWNERS> producers;
    std::array<Queue, OWNERS> consumers;
};

class OwnerThreads : public OwnerThreadsBase {
protected:
    // Opens every subscription and queue side; false if one did not open.
    [[nodiscard]] bool open(Sides& sides) const {
        for (std::size_t owner = 0; owner < OWNERS; ++owner) {
            const std::string name = owner_name(owner);
            const std::string queue = queue_name(owner);
            scada_sub* fast = nullptr;
            scada_sub* slow = nullptr;
            scada_queue* producer = nullptr;
            scada_queue* consumer = nullptr;
            const bool opened =
                scada_sub_open(sink(), name.c_str(), &fast) == SCADA_OK &&
                scada_sub_open(sink(), name.c_str(), &slow) == SCADA_OK &&
                scada_queue_open(multi(), queue.c_str(), &producer) ==
                    SCADA_OK &&
                scada_queue_open(sink(), queue.c_str(), &consumer) == SCADA_OK;
            sides.fast.at(owner).reset(fast);
            sides.slow.at(owner).reset(slow);
            sides.producers.at(owner).reset(producer);
            sides.consumers.at(owner).reset(consumer);
            if (!opened) {
                return false;
            }
        }
        return true;
    }

    // Starts the three threads of `owner`: its writer, which also posts,
    // the consumer of its ring that keeps up and the consumer of its queue.
    void start_owner(std::size_t owner, const Sides& sides,
                     Clock::time_point deadline, OwnerCounts& count,
                     Finished& finished, std::vector<std::jthread>& threads) {
        scada_sub* sub = sides.fast.at(owner).get();
        scada_queue* consumer = sides.consumers.at(owner).get();
        scada_queue* producer = sides.producers.at(owner).get();
        const std::span<const std::uint32_t> handles = handles_of(owner);
        threads.emplace_back([&finished, sub, handles, deadline, &count] {
            follow_ring(sub, handles, deadline, count);
            finished.one_more();
        });
        threads.emplace_back([&finished, consumer, owner, deadline, &count] {
            follow_queue(consumer, owner, deadline, count);
            finished.one_more();
        });
        threads.emplace_back(
            [this, &finished, owner, handles, producer, deadline, &count] {
                write_owner(multi(), owner, handles, producer, deadline, count);
                finished.one_more();
            });
    }

    // Runs the readers and every owner's threads, and writes the heartbeats
    // of both processes from this thread, their main loop, until every
    // owner and consumer has finished or DEADLINE has passed.
    void run(const Sides& sides, std::span<OwnerCounts> counts,
             std::span<ReaderCounts> reads) {
        OwnerHandles owners{};
        for (std::size_t owner = 0; owner < OWNERS; ++owner) {
            owners.at(owner) = handles_of(owner);
        }
        const Clock::time_point deadline = Clock::now() + DEADLINE;
        std::atomic<bool> readers_stop{false};
        Finished finished;
        std::vector<std::jthread> threads;
        threads.reserve(reads.size() + (3 * OWNERS));
        for (ReaderCounts& count : reads) {
            threads.emplace_back([this, &owners, &readers_stop, &count] {
                read_all(sink(), owners, readers_stop, count);
            });
        }
        for (std::size_t owner = 0; owner < OWNERS; ++owner) {
            start_owner(owner, sides, deadline, counts[owner], finished,
                        threads);
        }
        std::uint64_t failed_beats = 0;
        while (!finished.wait_for(3 * OWNERS, BEAT_PERIOD) &&
               Clock::now() < deadline) {
            if (scada_tag_heartbeat(multi()) != SCADA_OK ||
                scada_tag_heartbeat(sink()) != SCADA_OK) {
                ++failed_beats;
            }
        }
        EXPECT_EQ(failed_beats, 0U);
        // Release: the readers load with acquire.
        readers_stop.store(true, std::memory_order_release);
        // Every thread is joined here, before the counts are looked at.
        threads.clear();
    }
};

TEST_F(OwnerThreads, Conformance15OwnerThreadsWriteAndPostWithoutARace) {
    Sides sides;
    ASSERT_TRUE(open(sides));
    std::array<OwnerCounts, OWNERS> counts{};
    std::array<ReaderCounts, READERS> reads{};
    run(sides, counts, reads);

    std::uint64_t full = 0;
    for (std::size_t owner = 0; owner < OWNERS; ++owner) {
        SCOPED_TRACE(owner_name(owner));
        expect_owner(counts.at(owner));
        expect_consumers(counts.at(owner));
        EXPECT_TRUE(gives_one_overrun(sides.slow.at(owner).get()));
        EXPECT_EQ(tags_without_last_write(sink(), handles_of(owner)), 0U);
        full += counts.at(owner).full;
    }
    expect_readers(reads);
    RecordProperty("full", std::to_string(full));
}

}  // namespace
