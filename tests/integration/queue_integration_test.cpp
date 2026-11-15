// Integration test for the reliable queues of the tag database contract
// (section 9) with real processes: conformance tests 16, 17 and 19 across
// process boundaries, and one million records through one queue without
// loss, duplication or reordering (issue #16).
//
// scada-tagd and the helper scada-tag-test-queue are real processes; the
// test process itself attaches as `web` through the C interface.

#include "tag_processes_fixture.h"
#include "tagd_fixture.h"

#include "scada/tag/scada_tag.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace scada::test::tagd;
using scada::test::ChildProcess;
using Clock = std::chrono::steady_clock;

constexpr std::uint64_t ONE_MILLION = 1'000'000;
constexpr int NEXT_WAIT_MS = 1000;
constexpr int HIST_TIMEOUT_MS = 1000;
// Long against the helper's heartbeat period of 50 ms, so that a busy
// machine does not make the running driver look down.
constexpr int IO_DRIVER_TIMEOUT_MS = 10'000;
// The pause between two looks at a condition that has no event to wait
// on (whether a post is accepted), in ms.
constexpr std::chrono::milliseconds RETRY_PAUSE{10};
constexpr std::int64_t REQUEST_LIFETIME_NS = 30'000'000'000;
constexpr std::uint64_t PAYLOAD_FACTOR = 0x9E37'79B9'7F4A'7C15U;
constexpr unsigned BITS_PER_BYTE = 8;
constexpr std::size_t PAYLOAD_BYTES = 8;
// How often the consumer of the long test writes its heartbeat, in records.
constexpr std::uint64_t BEAT_EVERY = 4096;
// The long test gives up after this many times WAIT_LIMIT without a record.
constexpr int LONG_WAIT_FACTOR = 4;
// The first records the long test reports in full when they are wrong.
constexpr std::uint64_t REPORTED_MISMATCHES = 10;

std::int64_t monotonic_now() {
    timespec now{};
    EXPECT_EQ(::clock_gettime(CLOCK_MONOTONIC, &now), 0);
    constexpr std::int64_t NANOSECONDS_PER_SECOND = 1'000'000'000;
    return (std::int64_t{now.tv_sec} * NANOSECONDS_PER_SECOND) + now.tv_nsec;
}

scada_event_record audit_event() {
    scada_event_record record{};
    record.kind = SCADA_EVENT_AUDIT;
    record.source_handle = SCADA_INVALID_HANDLE;
    return record;
}

scada_write_request direct_request() {
    scada_write_request record{};
    record.target_handle = 1;
    record.operation = SCADA_OP_DIRECT;
    record.source = SCADA_SOURCE_OPERATOR;
    record.value_type = SCADA_TYPE_UINT64;
    record.value = 1;
    record.user_id = 1;
    record.expiry_mono_ns = monotonic_now() + REQUEST_LIFETIME_NS;
    return record;
}

// The numbers that follow `word` in `out`, in order: "got 3\n" gives 3.
std::vector<std::uint64_t> numbers_after(std::string_view out,
                                         std::string_view word) {
    std::vector<std::uint64_t> numbers;
    std::istringstream lines{std::string(out)};
    std::string line;
    while (std::getline(lines, line)) {
        if (line.starts_with(word)) {
            numbers.push_back(std::stoull(line.substr(word.size())));
        }
    }
    return numbers;
}

// One side of a queue of the test process; closed when it goes out of
// scope.
class OpenQueue {
public:
    OpenQueue(scada_ctx* ctx, const char* name) {
        EXPECT_EQ(scada_queue_open(ctx, name, &queue_), SCADA_OK) << name;
    }
    ~OpenQueue() { scada_queue_close(queue_); }
    OpenQueue(const OpenQueue&) = delete;
    OpenQueue& operator=(const OpenQueue&) = delete;
    OpenQueue(OpenQueue&&) = delete;
    OpenQueue& operator=(OpenQueue&&) = delete;

    [[nodiscard]] scada_queue* get() const { return queue_; }

private:
    scada_queue* queue_ = nullptr;
};

