// Unit tests for the reliable queues (contract, section 9, and conformance
// tests 16 to 19 as far as one process can show them): post, next and ack
// of the three kinds, FULL, PEER_DOWN, redelivery, the drain of the `cmd`
// queues at attach, expiry, record checks, cursors out of range, and wait
// sets. The side of scada-tagd is a GenerationHost in the test process; the
// processes are Attachments in it, so a consumer reads the queue through a
// read-only mapping of another "process" as a real one does. Dropping an
// Attachment and attaching again stands in for a restart: the regions and
// their cursors stay.

#include "queue.h"
#include "attach_fixture.h"
#include "attachment.h"
#include "layout.h"
#include "peer_regions.h"
#include "queue_region.h"
#include "shared_memory.h"
#include "wait_set.h"

#include "scada/common/clock.h"
#include "scada/common/mapped_region.h"
#include "scada/common/result.h"
#include "scada/tag/generation_host.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <poll.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>  // NOLINT(modernize-deprecated-headers): clock_gettime and CLOCK_THREAD_CPUTIME_ID are POSIX, declared here
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
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
using QueuePtr = std::unique_ptr<Queue>;

constexpr std::uint32_t CAPACITY = 4;
// Queue ids of the tag set: their position in name order.
constexpr std::size_t CMD_WEB = 1;
constexpr std::size_t EVENTS_IO = 2;
constexpr std::size_t RESULT_WEB = 4;
// Records that pass through a queue of CAPACITY places in the ordering test,
// and how many are posted between two looks of the consumer.
constexpr std::uint64_t MANY = 1000;
constexpr std::uint64_t BATCH = 3;
constexpr int WAIT_LIMIT_MS = 10'000;
constexpr std::int64_t NANOSECONDS_PER_MILLISECOND = 1'000'000;
// Longer than the heartbeat timeout of every process of the fixture.
constexpr std::int64_t LONG_AGO_NS = 10'000 * NANOSECONDS_PER_MILLISECOND;
constexpr std::int64_t EXPIRY_NS = 5'000 * NANOSECONDS_PER_MILLISECOND;
constexpr std::uint8_t PAYLOAD_BYTE = 0x5A;
constexpr std::uint16_t PAYLOAD_LENGTH = 3;
constexpr std::uint32_t USER = 7;
constexpr std::uint64_t VALUE = 42;
constexpr std::uint64_t NOT_A_POSITION = 7;
constexpr std::uint64_t SOME_ID = 99;
constexpr std::uint8_t NO_OPERATION = 6;
constexpr std::uint8_t NO_SOURCE = 4;
constexpr std::uint64_t BOOL_TWO = 2;
constexpr std::size_t LAST_RESERVED = 7;
constexpr std::size_t SHORT_RECORD = 8;
constexpr std::uint64_t RECORDS_SKIPPED = 3;
constexpr std::uint64_t REQUESTS_DRAINED = 3;

scada_event_record event(std::uint16_t kind = SCADA_EVENT_AUDIT,
                         std::uint16_t length = PAYLOAD_LENGTH) {
    scada_event_record record{};
    record.time_ns = scada::realtime_ns();
    record.kind = kind;
    record.payload_len = length;
    record.source_handle = SCADA_INVALID_HANDLE;
    std::ranges::fill(
        std::span(record.payload)
            .first(std::min<std::size_t>(length, SCADA_EVENT_PAYLOAD_MAX)),
        PAYLOAD_BYTE);
    return record;
}

scada_write_request request(std::uint8_t operation = SCADA_OP_DIRECT) {
    scada_write_request record{};
    record.target_handle = 1;
    record.operation = operation;
    record.source = SCADA_SOURCE_OPERATOR;
    record.user_id = USER;
    if (operation != SCADA_OP_CANCEL && operation != SCADA_OP_READ_NOW) {
        record.value_type = SCADA_TYPE_UINT64;
        record.value = VALUE;
    }
    if (operation == SCADA_OP_OPERATE || operation == SCADA_OP_CANCEL) {
        record.select_request_id = 1;
    }
    record.expiry_mono_ns = scada::monotonic_ns() + EXPIRY_NS;
    return record;
}

scada_write_result result(std::uint64_t request_id,
                          std::uint8_t status = SCADA_RESULT_OK) {
    scada_write_result record{};
    record.request_id = request_id;
    record.status = status;
    record.completed_time_ns = scada::realtime_ns();
    return record;
}

template <typename Record>
int post(Queue& queue, const Record& record) {
    return queue.post(std::as_bytes(std::span(&record, 1)));
}

template <typename Record>
int next(Queue& queue, Record& record, int timeout_ms = 0) {
    return queue.next(std::as_writable_bytes(std::span(&record, 1)),
                      timeout_ms);
}

// The post of `record` to `queue` returns `expected`.
template <typename Record>
::testing::AssertionResult posts(Queue& queue, const Record& record,
                                 int expected = SCADA_OK) {
    const int code = post(queue, record);
    if (code != expected) {
        return ::testing::AssertionFailure()
               << "post returned " << code << ", not " << expected;
    }
    return ::testing::AssertionSuccess();
}

// `count` events posted to `queue` are accepted.
::testing::AssertionResult posts_events(Queue& queue, std::uint64_t count) {
    for (std::uint64_t i = 0; i < count; ++i) {
        if (const int code = post(queue, event()); code != SCADA_OK) {
            return ::testing::AssertionFailure()
                   << "post " << i << " returned " << code;
        }
    }
    return ::testing::AssertionSuccess();
}

// The next look at `queue`, without waiting, returns `expected`.
template <typename Record>
::testing::AssertionResult gives(Queue& queue, int expected) {
    Record record{};
    const int code = next(queue, record);
    if (code != expected) {
        return ::testing::AssertionFailure()
               << "next returned " << code << ", not " << expected;
    }
    return ::testing::AssertionSuccess();
}

enum class Ack : std::uint8_t { YES, NO };

// `queue` gives the event `seq` now, which is then acknowledged if `ack`.
::testing::AssertionResult takes_event(Queue& queue, std::uint64_t seq,
                                       Ack ack = Ack::YES) {
    scada_event_record record{};
    if (const int code = next(queue, record); code != SCADA_OK) {
        return ::testing::AssertionFailure() << "next returned " << code;
    }
    if (record.seq != seq || record.payload_len != PAYLOAD_LENGTH ||
        record.payload[0] != PAYLOAD_BYTE) {
        return ::testing::AssertionFailure()
               << "event " << record.seq << ", not " << seq;
    }
    if (ack == Ack::YES && queue.ack() != SCADA_OK) {
        return ::testing::AssertionFailure() << "the ack failed";
    }
    return ::testing::AssertionSuccess();
}

// `queue` gives the events `first` to `last - 1` in order, each
// acknowledged, and then nothing.
::testing::AssertionResult takes_events(Queue& queue, std::uint64_t first,
                                        std::uint64_t last) {
    for (std::uint64_t seq = first; seq < last; ++seq) {
        if (const ::testing::AssertionResult taken = takes_event(queue, seq);
            !taken) {
            return taken;
        }
    }
    return gives<scada_event_record>(queue, SCADA_E_TIMEOUT);
}

// `count` results posted to `queue` are accepted.
::testing::AssertionResult posts_results(Queue& queue, std::uint64_t count) {
    for (std::uint64_t i = 0; i < count; ++i) {
        if (const int code = post(queue, result(i)); code != SCADA_OK) {
            return ::testing::AssertionFailure()
                   << "post " << i << " returned " << code;
        }
    }
    return ::testing::AssertionSuccess();
}

// `queue` gives the request `request_id` now.
::testing::AssertionResult takes_request(Queue& queue,
                                         std::uint64_t request_id) {
    scada_write_request record{};
    if (const int code = next(queue, record); code != SCADA_OK) {
        return ::testing::AssertionFailure() << "next returned " << code;
    }
    if (record.request_id != request_id) {
        return ::testing::AssertionFailure()
               << "request " << record.request_id << ", not " << request_id;
    }
    return ::testing::AssertionSuccess();
}

// `queue` gives the answers `status` to the requests of `cmd.io.web` at
// `positions`, in order, each acknowledged, and then nothing.
::testing::AssertionResult takes_answers(
    Queue& queue, std::uint8_t status,
    const std::vector<std::uint64_t>& positions) {
    for (const std::uint64_t position : positions) {
        scada_write_result record{};
        if (const int code = next(queue, record); code != SCADA_OK) {
            return ::testing::AssertionFailure()
                   << "next returned " << code << " for " << position;
        }
        if (record.request_id != request_id_of(CMD_WEB, position) ||
            record.status != status || queue.ack() != SCADA_OK) {
            return ::testing::AssertionFailure()
                   << "answer " << int{record.status} << " to "
                   << record.request_id << " for " << position;
        }
    }
    return gives<scada_write_result>(queue, SCADA_E_TIMEOUT);
}

using RequestChange = std::function<void(scada_write_request&)>;

// Every request `request()` changed by one of `changes` is refused at post.
::testing::AssertionResult refuses_all(
    Queue& queue, const std::vector<RequestChange>& changes) {
    for (std::size_t i = 0; i < changes.size(); ++i) {
        scada_write_request record = request();
        changes[i](record);
        if (const int code = post(queue, record); code != SCADA_E_INVALID) {
            return ::testing::AssertionFailure()
                   << "change " << i << " gave " << code;
        }
    }
    return ::testing::AssertionSuccess();
}

class Queues : public scada::tag::testing::AttachFixture {
protected:
    // scada-tagd; `io`, a driver with its events and the results of the
    // requests of `web`; `web`, which posts requests and consumes the
    // events and results; `late`, which produces events and never attaches.
    // `cmd.io.solo` has no result queue.
    [[nodiscard]] static TagSet queue_tag_set() {
        TagSet set = tag_set();
        set.processes.push_back({.name = "late", .uid = uid()});
        set.queues = {
            {.name = "events.io",
             .producer = "io",
             .consumer = "web",
             .capacity = CAPACITY},
            {.name = "cmd.io.web",
             .producer = "web",
             .consumer = "io",
             .capacity = CAPACITY},
            {.name = "result.io.web",
             .producer = "io",
             .consumer = "web",
             .capacity = CAPACITY},
            {.name = "cmd.io.solo",
             .producer = "web",
             .consumer = "io",
             .capacity = CAPACITY},
            {.name = "events.late", .producer = "late", .consumer = "web"},
        };
        return set;
    }

