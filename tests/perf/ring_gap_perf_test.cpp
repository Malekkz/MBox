// The gap half of contract section 16, "default change ring capacity":
// a consumer that keeps up with the reference load gets no gap from the
// change rings of the default capacity, 65,536 entries per owner (issue
// P1-13, decision D-64).
//
// scada-sim writes the reference load (50,000 tags, 10 owner threads,
// 50,000 updates per second, notify on every tick) with the default
// heartbeat timeout. This process subscribes to all 10 rings before the
// simulator writes, and one thread per ring takes every change. A gap of
// any reason fails the test, and every change written must arrive. The
// run lasts 10 s, or SCADA_CONFORMANCE_SECONDS (600 for the 10 minutes of
// the reference load; the workflow `measurements` runs that).

#include "tag_processes_fixture.h"
#include "tagd_fixture.h"

#include "scada/sim/layout.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/tag_set.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal numbers are declared here, not in <csignal>

#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using namespace scada::test::tagd;
using scada::test::ChildProcess;
using Clock = std::chrono::steady_clock;

constexpr std::uint64_t REFERENCE_RATE = 50'000;
constexpr std::uint64_t DEFAULT_SECONDS = 10;
constexpr std::uint64_t MAX_SECONDS = 3600;
constexpr int WAIT_MS = 100;
// Below a third of the default heartbeat timeout of 3,000 ms.
constexpr std::chrono::milliseconds BEAT_PERIOD{100};

// SCADA_CONFORMANCE_SECONDS if it is set to 1 to MAX_SECONDS, otherwise
// DEFAULT_SECONDS.
std::uint64_t load_seconds() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing changes the environment while the tests run
    const char* const text = std::getenv("SCADA_CONFORMANCE_SECONDS");
    if (text == nullptr) {
        return DEFAULT_SECONDS;
    }
    const std::string_view view(text);
    std::uint64_t seconds = 0;
    const std::from_chars_result parsed =
        std::from_chars(view.data(), view.data() + view.size(), seconds);
    if (parsed.ec != std::errc{} || seconds == 0 || seconds > MAX_SECONDS) {
        ADD_FAILURE() << "SCADA_CONFORMANCE_SECONDS is not 1 to "
                      << MAX_SECONDS;
        return DEFAULT_SECONDS;
    }
    return seconds;
}

struct SubClose {
    void operator()(scada_sub* sub) const noexcept { scada_sub_close(sub); }
};
using Sub = std::unique_ptr<scada_sub, SubClose>;

// What the consumer of one ring found. Written by its thread, read once it
// is joined, but for `done`.
struct RingCounts {
    std::uint64_t received = 0;
    std::uint64_t gaps = 0;
    std::uint8_t last_gap = SCADA_GAP_NONE;
    std::uint64_t errors = 0;
};

// Tells the main thread that a consumer has finished.
class Finished {
public:
    void one_more() {
        {
            const std::scoped_lock lock(mutex_);
            ++count_;
        }
        changed_.notify_all();
    }