class QueueProcesses : public TagProcesses {
protected:
    // scada-tagd; `feed`, which posts events to `web`; `hist`, which
    // consumes the events of `web`; `io`, a driver for the requests of
    // `web`; and `web`, the test process.
    void write_queue_tag_set() const {
        const std::string uid = std::to_string(::geteuid());
        std::ofstream file(tag_set_path());
        file << R"({"format": 1, "heartbeat_check_ms": )" << CHECK_MS
             << R"(, "processes": [{"name": "tagd", "uid": )" << uid
             << R"(}, {"name": "feed", "uid": )" << uid
             << R"(}, {"name": "hist", "uid": )" << uid
             << R"(, "heartbeat_timeout_ms": )" << HIST_TIMEOUT_MS
             << R"(}, {"name": "io", "uid": )" << uid
             << R"(, "heartbeat_timeout_ms": )" << IO_DRIVER_TIMEOUT_MS
             << R"(}, {"name": "web", "uid": )" << uid
             << R"(}], "owners": [], "tags": [], "queues": [)"
             << R"({"name": "events.feed", "producer": "feed", )"
             << R"("consumer": "web"}, )"
             << R"({"name": "events.web", "producer": "web", )"
             << R"("consumer": "hist"}, )"
             << R"({"name": "cmd.io.web", "producer": "web", )"
             << R"("consumer": "io"}, )"
             << R"({"name": "result.io.web", "producer": "io", )"
             << R"("consumer": "web"}]})" << "\n";
    }

    // Starts the helper as `process` in `mode` on `queue` and waits until
    // it has attached.
    [[nodiscard]] std::optional<ChildProcess> start_queue_helper(
        const std::string& process, const std::string& mode,
        const std::string& queue, const std::string& count = {}) const {
        std::vector<std::string> arguments{base(), process, mode, queue};
        if (!count.empty()) {
            arguments.push_back(count);
        }
        std::optional<ChildProcess> helper =
            start_helper("SCADA_TAG_QUEUE_COMMAND", arguments);
        if (helper.has_value()) {
            EXPECT_TRUE(helper->wait_for_output(
                [](std::string_view out) {
                    return out.find("attached ") != std::string_view::npos;
                },
                WAIT_LIMIT))
                << helper->err();
        }
        return helper;
    }

    // Posts `record` until the post is accepted, calling notify between
    // attempts so that the test process finds a consumer that attached
    // after it asked for its region (contract 11.2: at most once a second).
    // The return code of the last attempt.
public:
    template <typename Record>
    static int post_when_accepted(scada_ctx* ctx, scada_queue* queue,
                                  const Record& record) {
        const Clock::time_point deadline = Clock::now() + WAIT_LIMIT;
        int code = SCADA_E_PEER_DOWN;
        while (Clock::now() < deadline) {
            code = scada_queue_post(queue, &record, sizeof(record));
            static_cast<void>(scada_tag_notify(ctx));
            if (code != SCADA_E_PEER_DOWN) {
                break;
            }
            std::this_thread::sleep_for(RETRY_PAUSE);
        }
        return code;
    }
};

// `queue` delivers the records 0 to `count - 1` of the helper's `post`
// mode, in order, each once; each is acknowledged.
::testing::AssertionResult receives_in_order(const Attached& web,
                                             scada_queue* queue,
                                             std::uint64_t count) {
    const Clock::time_point start = Clock::now();
    std::uint64_t mismatches = 0;
    std::string first_mismatches;
    for (std::uint64_t received = 0; received < count;) {
        scada_event_record record{};
        const int code =
            scada_queue_next(queue, &record, sizeof(record), NEXT_WAIT_MS);
        if (code == SCADA_E_TIMEOUT &&
            Clock::now() - start < WAIT_LIMIT * LONG_WAIT_FACTOR) {
            static_cast<void>(scada_tag_heartbeat(web.ctx()));
            continue;
        }
        if (code != SCADA_OK || scada_queue_ack(queue) != SCADA_OK) {
            return ::testing::AssertionFailure()
                   << "code " << code << " after " << received << " records";
        }
        std::uint64_t payload = 0;
        for (std::size_t byte = 0; byte < PAYLOAD_BYTES; ++byte) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): byte is below PAYLOAD_BYTES, inside the array
            payload |= std::uint64_t{record.payload[byte]}
                       << (byte * BITS_PER_BYTE);
        }
        if (record.seq != received ||
            record.time_ns != static_cast<std::int64_t>(received) ||
            payload != received * PAYLOAD_FACTOR) {
            if (++mismatches <= REPORTED_MISMATCHES) {
                first_mismatches += " record " + std::to_string(received) +
                                    " has seq " + std::to_string(record.seq);
            }
        }
        ++received;
        if (received % BEAT_EVERY == 0) {
            static_cast<void>(scada_tag_heartbeat(web.ctx()));
        }
    }
    if (mismatches != 0) {
        return ::testing::AssertionFailure()
               << mismatches << " records out of place:" << first_mismatches;
    }
    return ::testing::AssertionSuccess();
}