    void SetUp() override {
        AttachFixture::SetUp();
        start_host(queue_tag_set());
        web_ = attached("web");
        io_ = attached("io");
        ASSERT_NE(io_, nullptr);
        ASSERT_NE(web_, nullptr);
        // A region that was missing when it was asked for is looked for
        // again only once a second (section 11.2): `web` asked for the
        // queues of `io` before `io` had created them.
        look_again(*web_);
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

    // The side of `name` that `process` has, or null with a failed
    // expectation.
    [[nodiscard]] static QueuePtr open(Attachment& process,
                                       std::string_view name) {
        QueuePtr queue;
        EXPECT_EQ(Queue::open(process, name, queue), SCADA_OK) << name;
        return queue;
    }

    // Drops the attachment of `process` and attaches it again, as a
    // restart does; every queue handle of it must be closed first.
    void restart(Attached& process, std::string_view name) {
        process.reset();
        process = attached(name);
        ASSERT_NE(process, nullptr);
    }

    // Makes `process` look again at once at the regions of other processes
    // it asked for, as scada_tag_notify does once a second. Each call is a
    // later time than the one before, so that every call looks.
    void look_again(Attachment& process) {
        look_ahead_ns_ += 2 * PEER_RETRY_INTERVAL_NS;
        static_cast<void>(process.peers().refresh(
            process.directory(), scada::monotonic_ns() + look_ahead_ns_));
    }

    // Makes the heartbeat of `process` older than its timeout.
    void stop_heartbeat(const std::string& process) const {
        const MappedRegion proc = map_file("proc." + process);
        store_shared(shared_at<ProcHeader>(proc.bytes(), sizeof(RegionHeader))
                         .heartbeat_mono_ns,
                     scada::monotonic_ns() - LONG_AGO_NS,
                     std::memory_order_release);
    }

    // Sets the state of the `proc` region of `process`, as its attach or
    // stop does.
    void set_state(const std::string& process, ProcessState state) const {
        const MappedRegion proc = map_file("proc." + process);
        store_shared(
            shared_at<ProcHeader>(proc.bytes(), sizeof(RegionHeader)).state,
            to_raw(state), std::memory_order_release);
    }

    // Sets the cursor of `queue_id` in the `proc` region of `process`.
    void set_cursor(const std::string& process, std::size_t queue_id,
                    std::uint64_t value) const {
        const MappedRegion proc = map_file("proc." + process);
        std::uint64_t* cursor = cursor_field(proc.bytes(), queue_id);
        ASSERT_NE(cursor, nullptr);
        store_shared(*cursor, value, std::memory_order_release);
    }

    // Changes the record at `position` of the queue region `name` with
    // `change`, as damage or another writer would.
    template <typename Record>
    void change_record(const std::string& name, std::uint64_t position,
                       void (*change)(Record&)) const {
        const MappedRegion region = map_file(name);
        const QueueHeader& header = queue_header(region.bytes());
        RecordWords words;
        load_record(region.bytes(), header.capacity, header.record_size,
                    position, words);
        auto record =
            copy_from<Record>(std::as_bytes(std::span(words.words)), 0);
        change(record);
        RecordWords changed;
        copy_to(std::as_writable_bytes(std::span(changed.words)), 0, record);
        store_record(region.bytes(), header.capacity, header.record_size,
                     position, changed);
    }

    [[nodiscard]] Attached& io() { return io_; }
    [[nodiscard]] Attached& web() { return web_; }

private:
    Attached io_;
    Attached web_;
    std::int64_t look_ahead_ns_ = 0;
};

// Conformance test 16, normal operation: no loss, no duplication, no
// reordering, through many rounds of a small queue; ids are the positions.
TEST_F(Queues, EventsArriveInOrderOnceEachWithTheirPositionAsId) {
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    std::uint64_t received = 0;
    for (std::uint64_t sent = 0; sent < MANY; sent += BATCH) {
        ASSERT_TRUE(posts_events(*out, BATCH));
        ASSERT_TRUE(takes_events(*reader, received, received + BATCH));
        received += BATCH;
    }
    EXPECT_GE(received, MANY);
}

// Conformance test 16: FULL when full; one ack makes one place.
TEST_F(Queues, APostToAFullQueueIsRefusedUntilARecordIsAcknowledged) {
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    ASSERT_TRUE(posts_events(*out, CAPACITY));
    EXPECT_TRUE(posts(*out, event(), SCADA_E_FULL));
    // Delivered is not processed: the place is still taken.
    ASSERT_TRUE(takes_event(*reader, 0, Ack::NO));
    EXPECT_TRUE(posts(*out, event(), SCADA_E_FULL));
    ASSERT_EQ(reader->ack(), SCADA_OK);
    EXPECT_TRUE(posts(*out, event()));
    EXPECT_TRUE(posts(*out, event(), SCADA_E_FULL));
}

// At least once: until the ack, the same record is delivered again.
TEST_F(Queues, ARecordIsDeliveredAgainUntilItIsAcknowledged) {
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    ASSERT_TRUE(posts_events(*out, 2));
    EXPECT_TRUE(takes_event(*reader, 0, Ack::NO));
    EXPECT_TRUE(takes_event(*reader, 0));
    EXPECT_EQ(reader->ack(), SCADA_E_INVALID);  // nothing waits for an ack
    EXPECT_TRUE(takes_events(*reader, 1, 2));
}

// Conformance test 16: redelivery after a consumer crash before the ack.
TEST_F(Queues, ARecordNotAcknowledgedBeforeARestartIsDeliveredAgain) {
    const QueuePtr out = open(*io(), "events.io");
    ASSERT_TRUE(posts_events(*out, 2));
    {
        const QueuePtr reader = open(*web(), "events.io");
        ASSERT_TRUE(takes_event(*reader, 0));
        ASSERT_TRUE(takes_event(*reader, 1, Ack::NO));
    }
    restart(web(), "web");
    const QueuePtr reader = open(*web(), "events.io");
    EXPECT_EQ(reader->stats().skipped, 0U);
    EXPECT_TRUE(takes_events(*reader, 1, 2));
}

// Conformance tests 16 and 19: PEER_DOWN when the consumer is stopped or
// has a stale heartbeat; nothing is queued then.
TEST_F(Queues, APostIsRefusedWhileTheConsumerIsDown) {
    const QueuePtr out = open(*io(), "events.io");
    web().reset();  // a clean stop: down at once
    EXPECT_TRUE(posts(*out, event(), SCADA_E_PEER_DOWN));
    web() = attached("web");
    ASSERT_TRUE(posts(*out, event()));
    stop_heartbeat("web");
    EXPECT_TRUE(posts(*out, event(), SCADA_E_PEER_DOWN));
    ASSERT_EQ(web()->heartbeat(), SCADA_OK);
    ASSERT_TRUE(posts(*out, event()));
    const QueuePtr reader = open(*web(), "events.io");
    // The refused posts left nothing behind.
    EXPECT_TRUE(takes_events(*reader, 0, 2));
}

// Conformance test 16: PEER_DOWN when the consumer has no region.
TEST_F(Queues, AConsumerWithoutARegionIsDown) {
    web().reset();
    std::filesystem::remove(generation_dir() / "proc.web");
    restart(io(), "io");
    const QueuePtr out = open(*io(), "events.io");
    EXPECT_TRUE(posts(*out, event(), SCADA_E_PEER_DOWN));
}

// Section 9.2, last paragraph: a new `proc` region starts at the end.
TEST_F(Queues, ANewProcRegionStartsAtTheEndAndReportsWhatItSkipped) {
    const QueuePtr out = open(*io(), "events.io");
    ASSERT_TRUE(posts_events(*out, RECORDS_SKIPPED));
    web().reset();
    std::filesystem::remove(generation_dir() / "proc.web");
    web() = attached("web");
    ASSERT_NE(web(), nullptr);
    // The producer still maps the removed file, whose process stopped.
    EXPECT_TRUE(posts(*out, event(), SCADA_E_PEER_DOWN));
    look_again(*io());
    const QueuePtr reader = open(*web(), "events.io");
    EXPECT_EQ(reader->stats().skipped, RECORDS_SKIPPED);
    EXPECT_TRUE(gives<scada_event_record>(*reader, SCADA_E_TIMEOUT));
    ASSERT_TRUE(posts(*out, event()));
    EXPECT_TRUE(takes_events(*reader, RECORDS_SKIPPED, RECORDS_SKIPPED + 1));
}

// Conformance test 18: ids continue after a producer restart.
TEST_F(Queues, IdsContinueAfterAProducerRestart) {
    {
        const QueuePtr out = open(*io(), "events.io");
        ASSERT_TRUE(posts_events(*out, 2));
    }
    restart(io(), "io");
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    ASSERT_TRUE(posts_events(*out, 1));
    EXPECT_TRUE(takes_events(*reader, 0, 3));
}

// Conformance test 18: a cursor out of range is rejected, not followed.
TEST_F(Queues, ACursorOutOfRangeIsRejectedOnBothSides) {
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    ASSERT_TRUE(posts_events(*out, 1));
    set_cursor("web", EVENTS_IO, 2);  // beyond write_seq
    EXPECT_TRUE(posts(*out, event(), SCADA_E_PEER_DOWN));
    EXPECT_TRUE(gives<scada_event_record>(*reader, SCADA_E_LAYOUT));
    EXPECT_FALSE(reader->ready());
    set_cursor("web", EVENTS_IO, 0);
    EXPECT_TRUE(takes_events(*reader, 0, 1));
}

// Conformance test 18: a `write_seq` out of range is rejected.
TEST_F(Queues, AWriteSeqTooFarAheadIsRejected) {
    const QueuePtr reader = open(*web(), "events.io");
    {
        const MappedRegion region = map_file("events.io");
        store_shared(queue_header(region.bytes()).write_seq,
                     std::uint64_t{CAPACITY} + 1, std::memory_order_release);
    }
    EXPECT_TRUE(gives<scada_event_record>(*reader, SCADA_E_LAYOUT));
    const QueuePtr out = open(*io(), "events.io");
    EXPECT_TRUE(posts(*out, event(), SCADA_E_PEER_DOWN));
}

// Issue #53, point 1: scada-tagd applies the range rule to every queue at
// each liveness check and reports a queue that became broken, once, and
// again when it is no longer broken.
TEST_F(Queues, TagdReportsAQueueWhoseCursorIsOutOfRange) {
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    ASSERT_TRUE(posts_events(*out, 1));
    EXPECT_TRUE(host().check(scada::monotonic_ns()).queues.empty());

    set_cursor("web", EVENTS_IO, 2);  // beyond write_seq
    const CheckReport broken = host().check(scada::monotonic_ns());
    ASSERT_EQ(broken.queues.size(), 1U);
    EXPECT_EQ(broken.queues[0].queue, "events.io");
    EXPECT_EQ(broken.queues[0].event, QueueEvent::BROKEN);
    EXPECT_FALSE(broken.queues[0].replaced);
    EXPECT_EQ(broken.queues[0].cursor, 2U);
    EXPECT_EQ(broken.queues[0].write_seq, 1U);
    EXPECT_EQ(broken.queues[0].capacity, CAPACITY);
    // Reported once, not at every check.
    EXPECT_TRUE(host().check(scada::monotonic_ns()).queues.empty());

    set_cursor("web", EVENTS_IO, 0);
    const CheckReport repaired = host().check(scada::monotonic_ns());
    ASSERT_EQ(repaired.queues.size(), 1U);
    EXPECT_EQ(repaired.queues[0].event, QueueEvent::NO_LONGER_BROKEN);
    EXPECT_EQ(repaired.queues[0].cursor, 0U);
    EXPECT_EQ(repaired.queues[0].write_seq, 1U);
}

// Review of PR #68: a consumer whose `proc` region was just created holds
// cursor 0 until its attach sets the cursors to `write_seq`. A check in
// that window must not report the queue broken: it is judged only while
// the consumer is RUNNING. Fails without that rule.
TEST_F(Queues, TagdJudgesNoQueueOfAConsumerThatIsNotRunning) {
    const QueuePtr reader = open(*web(), "events.io");
    {
        const MappedRegion region = map_file("events.io");
        store_shared(queue_header(region.bytes()).write_seq,
                     std::uint64_t{CAPACITY} + 1, std::memory_order_release);
    }
    set_cursor("web", EVENTS_IO, 0);
    set_state("web", ProcessState::STARTING);
    EXPECT_TRUE(host().check(scada::monotonic_ns()).queues.empty());
    set_state("web", ProcessState::STOPPING);
    EXPECT_TRUE(host().check(scada::monotonic_ns()).queues.empty());
    // The same pair with the consumer running is broken.
    set_state("web", ProcessState::RUNNING);
    const CheckReport report = host().check(scada::monotonic_ns());
    ASSERT_EQ(report.queues.size(), 1U);
    EXPECT_EQ(report.queues[0].event, QueueEvent::BROKEN);
}

TEST_F(Queues, TagdReportsAWriteSeqTooFarAhead) {
    const QueuePtr reader = open(*web(), "events.io");
    {
        const MappedRegion region = map_file("events.io");
        store_shared(queue_header(region.bytes()).write_seq,
                     std::uint64_t{CAPACITY} + 1, std::memory_order_release);
    }
    const CheckReport report = host().check(scada::monotonic_ns());
    ASSERT_EQ(report.queues.size(), 1U);
    EXPECT_EQ(report.queues[0].queue, "events.io");
    EXPECT_EQ(report.queues[0].event, QueueEvent::BROKEN);
    EXPECT_EQ(report.queues[0].cursor, 0U);
    EXPECT_EQ(report.queues[0].write_seq, std::uint64_t{CAPACITY} + 1);
}

// A consumer that found its queue file replaced stores the all-ones cursor
// (decision D-40); scada-tagd reports the queue broken as replaced.
TEST_F(Queues, TagdReportsAReplacedQueueAsBroken) {
    {
        QueuePtr reader = open(*web(), "events.io");
        // Shrunk in place while the consumer has it mapped: the consumer
        // stops the queue (section 3.3) and stores the all-ones cursor.
        std::filesystem::resize_file(generation_dir() / "events.io",
                                     sizeof(RegionHeader));
        look_again(*web());
        ASSERT_TRUE(gives<scada_event_record>(*reader, SCADA_E_LAYOUT));
    }
    restart(io(), "io");  // creates the queue file anew
    const CheckReport report = host().check(scada::monotonic_ns());
    ASSERT_EQ(report.queues.size(), 1U);
    EXPECT_EQ(report.queues[0].event, QueueEvent::BROKEN);
    EXPECT_TRUE(report.queues[0].replaced);
    EXPECT_EQ(report.queues[0].cursor, DEAD_CURSOR);
}

// Condition C13: scada-tagd reports a queue file created anew with the new
// base, the old file's last `write_seq` it saw and the consumer's cursor,
// so that the records that may be lost, [cursor, old write_seq), reach the
// system log.
TEST_F(Queues, TagdReportsAQueueFileCreatedAnew) {
    {
        const QueuePtr out = open(*io(), "events.io");
        const QueuePtr reader = open(*web(), "events.io");
        ASSERT_TRUE(posts_events(*out, 3));
        ASSERT_TRUE(takes_event(*reader, 0));  // the cursor is 1
    }
    EXPECT_TRUE(host().check(scada::monotonic_ns()).queues.empty());
    std::filesystem::remove(generation_dir() / "events.io");
    restart(io(), "io");
    const CheckReport report = host().check(scada::monotonic_ns());
    ASSERT_EQ(report.queues.size(), 1U);
    const QueueChange& change = report.queues[0];
    EXPECT_EQ(change.event, QueueEvent::RECREATED);
    EXPECT_EQ(change.base_seq, std::optional<std::uint64_t>(1 + CAPACITY));
    EXPECT_EQ(change.old_write_seq, 3U);
    EXPECT_TRUE(change.cursor_known);
    EXPECT_EQ(change.cursor, 1U);
    const LostIds lost = lost_ids(change);
    EXPECT_TRUE(lost.bounded);
    EXPECT_EQ(lost.from, 1U);
    EXPECT_EQ(lost.until, 1 + CAPACITY);
    // Once, not at every check.
    EXPECT_TRUE(host().check(scada::monotonic_ns()).queues.empty());
}

// Review of PR #77, B1: the reproduction of the SCADA and Linux reviewers.
// Five events posted and one taken, the last check saw `write_seq` 3, and
// the consumer attached again and jumped to the new base before the check
// that finds the new file. The range starts at the cursor of the last
// check, not at the cursor now, and ends at the base, so that it bounds
// also the posts after that check.
TEST_F(Queues, TagdBoundsTheLostIdsAlsoWhenTheConsumerJumpedFirst) {
    {
        const QueuePtr out = open(*io(), "events.io");
        const QueuePtr reader = open(*web(), "events.io");
        ASSERT_TRUE(posts_events(*out, 3));
        ASSERT_TRUE(takes_event(*reader, 0));  // the cursor is 1
        EXPECT_TRUE(host().check(scada::monotonic_ns()).queues.empty());
        ASSERT_TRUE(posts_events(*out, 2));  // after the last check
    }
    std::filesystem::remove(generation_dir() / "events.io");
    restart(io(), "io");
    restart(web(), "web");  // jumps to the new base before the check
    const CheckReport report = host().check(scada::monotonic_ns());
    ASSERT_EQ(report.queues.size(), 1U);
    const QueueChange& change = report.queues[0];
    ASSERT_EQ(change.event, QueueEvent::RECREATED);
    EXPECT_EQ(change.cursor, 1U);
    const LostIds lost = lost_ids(change);
    EXPECT_TRUE(lost.bounded);
    EXPECT_EQ(lost.from, 1U);
    EXPECT_EQ(std::optional<std::uint64_t>(lost.until), change.base_seq);
    EXPECT_GE(lost.until - lost.from, 4U);  // the four lost events
}

// Without a base nothing bounds the posts after the last check, and the
// range is never inverted.
TEST(LostIds, AreUnboundedWithoutABaseAndNeverInverted) {
    constexpr std::uint64_t CURSOR_AFTER = 5;
    QueueChange change;
    change.event = QueueEvent::RECREATED;
    change.old_write_seq = 3;
    change.cursor = CURSOR_AFTER;
    change.cursor_known = true;
    const LostIds lost = lost_ids(change);
    EXPECT_FALSE(lost.bounded);
    EXPECT_EQ(lost.from, CURSOR_AFTER);
    EXPECT_EQ(lost.until, CURSOR_AFTER);
}

// Issue #53, review of the design: an events or command queue whose file
// is missing at two checks in a row while its producer is up is reported,
// once, and again when it is there again. One miss is not: a check may
// fall into a recreation.
TEST_F(Queues, TagdReportsAQueueFileMissingForMoreThanOneCheck) {
    std::filesystem::remove(generation_dir() / "events.io");
    EXPECT_TRUE(host().check(scada::monotonic_ns()).queues.empty());
    const CheckReport missing = host().check(scada::monotonic_ns());
    ASSERT_EQ(missing.queues.size(), 1U);
    EXPECT_EQ(missing.queues[0].queue, "events.io");
    EXPECT_EQ(missing.queues[0].event, QueueEvent::MISSING);
    EXPECT_TRUE(host().check(scada::monotonic_ns()).queues.empty());

    restart(io(), "io");  // creates the queue file anew
    const CheckReport found = host().check(scada::monotonic_ns());
    ASSERT_EQ(found.queues.size(), 1U);
    EXPECT_EQ(found.queues[0].queue, "events.io");
    EXPECT_EQ(found.queues[0].event, QueueEvent::FOUND);
}

// Review of PR #68: a fast restart of the producer after its queue file was
// deleted gives no report. Its old heartbeat is still fresh, so the first
// check counts the file missing; the attach sets the reused region
// STARTING, which is down, and creates the file, so the count never
// reaches two, however many checks the attach spans.
TEST_F(Queues, TagdReportsNoMissingFileAcrossAFastRestartOfTheProducer) {
    std::filesystem::remove(generation_dir() / "events.io");
    EXPECT_TRUE(host().check(scada::monotonic_ns()).queues.empty());
    // The attach of the restarted producer, before it creates the file.
    set_state("io", ProcessState::STARTING);
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(host().check(scada::monotonic_ns()).queues.empty());
    }
    restart(io(), "io");  // reuses its proc region, creates the queue file
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(host().check(scada::monotonic_ns()).queues.empty());
    }
}

