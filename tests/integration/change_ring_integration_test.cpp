// Integration tests for the change rings with real processes (contract,
// sections 8 and 15): a writer process notifies, and this process consumes
// its rings through read-only mappings and is woken across the process
// boundary by a futex on those mappings (decision D-32). Conformance tests
// 7 (the ring), 13 and 24.
//
// Every wait is on a condition with a limit.

#include "tag_processes_fixture.h"
#include "tagd_fixture.h"

#include "scada/tag/scada_tag.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <poll.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace scada::test::tagd;
using scada::test::ChildProcess;
using Clock = std::chrono::steady_clock;

// The writer: WRITER_OWNERS owners of the large tag set, WRITER_TAGS tags
// each, ROUNDS writes of every tag at WRITER_RATE updates per second of
// all owners together, with scada_tag_notify after each write.
constexpr std::size_t WRITER_OWNERS = 2;
constexpr std::size_t WRITER_TAGS = 100;
constexpr std::uint64_t ROUNDS = 100;
constexpr std::uint64_t WRITER_RATE = 20'000;
constexpr std::uint64_t UPDATES = ROUNDS * WRITER_OWNERS * WRITER_TAGS;
constexpr int WAIT_MS = 10'000;

// The fields the writer derives from the write count of a tag; see
// writer_main.cpp.
constexpr std::int64_t BASE_SOURCE_TIME = 1'700'000'000'000'000'000;
constexpr std::uint64_t FLAGS_MASK = 0x0FDF;
constexpr std::uint64_t QUALITIES = 3;

std::string owner_name(std::size_t owner) {
    return "load.w" + std::to_string(owner);
}

// True if `change` is whole: its fields belong to one write of the writer.
bool is_one_write(const scada_change& change) {
    const std::uint64_t count = change.value;
    return change.type == SCADA_TYPE_UINT64 &&
           change.quality == count % QUALITIES &&
           change.quality_flags == (count & FLAGS_MASK) &&
           change.source_time_ns ==
               BASE_SOURCE_TIME + static_cast<std::int64_t>(count);
}

// A subscription, closed when it goes out of scope.
class Subscribed {
public:
    Subscribed(scada_ctx* ctx, const std::string& owner) {
        EXPECT_EQ(scada_sub_open(ctx, owner.c_str(), &sub_), SCADA_OK) << owner;
    }
    ~Subscribed() { scada_sub_close(sub_); }
    Subscribed(const Subscribed&) = delete;
    Subscribed& operator=(const Subscribed&) = delete;
    Subscribed(Subscribed&&) = delete;
    Subscribed& operator=(Subscribed&&) = delete;
    [[nodiscard]] scada_sub* get() const { return sub_; }

private:
    scada_sub* sub_ = nullptr;
};

// A wait set, destroyed when it goes out of scope.
class WaitSet {
public:
    explicit WaitSet(scada_ctx* ctx) {
        EXPECT_EQ(scada_waitset_create(ctx, &set_), SCADA_OK);
    }
    ~WaitSet() { scada_waitset_destroy(set_); }
    WaitSet(const WaitSet&) = delete;
    WaitSet& operator=(const WaitSet&) = delete;
    WaitSet(WaitSet&&) = delete;
    WaitSet& operator=(WaitSet&&) = delete;
    [[nodiscard]] scada_waitset* get() const { return set_; }

private:
    scada_waitset* set_ = nullptr;
};

class ChangeRingProcesses : public TagProcesses {
protected:
    // Starts the writer on the large tag set with `mode` as its last
    // argument and waits until it has attached.
    [[nodiscard]] std::optional<ChildProcess> start_writer(
        std::uint64_t updates, std::uint64_t rate, const char* mode) const {
        std::optional<ChildProcess> writer =
            start_helper("SCADA_TAG_WRITER_COMMAND",
                         {base(), "load", std::to_string(WRITER_OWNERS),
                          std::to_string(WRITER_TAGS), std::to_string(rate),
                          std::to_string(updates), mode});
        if (writer.has_value() &&
            !writer->wait_for_output(attached, WAIT_LIMIT)) {
            ADD_FAILURE() << "the writer did not attach: " << writer->err();
            return std::nullopt;
        }
        return writer;
    }
};