// Posts events to `queue`, a pause apart, until a post is refused with
// PEER_DOWN, and returns how many were accepted before; nothing if no post
// is refused within WAIT_LIMIT or a post fails otherwise.
std::optional<std::uint64_t> posts_until_down(const Attached& web,
                                              scada_queue* queue) {
    std::uint64_t accepted = 0;
    const Clock::time_point start = Clock::now();
    while (Clock::now() - start < WAIT_LIMIT) {
        const scada_event_record record = audit_event();
        const int code = scada_queue_post(queue, &record, sizeof(record));
        static_cast<void>(scada_tag_heartbeat(web.ctx()));
        if (code == SCADA_E_PEER_DOWN) {
            return accepted;
        }
        if (code != SCADA_OK) {
            return std::nullopt;
        }
        ++accepted;
        std::this_thread::sleep_for(RETRY_PAUSE);
    }
    return std::nullopt;
}

// The numbers 0 to `count - 1`.
std::vector<std::uint64_t> first_numbers(std::uint64_t count) {
    std::vector<std::uint64_t> numbers;
    for (std::uint64_t i = 0; i < count; ++i) {
        numbers.push_back(i);
    }
    return numbers;
}

// `queue` gives the answer `status` to `request_id` within the wait,
// which is then acknowledged.
::testing::AssertionResult answers(scada_queue* queue, std::uint64_t request_id,
                                   std::uint8_t status) {
    scada_write_result answer{};
    const int code =
        scada_queue_next(queue, &answer, sizeof(answer), NEXT_WAIT_MS * 10);
    if (code != SCADA_OK) {
        return ::testing::AssertionFailure() << "next returned " << code;
    }
    if (answer.request_id != request_id || answer.status != status ||
        scada_queue_ack(queue) != SCADA_OK) {
        return ::testing::AssertionFailure() << "answer " << int{answer.status}
                                             << " to " << answer.request_id;
    }
    return ::testing::AssertionSuccess();
}

// Issue #16, acceptance: one million records pass through a queue of
// 1,024 places between two processes with no loss, duplication or
// reordering. Each record carries its number and a payload derived from
// it.
TEST_F(QueueProcesses, OneMillionRecordsWithoutLossDuplicationOrReordering) {
    write_queue_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    const Attached web(base(), "web");
    const OpenQueue reader(web.ctx(), "events.feed");
    std::optional<ChildProcess> feed = start_queue_helper(
        "feed", "post", "events.feed", std::to_string(ONE_MILLION));
    if (!feed.has_value()) {
        return;
    }
    const Clock::time_point start = Clock::now();
    EXPECT_TRUE(receives_in_order(web, reader.get(), ONE_MILLION));
    RecordProperty(
        "elapsed_ms",
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                           Clock::now() - start)
                           .count()));
    EXPECT_TRUE(prints(*feed, "posted 1000000\n"));
    scada_event_record extra{};
    EXPECT_EQ(scada_queue_next(reader.get(), &extra, sizeof(extra), 0),
              SCADA_E_TIMEOUT)
        << "a record too many";
    EXPECT_TRUE(stops_cleanly(*feed));
    stop(*tagd);
}

// Issue #53 (D-44), with the real scada-tagd: a queue whose consumer's
// cursor is out of range is logged as broken, and an events queue file
// removed under a running producer as missing. The cursor is changed in
// the file, as damage would: `proc.web` holds it after the RegionHeader and
// the ProcHeader (64 bytes each), at the queue id `events.feed` has in name
// order (contract 9, 10).
TEST_F(QueueProcesses, TagdLogsABrokenAndAMissingQueue) {
    constexpr std::size_t EVENTS_FEED_ID = 1;
    constexpr std::size_t CURSORS_OFFSET = 128;
    constexpr std::uint64_t BEYOND = 7;
    write_queue_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    const Attached web(base(), "web");
    std::optional<ChildProcess> feed =
        start_queue_helper("feed", "post", "events.feed", "2");
    if (!feed.has_value()) {
        return;
    }
    EXPECT_TRUE(prints(*feed, "posted 2\n"));
    {
        std::fstream proc(
            std::filesystem::path(base()) / "current" / "proc.web",
            std::ios::in | std::ios::out | std::ios::binary);
        proc.seekp(static_cast<std::streamoff>(
            CURSORS_OFFSET + (EVENTS_FEED_ID * sizeof(std::uint64_t))));
        const std::uint64_t value = BEYOND;  // little-endian targets only
        proc.write(
            reinterpret_cast<const char*>(
                &value),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): bytes of an integer for a file
            sizeof(value));
        ASSERT_TRUE(proc.good());
    }
    EXPECT_TRUE(logs(*tagd, R"(queue broken QUEUE="events.feed" CURSOR="7" )"
                            R"(WRITE_SEQ="2" CAPACITY=")"));
    std::filesystem::remove(std::filesystem::path(base()) / "current" /
                            "events.feed");
    EXPECT_TRUE(logs(*tagd, R"(queue file missing QUEUE="events.feed")"));
    EXPECT_TRUE(stops_cleanly(*feed));
    stop(*tagd);
}