// A producer that is down may not have created its queue files yet, and a
// result queue is not reported missing.
TEST_F(Queues, TagdReportsNoMissingFileOfAProducerThatIsDownOrOfAResultQueue) {
    std::filesystem::remove(generation_dir() / "result.io.web");
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(host().check(scada::monotonic_ns()).queues.empty());
    }
    stop_heartbeat("io");
    std::filesystem::remove(generation_dir() / "events.io");
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(host().check(scada::monotonic_ns()).queues.empty());
    }
}

// A queue whose files or consumer are missing is not judged, so it is never
// reported: the queue `events.late` has no producer, and nothing else is
// broken.
TEST_F(Queues, TagdReportsNoQueueThatIsMissingOrInRange) {
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    ASSERT_TRUE(posts_events(*out, CAPACITY));  // full is in range
    EXPECT_TRUE(host().check(scada::monotonic_ns()).queues.empty());
}

// The reads of scada-tagd, in the order of queue_pair_broken: the sides
// move between them. Each case is a pair in range at every moment, which a
// single read of `write_seq` (before or after the cursor) judges broken.
TEST(QueuePairBroken, TheReadsOfAThirdProcessGiveNoFalseReport) {
    // Cursor read at 0; then the consumer takes 3 and the producer posts 7.
    // `write_seq` read once, after: 7 - 0 > 4.
    EXPECT_FALSE(queue_pair_broken(0, 0, 7, CAPACITY));
    // `write_seq` read at 2; then the producer posts 3 and the consumer
    // takes 4, and the cursor is read at 4. `write_seq` read once, before:
    // 4 > 2.
    EXPECT_FALSE(queue_pair_broken(2, 4, 5, CAPACITY));
    // A full queue and an empty one, read while nothing moves.
    EXPECT_FALSE(queue_pair_broken(CAPACITY, 0, CAPACITY, CAPACITY));
    EXPECT_FALSE(queue_pair_broken(3, 3, 3, CAPACITY));
}

TEST(QueuePairBroken, APairThatDoesNotMoveIsJudgedByTheRangeRule) {
    EXPECT_TRUE(queue_pair_broken(1, 2, 1, CAPACITY));  // cursor ahead
    EXPECT_TRUE(queue_pair_broken(CAPACITY + 1, 0, CAPACITY + 1, CAPACITY));
    EXPECT_TRUE(queue_pair_broken(0, DEAD_CURSOR, 0, CAPACITY));
    const std::uint64_t top = std::numeric_limits<std::uint64_t>::max() - 1;
    EXPECT_TRUE(queue_pair_broken(top, 0, top, CAPACITY));  // no overflow
    EXPECT_FALSE(queue_pair_broken(top, top - CAPACITY, top, CAPACITY));
}

// No false report while both sides run: a producer and a consumer pass
// records through a queue of CAPACITY places while the check runs as often
// as it can. It checks the reads of the running code, but it cannot catch
// a wrong read order: tried against a single read of `write_seq`, it still
// passed, since the window between two loads is too short to hit. The
// explicit interleavings of QueuePairBroken above catch that.
// The end of a wait of WAIT_LIMIT_MS from now.
std::int64_t wait_deadline() {
    return scada::monotonic_ns() +
           (std::int64_t{WAIT_LIMIT_MS} * NANOSECONDS_PER_MILLISECOND);
}

// Posts `count` events to `out`, retrying while it is full, for at most
// WAIT_LIMIT_MS. Adds a failure and stops at any other refusal.
void post_all(Queue& out, std::uint64_t count) {
    const std::int64_t deadline = wait_deadline();
    std::uint64_t posted = 0;
    while (posted < count) {
        if (scada::monotonic_ns() > deadline) {
            ADD_FAILURE() << "posted " << posted << " in the time";
            return;
        }
        const int code = post(out, event());
        if (code == SCADA_OK) {
            ++posted;
        } else if (code != SCADA_E_FULL) {
            ADD_FAILURE() << "post returned " << code;
            return;
        }
    }
}

// Takes and acknowledges the events 0 to `count - 1` from `reader`, in
// order, for at most WAIT_LIMIT_MS, then sets `done`. Adds a failure and
// stops at anything else.
void take_all(Queue& reader, std::uint64_t count, std::atomic<bool>& done) {
    const std::int64_t deadline = wait_deadline();
    std::uint64_t taken = 0;
    while (taken < count) {
        if (scada::monotonic_ns() > deadline) {
            ADD_FAILURE() << "took " << taken << " in the time";
            break;
        }
        scada_event_record record{};
        const int code = next(reader, record, 0);
        if (code == SCADA_OK &&
            (record.seq != taken || reader.ack() != SCADA_OK)) {
            ADD_FAILURE() << "record " << record.seq;
            break;
        }
        if (code != SCADA_OK && code != SCADA_E_TIMEOUT) {
            ADD_FAILURE() << "next returned " << code;
            break;
        }
        taken += code == SCADA_OK ? 1 : 0;
    }
    done.store(true, std::memory_order_release);
}

TEST_F(Queues, TagdReportsNoBrokenQueueWhileBothSidesRun) {
    constexpr std::uint64_t RECORDS = 20'000;
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    std::atomic<bool> done{false};
    std::thread producer([&] { post_all(*out, RECORDS); });
    std::thread consumer([&] { take_all(*reader, RECORDS, done); });
    std::uint64_t checks = 0;
    std::uint64_t reports = 0;
    while (!done.load(std::memory_order_acquire)) {
        reports += host().check(scada::monotonic_ns()).queues.size();
        ++checks;
        // The main loops of the two processes: a run longer than the
        // heartbeat timeout (ThreadSanitizer) must not see the consumer
        // down, which refuses posts with SCADA_E_PEER_DOWN.
        static_cast<void>(io()->heartbeat());
        static_cast<void>(web()->heartbeat());
    }
    producer.join();
    consumer.join();
    EXPECT_EQ(reports, 0U) << "in " << checks << " checks";
    EXPECT_GT(checks, 0U);
}

