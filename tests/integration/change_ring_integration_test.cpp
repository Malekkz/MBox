// Integration tests for the change rings with real processes (contract,
// sections 8 and 15): a writer process notifies, and this process consumes
// its rings through read-only mappings and is woken across the process
// boundary by a futex on those mappings (decision D-31). Conformance tests
// 7 (the ring), 13 and 24.
//
// Every wait is on a condition with a limit.

#include "tag_processes_fixture.h"
#include "tagd_fixture.h"

#include "scada/tag/scada_tag.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <poll.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
        ASSERT_EQ(scada_waitset_add_sub(set.get(), subs.back()->get()),
                  SCADA_OK);
    }

    std::array<RingCounts, WRITER_OWNERS> counts{};
    // The count each tag the writer writes had last, 0 before its first
    // change. `complete` tags have reached ROUNDS.
    std::map<std::uint32_t, std::uint64_t> last;
    for (std::size_t owner = 0; owner < WRITER_OWNERS; ++owner) {
        for (std::size_t tag = 0; tag < WRITER_TAGS; ++tag) {
            last[reader.resolve(owner_name(owner) + ".t" +
                                std::to_string(tag))] = 0;
        }
    }
    std::size_t complete = 0;
    const Clock::time_point deadline = Clock::now() + WAIT_LIMIT;
    while (complete < WRITER_OWNERS * WRITER_TAGS && Clock::now() < deadline) {
        const int waited = scada_waitset_wait(set.get(), WAIT_MS);
        if (waited == SCADA_E_TIMEOUT) {
            break;
        }
        ASSERT_EQ(waited, SCADA_OK);
        for (std::size_t owner = 0; owner < WRITER_OWNERS; ++owner) {
            RingCounts& count = counts.at(owner);
            for (;;) {
                scada_change change{};
                const int code = scada_sub_next(subs[owner]->get(), &change, 0);
                if (code == SCADA_E_TIMEOUT) {
                    break;
                }
                if (code == SCADA_E_GAP) {
                    continue;  // counted by the library
                }
                if (code != SCADA_OK) {
                    ++count.other;
                    continue;
                }
                ++count.received;
                if (!is_one_write(change)) {
                    ++count.torn;
                }
                const auto tag = last.find(change.handle);
                if (tag == last.end()) {
                    ++count.other;
                    continue;
                }
                std::uint64_t& previous = tag->second;
                if (previous != 0 && change.value != previous + 1) {
                    ++count.out_of_order;
                }
                previous = change.value;
                if (previous == ROUNDS) {
                    ++complete;
                }
            }
        }
    }
    EXPECT_EQ(complete, WRITER_OWNERS * WRITER_TAGS);
    for (std::size_t owner = 0; owner < WRITER_OWNERS; ++owner) {
        const RingCounts& count = counts.at(owner);
        EXPECT_GT(count.received, 0U) << owner;
        EXPECT_EQ(count.torn, 0U) << owner;
        EXPECT_EQ(count.out_of_order, 0U) << owner;
        EXPECT_EQ(count.other, 0U) << owner;
        scada_sub_stats_t stats{};
        ASSERT_EQ(scada_sub_stats(subs[owner]->get(), &stats), SCADA_OK);
        EXPECT_EQ(stats.gaps, 0U) << owner;
        EXPECT_EQ(stats.lost, 0U) << owner;
    }
    EXPECT_TRUE(stops_cleanly(*writer));
    stop(*tagd);
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
    pollfd entry{.fd = descriptor, .events = POLLIN, .revents = 0};
    constexpr int CHANGES = 5;
    for (int got = 0; got < CHANGES;) {
        ASSERT_EQ(::poll(&entry, 1, WAIT_MS), 1) << got;
        std::uint64_t ignored = 0;
        // Non-blocking: a second readiness may have been read already.
        static_cast<void>(::read(descriptor, &ignored, sizeof(ignored)));
        scada_change change{};
        while (scada_sub_next(sub.get(), &change, 0) == SCADA_OK) {
            ++got;
        }
    }

    // The cycle counter of the writer's process.
    scada_tag_info_t info{};
    ASSERT_EQ(scada_tag_info(reader.ctx(), reader.resolve("load.w0.t0"), &info),
              SCADA_OK);
    const WaitSet cycles(reader.ctx());
    ASSERT_EQ(scada_waitset_add_cycle(cycles.get(), info.process_index),
              SCADA_OK);
    for (int cycle = 0; cycle < CHANGES; ++cycle) {
        EXPECT_EQ(scada_waitset_wait(cycles.get(), WAIT_MS), SCADA_OK) << cycle;
    }
    EXPECT_TRUE(stops_cleanly(*writer));
    stop(*tagd);
}

// Conformance test 7, the part of the ring, across processes: the owner is
// killed and starts again; a consumer gets SCADA_E_GAP once and then the
// writes of the new owner.
TEST_F(ChangeRingProcesses, Conformance07AKilledOwnerThatRestartsGivesOneGap) {
    write_small_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> owner = attached_io();
    if (!tagd.has_value() || !owner.has_value()) {
        return;
    }
    const Attached reader(base(), "reader");
    const Subscribed sub(reader.ctx(), "io");
    scada_change change{};
    EXPECT_EQ(scada_sub_next(sub.get(), &change, 0), SCADA_E_TIMEOUT);
    kill(*owner);
    EXPECT_EQ(scada_sub_next(sub.get(), &change, 0), SCADA_E_TIMEOUT);

    std::optional<ChildProcess> again = attached_io();
    if (!again.has_value()) {
        return;
    }
    EXPECT_EQ(scada_sub_next(sub.get(), &change, 0), SCADA_E_GAP);
    EXPECT_EQ(scada_sub_next(sub.get(), &change, 0), SCADA_E_TIMEOUT);
    scada_sub_stats_t stats{};
    ASSERT_EQ(scada_sub_stats(sub.get(), &stats), SCADA_OK);
    EXPECT_EQ(stats.gaps, 1U);
    EXPECT_TRUE(stops_cleanly(*again));
    stop(*tagd);
}

}  // namespace