// `web` posts two events to `out`; `hist`, which consumes them, prints that
// it holds the first without its ack.
::testing::AssertionResult hist_holds_the_first_of_two(const Attached& web,
                                                       scada_queue* out,
                                                       ChildProcess& hist) {
    if (QueueProcesses::post_when_accepted(web.ctx(), out, audit_event()) !=
        SCADA_OK) {
        return ::testing::AssertionFailure() << "the first post failed";
    }
    const scada_event_record second = audit_event();
    if (scada_queue_post(out, &second, sizeof(second)) != SCADA_OK ||
        scada_tag_notify(web.ctx()) != SCADA_OK) {
        return ::testing::AssertionFailure() << "the second post failed";
    }
    return prints(hist, "holding 0\n");
}

// `hist` prints that it got the events 0 to `count - 1`, in order, once
// each, and that attach skipped none.
::testing::AssertionResult hist_gets_every_event(ChildProcess& hist,
                                                 std::uint64_t count) {
    if (const ::testing::AssertionResult printed =
            prints(hist, "got " + std::to_string(count - 1) + "\n");
        !printed) {
        return printed;
    }
    if (numbers_after(hist.out(), "got ") != first_numbers(count) ||
        hist.out().find(" skipped=0\n") == std::string::npos) {
        return ::testing::AssertionFailure() << hist.out();
    }
    return ::testing::AssertionSuccess();
}

// Conformance tests 16 and 19: a consumer killed before its ack gets the
// record again after its restart; while it is dead a post is accepted only
// until its heartbeat times out, and refused with PEER_DOWN from then on;
// after a clean stop it is refused at once.
TEST_F(QueueProcesses, Conformance16RedeliveryAfterAConsumerIsKilled) {
    write_queue_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> hist =
        start_queue_helper("hist", "hold", "events.web");
    if (!tagd.has_value() || !hist.has_value()) {
        return;
    }
    const Attached web(base(), "web");
    const OpenQueue out(web.ctx(), "events.web");
    ASSERT_TRUE(hist_holds_the_first_of_two(web, out.get(), *hist));
    kill(*hist);
    // The residual window of contract 9.3: accepted until the heartbeat of
    // `hist` is older than its timeout.
    const std::optional<std::uint64_t> late = posts_until_down(web, out.get());
    if (!late.has_value()) {
        ADD_FAILURE() << "the dead consumer was never down";
        return;
    }
    const std::uint64_t accepted = *late + 2;
    std::optional<ChildProcess> restarted = start_queue_helper(
        "hist", "consume", "events.web", std::to_string(accepted));
    if (!restarted.has_value()) {
        return;
    }
    // The record held at the kill comes first, once; nothing is lost.
    EXPECT_TRUE(hist_gets_every_event(*restarted, accepted));
    EXPECT_TRUE(stops_cleanly(*restarted));
    const scada_event_record after_stop = audit_event();
    EXPECT_EQ(scada_queue_post(out.get(), &after_stop, sizeof(after_stop)),
              SCADA_E_PEER_DOWN);
    stop(*tagd);
}

// `web` posts a request, which `holder` prints it took, and a second one,
// which `holder` does not take; then `holder` is killed.
::testing::AssertionResult driver_dies_holding_one(const Attached& web,
                                                   scada_queue* requests,
                                                   ChildProcess& holder) {
    if (QueueProcesses::post_when_accepted(web.ctx(), requests,
                                           direct_request()) != SCADA_OK) {
        return ::testing::AssertionFailure() << "the first post failed";
    }
    if (const ::testing::AssertionResult took = prints(holder, "took 0\n");
        !took) {
        return took;
    }
    // Posted before the kill, so that it does not depend on how long the
    // dead driver still counts as up: `holder` takes no second request, so
    // this one is still queued when the driver attaches again, as a request
    // posted in the residual window of contract 9.3 would be.
    const scada_write_request queued = direct_request();
    if (scada_queue_post(requests, &queued, sizeof(queued)) != SCADA_OK) {
        return ::testing::AssertionFailure() << "the second post failed";
    }
    kill(holder);
    return ::testing::AssertionSuccess();
}