// The CPU time the calling thread has used so far, in ns.
std::int64_t thread_cpu_ns() {
    timespec now{};
    EXPECT_EQ(::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now), 0);
    constexpr std::int64_t NANOSECONDS_PER_SECOND = 1'000'000'000;
    return (std::int64_t{now.tv_sec} * NANOSECONDS_PER_SECOND) + now.tv_nsec;
}

// Review of PR #56: a stopped queue must not make its consumer spin. A wait
// of 300 ms on it, through next and through a wait set, sleeps in slices
// and uses about 1 ms of CPU time; a spinning wait uses nearly all 300 ms.
// The CPU time of the waiting thread does not grow when the machine is
// busy (only the wall time does), so the bound of half the wait holds on a
// loaded runner and in a slow sanitizer build.
TEST_F(Queues, AWaitOnAStoppedQueueDoesNotSpin) {
    constexpr int WAIT_MS = 300;
    constexpr std::int64_t CPU_LIMIT_NS = 150 * NANOSECONDS_PER_MILLISECOND;
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    ASSERT_TRUE(posts_events(*out, 1));
    set_cursor("web", EVENTS_IO, 2);  // beyond write_seq: stopped
    WaitSet set(*web());
    ASSERT_EQ(set.add_queue(*reader), SCADA_OK);

    scada_event_record record{};
    std::int64_t start = thread_cpu_ns();
    const std::int64_t called = scada::monotonic_ns();
    EXPECT_EQ(next(*reader, record, WAIT_MS), SCADA_E_LAYOUT);
    // It waited out its timeout, so that a loop over it does not spin.
    EXPECT_GE(scada::monotonic_ns() - called,
              std::int64_t{WAIT_MS} * NANOSECONDS_PER_MILLISECOND);
    EXPECT_LT(thread_cpu_ns() - start, CPU_LIMIT_NS) << "next spun";
    start = thread_cpu_ns();
    EXPECT_EQ(set.wait(WAIT_MS), SCADA_E_TIMEOUT);
    EXPECT_LT(thread_cpu_ns() - start, CPU_LIMIT_NS) << "the wait set spun";
}

// A record that is not valid is passed over with SCADA_E_LAYOUT.
TEST_F(Queues, ARecordThatIsNotValidIsPassedOver) {
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    ASSERT_TRUE(posts_events(*out, 3));
    change_record<EventRecord>("events.io", 0, [](EventRecord& record) {
        record.payload_len = SCADA_EVENT_PAYLOAD_MAX + 1;
    });
    change_record<EventRecord>("events.io", 1, [](EventRecord& record) {
        record.seq = NOT_A_POSITION;
    });
    EXPECT_TRUE(gives<scada_event_record>(*reader, SCADA_E_LAYOUT));
    EXPECT_TRUE(gives<scada_event_record>(*reader, SCADA_E_LAYOUT));
    EXPECT_TRUE(takes_events(*reader, 2, 3));
}

TEST_F(Queues, AnEventThatIsNotValidIsRefusedAtPost) {
    const QueuePtr events = open(*io(), "events.io");
    EXPECT_TRUE(posts(*events, event(0), SCADA_E_INVALID));
    EXPECT_TRUE(posts(*events, event(SCADA_EVENT_SYSTEM + 1), SCADA_E_INVALID));
    EXPECT_TRUE(
        posts(*events, event(SCADA_EVENT_ALARM, SCADA_EVENT_PAYLOAD_MAX)));
    EXPECT_TRUE(posts(*events,
                      event(SCADA_EVENT_ALARM, SCADA_EVENT_PAYLOAD_MAX + 1),
                      SCADA_E_INVALID));
    const std::array<std::byte, SHORT_RECORD> short_record{};
    EXPECT_EQ(events->post(short_record), SCADA_E_INVALID);
}

TEST_F(Queues, ARequestThatIsNotValidIsRefusedAtPost) {
    const QueuePtr cmd = open(*web(), "cmd.io.web");
    const std::vector<RequestChange> changes{
        [](scada_write_request& req) { req.operation = 0; },
        [](scada_write_request& req) { req.operation = NO_OPERATION; },
        [](scada_write_request& req) { req.source = NO_SOURCE; },
        [](scada_write_request& req) { req.value_type = SCADA_TYPE_STRING; },
        [](scada_write_request& req) { req.value_type = 0; },
        [](scada_write_request& req) {
            req.value_type = SCADA_TYPE_BOOL;
            req.value = BOOL_TWO;
        },
        // AUTO and REMOTE with a user id.
        [](scada_write_request& req) { req.source = SCADA_SOURCE_AUTO; },
        [](scada_write_request& req) { req.source = SCADA_SOURCE_REMOTE; },
        // OPERATOR without a user (issue #58).
        [](scada_write_request& req) { req.user_id = 0; },
        [](scada_write_request& req) { req.select_request_id = 1; },
        [](scada_write_request& req) { req.reserved0 = 1; },
        [](scada_write_request& req) { req.reserved1 = 1; },
        [](scada_write_request& req) {
            std::span(req.reserved)[LAST_RESERVED] = 1;
        },
        [](scada_write_request& req) {
            req.expiry_mono_ns = scada::monotonic_ns() - 1;
        },
        [](scada_write_request& req) {
            req.expiry_mono_ns =
                scada::monotonic_ns() + MAX_REQUEST_LIFETIME_NS + EXPIRY_NS;
        },
    };
    EXPECT_TRUE(refuses_all(*cmd, changes));
    scada_write_request operate = request(SCADA_OP_OPERATE);
    EXPECT_TRUE(posts(*cmd, operate));
    operate.select_request_id = 0;
    EXPECT_TRUE(posts(*cmd, operate, SCADA_E_INVALID));
    EXPECT_TRUE(posts(*cmd, request(SCADA_OP_READ_NOW)));
}

// Issue #58: an operator request without a user, written into the region
// past the check at post, is passed over when taken and counted; the next
// request is delivered.
TEST_F(Queues, AnOperatorRequestWithoutAUserIsPassedOverWhenTaken) {
    const QueuePtr out = open(*web(), "cmd.io.web");
    const QueuePtr reader = open(*io(), "cmd.io.web");
    ASSERT_TRUE(posts(*out, request()));
    ASSERT_TRUE(posts(*out, request()));
    change_record<WriteRequest>(
        "cmd.io.web", 0, [](WriteRequest& record) { record.user_id = 0; });
    EXPECT_TRUE(gives<scada_write_request>(*reader, SCADA_E_LAYOUT));
    EXPECT_EQ(reader->stats().invalid, 1U);
    EXPECT_TRUE(takes_request(*reader, request_id_of(CMD_WEB, 1)));
    // The requester gets no answer for it and sees it expire.
    const QueuePtr results = open(*web(), "result.io.web");
    EXPECT_TRUE(gives<scada_write_result>(*results, SCADA_E_TIMEOUT));
}

// The same request found by the drain at attach is counted invalid, not
// aborted, and gets no answer; the others are answered ABORTED.
TEST_F(Queues, AnOperatorRequestWithoutAUserIsCountedInvalidByTheDrain) {
    const QueuePtr out = open(*web(), "cmd.io.web");
    ASSERT_TRUE(posts(*out, request()));
    ASSERT_TRUE(posts(*out, request()));
    change_record<WriteRequest>(
        "cmd.io.web", 0, [](WriteRequest& record) { record.user_id = 0; });
    restart(io(), "io");
    const QueuePtr reader = open(*io(), "cmd.io.web");
    EXPECT_EQ(reader->stats().invalid, 1U);
    EXPECT_EQ(reader->stats().aborted, 1U);
    const QueuePtr results = open(*web(), "result.io.web");
    EXPECT_TRUE(takes_answers(*results, SCADA_RESULT_ABORTED, {1}));
}

TEST_F(Queues, AResultThatIsNotValidIsRefusedAtPost) {
    const QueuePtr results = open(*io(), "result.io.web");
    EXPECT_TRUE(posts(*results, result(1, 0), SCADA_E_INVALID));
    EXPECT_TRUE(
        posts(*results, result(1, SCADA_RESULT_ABORTED + 1), SCADA_E_INVALID));
    scada_write_result reserved = result(1);
    std::span(reserved.reserved0)[1] = 1;
    EXPECT_TRUE(posts(*results, reserved, SCADA_E_INVALID));
    EXPECT_TRUE(posts(*results, result(1)));
}

// Section 9.4: the library sets the request id and the time of the post.
TEST_F(Queues, ARequestGetsItsIdAndPostTimeFromTheLibrary) {
    const QueuePtr out = open(*web(), "cmd.io.web");
    const QueuePtr reader = open(*io(), "cmd.io.web");
    scada_write_request record = request();
    record.request_id = SOME_ID;
    record.posted_mono_ns = 1;
    const std::int64_t before = scada::monotonic_ns();
    ASSERT_TRUE(posts(*out, record));
    scada_write_request got{};
    ASSERT_EQ(next(*reader, got), SCADA_OK);
    EXPECT_EQ(got.request_id, request_id_of(CMD_WEB, 0));
    EXPECT_TRUE(got.posted_mono_ns >= before &&
                got.posted_mono_ns <= scada::monotonic_ns());
    EXPECT_EQ(got.value, VALUE);
    ASSERT_TRUE(posts(*out, record));
    EXPECT_TRUE(takes_request(*reader, (std::uint64_t{CMD_WEB} << 48U) | 1U));
}

// Conformance test 17: a request taken is never delivered again, also not
// after a restart of the driver; there is no ack, and no answer.
TEST_F(Queues, ARequestTakenIsNeverDeliveredAgain) {
    const QueuePtr out = open(*web(), "cmd.io.web");
    ASSERT_TRUE(posts(*out, request()));
    {
        const QueuePtr reader = open(*io(), "cmd.io.web");
        ASSERT_TRUE(takes_request(*reader, request_id_of(CMD_WEB, 0)));
        EXPECT_EQ(reader->ack(), SCADA_E_INVALID);
        EXPECT_TRUE(gives<scada_write_request>(*reader, SCADA_E_TIMEOUT));
    }
    restart(io(), "io");
    const QueuePtr reader = open(*io(), "cmd.io.web");
    EXPECT_EQ(reader->stats().aborted, 0U);
    EXPECT_TRUE(gives<scada_write_request>(*reader, SCADA_E_TIMEOUT));
    const QueuePtr results = open(*web(), "result.io.web");
    EXPECT_TRUE(gives<scada_write_result>(*results, SCADA_E_TIMEOUT));
}

// Conformance test 17: requests still queued when a driver attaches are
// answered ABORTED and not executed; without a result queue the answer is
// dropped.
TEST_F(Queues, RequestsQueuedBeforeTheDriverAttachesAreAnsweredAborted) {
    const QueuePtr out = open(*web(), "cmd.io.web");
    const QueuePtr solo = open(*web(), "cmd.io.solo");
    ASSERT_TRUE(posts(*out, request()));
    ASSERT_TRUE(posts(*out, request(SCADA_OP_SELECT)));
    ASSERT_TRUE(posts(*solo, request()));
    restart(io(), "io");
    const QueuePtr reader = open(*io(), "cmd.io.web");
    const QueuePtr solo_reader = open(*io(), "cmd.io.solo");
    EXPECT_EQ(reader->stats().aborted + solo_reader->stats().aborted,
              REQUESTS_DRAINED);
    // Without a result queue the answer was dropped and counted.
    EXPECT_EQ(solo_reader->stats().answers_dropped, 1U);
    EXPECT_TRUE(gives<scada_write_request>(*reader, SCADA_E_TIMEOUT));
    EXPECT_TRUE(gives<scada_write_request>(*solo_reader, SCADA_E_TIMEOUT));
    const QueuePtr results = open(*web(), "result.io.web");
    EXPECT_TRUE(takes_answers(*results, SCADA_RESULT_ABORTED, {0, 1}));
}

// After the drain, new requests are delivered.
TEST_F(Queues, RequestsAfterTheDrainAreDelivered) {
    const QueuePtr out = open(*web(), "cmd.io.web");
    ASSERT_TRUE(posts(*out, request()));
    restart(io(), "io");
    const QueuePtr reader = open(*io(), "cmd.io.web");
    ASSERT_TRUE(posts(*out, request()));
    EXPECT_TRUE(takes_request(*reader, request_id_of(CMD_WEB, 1)));
}

