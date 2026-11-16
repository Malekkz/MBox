// Performance test of scada_queue_post with the high-water mark of the base
// sequence (issue #53, decision D-49, condition C9). Two contexts of this
// process, `a` and `b`, each produce one events queue and consume the
// other's. Queue ids are in name order, so in each `proc` region the
// high-water mark of the produced queue and the cursor of the consumed one
// are neighbours on one cache line: every post stores the mark while the
// other producer reads the cursor beside it. Four threads pass one million
// records each way and the test reports the time per record.
//
// The gate is loose (10 microseconds per record on x86-64); the figures
// before and after the mark are compared in the pull request. With
// SCADA_PERF_REPORT_ONLY=1 it only prints them (decisions D-13, D-32).

#include "tag_processes_fixture.h"
#include "tagd_fixture.h"

#include "scada/tag/scada_tag.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <latch>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace scada::test::tagd;
using Clock = std::chrono::steady_clock;

constexpr std::uint64_t RECORDS = 1'000'000;
constexpr std::uint32_t CAPACITY = 1024;
constexpr double LIMIT_NS_PER_RECORD = 10'000.0;
// Every loop gives up after this, so that a failure cannot hang the test.
constexpr std::chrono::seconds DEADLINE{60};
constexpr std::chrono::milliseconds BEAT_PERIOD{100};
#if defined(__x86_64__)
constexpr bool IS_X86_64 = true;
#else
constexpr bool IS_X86_64 = false;
#endif

bool report_only() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing changes the environment while the tests run
    const char* const text = std::getenv("SCADA_PERF_REPORT_ONLY");
    return text != nullptr && std::string_view(text) == "1";
}

scada_event_record audit() {
    scada_event_record record{};
    record.kind = SCADA_EVENT_AUDIT;
    record.source_handle = SCADA_INVALID_HANDLE;
    return record;
}

// Posts the warm-up record 0, waits at `ready` for the other threads, then
// posts the records 1 to RECORDS. Retries while the queue is full or its
// consumer has not been found yet; sets `failed` on any other code.
void post_all(scada_ctx* ctx, scada_queue* queue, std::latch& ready,
              std::atomic<bool>& failed) {
    const scada_event_record record = audit();
    const Clock::time_point deadline = Clock::now() + DEADLINE;
    bool warm = false;
    for (std::uint64_t posted = 0; posted <= RECORDS && !failed.load();) {
        if (posted == 1 && !warm) {
            warm = true;
            ready.arrive_and_wait();
        }
        if (Clock::now() > deadline) {
            failed.store(true);
            break;
        }
        const int code = scada_queue_post(queue, &record, sizeof(record));
        if (code == SCADA_OK) {
            ++posted;
        } else if (code == SCADA_E_PEER_DOWN) {
            static_cast<void>(scada_tag_notify(ctx));
        } else if (code != SCADA_E_FULL) {
            failed.store(true);
        }
    }
    if (!warm) {
        ready.arrive_and_wait();  // every thread arrives, also on failure
    }
}

// Takes the warm-up record 0 with a waiting next: a consumer that attached
// before the queue file existed finds it in the wait (at most a second,
// contract 11.2), which a poll with timeout 0 never does. Then waits at
// `ready` and takes and acknowledges the records 1 to RECORDS. Sets
// `failed` on a code other than OK and TIMEOUT.
void take_all(scada_queue* queue, std::latch& ready,
              std::atomic<bool>& failed) {
    constexpr int WARM_UP_WAIT_MS = 5000;
    scada_event_record first{};
    if (scada_queue_next(queue, &first, sizeof(first), WARM_UP_WAIT_MS) !=
            SCADA_OK ||
        scada_queue_ack(queue) != SCADA_OK) {
        failed.store(true);
    }
    ready.arrive_and_wait();
    const Clock::time_point deadline = Clock::now() + DEADLINE;
    for (std::uint64_t taken = 1; taken <= RECORDS && !failed.load();) {
        if (Clock::now() > deadline) {
            failed.store(true);
            break;
        }
        scada_event_record record{};
        const int code = scada_queue_next(queue, &record, sizeof(record), 0);
        if (code == SCADA_OK) {
            failed.store(record.seq != taken ||
                         scada_queue_ack(queue) != SCADA_OK);
            ++taken;
        } else if (code != SCADA_E_TIMEOUT) {
            failed.store(true);
        }
    }
}