    // Waits up to `period` for `count` consumers to have finished; true if
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

// Takes every change of `sub` until `expected` arrived or `deadline`
// passed.
void consume(scada_sub* sub, std::uint64_t expected, Clock::time_point deadline,
             RingCounts& counts, Finished& finished) {
    while (counts.received < expected && Clock::now() < deadline) {
        scada_change change{};
        const int code = scada_sub_next(sub, &change, WAIT_MS);
        if (code == SCADA_OK) {
            ++counts.received;
        } else if (code == SCADA_E_GAP) {
            ++counts.gaps;
            scada_sub_stats_t stats{};
            if (scada_sub_stats(sub, &stats) == SCADA_OK) {
                counts.last_gap = stats.last_gap;
            }
        } else if (code != SCADA_E_TIMEOUT) {
            ++counts.errors;
        }
    }
    finished.one_more();
}

class RingGap : public TagProcesses {
protected:
    // Subscribes `reader` to the ring of every owner of the reference load.
    // Empty, with a failed expectation, if one did not open.
    [[nodiscard]] static std::vector<Sub> subscribe_all(
        const Attached& reader) {
        std::vector<Sub> subs;
        const scada::sim::Layout layout = load_layout();
        for (std::uint32_t owner = 0; owner < layout.owners; ++owner) {
            const std::string name = scada::sim::owner_name(layout, owner);
            scada_sub* sub = nullptr;
            if (scada_sub_open(reader.ctx(), name.c_str(), &sub) != SCADA_OK) {
                ADD_FAILURE() << "cannot subscribe to " << name;
                return {};
            }
            subs.emplace_back(sub);
        }
        return subs;
    }
};

// Consumes every ring in a thread of its own; this thread writes the
// heartbeat until every consumer is done.
void run_consumers(const Attached& reader, std::vector<Sub>& subs,
                   std::uint64_t per_owner, std::chrono::seconds limit,
                   std::vector<RingCounts>& counts) {
    const Clock::time_point deadline = Clock::now() + limit;
    Finished finished;
    std::vector<std::jthread> threads;
    threads.reserve(subs.size());
    for (std::size_t i = 0; i < subs.size(); ++i) {
        threads.emplace_back([sub = subs[i].get(), per_owner, deadline,
                              &count = counts[i], &finished] {
            consume(sub, per_owner, deadline, count, finished);
        });
    }
    // The main loop of the reader: its heartbeat, until every consumer has
    // finished; each consumer stops by itself at `deadline`.
    while (!finished.wait_for(subs.size(), BEAT_PERIOD)) {
        static_cast<void>(scada_tag_heartbeat(reader.ctx()));
    }
}

// Every consumer got all `per_owner` changes of its ring and no gap. Prints
// and records the counts of a run of `seconds`.
void expect_every_change(const std::vector<RingCounts>& counts,
                         std::uint64_t per_owner, std::uint64_t seconds) {
    std::uint64_t received = 0;
    std::uint64_t gaps = 0;
    for (const RingCounts& count : counts) {
        EXPECT_EQ(count.gaps, 0U) << "last gap reason " << int{count.last_gap};
        EXPECT_EQ(count.errors, 0U);
        EXPECT_EQ(count.received, per_owner);
        received += count.received;
        gaps += count.gaps;
    }
    std::cout << "ring gap: seconds=" << seconds
              << " capacity=" << scada::tag::DEFAULT_CHANGE_RING_CAPACITY
              << " owners=" << counts.size() << " received=" << received
              << " gaps=" << gaps << "\n";
    ::testing::Test::RecordProperty("received", std::to_string(received));
    ::testing::Test::RecordProperty("gaps", std::to_string(gaps));
}

TEST_F(RingGap, AConsumerThatKeepsUpWithTheReferenceLoadGetsNoGap) {
    const std::uint64_t seconds = load_seconds();
    // In range: at most MAX_SECONDS.
    const std::chrono::seconds limit(static_cast<std::int64_t>(seconds * 4) +
                                     WAIT_LIMIT.count());
    const scada::sim::Layout layout = load_layout();
    write_sim_tag_set(layout);
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        FAIL() << "no tagd";
    }
    std::optional<ChildProcess> writer =
        start_sim(layout, {"--hold", "--rate", std::to_string(REFERENCE_RATE),
                           "--rounds", std::to_string(seconds)});
    if (!writer.has_value()) {
        FAIL() << "no writer";
    }
    ASSERT_TRUE(prints(*writer, "ready\n"));
    const Attached reader(base(), "reader");
    std::vector<Sub> subs = subscribe_all(reader);
    ASSERT_EQ(subs.size(), layout.owners);

    // Every tag is written `seconds` times, one change each.
    const std::uint64_t per_owner =
        scada::sim::tags_of_owner(layout, 0) * seconds;
    std::vector<RingCounts> counts(subs.size());
    EXPECT_TRUE(writer->send_signal(SIGUSR1));
    run_consumers(reader, subs, per_owner, limit, counts);
    EXPECT_TRUE(stops_cleanly(*writer));
    subs.clear();
    stop(*tagd);

    expect_every_change(counts, per_owner, seconds);
}

}  // namespace