// Conformance test 17: an expired or over-long request is answered EXPIRED
// and not delivered.
TEST_F(Queues, AnExpiredOrOverLongRequestIsAnsweredExpired) {
    const QueuePtr out = open(*web(), "cmd.io.web");
    const QueuePtr reader = open(*io(), "cmd.io.web");
    const QueuePtr results = open(*web(), "result.io.web");
    ASSERT_TRUE(posts(*out, request()));
    ASSERT_TRUE(posts(*out, request()));
    ASSERT_TRUE(posts(*out, request()));
    change_record<WriteRequest>("cmd.io.web", 0, [](WriteRequest& record) {
        record.expiry_mono_ns = record.posted_mono_ns;  // in the past now
    });
    change_record<WriteRequest>("cmd.io.web", 1, [](WriteRequest& record) {
        record.expiry_mono_ns =
            record.posted_mono_ns + MAX_REQUEST_LIFETIME_NS + 1;
    });
    EXPECT_TRUE(takes_request(*reader, request_id_of(CMD_WEB, 2)));
    EXPECT_TRUE(takes_answers(*results, SCADA_RESULT_EXPIRED, {0, 1}));
}

// Decision D-40, condition 3: an EXPIRED answer that cannot be posted is
// dropped and counted, and the request is still not delivered.
TEST_F(Queues, AnExpiredRequestIsNotDeliveredWhenItsAnswerCannotBePosted) {
    const QueuePtr out = open(*web(), "cmd.io.web");
    const QueuePtr reader = open(*io(), "cmd.io.web");
    const QueuePtr results = open(*io(), "result.io.web");
    // The result queue is full.
    ASSERT_TRUE(posts_results(*results, CAPACITY));
    ASSERT_TRUE(posts(*out, request()));
    change_record<WriteRequest>("cmd.io.web", 0, [](WriteRequest& record) {
        record.expiry_mono_ns = record.posted_mono_ns;
    });
    EXPECT_TRUE(gives<scada_write_request>(*reader, SCADA_E_TIMEOUT));
    const scada_queue_stats_t stats = reader->stats();
    EXPECT_EQ(stats.expired, 1U);
    EXPECT_EQ(stats.answers_dropped, 1U);
    EXPECT_EQ(stats.delivered, 0U);
}

TEST_F(Queues, AnExpiredRequestWithoutAResultQueueIsDroppedAndCounted) {
    const QueuePtr out = open(*web(), "cmd.io.solo");
    const QueuePtr reader = open(*io(), "cmd.io.solo");
    ASSERT_TRUE(posts(*out, request()));
    change_record<WriteRequest>("cmd.io.solo", 0, [](WriteRequest& record) {
        record.expiry_mono_ns = record.posted_mono_ns;
    });
    EXPECT_TRUE(gives<scada_write_request>(*reader, SCADA_E_TIMEOUT));
    EXPECT_EQ(reader->stats().answers_dropped, 1U);
}

// Decision D-40, condition 2: a driver that dies after the cursor moved on
// and before the EXPIRED answer gives no answer at all, never the request
// again nor ABORTED after it.
TEST_F(Queues, ACrashBetweenTakingAndAnsweringGivesNoSecondAnswer) {
    const QueuePtr out = open(*web(), "cmd.io.web");
    ASSERT_TRUE(posts(*out, request()));
    change_record<WriteRequest>("cmd.io.web", 0, [](WriteRequest& record) {
        record.expiry_mono_ns = record.posted_mono_ns;
    });
    // What the dying driver left: the cursor past the request, no answer.
    set_cursor("io", CMD_WEB, 1);
    restart(io(), "io");
    const QueuePtr reader = open(*io(), "cmd.io.web");
    EXPECT_EQ(reader->stats().aborted, 0U);
    EXPECT_TRUE(gives<scada_write_request>(*reader, SCADA_E_TIMEOUT));
    const QueuePtr results = open(*web(), "result.io.web");
    EXPECT_TRUE(gives<scada_write_result>(*results, SCADA_E_TIMEOUT));
}

// A producer that dies in the middle of a post leaves a record that no
// consumer sees: `write_seq` publishes it, and was not stored.
TEST_F(Queues, AProducerKilledInTheMiddleOfAPostLeavesNothing) {
    const QueuePtr reader = open(*web(), "events.io");
    {
        const MappedRegion region = map_file("events.io");
        const QueueHeader& header = queue_header(region.bytes());
        RecordWords half;
        half.words.fill(~std::uint64_t{0});
        store_record(region.bytes(), header.capacity, header.record_size, 0,
                     half);
    }
    EXPECT_TRUE(gives<scada_event_record>(*reader, SCADA_E_TIMEOUT));
    restart(io(), "io");
    const QueuePtr out = open(*io(), "events.io");
    ASSERT_TRUE(posts(*out, event()));
    EXPECT_TRUE(takes_events(*reader, 0, 1));
}

// The decision is made on the copy: a request changed in the region after
// it was taken changes nothing for the driver and is not taken again.
TEST_F(Queues, ARequestChangedAfterItWasTakenIsNotTakenAgain) {
    const QueuePtr out = open(*web(), "cmd.io.web");
    const QueuePtr reader = open(*io(), "cmd.io.web");
    ASSERT_TRUE(posts(*out, request()));
    scada_write_request taken{};
    ASSERT_EQ(next(*reader, taken), SCADA_OK);
    change_record<WriteRequest>("cmd.io.web", 0, [](WriteRequest& record) {
        record.value = 0;
        record.expiry_mono_ns = record.posted_mono_ns;
    });
    EXPECT_EQ(taken.value, VALUE);
    EXPECT_TRUE(gives<scada_write_request>(*reader, SCADA_E_TIMEOUT));
    EXPECT_EQ(reader->stats().expired, 0U);
}