class QueuePostPerf : public TagProcesses {
protected:
    void write_pair_tag_set() const {
        const std::string uid = std::to_string(::geteuid());
        std::ofstream file(tag_set_path());
        file << R"({"format": 1, "processes": [{"name": "tagd", "uid": )" << uid
             << R"(}, {"name": "a", "uid": )" << uid
             << R"(}, {"name": "b", "uid": )" << uid
             << R"(}], "owners": [], "tags": [], "queues": [)"
             << R"({"name": "events.a", "producer": "a", "consumer": "b", )"
             << R"("capacity": )" << CAPACITY << "}, "
             << R"({"name": "events.b", "producer": "b", "consumer": "a", )"
             << R"("capacity": )" << CAPACITY << "}]}\n";
    }
};

TEST_F(QueuePostPerf, TwoWaysWithTheMarkBesideTheCursor) {
    write_pair_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    const Attached a(base(), "a");
    const Attached b(base(), "b");
    scada_queue* a_out = nullptr;
    scada_queue* a_in = nullptr;
    scada_queue* b_out = nullptr;
    scada_queue* b_in = nullptr;
    ASSERT_EQ(scada_queue_open(a.ctx(), "events.a", &a_out), SCADA_OK);
    ASSERT_EQ(scada_queue_open(b.ctx(), "events.a", &b_in), SCADA_OK);
    ASSERT_EQ(scada_queue_open(b.ctx(), "events.b", &b_out), SCADA_OK);
    ASSERT_EQ(scada_queue_open(a.ctx(), "events.b", &a_in), SCADA_OK);
    std::atomic<bool> failed{false};
    constexpr std::ptrdiff_t THREADS_AND_MAIN = 5;
    std::latch ready(THREADS_AND_MAIN);
    std::thread pa([&] { post_all(a.ctx(), a_out, ready, failed); });
    std::thread pb([&] { post_all(b.ctx(), b_out, ready, failed); });
    std::thread ca([&] { take_all(a_in, ready, failed); });
    std::thread cb([&] { take_all(b_in, ready, failed); });
    std::atomic<int> running{4};
    const auto done = [&running] { running.fetch_sub(1); };
    // The main loops of `a` and `b`: their heartbeats, so that neither
    // looks down to the other's producer during the run.
    std::thread beats([&] {
        while (running.load() > 0) {
            static_cast<void>(scada_tag_heartbeat(a.ctx()));
            static_cast<void>(scada_tag_heartbeat(b.ctx()));
            std::this_thread::sleep_for(BEAT_PERIOD);
        }
    });
    ready.arrive_and_wait();
    const Clock::time_point start = Clock::now();
    pa.join();
    done();
    pb.join();
    done();
    ca.join();
    done();
    cb.join();
    done();
    beats.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             Clock::now() - start)
                             .count();
    EXPECT_FALSE(failed.load());
    const double per_record =
        static_cast<double>(elapsed) / static_cast<double>(RECORDS);
    std::cout << "queue post, two ways, mark beside the cursor: " << per_record
              << " ns per record\n";
    RecordProperty("ns_per_record", std::to_string(per_record));
    if (IS_X86_64 && !report_only()) {
        EXPECT_LT(per_record, LIMIT_NS_PER_RECORD);
    }
    for (scada_queue* queue : {a_out, a_in, b_out, b_in}) {
        scada_queue_close(queue);
    }
    stop(*tagd);
}

}  // namespace