// What the consumer of one ring found.
struct RingCounts {
    std::uint64_t received = 0;
    std::uint64_t torn = 0;          // fields of two writes
    std::uint64_t out_of_order = 0;  // a count that is not the last one + 1
    std::uint64_t other = 0;         // a return code but OK and TIMEOUT
};

// The consumer of the writer's rings: checks every change of every tag.
class Consumer {
public:
    // Will follow the tags `load.w<k>.t<i>` that the writer writes, as
    // `reader` resolves them.
    explicit Consumer(const Attached& reader) {
        for (std::size_t owner = 0; owner < WRITER_OWNERS; ++owner) {
            for (std::size_t tag = 0; tag < WRITER_TAGS; ++tag) {
                last_[reader.resolve(owner_name(owner) + ".t" +
                                     std::to_string(tag))] = 0;
            }
        }
    }

    // Waits on `set` and takes what `subs` deliver until every tag has
    // reached ROUNDS, or nothing comes for WAIT_MS. False if a wait failed.
    bool run(scada_waitset* set,
             std::span<const std::unique_ptr<Subscribed>> subs) {
        const Clock::time_point deadline = Clock::now() + WAIT_LIMIT;
        while (!complete() && Clock::now() < deadline) {
            const int waited = scada_waitset_wait(set, WAIT_MS);
            if (waited != SCADA_OK) {
                return waited == SCADA_E_TIMEOUT;
            }
            for (std::size_t owner = 0; owner < subs.size(); ++owner) {
                drain(subs[owner]->get(), counts_.at(owner));
            }
        }
        return true;
    }

    [[nodiscard]] bool complete() const {
        return complete_ == WRITER_OWNERS * WRITER_TAGS;
    }
    [[nodiscard]] const RingCounts& counts(std::size_t owner) const {
        return counts_.at(owner);
    }

private:
    // Takes every change `sub` has now.
    void drain(scada_sub* sub, RingCounts& count) {
        for (;;) {
            scada_change change{};
            const int code = scada_sub_next(sub, &change, 0);
            if (code == SCADA_E_TIMEOUT) {
                return;
            }
            if (code == SCADA_OK) {
                judge(change, count);
            } else if (code !=
                       SCADA_E_GAP) {  // a gap is counted by the library
                ++count.other;
            }
        }
    }

    void judge(const scada_change& change, RingCounts& count) {
        ++count.received;
        if (!is_one_write(change)) {
            ++count.torn;
        }
        const auto tag = last_.find(change.handle);
        if (tag == last_.end()) {
            ++count.other;
            return;
        }
        if (tag->second != 0 && change.value != tag->second + 1) {
            ++count.out_of_order;
        }
        tag->second = change.value;
        if (tag->second == ROUNDS) {
            ++complete_;
        }
    }

    // The count each tag had last, 0 before its first change.
    std::map<std::uint32_t, std::uint64_t> last_;
    std::size_t complete_ = 0;  // tags that have reached ROUNDS
    std::array<RingCounts, WRITER_OWNERS> counts_{};
};

// The consumer of one ring received changes, every one whole and in order,
// and `sub` saw no gap and lost nothing.
::testing::AssertionResult ring_was_clean(const RingCounts& count,
                                          scada_sub* sub) {
    if (count.received == 0 ||
        count.torn + count.out_of_order + count.other != 0) {
        return ::testing::AssertionFailure()
               << count.received << " received, " << count.torn << " torn, "
               << count.out_of_order << " out of order, " << count.other
               << " other";
    }
    scada_sub_stats_t stats{};
    if (scada_sub_stats(sub, &stats) != SCADA_OK) {
        return ::testing::AssertionFailure() << "no statistics";
    }
    if (stats.gaps != 0 || stats.lost != 0) {
        return ::testing::AssertionFailure()
               << stats.gaps << " gaps, " << stats.lost << " lost";
    }
    return ::testing::AssertionSuccess();
}