// The restarted `driver` reports one request drained; `web` gets ABORTED
// for the request queued while it was dead, then OK for a new one, and no
// answer for the request the killed driver took.
::testing::AssertionResult restarted_driver_answers(const Attached& web,
                                                    scada_queue* requests,
                                                    scada_queue* results,
                                                    ChildProcess& driver) {
    if (driver.out().find("attached aborted=1 ") == std::string::npos) {
        return ::testing::AssertionFailure() << driver.out();
    }
    if (const ::testing::AssertionResult aborted =
            answers(results, 1, SCADA_RESULT_ABORTED);
        !aborted) {
        return aborted;
    }
    if (QueueProcesses::post_when_accepted(web.ctx(), requests,
                                           direct_request()) != SCADA_OK) {
        return ::testing::AssertionFailure() << "the third post failed";
    }
    if (const ::testing::AssertionResult done =
            answers(results, 2, SCADA_RESULT_OK);
        !done) {
        return done;
    }
    scada_write_result none{};
    if (scada_queue_next(results, &none, sizeof(none), 0) != SCADA_E_TIMEOUT) {
        return ::testing::AssertionFailure() << "an answer too many";
    }
    return ::testing::AssertionSuccess();
}

// Conformance test 17: a driver killed after taking a request never sees
// it again; a request queued while it was dead is answered ABORTED when it
// attaches and is not executed; later requests are executed and answered.
TEST_F(QueueProcesses, Conformance17ARequestIsNeverExecutedTwice) {
    write_queue_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> holder =
        start_queue_helper("io", "drive-hold", "cmd.io.web");
    if (!tagd.has_value() || !holder.has_value()) {
        return;
    }
    const Attached web(base(), "web");
    const OpenQueue requests(web.ctx(), "cmd.io.web");
    const OpenQueue results(web.ctx(), "result.io.web");
    ASSERT_TRUE(driver_dies_holding_one(web, requests.get(), *holder));
    std::optional<ChildProcess> driver =
        start_queue_helper("io", "drive", "cmd.io.web");
    if (!driver.has_value()) {
        return;
    }
    EXPECT_TRUE(
        restarted_driver_answers(web, requests.get(), results.get(), *driver));
    EXPECT_TRUE(stops_cleanly(*driver));
    EXPECT_EQ(numbers_after(driver->out(), "took "),
              std::vector<std::uint64_t>{2});
    stop(*tagd);
}

// The library checks the thread of a queue (contract 13.2) in every build:
// one built without NDEBUG stops the process, another refuses the post.
// CTest says which this build is.
bool stops_on_a_second_thread() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing changes the environment while the tests run
    const char* const checked = std::getenv("SCADA_TAG_THREAD_CHECK");
    return checked != nullptr && std::string_view(checked) == "1";
}

// `feed` was stopped at its second posting thread (a debug build), or that
// post was refused and it stops cleanly (any other build).
::testing::AssertionResult stopped_or_refused(ChildProcess& feed) {
    if (stops_on_a_second_thread()) {
        if (feed.wait_for_signal_exit(WAIT_LIMIT) != SIGABRT ||
            feed.err().find("posted from two threads") == std::string::npos) {
            return ::testing::AssertionFailure()
                   << "not stopped: " << feed.out() << feed.err();
        }
        return ::testing::AssertionSuccess();
    }
    if (const ::testing::AssertionResult refused =
            prints(feed, "second=" + std::to_string(SCADA_E_INVALID) + "\n");
        !refused) {
        return refused;
    }
    return stops_cleanly(feed);
}

// Decision D-40, condition 4: a second thread that posts to a queue stops a
// debug build and is refused with SCADA_E_INVALID by any other build.
TEST_F(QueueProcesses, ASecondPostingThreadIsStoppedOrRefused) {
    write_queue_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    const Attached web(base(), "web");
    const OpenQueue reader(web.ctx(), "events.feed");
    std::optional<ChildProcess> feed =
        start_queue_helper("feed", "two-threads", "events.feed");
    if (!feed.has_value()) {
        return;
    }
    EXPECT_TRUE(stopped_or_refused(*feed));
    stop(*tagd);
}

}  // namespace