// True once the thread `tid` of this process sleeps in the kernel (state
// S in its stat file), looked at until WAIT_LIMIT_MS has passed.
bool sleeps_soon(pid_t tid) {
    const std::string path = "/proc/self/task/" + std::to_string(tid) + "/stat";
    const std::int64_t deadline =
        scada::monotonic_ns() +
        (std::int64_t{WAIT_LIMIT_MS} * NANOSECONDS_PER_MILLISECOND);
    while (scada::monotonic_ns() < deadline) {
        std::ifstream file(path);
        std::string stat;
        std::getline(file, stat);
        // The state follows the name in parentheses, which may hold spaces.
        const std::size_t name_end = stat.rfind(')');
        if (name_end != std::string::npos && name_end + 2 < stat.size() &&
            stat[name_end + 2] == 'S') {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

// Decision D-40, condition 3: the EXPIRED answer is posted without a wake,
// and a requester already waiting for it still gets it. Nothing ever wakes
// the requester here, so only the look at the end of each wait slice can
// deliver the answer: its arrival shows that look. The length of the slice
// is shown by the wait tests of the change rings (`Changes.*`).
TEST_F(Queues, AnExpiredAnswerArrivesWithoutNotify) {
    const QueuePtr out = open(*web(), "cmd.io.web");
    const QueuePtr reader = open(*io(), "cmd.io.web");
    const QueuePtr results = open(*web(), "result.io.web");
    ASSERT_TRUE(posts(*out, request()));
    change_record<WriteRequest>("cmd.io.web", 0, [](WriteRequest& record) {
        record.expiry_mono_ns = record.posted_mono_ns;
    });
    std::atomic<pid_t> tid{0};
    std::atomic<int> code{-1};
    std::thread requester([&results, &code, &tid] {
        tid.store(::gettid());
        scada_write_result answer{};
        code.store(next(*results, answer, WAIT_LIMIT_MS));
    });
    while (tid.load() == 0) {
        std::this_thread::yield();
    }
    // The requester is blocked in its wait before the answer is posted, so
    // the answer cannot be seen by its first look.
    EXPECT_TRUE(sleeps_soon(tid.load()));
    EXPECT_TRUE(gives<scada_write_request>(*reader, SCADA_E_TIMEOUT));
    requester.join();
    EXPECT_EQ(code.load(), SCADA_OK);
    EXPECT_EQ(reader->stats().expired, 1U);
}

// The result of `call` run in a thread of its own, which the thread marks
// of the library tell from the test's thread.
template <typename Call>
bool in_other_thread(Call call) {
    bool result = false;
    std::thread other([&result, &call] { result = call(); });
    other.join();
    return result;
}

// Review of PR #56: a thread that was refused, and closes and opens its
// handle again, cannot take the answer queue from the thread that takes the
// requests. Before the fix its close let go of the other thread's binding.
TEST_F(Queues, ClosingAHandleDoesNotUnbindAnotherThread) {
    const QueuePtr requests = open(*io(), "cmd.io.web");
    // The test's thread takes the requests, so it posts their answers.
    ASSERT_TRUE(gives<scada_write_request>(*requests, SCADA_E_TIMEOUT));
    Attachment& driver = *io();
    EXPECT_FALSE(in_other_thread([&driver] {
        {
            const QueuePtr results = open(driver, "result.io.web");
        }  // closed without posting: no stop, also in a debug build
        const QueuePtr results = open(driver, "result.io.web");
        return driver.bind_queue_thread(RESULT_WEB);
    }));
    // Still the test's thread, also once a producer side was opened and
    // closed here.
    { const QueuePtr results = open(driver, "result.io.web"); }
    EXPECT_TRUE(driver.bind_queue_thread(RESULT_WEB));
}

// A thread that runs `call` and then stays alive until `finish`, so that
// no thread made meanwhile gets its thread mark (a thread that has ended
// may leave its mark to a new one, which would hide a binding).
class ParkedThread {
public:
    template <typename Call>
    explicit ParkedThread(Call call)
        : thread_([this, call]() mutable {
              call();
              done_.store(true);
              while (!finish_.load()) {
                  std::this_thread::yield();
              }
          }) {}
    ~ParkedThread() { finish(); }
    ParkedThread(const ParkedThread&) = delete;
    ParkedThread& operator=(const ParkedThread&) = delete;
    ParkedThread(ParkedThread&&) = delete;
    ParkedThread& operator=(ParkedThread&&) = delete;

    // Waits until `call` has returned.
    void wait_done() const {
        while (!done_.load()) {
            std::this_thread::yield();
        }
    }
    // Lets the thread end and joins it.
    void finish() {
        finish_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    std::atomic<bool> done_{false};
    std::atomic<bool> finish_{false};
    std::thread thread_;
};

// Review of PR #56: the binding of an answer queue ends when the last side
// that posts to it closes, whichever thread closes it. Here a worker binds
// it by taking requests, and the owning thread closes the worker's handle;
// another thread can then answer, while the worker still lives.
TEST_F(Queues, AnAnswerQueueIsFreeOnceItsLastSideIsClosedByAnyThread) {
    Attachment& driver = *io();
    QueuePtr requests = open(driver, "cmd.io.web");
    const ParkedThread worker([&requests] {
        static_cast<void>(
            gives<scada_write_request>(*requests, SCADA_E_TIMEOUT));
    });
    worker.wait_done();
    EXPECT_FALSE(driver.bind_queue_thread(RESULT_WEB));  // the worker's
    requests.reset();  // closed by the owning thread, not by the worker
    EXPECT_TRUE(in_other_thread(
        [&driver] { return driver.bind_queue_thread(RESULT_WEB); }));
}

// The same when each thread closes its own handle, the bound producer side
// first and the side that takes the requests last.
TEST_F(Queues, AnAnswerQueueIsFreeWhenItsSidesCloseInEitherOrder) {
    Attachment& driver = *io();
    QueuePtr results = open(driver, "result.io.web");
    QueuePtr requests = open(driver, "cmd.io.web");
    const ParkedThread poster([&results] {
        static_cast<void>(post(*results, result(1)));  // binds `poster`
        results.reset();  // the side that takes the requests is still open
    });
    poster.wait_done();
    EXPECT_FALSE(driver.bind_queue_thread(RESULT_WEB));  // still `poster`'s
    const ParkedThread taker([&requests] {
        static_cast<void>(
            gives<scada_write_request>(*requests, SCADA_E_TIMEOUT));
        requests.reset();  // the last side that posts to the answer queue
    });
    taker.wait_done();
    EXPECT_TRUE(in_other_thread(
        [&driver] { return driver.bind_queue_thread(RESULT_WEB); }));
}

// Review of PR #56: a close that finds no posting side open clears the
// binding in one step with that test, so a side opened by another thread
// at the same time keeps the binding it takes. One thread opens and closes
// the side that takes the requests; the other opens the answer queue,
// binds it, and checks that it stays bound while its side is open. Without
// the step, the close cleared the binding under the open side within a few
// thousand rounds.
TEST_F(Queues, AQueueStaysBoundWhileASideOpensAsAnotherCloses) {
    constexpr int ROUNDS = 50'000;
    Attachment& driver = *io();
    std::atomic<bool> stop{false};
    std::atomic<int> unbound{0};
    std::jthread closer([&driver, &stop] {
        while (!stop.load()) {
            QueuePtr requests;
            if (Queue::open(driver, "cmd.io.web", requests) != SCADA_OK) {
                return;
            }
        }
    });
    std::jthread poster([&driver, &stop, &unbound] {
        for (int round = 0; round < ROUNDS; ++round) {
            QueuePtr results;
            if (Queue::open(driver, "result.io.web", results) != SCADA_OK ||
                !driver.bind_queue_thread(RESULT_WEB)) {
                unbound.fetch_add(1);
                break;
            }
            if (!driver.queue_thread_recorded(RESULT_WEB)) {
                unbound.fetch_add(1);
            }
        }
        stop.store(true);
    });
    poster.join();
    closer.join();
    EXPECT_EQ(unbound.load(), 0);
}

// Decision D-40, condition 4: the ABORTED answers of the drain at attach do
// not bind the attaching thread; the driver's thread can post its answers.
TEST_F(Queues, TheDrainAtAttachBindsNoThread) {
    const QueuePtr out = open(*web(), "cmd.io.web");
    ASSERT_TRUE(posts(*out, request()));
    restart(io(), "io");  // drains in the test's thread
    Attachment& driver = *io();
    EXPECT_TRUE(in_other_thread(
        [&driver] { return driver.bind_queue_thread(RESULT_WEB); }));
}

// Decision D-40, condition 3: a thread that takes requests while another
// thread holds the answer queue gets no answer posted; the request is not
// delivered, and the drop is counted, in every build.
TEST_F(Queues, AnAnswerOfAnUnboundThreadIsDroppedAndCounted) {
    const QueuePtr out = open(*web(), "cmd.io.web");
    const QueuePtr reader = open(*io(), "cmd.io.web");
    Attachment& driver = *io();
    ASSERT_TRUE(in_other_thread(
        [&driver] { return driver.bind_queue_thread(RESULT_WEB); }));
    ASSERT_TRUE(posts(*out, request()));
    change_record<WriteRequest>("cmd.io.web", 0, [](WriteRequest& record) {
        record.expiry_mono_ns = record.posted_mono_ns;
    });
    EXPECT_TRUE(gives<scada_write_request>(*reader, SCADA_E_TIMEOUT));
    const scada_queue_stats_t stats = reader->stats();
    EXPECT_EQ(stats.expired, 1U);
    EXPECT_EQ(stats.answers_dropped, 1U);
    const QueuePtr results = open(*web(), "result.io.web");
    EXPECT_TRUE(gives<scada_write_result>(*results, SCADA_E_TIMEOUT));
}

#ifdef NDEBUG
// Decision D-40, condition 4, in a build without NDEBUG checks: a second
// thread that posts to a queue is refused and writes nothing. A build with
// them stops the process instead, which no unit test can show.
TEST_F(Queues, ASecondPostingThreadIsRefusedInAReleaseBuild) {
    const QueuePtr out = open(*io(), "result.io.web");
    const QueuePtr reader = open(*web(), "result.io.web");
    ASSERT_TRUE(posts(*out, result(1)));
    std::atomic<int> code{-1};
    std::thread other([&out, &code] { code.store(post(*out, result(2))); });
    other.join();
    EXPECT_EQ(code.load(), SCADA_E_INVALID);
    scada_write_result first{};
    ASSERT_EQ(next(*reader, first), SCADA_OK);
    EXPECT_EQ(first.request_id, 1U);
    ASSERT_EQ(reader->ack(), SCADA_OK);
    EXPECT_TRUE(gives<scada_write_result>(*reader, SCADA_E_TIMEOUT));
}
#endif

TEST_F(Queues, TheCountsOfASideAreReported) {
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    ASSERT_TRUE(posts_events(*out, 2));
    ASSERT_TRUE(takes_event(*reader, 0, Ack::NO));
    ASSERT_TRUE(takes_event(*reader, 0));
    const scada_queue_stats_t produced = out->stats();
    const scada_queue_stats_t consumed = reader->stats();
    EXPECT_EQ(produced.posted, 2U);
    EXPECT_EQ(produced.producer, 1U);
    EXPECT_EQ(consumed.delivered, 2U);  // one record twice
    EXPECT_EQ(consumed.queue_id, EVENTS_IO);
    EXPECT_EQ(consumed.kind, to_raw(QueueKind::EVENTS));
}

// How a queue file can be lost inside a generation (decision D-40).
enum class Damage : std::uint8_t { UNLINK, TRUNCATE, HEADER };

// Decision D-40: a queue file that is removed, shrunk or damaged and then
// created anew by its producer inside the generation kills the queue. The
// consumer gets SCADA_E_LAYOUT after its next look, without a signal from
// the old mapping; the producer gets SCADA_E_PEER_DOWN; nothing is
// accepted; the counts show it.
class ReplacedQueue : public Queues {
protected:
    void replace(Damage damage) {
        {
            const QueuePtr out = open(*io(), "events.io");
            const QueuePtr reader = open(*web(), "events.io");
            ASSERT_TRUE(posts_events(*out, 2));
            ASSERT_TRUE(takes_event(*reader, 0));  // the cursor is 1
        }
        const std::filesystem::path file = generation_dir() / "events.io";
        switch (damage) {
            case Damage::UNLINK:
                std::filesystem::remove(file);
                break;
            case Damage::TRUNCATE:
                std::filesystem::resize_file(file, sizeof(RegionHeader));
                break;
            case Damage::HEADER: {
                const MappedRegion region = map_file("events.io");
                store_shared(
                    shared_at<RegionHeader>(region.bytes(), 0).generation,
                    std::uint64_t{0}, std::memory_order_relaxed);
                break;
            }
        }
        // `io` drops its mapping and creates the file anew.
        restart(io(), "io");
        look_again(*web());
    }

    // The queue is dead on both sides and nothing gets through.
    ::testing::AssertionResult dead() {
        const QueuePtr out = open(*io(), "events.io");
        const QueuePtr reader = open(*web(), "events.io");
        if (out->stats().replaced != 1) {
            return ::testing::AssertionFailure() << "the producer counted no "
                                                    "file created anew";
        }
        if (const ::testing::AssertionResult refused =
                gives<scada_event_record>(*reader, SCADA_E_LAYOUT);
            !refused) {
            return refused;
        }
        if (const ::testing::AssertionResult refused =
                posts(*out, event(), SCADA_E_PEER_DOWN);
            !refused) {
            return refused;
        }
        if (reader->stats().replaced != 1 || out->stats().posted != 0) {
            return ::testing::AssertionFailure() << "counts";
        }
        return gives<scada_event_record>(*reader, SCADA_E_LAYOUT);
    }

    // Issue #53: the consumer first delivers what the old file holds (the
    // event 1 it had not taken), then moves to the new file, whose base is
    // the larger of the producer's high-water mark (2) and the consumer's
    // cursor at the recreation plus the capacity (1 + 4). The positions
    // 2 to 4 were never given and are counted skipped; the new events
    // start at 5. Nothing accepted is lost and no id repeats.
    ::testing::AssertionResult followed() {
        constexpr std::uint64_t BASE = 1 + CAPACITY;
        const QueuePtr out = open(*io(), "events.io");
        const QueuePtr reader = open(*web(), "events.io");
        if (out->stats().replaced != 1) {
            return ::testing::AssertionFailure() << "the producer counted no "
                                                    "file created anew";
        }
        // Full until the consumer has moved on: a refusal, not a loss.
        if (const ::testing::AssertionResult full =
                posts(*out, event(), SCADA_E_FULL);
            !full) {
            return full;
        }
        if (const ::testing::AssertionResult old = takes_event(*reader, 1);
            !old) {
            return old;
        }
        // One place is free until the consumer's next look moves it on.
        if (const ::testing::AssertionResult accepted = posts_events(*out, 1);
            !accepted) {
            return accepted;
        }
        if (const ::testing::AssertionResult taken = takes_event(*reader, BASE);
            !taken) {
            return taken;
        }
        if (const ::testing::AssertionResult accepted = posts_events(*out, 2);
            !accepted) {
            return accepted;
        }
        if (const ::testing::AssertionResult taken =
                takes_events(*reader, BASE + 1, BASE + 3);
            !taken) {
            return taken;
        }
        const scada_queue_stats_t stats = reader->stats();
        if (stats.replaced != 1 || stats.skipped != BASE - 2) {
            return ::testing::AssertionFailure()
                   << "replaced " << stats.replaced << ", skipped "
                   << stats.skipped;
        }
        return ::testing::AssertionSuccess();
    }
};

TEST_F(ReplacedQueue, AnUnlinkedQueueFileIsFollowedWithoutLoss) {
    replace(Damage::UNLINK);
    EXPECT_TRUE(followed());
}

// Shrunk in place, then created anew under the name: no look by name sees
// the shrink, so the consumer checks the size of the file it still reads
// before it moves on, and stops the queue instead of reading it (SIGBUS).
TEST_F(ReplacedQueue, ATruncatedQueueFileKillsTheQueue) {
    replace(Damage::TRUNCATE);
    EXPECT_TRUE(dead());
}

// The records of a file whose RegionHeader was damaged in place are still
// read, each checked; then the consumer moves to the new file.
TEST_F(ReplacedQueue, AQueueFileWithADamagedHeaderIsFollowedWithoutLoss) {
    replace(Damage::HEADER);
    EXPECT_TRUE(followed());
}

// A queue file removed while the producer runs: both keep their mapping
// of the unlinked file, so every record still arrives (issue #53; before,
// the queue died).
TEST_F(Queues, AQueueFileRemovedUnderARunningProducerStillDelivers) {
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    ASSERT_TRUE(posts_events(*out, 1));
    std::filesystem::remove(generation_dir() / "events.io");
    look_again(*web());
    EXPECT_TRUE(takes_event(*reader, 0));
    ASSERT_TRUE(posts_events(*out, 1));
    EXPECT_TRUE(takes_events(*reader, 1, 2));
    EXPECT_EQ(reader->stats().replaced, 0U);
}

// The case of the committee's question (decision D-40), turned around by
// the base sequence (issue #53): the consumer's cursor is 0, so the
// producer's high-water mark (2) is the base. What the producer posts
// before the consumer's next look lands in the new file at 2 and is
// delivered after the backlog of the old one.
TEST_F(Queues, AReplacedQueueFileLosesNothingAlsoWithTheCursorAtZero) {
    {
        const QueuePtr out = open(*io(), "events.io");
        ASSERT_TRUE(posts_events(*out, 2));  // a backlog, not taken
    }
    const QueuePtr reader = open(*web(), "events.io");  // the old file
    std::filesystem::remove(generation_dir() / "events.io");
    restart(io(), "io");
    const QueuePtr out = open(*io(), "events.io");
    EXPECT_EQ(out->stats().replaced, 0U);  // the cursor was 0
    ASSERT_TRUE(posts_events(*out, 1));    // the former window: accepted
    look_again(*web());
    EXPECT_TRUE(takes_events(*reader, 0, 3));
    EXPECT_EQ(out->stats().posted, 1U);
    EXPECT_EQ(reader->stats().delivered, 3U);
    EXPECT_EQ(reader->stats().skipped, 0U);
}

// The cursor slot of `queue_id` in the `proc` region of `process`: a
// cursor for a queue it consumes, the high-water mark for one it produces.
std::uint64_t slot_of(const MappedRegion& proc, std::size_t queue_id) {
    const std::uint64_t* field = cursor_field(proc.bytes(), queue_id);
    return field == nullptr ? 0
                            : load_shared(*field, std::memory_order_acquire);
}

// Condition C4: the producer's high-water mark follows every accepted post
// and no refused one.
TEST_F(Queues, TheProducerKeepsItsHighWaterMark) {
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    ASSERT_TRUE(posts_events(*out, CAPACITY));
    EXPECT_TRUE(posts(*out, event(), SCADA_E_FULL));
    EXPECT_TRUE(posts(*out, event(SCADA_EVENT_AUDIT, SHORT_RECORD * 100),
                      SCADA_E_INVALID));
    EXPECT_EQ(slot_of(map_file("proc.io"), EVENTS_IO), CAPACITY);
}

// A producer killed between the store of its high-water mark and that of
// `write_seq` burned the id: a file it creates anew starts above it, so
// the id is never given twice (condition C4).
TEST_F(Queues, AnIdBurnedByACrashIsNotGivenAgain) {
    {
        const QueuePtr out = open(*io(), "events.io");
        ASSERT_TRUE(posts_events(*out, 1));
    }
    set_cursor("io", EVENTS_IO, 2);  // the mark of a post that never ended
    std::filesystem::remove(generation_dir() / "events.io");
    restart(io(), "io");
    const MappedRegion region = map_file("events.io");
    EXPECT_EQ(queue_base(region.bytes()), std::optional<std::uint64_t>(2));
}

// A high-water mark above the consumer's cursor plus the capacity cannot
// be (every id is below that): it is damage, and the base comes from the
// cursor alone.
TEST_F(Queues, AnImplausibleHighWaterMarkIsIgnored) {
    {
        const QueuePtr out = open(*io(), "events.io");
        const QueuePtr reader = open(*web(), "events.io");
        ASSERT_TRUE(posts_events(*out, 1));
        ASSERT_TRUE(takes_event(*reader, 0));
    }
    constexpr std::uint64_t IMPLAUSIBLE_MARK = std::uint64_t{1} << 40U;
    set_cursor("io", EVENTS_IO, IMPLAUSIBLE_MARK);
    std::filesystem::remove(generation_dir() / "events.io");
    restart(io(), "io");
    const MappedRegion region = map_file("events.io");
    EXPECT_EQ(queue_base(region.bytes()),
              std::optional<std::uint64_t>(1 + CAPACITY));
}

// A new generation's first queue file starts at 0 (issue #17), also when
// the consumer attached first.
TEST_F(Queues, AFirstQueueFileStartsAtZero) {
    const MappedRegion region = map_file("events.io");
    EXPECT_EQ(queue_base(region.bytes()), std::optional<std::uint64_t>(0));
    EXPECT_EQ(queue_write_seq(region.bytes()), 0U);
}

// Condition C1: an ack after the cursor moved on (a jump to a new file's
// base) is refused and moves nothing back.
TEST_F(Queues, AnAckAfterTheCursorMovedOnIsRefused) {
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    ASSERT_TRUE(posts_events(*out, 1));
    ASSERT_TRUE(takes_event(*reader, 0, Ack::NO));
    set_cursor("web", EVENTS_IO, 1 + CAPACITY);  // as the jump stores it
    EXPECT_EQ(reader->ack(), SCADA_E_INVALID);
    EXPECT_EQ(slot_of(map_file("proc.web"), EVENTS_IO), 1 + CAPACITY);
}

// Condition C2: a new file whose base is below what the file in use holds
// would repeat ids; the consumer stops the queue.
TEST_F(Queues, ANewFileWithABaseBelowTheOldWriteSeqKillsTheQueue) {
    {
        const QueuePtr out = open(*io(), "events.io");
        ASSERT_TRUE(posts_events(*out, 2));
    }
    const QueuePtr reader = open(*web(), "events.io");
    std::filesystem::remove(generation_dir() / "events.io");
    restart(io(), "io");
    {
        const MappedRegion region = map_file("events.io");
        store_shared(queue_header(region.bytes()).base_seq, std::uint64_t{1},
                     std::memory_order_release);
    }
    look_again(*web());
    EXPECT_TRUE(gives<scada_event_record>(*reader, SCADA_E_LAYOUT));
    EXPECT_EQ(reader->stats().replaced, 1U);
    EXPECT_EQ(slot_of(map_file("proc.web"), EVENTS_IO), DEAD_CURSOR);
}

// A new file made by an older build has no base: the consumer cannot place
// its cursor in it and stops the queue, as decision D-40 does.
TEST_F(Queues, ANewFileWithoutABaseKillsTheQueue) {
    const QueuePtr reader = open(*web(), "events.io");
    std::filesystem::remove(generation_dir() / "events.io");
    restart(io(), "io");
    {
        const MappedRegion region = map_file("events.io");
        store_shared(queue_header(region.bytes()).flags, std::uint32_t{0},
                     std::memory_order_release);
    }
    look_again(*web());
    EXPECT_TRUE(gives<scada_event_record>(*reader, SCADA_E_LAYOUT));
    EXPECT_EQ(reader->stats().replaced, 1U);
}

// No position of the queue file `region` from `first` to below `base`
// holds a record an older consumer would deliver: each fails its id check
// as an event and its status check as a result.
::testing::AssertionResult no_record_below(const MappedRegion& region,
                                           std::uint64_t first,
                                           std::uint64_t base) {
    for (std::uint64_t position = first; position < base; ++position) {
        RecordWords words;
        load_record(region.bytes(), CAPACITY, sizeof(EventRecord), position,
                    words);
        const EventRecord record = event_of(words);
        if ((record.seq == position && event_valid(record)) ||
            result_valid(result_of(words))) {
            return ::testing::AssertionFailure() << "a record at " << position;
        }
    }
    return ::testing::AssertionSuccess();
}

// Condition C6: an older consumer, which knows no base, reads a new file
// from a cursor below its base. The positions below the base were never
// written, so each fails its id or status check and is passed over; none
// is delivered as a record.
TEST_F(Queues, AnOlderConsumerFindsNoRecordBelowTheBase) {
    {
        const QueuePtr out = open(*io(), "events.io");
        const QueuePtr reader = open(*web(), "events.io");
        ASSERT_TRUE(posts_events(*out, 1));
        ASSERT_TRUE(takes_event(*reader, 0));
    }
    std::filesystem::remove(generation_dir() / "events.io");
    restart(io(), "io");
    const QueuePtr out = open(*io(), "events.io");
    {
        // This build's consumer moves on, so that the producer has room.
        const QueuePtr reader = open(*web(), "events.io");
        look_again(*web());
        ASSERT_TRUE(gives<scada_event_record>(*reader, SCADA_E_TIMEOUT));
    }
    ASSERT_TRUE(posts_events(*out, 1));
    const MappedRegion region = map_file("events.io");
    const std::uint64_t base = queue_base(region.bytes()).value_or(0);
    ASSERT_EQ(base, 1 + CAPACITY);
    EXPECT_TRUE(no_record_below(region, 1, base));
}

// Case 3 of the design: a consumer that was not attached during the
// recreation restarts with its region kept, finds its cursor below the
// new base and jumps there; ids do not repeat.
TEST_F(Queues, AConsumerThatWasAwayJumpsToTheNewBase) {
    {
        const QueuePtr out = open(*io(), "events.io");
        const QueuePtr reader = open(*web(), "events.io");
        ASSERT_TRUE(posts_events(*out, 2));
        ASSERT_TRUE(takes_event(*reader, 0));
    }
    web().reset();
    std::filesystem::remove(generation_dir() / "events.io");
    restart(io(), "io");
    restart(web(), "web");
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    look_again(*io());
    ASSERT_TRUE(posts_events(*out, 1));
    // Base: the mark 2 against the cursor 1 plus the capacity.
    EXPECT_TRUE(takes_events(*reader, 1 + CAPACITY, 2 + CAPACITY));
    EXPECT_EQ(reader->stats().skipped, CAPACITY);  // at most: 1 was posted
}

// Condition C12: requests left in an old `cmd` file that the driver did
// not take before it went away are never executed and get no answer; the
// requester sees them expire. The drain at attach answers only the new
// file's requests.
TEST_F(Queues, RequestsLeftInAnOldCmdFileAreNeverExecuted) {
    {
        const QueuePtr requests = open(*web(), "cmd.io.web");
        look_again(*web());
        ASSERT_TRUE(posts(*requests, request()));
        ASSERT_TRUE(posts(*requests, request()));
    }
    io().reset();
    std::filesystem::remove(generation_dir() / "cmd.io.web");
    restart(web(), "web");  // creates `cmd.io.web` anew
    restart(io(), "io");
    const QueuePtr driver = open(*io(), "cmd.io.web");
    EXPECT_TRUE(gives<scada_write_request>(*driver, SCADA_E_TIMEOUT));
    EXPECT_EQ(driver->stats().aborted, 0U);
    EXPECT_EQ(driver->stats().delivered, 0U);
    EXPECT_EQ(driver->stats().skipped, 2U);  // at most: the mark is 2
    const QueuePtr answers = open(*web(), "result.io.web");
    look_again(*web());
    EXPECT_TRUE(gives<scada_write_result>(*answers, SCADA_E_TIMEOUT));
}

// Review of PR #77, B2: a file mapped again is the file in use. The name
// is gone for one look and back for the next: the look in between drops the
// mapping, the next maps the same file anew, and the consumer goes on.
TEST_F(Queues, TheSameFileMappedAgainIsTheFileInUse) {
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    ASSERT_TRUE(posts_events(*out, 1));
    const std::filesystem::path file = generation_dir() / "events.io";
    const std::filesystem::path away = generation_dir() / "away";
    std::filesystem::rename(file, away);
    look_again(*web());
    std::filesystem::rename(away, file);
    look_again(*web());
    EXPECT_TRUE(takes_event(*reader, 0));
    ASSERT_TRUE(posts_events(*out, 1));
    EXPECT_TRUE(takes_events(*reader, 1, 2));
    EXPECT_EQ(reader->stats().replaced, 0U);
}

// Review of PR #77, B5: the header of a new file is outside input. Each
// change makes the file one the consumer must not follow: it stops the
// queue, counts it and stores the all-ones cursor.
class BrokenNewFile : public Queues {
protected:
    // Creates `events.io` anew under a consumer that holds the old file,
    // changes its header with `change`, and expects the queue dead.
    ::testing::AssertionResult refused(void (*change)(QueueHeader&)) {
        {
            const QueuePtr out = open(*io(), "events.io");
            if (!posts_events(*out, 1)) {
                return ::testing::AssertionFailure() << "no post";
            }
        }
        const QueuePtr reader = open(*web(), "events.io");
        std::filesystem::remove(generation_dir() / "events.io");
        restart(io(), "io");
        {
            const MappedRegion region = map_file("events.io");
            change(queue_header(region.bytes()));
        }
        look_again(*web());
        if (!gives<scada_event_record>(*reader, SCADA_E_LAYOUT)) {
            return ::testing::AssertionFailure() << "the queue goes on";
        }
        if (reader->stats().replaced != 1 ||
            slot_of(map_file("proc.web"), EVENTS_IO) != DEAD_CURSOR) {
            return ::testing::AssertionFailure() << "not dead";
        }
        return ::testing::AssertionSuccess();
    }
};

TEST_F(BrokenNewFile, ABaseBeyondWriteSeqIsRefused) {
    EXPECT_TRUE(refused([](QueueHeader& header) {
        store_shared(
            header.base_seq,
            load_shared(header.write_seq, std::memory_order_relaxed) + 1,
            std::memory_order_release);
    }));
}

TEST_F(BrokenNewFile, AnAllOnesBaseIsRefused) {
    EXPECT_TRUE(refused([](QueueHeader& header) {
        store_shared(header.base_seq, DEAD_CURSOR, std::memory_order_release);
        store_shared(header.write_seq, DEAD_CURSOR, std::memory_order_release);
    }));
}

TEST_F(BrokenNewFile, AFlagBitThatIsNoneIsRefused) {
    EXPECT_TRUE(refused([](QueueHeader& header) {
        store_shared(header.flags, QUEUE_BASE_SET | (QUEUE_BASE_SET << 1U),
                     std::memory_order_release);
    }));
}

TEST_F(BrokenNewFile, AReservedFieldThatIsNotZeroIsRefused) {
    EXPECT_TRUE(refused([](QueueHeader& header) {
        store_shared(header.reserved0, std::uint32_t{1},
                     std::memory_order_release);
    }));
}

// The first file a consumer sees is checked too.
TEST_F(Queues, ABrokenFirstFileIsRefused) {
    web().reset();
    {
        const MappedRegion region = map_file("events.io");
        store_shared(queue_header(region.bytes()).reserved0, std::uint32_t{1},
                     std::memory_order_release);
    }
    restart(web(), "web");
    const QueuePtr reader = open(*web(), "events.io");
    EXPECT_TRUE(gives<scada_event_record>(*reader, SCADA_E_LAYOUT));
}

// N3: a base that leaves no room for ids creates the queue dead: base and
// `write_seq` all-ones, refused by the consumer, PEER_DOWN for the producer.
TEST_F(Queues, AFileWithoutRoomForIdsIsCreatedDead) {
    {
        const QueuePtr reader = open(*web(), "events.io");
        look_again(*web());
    }
    set_cursor("web", EVENTS_IO, DEAD_CURSOR - CAPACITY - 1);
    std::filesystem::remove(generation_dir() / "events.io");
    restart(io(), "io");
    const MappedRegion region = map_file("events.io");
    EXPECT_EQ(queue_write_seq(region.bytes()), DEAD_CURSOR);
    const QueuePtr out = open(*io(), "events.io");
    EXPECT_TRUE(posts(*out, event(), SCADA_E_PEER_DOWN));
}

// N2: the 17th file under one name is not mapped (MAX_MAPPINGS_PER_REGION);
// the consumer stops the queue.
TEST_F(Queues, TheSeventeenthFileUnderANameStopsTheQueue) {
    const QueuePtr reader = open(*web(), "events.io");
    for (std::uint32_t file = 1; file <= MAX_MAPPINGS_PER_REGION; ++file) {
        std::filesystem::remove(generation_dir() / "events.io");
        restart(io(), "io");
        look_again(*web());
        if (file < MAX_MAPPINGS_PER_REGION) {
            ASSERT_TRUE(gives<scada_event_record>(*reader, SCADA_E_TIMEOUT))
                << file;
        }
    }
    EXPECT_TRUE(gives<scada_event_record>(*reader, SCADA_E_LAYOUT));
    EXPECT_EQ(slot_of(map_file("proc.web"), EVENTS_IO), DEAD_CURSOR);
}

// Review of PR #77, B4 (condition C4): the high-water mark is stored before
// the record. A child posts with its queue mapping made read-only, so the
// post faults at the record: the mark must already hold the position after
// it, and `write_seq` must not have moved. With the mark stored after the
// record or after `write_seq`, the mark is still at the old position.
TEST_F(Queues, TheMarkIsStoredBeforeTheRecord) {
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    ASSERT_TRUE(posts_events(*out, 1));
    const std::span<std::byte> own = io()->queue(EVENTS_IO).own_region;
    // A plain fork, not a death test: the child shares the mappings and
    // never returns to the test.
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        static_cast<void>(::mprotect(own.data(), own.size(), PROT_READ));
        static_cast<void>(post(*out, event()));
        ::_exit(0);  // not reached: the post faults at the record
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    EXPECT_FALSE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
        << "the post did not fault";
    EXPECT_EQ(slot_of(map_file("proc.io"), EVENTS_IO), 2U);
    EXPECT_EQ(queue_write_seq(map_file("events.io").bytes()), 1U);
}

TEST_F(Queues, ResultsReachTheRequester) {
    const QueuePtr out = open(*io(), "result.io.web");
    const QueuePtr reader = open(*web(), "result.io.web");
    ASSERT_TRUE(posts(*out, result(request_id_of(CMD_WEB, 0))));
    EXPECT_TRUE(takes_answers(*reader, SCADA_RESULT_OK, {0}));
}

TEST_F(Queues, WhoMayOpenWhat) {
    QueuePtr queue;
    EXPECT_EQ(Queue::open(*io(), "events.none", queue), SCADA_E_NOT_FOUND);
    EXPECT_EQ(Queue::open(*io(), "Events.io", queue), SCADA_E_INVALID);
    EXPECT_EQ(Queue::open(*io(), "", queue), SCADA_E_INVALID);
    EXPECT_EQ(Queue::open(*io(), "events.late", queue), SCADA_E_NOT_OWNER);
    {
        const QueuePtr first = open(*io(), "events.io");
        EXPECT_EQ(Queue::open(*io(), "events.io", queue), SCADA_E_BUSY);
        // The other side is another handle, in another process.
        const QueuePtr other = open(*web(), "events.io");
    }
    EXPECT_EQ(Queue::open(*io(), "events.io", queue), SCADA_OK);
}

TEST_F(Queues, EachSideDoesOnlyItsOwnPart) {
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    EXPECT_TRUE(gives<scada_event_record>(*out, SCADA_E_INVALID));
    EXPECT_EQ(out->ack(), SCADA_E_INVALID);
    EXPECT_TRUE(posts(*reader, event(), SCADA_E_INVALID));
    scada_event_record record{};
    EXPECT_EQ(next(*reader, record, -1), SCADA_E_INVALID);
    std::array<std::byte, SHORT_RECORD> small{};
    EXPECT_EQ(reader->next(small, 0), SCADA_E_INVALID);
}

// A producer that has not attached yet: nothing to take, no error.
TEST_F(Queues, AQueueWhoseProducerNeverAttachedIsEmpty) {
    const QueuePtr reader = open(*web(), "events.late");
    EXPECT_TRUE(gives<scada_event_record>(*reader, SCADA_E_TIMEOUT));
    EXPECT_FALSE(reader->ready());
    EXPECT_FALSE(reader->waiter().has_value());
}

// Section 8.4: a consumer blocked in next is woken by notify.
TEST_F(Queues, ABlockedConsumerIsWokenByNotify) {
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    std::atomic<int> code{-1};
    std::thread consumer([&reader, &code] {
        scada_event_record record{};
        code.store(next(*reader, record, WAIT_LIMIT_MS));
    });
    ASSERT_TRUE(posts(*out, event()));
    EXPECT_EQ(io()->notify(), SCADA_OK);
    consumer.join();
    EXPECT_EQ(code.load(), SCADA_OK);
}

TEST_F(Queues, AWaitSetWakesForAQueue) {
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    WaitSet set(*web());
    EXPECT_EQ(set.add_queue(*out), SCADA_E_INVALID);  // a producer
    ASSERT_EQ(set.add_queue(*reader), SCADA_OK);
    EXPECT_EQ(set.add_queue(*reader), SCADA_E_INVALID);  // in a set already
    EXPECT_EQ(set.wait(0), SCADA_E_TIMEOUT);
    ASSERT_TRUE(posts(*out, event()));
    EXPECT_EQ(set.wait(0), SCADA_OK);
}

TEST_F(Queues, TheDescriptorOfAWaitSetBecomesReadableForAQueue) {
    const QueuePtr out = open(*io(), "events.io");
    const QueuePtr reader = open(*web(), "events.io");
    WaitSet set(*web());
    ASSERT_EQ(set.add_queue(*reader), SCADA_OK);
    int descriptor = -1;
    ASSERT_EQ(set.fd(descriptor), SCADA_OK);
    ASSERT_TRUE(posts(*out, event()));
    EXPECT_EQ(io()->notify(), SCADA_OK);
    pollfd ready{.fd = descriptor, .events = POLLIN, .revents = 0};
    EXPECT_EQ(::poll(&ready, 1, WAIT_LIMIT_MS), 1);
    std::uint64_t count = 0;
    EXPECT_EQ(::read(descriptor, &count, sizeof(count)),
              static_cast<ssize_t>(sizeof(count)));
    EXPECT_TRUE(takes_events(*reader, 0, 1));
}

TEST_F(Queues, ClosingAQueueTakesItOutOfItsWaitSet) {
    WaitSet set(*web());
    {
        const QueuePtr reader = open(*web(), "events.io");
        ASSERT_EQ(set.add_queue(*reader), SCADA_OK);
    }
    EXPECT_EQ(set.wait(0), SCADA_E_TIMEOUT);
}

// The record checks on their own: the five reasons of decision D-40 why a
// request must not be executed, at their limits.
TEST(QueueRecords, ARequestIsExpiredForEachOfFiveReasons) {
    constexpr std::int64_t NOW = 1'000'000'000'000;
    constexpr std::int64_t LIMIT = MAX_REQUEST_LIFETIME_NS;
    struct Case {
        std::int64_t posted;
        std::int64_t expiry;
        bool expired;
    };
    const std::vector<Case> cases{
        {.posted = NOW - 1, .expiry = NOW + 1, .expired = false},
        // 1. The expiry is not after now.
        {.posted = NOW - 1, .expiry = NOW, .expired = true},
        // 2. Posted after now.
        {.posted = NOW + 1, .expiry = NOW + 2, .expired = true},
        // 3. Expires before it was posted.
        {.posted = NOW, .expiry = NOW - 1, .expired = true},
        // 4. A lifetime longer than the limit.
        {.posted = NOW - 1, .expiry = NOW - 1 + LIMIT, .expired = false},
        {.posted = NOW - 1, .expiry = NOW + LIMIT, .expired = true},
        // 5. An expiry further than the limit ahead of now.
        {.posted = NOW, .expiry = NOW + LIMIT, .expired = false},
        {.posted = NOW, .expiry = NOW + LIMIT + 1, .expired = true},
    };
    std::size_t wrong = cases.size();
    for (std::size_t i = 0; i < cases.size(); ++i) {
        WriteRequest record;
        record.posted_mono_ns = cases[i].posted;
        record.expiry_mono_ns = cases[i].expiry;
        if (request_expired(record, NOW) != cases[i].expired) {
            wrong = i;
            break;
        }
    }
    EXPECT_EQ(wrong, cases.size()) << "the first case judged wrongly";
}

// Issue #58: a request carries a user exactly when its source is OPERATOR.
TEST(QueueRecords, ARequestCarriesAUserExactlyWhenItIsAnOperators) {
    constexpr std::uint32_t SOME_USER = 7;
    constexpr std::uint32_t HIGHEST_USER =
        std::numeric_limits<std::uint32_t>::max();
    struct Case {
        WriteSource source;
        std::uint32_t user;
        bool valid;
    };
    const std::vector<Case> cases{
        {.source = WriteSource::OPERATOR, .user = SOME_USER, .valid = true},
        {.source = WriteSource::OPERATOR, .user = HIGHEST_USER, .valid = true},
        {.source = WriteSource::OPERATOR, .user = 0, .valid = false},
        {.source = WriteSource::AUTO, .user = 0, .valid = true},
        {.source = WriteSource::AUTO, .user = SOME_USER, .valid = false},
        {.source = WriteSource::REMOTE, .user = 0, .valid = true},
        {.source = WriteSource::REMOTE, .user = SOME_USER, .valid = false},
    };
    std::size_t wrong = cases.size();
    for (std::size_t i = 0; i < cases.size(); ++i) {
        WriteRequest record;
        record.operation = to_raw(WriteOperation::READ_NOW);
        record.source = to_raw(cases[i].source);
        record.user_id = cases[i].user;
        if (request_valid(record) != cases[i].valid) {
            wrong = i;
            break;
        }
    }
    EXPECT_EQ(wrong, cases.size()) << "the first case judged wrongly";
}

TEST(QueueRecords, NoTimesOverflowTheExpiryCheck) {
    constexpr std::int64_t LOWEST = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t HIGHEST = std::numeric_limits<std::int64_t>::max();
    WriteRequest record;
    record.posted_mono_ns = LOWEST;
    record.expiry_mono_ns = HIGHEST;
    EXPECT_TRUE(request_expired(record, 0));
    EXPECT_TRUE(request_expired(record, LOWEST));
    EXPECT_TRUE(request_expired(record, HIGHEST));
    record.posted_mono_ns = HIGHEST;
    record.expiry_mono_ns = LOWEST;
    EXPECT_TRUE(request_expired(record, 0));
    record.posted_mono_ns = HIGHEST - 1;
    record.expiry_mono_ns = HIGHEST;
    EXPECT_FALSE(request_expired(record, HIGHEST - 1));
}

TEST(QueueRecords, ACursorFieldOutsideTheRegionIsNone) {
    // The two headers and one cursor.
    constexpr std::size_t WORDS =
        ((sizeof(RegionHeader) + sizeof(ProcHeader)) / sizeof(std::uint64_t)) +
        1;
    std::array<std::uint64_t, WORDS> region{};
    const std::span<std::byte> bytes =
        std::as_writable_bytes(std::span(region));
    EXPECT_NE(cursor_field(bytes, 0), nullptr);
    EXPECT_EQ(cursor_field(bytes, 1), nullptr);
    EXPECT_EQ(cursor_field(bytes, std::size_t{1} << 62U), nullptr);
    EXPECT_EQ(cursor_field({}, 0), nullptr);
}

}  // namespace