// Conformance test 13 across processes: a consumer that keeps up sees
// every entry of every ring, whole and in order, woken by the notify of
// the writer through a wait set. The consumer subscribes once the writer
// has attached, so it may miss the first writes; from the first change of
// a tag on it must see every one.
TEST_F(ChangeRingProcesses, Conformance13AFastConsumerSeesEveryEntryInOrder) {
    write_load_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    std::optional<ChildProcess> writer =
        start_writer(UPDATES, WRITER_RATE, "notify");
    if (!writer.has_value()) {
        return;
    }
    const Attached reader(base(), "reader");
    std::vector<std::unique_ptr<Subscribed>> subs;
    const WaitSet set(reader.ctx());
    for (std::size_t owner = 0; owner < WRITER_OWNERS; ++owner) {
        subs.push_back(
            std::make_unique<Subscribed>(reader.ctx(), owner_name(owner)));
        ASSERT_TRUE(scada_waitset_add_sub(set.get(), subs.back()->get()) ==
                    SCADA_OK)
            << owner;
    }

    Consumer consumer(reader);
    EXPECT_TRUE(consumer.run(set.get(), subs) && consumer.complete());
    for (std::size_t owner = 0; owner < WRITER_OWNERS; ++owner) {
        EXPECT_TRUE(ring_was_clean(consumer.counts(owner), subs[owner]->get()))
            << owner;
    }
    EXPECT_TRUE(stops_cleanly(*writer));
    stop(*tagd);
}

// `descriptor`, the descriptor of a wait set that holds `sub`, becomes
// readable again and again until `sub` has delivered `changes` changes.
::testing::AssertionResult receives_by_poll(int descriptor, scada_sub* sub,
                                            int changes) {
    pollfd entry{.fd = descriptor, .events = POLLIN, .revents = 0};
    int got = 0;
    while (got < changes) {
        if (::poll(&entry, 1, WAIT_MS) != 1) {
            return ::testing::AssertionFailure()
                   << "not readable after " << got << " changes";
        }
        std::uint64_t count = 0;
        // Non-blocking: a second readiness may have been read already.
        const ssize_t read = ::read(descriptor, &count, sizeof(count));
        static_cast<void>(read);
        scada_change change{};
        while (scada_sub_next(sub, &change, 0) == SCADA_OK) {
            ++got;
        }
    }
    return ::testing::AssertionSuccess();
}

// The process index of the owner of the tag `name`, as `reader` sees it.
std::uint16_t process_of(const Attached& reader, const std::string& name) {
    scada_tag_info_t info{};
    EXPECT_EQ(scada_tag_info(reader.ctx(), reader.resolve(name), &info),
              SCADA_OK);
    return info.process_index;
}

// A wait set over the cycle counter of `process_index` wakes `cycles`
// times.
::testing::AssertionResult wakes_for_cycles(scada_ctx* ctx,
                                            std::uint16_t process_index,
                                            int cycles) {
    const WaitSet set(ctx);
    if (scada_waitset_add_cycle(set.get(), process_index) != SCADA_OK) {
        return ::testing::AssertionFailure() << "cannot add the cycle";
    }
    for (int cycle = 0; cycle < cycles; ++cycle) {
        if (scada_waitset_wait(set.get(), WAIT_MS) != SCADA_OK) {
            return ::testing::AssertionFailure() << "no cycle " << cycle;
        }
    }
    return ::testing::AssertionSuccess();
}

// Conformance test 24 across processes: the descriptor of a wait set
// becomes readable for a change written by another process, and a waiter
// on that process's cycle counter wakes with every notify.
TEST_F(ChangeRingProcesses, Conformance24TheDescriptorAndTheCycleWorkAcross) {
    write_load_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    // A slow writer: one update every 10 ms, for 3 s. It stops only once it
    // has written them all.
    constexpr std::uint64_t SLOW_RATE = 100;
    constexpr std::uint64_t SLOW_UPDATES = 300;
    constexpr int CHANGES = 5;
    std::optional<ChildProcess> writer =
        start_writer(SLOW_UPDATES, SLOW_RATE, "notify");
    if (!writer.has_value()) {
        return;
    }
    const Attached reader(base(), "reader");
    const Subscribed sub(reader.ctx(), owner_name(0));
    const WaitSet set(reader.ctx());
    ASSERT_EQ(scada_waitset_add_sub(set.get(), sub.get()), SCADA_OK);
    int descriptor = -1;
    ASSERT_EQ(scada_waitset_fd(set.get(), &descriptor), SCADA_OK);
    EXPECT_TRUE(receives_by_poll(descriptor, sub.get(), CHANGES));

    // The cycle counter of the writer's process.
    EXPECT_TRUE(wakes_for_cycles(reader.ctx(), process_of(reader, "load.w0.t0"),
                                 CHANGES));
    EXPECT_TRUE(stops_cleanly(*writer));
    stop(*tagd);
}

// The return code of `scada_sub_next` on `sub` without waiting.
int next_code(scada_sub* sub) {
    scada_change change{};
    return scada_sub_next(sub, &change, 0);
}

// `sub` gives SCADA_E_GAP once and then nothing, and has counted one gap.
::testing::AssertionResult gives_one_gap(scada_sub* sub) {
    const int first = next_code(sub);
    const int second = next_code(sub);
    scada_sub_stats_t stats{};
    if (first != SCADA_E_GAP || second != SCADA_E_TIMEOUT ||
        scada_sub_stats(sub, &stats) != SCADA_OK || stats.gaps != 1) {
        return ::testing::AssertionFailure()
               << "codes " << first << " and " << second << ", " << stats.gaps
               << " gaps";
    }
    return ::testing::AssertionSuccess();
}

// The owner state `sub` saw: changes, and whether it is down now.
::testing::AssertionResult saw_owner(scada_sub* sub, std::uint32_t changes,
                                     bool down) {
    scada_sub_stats_t stats{};
    if (scada_sub_stats(sub, &stats) != SCADA_OK ||
        stats.owner_changes != changes || (stats.owner_down != 0) != down) {
        return ::testing::AssertionFailure()
               << stats.owner_changes << " owner changes, owner_down "
               << int{stats.owner_down};
    }
    return ::testing::AssertionSuccess();
}

// How long after the kill of an owner a waiting consumer may still not
// have its gap: the heartbeat timeout, the wait slice, and time for this
// thread to be scheduled on a busy machine.
constexpr int DOWN_LIMIT_MS = IO_TIMEOUT_MS + SCADA_WAIT_SLICE_MS + 1000;

// Issue #42, point 2, across processes: an owner killed with SIGKILL writes
// no change; a consumer waiting on its ring gets SCADA_E_GAP within the
// heartbeat timeout, reads the tag as OWNER_DOWN, and gets one more gap
// when the owner is back.
TEST_F(ChangeRingProcesses, AKilledOwnerGivesAGapWithinItsHeartbeatTimeout) {
    write_small_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> owner = attached_io();
    if (!tagd.has_value() || !owner.has_value()) {
        return;
    }
    const Attached reader(base(), "reader");
    const Subscribed sub(reader.ctx(), "io");
    const std::uint32_t level = reader.resolve("plant.level");
    kill(*owner);
    const Clock::time_point killed = Clock::now();
    scada_change change{};
    EXPECT_EQ(scada_sub_next(sub.get(), &change, WAIT_MS), SCADA_E_GAP);
    EXPECT_LE(Clock::now() - killed, std::chrono::milliseconds(DOWN_LIMIT_MS));
    EXPECT_TRUE(saw_owner(sub.get(), 1, true));
    scada_value value{};
    EXPECT_EQ(scada_tag_read(reader.ctx(), level, &value), SCADA_OK);
    EXPECT_NE(value.quality_flags & SCADA_FLAG_OWNER_DOWN, 0);

    std::optional<ChildProcess> again = attached_io();
    if (!again.has_value()) {
        return;
    }
    EXPECT_EQ(scada_sub_next(sub.get(), &change, WAIT_MS), SCADA_E_GAP);
    EXPECT_TRUE(saw_owner(sub.get(), 2, false));
    EXPECT_TRUE(stops_cleanly(*again));
    stop(*tagd);
}

// Conformance test 7, the part of the ring, across processes: the owner is
// killed and starts again; a consumer that does not look meanwhile gets
// SCADA_E_GAP once (one more if it sees the owner down, as above).
TEST_F(ChangeRingProcesses, Conformance07AKilledOwnerThatRestartsGivesOneGap) {
    write_small_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> owner = attached_io();
    if (!tagd.has_value() || !owner.has_value()) {
        return;
    }
    const Attached reader(base(), "reader");
    const Subscribed sub(reader.ctx(), "io");
    EXPECT_EQ(next_code(sub.get()), SCADA_E_TIMEOUT);
    kill(*owner);

    std::optional<ChildProcess> again = attached_io();
    if (!again.has_value()) {
        return;
    }
    EXPECT_TRUE(gives_one_gap(sub.get()));
    EXPECT_TRUE(stops_cleanly(*again));
    stop(*tagd);
}

}  // namespace
