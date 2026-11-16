// Unit tests for the log records of the liveness check (issue #61, D-47):
// each record kind reaches the journal's native protocol with its fixed
// MESSAGE_ID and its fields. A datagram socket stands in for the journal.

#include "report_log.h"
#include "late_log.h"

#include "scada/common/fd.h"
#include "scada/common/log.h"
#include "scada/common/result.h"
#include "scada/tag/generation_host.h"
#include "scada/tag/liveness.h"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using scada::Fd;
using scada::Logger;
using scada::LogSink;
using scada::Result;
using scada::tag::CheckReport;
using scada::tag::DownReason;
using scada::tag::LateHeartbeats;
using scada::tag::ProcessChange;
using scada::tag::QueueChange;
using scada::tag::QueueEvent;
using scada::tag::Restarts;
using scada::tagd::LateLog;
using scada::tagd::log_late;
using scada::tagd::log_report;

constexpr std::chrono::milliseconds FLUSH_TIMEOUT{30'000};
constexpr std::int64_t INTERVAL_NS = 1'000;
constexpr std::int64_t START_NS = 5'000;

// A datagram socket bound in a private temporary directory, playing the
// journal. Removes the socket and the directory when destroyed.
class FakeJournal {
public:
    FakeJournal() {
        static int instances = 0;
        ++instances;
        std::string name = "scada-tagd-log-";
        name.append(std::to_string(::getpid()));
        name.append("-").append(std::to_string(instances));
        const std::filesystem::path directory =
            std::filesystem::temp_directory_path() / name;
        std::error_code failed;
        if (!std::filesystem::create_directory(directory, failed)) {
            return;
        }
        directory_ = directory.string();
        path_ = directory_ + "/socket";
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path_.size() >= sizeof(address.sun_path)) {
            return;
        }
        std::copy(path_.begin(), path_.end(), std::begin(address.sun_path));
        Fd socket(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,scada-banned): the socket interface takes every address type through a sockaddr pointer
        const auto* generic = reinterpret_cast<const sockaddr*>(&address);
        if (socket.valid() &&
            ::bind(socket.get(), generic, sizeof(address)) == 0) {
            socket_ = std::move(socket);
        }
    }

    ~FakeJournal() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    FakeJournal(const FakeJournal&) = delete;
    FakeJournal& operator=(const FakeJournal&) = delete;
    FakeJournal(FakeJournal&&) = delete;
    FakeJournal& operator=(FakeJournal&&) = delete;

    [[nodiscard]] bool ready() const { return socket_.valid(); }
    [[nodiscard]] const std::string& path() const { return path_; }

    // Every datagram queued now, oldest first; call after Logger::flush.
    [[nodiscard]] std::vector<std::string> received() const {
        constexpr std::size_t BUFFER_BYTES = 8192;
        std::vector<std::string> out;
        std::array<char, BUFFER_BYTES> buffer{};
        for (;;) {
            const ssize_t count = ::recv(socket_.get(), buffer.data(),
                                         buffer.size(), MSG_DONTWAIT);
            if (count <= 0) {
                return out;
            }
            out.emplace_back(buffer.data(), static_cast<std::size_t>(count));
        }
    }

private:
    std::string directory_;
    std::string path_;
    Fd socket_;
};

// A logger that writes to `journal`, or null if it cannot be made.
std::unique_ptr<Logger> logger_for(const FakeJournal& journal) {
    Result<std::unique_ptr<LogSink>> sink =
        scada::make_journal_sink(journal.path());
    if (!sink.ok()) {
        return nullptr;
    }
    Result<Logger> logger =
        Logger::create(std::move(sink).value(), {.component = "scada-tagd"});
    if (!logger.ok()) {
        return nullptr;
    }
    return std::make_unique<Logger>(std::move(logger).value());
}

// True if `datagram` holds the line `KEY=value`.
bool has(const std::string& datagram, std::string_view line) {
    const std::string wanted = std::string(line) + "\n";
    return datagram.starts_with(wanted) ||
           datagram.find("\n" + wanted) != std::string::npos;
}

// The records of `records` whose message is `message`.
std::vector<std::string> with(const std::vector<std::string>& records,
                              std::string_view message) {
    const std::string line = "MESSAGE=" + std::string(message);
    std::vector<std::string> out;
    std::ranges::copy_if(
        records, std::back_inserter(out),
        [&](const std::string& record) { return has(record, line); });
    return out;
}

class ReportLog : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(journal_.ready());
        logger_ = logger_for(journal_);
        ASSERT_NE(logger_, nullptr);
    }

    // The logger SetUp made; a test that gets here has one.
    [[nodiscard]] Logger& logger() { return *logger_; }

    // What the journal received after the logger delivered everything.
    [[nodiscard]] std::vector<std::string> delivered() {
        EXPECT_TRUE(logger().flush(FLUSH_TIMEOUT).ok());
        return journal_.received();
    }

    // One check at `now` that found `process` changed to `reason` and
    // `added` late heartbeats.
    void log_check(std::string process, DownReason reason, std::uint32_t added,
                   LateLog& late_log, std::int64_t now) {
        CheckReport report;
        report.changes.push_back(ProcessChange{process, reason});
        report.late.push_back(
            LateHeartbeats{.process = std::move(process), .added = added});
        log_report(logger(), report, late_log, now);
    }

private:
    FakeJournal journal_;
    std::unique_ptr<Logger> logger_;
};

// Figures of the record kinds, each different, so that a field taken from
// the wrong place shows.
constexpr std::uint32_t STARTS_TOTAL = 7;
constexpr std::uint32_t LATE_TOTAL = 9;
constexpr std::int64_t LONGEST_GAP_NS = 1'234'567'890;  // 1234 ms
constexpr std::int64_t LAST_LATE_NS = 5'000'123'999;    // 5000123 us
constexpr std::uint32_t BASELINE_BEATS = 4;
constexpr std::int64_t BASELINE_GAP_NS = 2'000'000;
constexpr std::int64_t BASELINE_LAST_NS = 3'000;

TEST_F(ReportLog, EachRecordKindCarriesItsMessageIdAndFields) {
    CheckReport report;
    report.changes.push_back(ProcessChange{"io", DownReason::HEARTBEAT_STALE});
    report.changes.push_back(ProcessChange{"control", DownReason::NONE});
    report.restarts.push_back(Restarts{
        .process = "hmi", .added = 2, .unseen = 1, .total = STARTS_TOTAL});
    report.late.push_back(LateHeartbeats{.process = "io",
                                         .added = 3,
                                         .total = LATE_TOTAL,
                                         .longest_gap_ns = LONGEST_GAP_NS,
                                         .last_late_mono_ns = LAST_LATE_NS,
                                         .baseline = false});
    report.late.push_back(LateHeartbeats{.process = "web",
                                         .added = BASELINE_BEATS,
                                         .total = BASELINE_BEATS,
                                         .longest_gap_ns = BASELINE_GAP_NS,
                                         .last_late_mono_ns = BASELINE_LAST_NS,
                                         .baseline = true});
    LateLog late_log(INTERVAL_NS);
    log_report(logger(), report, late_log, START_NS);

    const std::vector<std::string> records = delivered();
    ASSERT_EQ(records.size(), 5U);
    // Process down: WARNING.
    EXPECT_TRUE(has(records[0], "MESSAGE=process down"));
    EXPECT_TRUE(has(records[0], "PRIORITY=4"));
    EXPECT_TRUE(has(records[0], "MESSAGE_ID=51ad850aac4848fab6eade0000eaa868"));
    EXPECT_TRUE(has(records[0], "PROCESS=io"));
    EXPECT_TRUE(has(records[0], "REASON=heartbeat stale"));
    // Process up: NOTICE.
    EXPECT_TRUE(has(records[1], "MESSAGE=process up"));
    EXPECT_TRUE(has(records[1], "PRIORITY=5"));
    EXPECT_TRUE(has(records[1], "MESSAGE_ID=3e293a9e793b4fa08fafb02b5f16ae7f"));
    EXPECT_TRUE(has(records[1], "PROCESS=control"));
    // A restart no check saw: WARNING.
    EXPECT_TRUE(has(records[2], "MESSAGE=process restarted"));
    EXPECT_TRUE(has(records[2], "PRIORITY=4"));
    EXPECT_TRUE(has(records[2], "MESSAGE_ID=f35dfeb8e8124cc88b219cabf6947b00"));
    EXPECT_TRUE(has(records[2], "PROCESS=hmi"));
    EXPECT_TRUE(has(records[2], "RESTARTS=2"));
    EXPECT_TRUE(has(records[2], "RESTARTS_UNSEEN=1"));
    EXPECT_TRUE(has(records[2], "STARTS_TOTAL=7"));
    // The baseline is returned at once by the rate limit, before the record
    // that waits for the end of the check.
    EXPECT_TRUE(has(records[3], "MESSAGE=heartbeat late before this check"));
    EXPECT_TRUE(has(records[3], "PRIORITY=6"));
    EXPECT_TRUE(has(records[3], "MESSAGE_ID=c4de2438d3574ca38f45771de9254e3a"));
    EXPECT_TRUE(has(records[3], "PROCESS=web"));
    EXPECT_TRUE(has(records[3], "LATE_BEATS=4"));
    // Heartbeat late: WARNING; the gap in ms, the time in µs, both cut.
    EXPECT_TRUE(has(records[4], "MESSAGE=heartbeat late"));
    EXPECT_TRUE(has(records[4], "PRIORITY=4"));
    EXPECT_TRUE(has(records[4], "MESSAGE_ID=31aad0d613b645d39a83e7978c4531ec"));
    EXPECT_TRUE(has(records[4], "PROCESS=io"));
    EXPECT_TRUE(has(records[4], "LATE_BEATS=3"));
    EXPECT_TRUE(has(records[4], "LATE_BEATS_TOTAL=9"));
    EXPECT_TRUE(has(records[4], "LONGEST_GAP_MS=1234"));
    EXPECT_TRUE(has(records[4], "LAST_LATE_MONO_US=5000123"));
    EXPECT_TRUE(has(records[4], "CHECKS=1"));
}

// Issue #53 (decision D-49, review of PR #77, B1 and N1): the record of a
// queue file created anew. With a base, the lost ids run from the cursor of
// the last check of the old file to the base, a true upper bound.
TEST_F(ReportLog, AQueueFileCreatedAnewBoundsTheLostIds) {
    constexpr std::uint64_t BASE = 5;
    CheckReport report;
    QueueChange change;
    change.queue = "events.io";
    change.event = QueueEvent::RECREATED;
    change.base_seq = BASE;
    change.old_write_seq = 3;
    change.cursor = 1;
    change.cursor_known = true;
    report.queues.push_back(change);
    LateLog late_log(INTERVAL_NS);
    log_report(logger(), report, late_log, START_NS);
    const std::vector<std::string> records = delivered();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_TRUE(has(records[0],
                    "MESSAGE=queue file created anew; audit "
                    "records may be lost"));
    EXPECT_TRUE(has(records[0], "PRIORITY=4"));
    EXPECT_TRUE(has(records[0], "MESSAGE_ID=330f9ce1bb0547c098562d406ce2f774"));
    EXPECT_TRUE(has(records[0], "BASE_SEQ=5"));
    EXPECT_TRUE(has(records[0], "OLD_WRITE_SEQ=3"));
    EXPECT_TRUE(has(records[0], "CURSOR=1"));
    EXPECT_TRUE(has(records[0], "LOST_AT_MOST=4"));
    EXPECT_TRUE(has(records[0], "LOST_IDS=[1, 5)"));
}

// Without a base the bound is unknown, and the range is never inverted.
TEST_F(ReportLog, AQueueFileCreatedAnewWithoutABaseHasNoBound) {
    constexpr std::uint64_t CURSOR_AFTER = 5;
    CheckReport report;
    QueueChange change;
    change.queue = "cmd.io.web";
    change.event = QueueEvent::RECREATED;
    change.old_write_seq = 3;
    change.cursor = CURSOR_AFTER;
    change.cursor_known = true;
    report.queues.push_back(change);
    LateLog late_log(INTERVAL_NS);
    log_report(logger(), report, late_log, START_NS);
    const std::vector<std::string> records = delivered();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_TRUE(has(records[0],
                    "MESSAGE=queue file created anew; records "
                    "may be lost"));
    EXPECT_TRUE(has(records[0], "LOST_AT_MOST=unknown"));
    EXPECT_TRUE(has(records[0], "LOST_IDS=[5, ?)"));
    EXPECT_TRUE(has(records[0], "REASON=no base: the queue stops"));
}

// Review of PR #77, F1: a base below the old `write_seq` (the queue stops)
// bounds nothing; the range is not shown as empty.
TEST_F(ReportLog, AQueueFileCreatedAnewWithABaseBelowTheOldWriteSeqHasNoBound) {
    constexpr std::uint64_t OLD_WRITTEN = 5;
    CheckReport report;
    QueueChange change;
    change.queue = "events.io";
    change.event = QueueEvent::RECREATED;
    change.base_seq = 0;
    change.old_write_seq = OLD_WRITTEN;
    change.cursor = 0;
    change.cursor_known = true;
    report.queues.push_back(change);
    LateLog late_log(INTERVAL_NS);
    log_report(logger(), report, late_log, START_NS);
    const std::vector<std::string> records = delivered();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_TRUE(has(records[0], "LOST_AT_MOST=unknown"));
    EXPECT_TRUE(has(records[0], "LOST_IDS=[0, ?)"));
    EXPECT_TRUE(has(records[0],
                    "REASON=base below the old write_seq: the queue stops"));
}

// Decision D-51: a `write_seq` that fell in the same file is an ERROR.
TEST_F(ReportLog, AWriteSeqThatFellIsAnError) {
    CheckReport report;
    QueueChange change;
    change.queue = "events.io";
    change.event = QueueEvent::WRITE_SEQ_FELL;
    change.old_write_seq = 3;
    change.write_seq = 1;
    report.queues.push_back(change);
    LateLog late_log(INTERVAL_NS);
    log_report(logger(), report, late_log, START_NS);
    const std::vector<std::string> records = delivered();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_TRUE(
        has(records[0], "MESSAGE=queue write_seq fell; ids may repeat"));
    EXPECT_TRUE(has(records[0], "PRIORITY=3"));
    EXPECT_TRUE(has(records[0], "MESSAGE_ID=d673f795274a49d1882153ae03003b15"));
    EXPECT_TRUE(has(records[0], "OLD_WRITE_SEQ=3"));
    EXPECT_TRUE(has(records[0], "WRITE_SEQ=1"));
}

TEST_F(ReportLog, ARestartSeenAsDownAndUpIsANotice) {
    CheckReport report;
    report.restarts.push_back(
        Restarts{.process = "io", .added = 1, .unseen = 0, .total = 2});
    LateLog late_log(INTERVAL_NS);
    log_report(logger(), report, late_log, START_NS);
    const std::vector<std::string> records = delivered();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_TRUE(has(records[0], "PRIORITY=5"));
    EXPECT_TRUE(has(records[0], "RESTARTS_UNSEEN=0"));
}

// Issue #60: each process not attached at the first check of a new
// generation is one INFO record, before the changes of that check.
TEST_F(ReportLog, EachProcessNotAttachedIsOneInfoRecord) {
    CheckReport report;
    report.not_attached = {"io", "web"};
    report.changes.push_back(ProcessChange{"control", DownReason::NONE});
    LateLog late_log(INTERVAL_NS);
    log_report(logger(), report, late_log, START_NS);
    const std::vector<std::string> records = delivered();
    ASSERT_EQ(records.size(), 3U);
    EXPECT_TRUE(has(records[0], "MESSAGE=process not attached"));
    EXPECT_TRUE(has(records[0], "PRIORITY=6"));
    EXPECT_TRUE(has(records[0], "MESSAGE_ID=0f57ce3d6dbc4891abd0e55e240f04f9"));
    EXPECT_TRUE(has(records[0], "PROCESS=io"));
    EXPECT_TRUE(has(records[1], "MESSAGE=process not attached"));
    EXPECT_TRUE(has(records[1], "PROCESS=web"));
    EXPECT_TRUE(has(records[2], "MESSAGE=process up"));
}

// Issue #61: the rate limit holds `heartbeat late` back and the next record
// carries the sum; `process down` and `process up` are never held.
TEST_F(ReportLog, TheRateLimitSumsLateHeartbeatsAndNeverHoldsChanges) {
    LateLog late_log(INTERVAL_NS);
    log_check("io", DownReason::NOT_RUNNING, 1, late_log, START_NS);
    log_check("io", DownReason::NONE, 2, late_log, START_NS + 1);
    log_check("io", DownReason::NOT_RUNNING, 3, late_log, START_NS + 2);
    // The interval ends at a check that found nothing.
    log_report(logger(), CheckReport{}, late_log, START_NS + INTERVAL_NS);

    const std::vector<std::string> records = delivered();
    const std::vector<std::string> late = with(records, "heartbeat late");
    EXPECT_EQ(with(records, "process down").size(), 2U);
    EXPECT_EQ(with(records, "process up").size(), 1U);
    ASSERT_EQ(late.size(), 2U);
    EXPECT_TRUE(has(late[0], "LATE_BEATS=1"));
    EXPECT_TRUE(has(late[0], "CHECKS=1"));
    EXPECT_TRUE(has(late[1], "LATE_BEATS=5"));
    EXPECT_TRUE(has(late[1], "CHECKS=2"));
}

TEST_F(ReportLog, HeldLateHeartbeatsAreLoggedAtTheStop) {
    LateLog late_log(INTERVAL_NS);
    for (std::uint32_t check = 0; check < 2; ++check) {
        CheckReport report;
        report.late.push_back(LateHeartbeats{.process = "io", .added = 2});
        log_report(logger(), report, late_log, START_NS + check);
    }
    log_late(logger(), late_log.drain());
    const std::vector<std::string> records = delivered();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_TRUE(has(records[1], "LATE_BEATS=2"));
}

// Positions of records left in a queue, for the record of decision D-54.
constexpr std::uint64_t FIRST_LEFT = 10;
constexpr std::uint64_t END_LEFT = 13;

// Decision D-54 (review of PR #82, B5): records left in a removed
// generation, a known count with its positions, and an unknown one.
TEST_F(ReportLog, UnconsumedRecordsCarryTheirMessageIdRangeOrUnknown) {
    scada::tag::RetiredGeneration retired;
    retired.generation = 4;
    retired.removed = true;
    scada::tag::UnconsumedQueue events;
    events.queue = "events.io";
    events.events = true;
    events.known = true;
    events.first = FIRST_LEFT;
    events.end = END_LEFT;
    events.records = END_LEFT - FIRST_LEFT;
    retired.unconsumed.push_back(events);
    scada::tag::UnconsumedQueue requests;
    requests.queue = "cmd.io.web";
    retired.unconsumed.push_back(requests);
    scada::tagd::log_unconsumed(logger(), retired);
    const std::vector<std::string> records = delivered();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_TRUE(has(records[0],
                    "MESSAGE=audit records left unconsumed in a removed "
                    "generation"));
    EXPECT_TRUE(has(records[0], "PRIORITY=4"));
    EXPECT_TRUE(has(records[0], "MESSAGE_ID=42c8e35948cb467eb6d894d3bb0476a8"));
    EXPECT_TRUE(has(records[0], "GENERATION=4"));
    EXPECT_TRUE(has(records[0], "QUEUE=events.io"));
    EXPECT_TRUE(has(records[0], "RECORDS=3"));
    EXPECT_TRUE(has(records[0], "FIRST_SEQ=10"));
    EXPECT_TRUE(has(records[0], "END_SEQ=13"));
    EXPECT_TRUE(has(records[1],
                    "MESSAGE=records left unconsumed in a removed generation"));
    EXPECT_TRUE(has(records[1], "QUEUE=cmd.io.web"));
    EXPECT_TRUE(has(records[1], "RECORDS=unknown"));
    EXPECT_FALSE(has(records[1], "FIRST_SEQ="));
}

// Review of PR #82 (T5): a generation whose header cannot be read gives one
// record, with its queue unknown, as one that may hold audit records.
TEST_F(ReportLog, AGenerationWithoutAHeaderGivesOneUnknownRecord) {
    scada::tag::RetiredGeneration retired;
    retired.generation = 4;
    retired.removed = true;
    scada::tag::UnconsumedQueue unknown;
    unknown.events = true;
    retired.unconsumed.push_back(unknown);
    scada::tagd::log_unconsumed(logger(), retired);
    const std::vector<std::string> records = delivered();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_TRUE(has(records[0],
                    "MESSAGE=audit records left unconsumed in a removed "
                    "generation"));
    EXPECT_TRUE(has(records[0], "QUEUE=unknown"));
    EXPECT_TRUE(has(records[0], "RECORDS=unknown"));
    EXPECT_TRUE(has(records[0], "MESSAGE_ID=42c8e35948cb467eb6d894d3bb0476a8"));
}

}  // namespace

namespace {

using scada::tag::RetainProblem;
using scada::tag::RetainProblemKind;

// Decision D-59, point 3: each retain problem has a record and an ID of its
// own.
TEST_F(ReportLog, EachRetainProblemHasItsOwnRecordAndMessageId) {
    scada::tagd::log_retain_problem(
        logger(), RetainProblem{"io.bin",
                                "ignored: the check sum does not match",
                                RetainProblemKind::FILE_REJECTED,
                                {},
                                {}});
    scada::tagd::log_retain_problem(
        logger(), RetainProblem{"io",
                                "cannot write: no space",
                                RetainProblemKind::WRITE_FAILED,
                                {},
                                {}});
    scada::tagd::log_retain_problem(
        logger(), RetainProblem{"retain writer",
                                "not within 5000 ms",
                                RetainProblemKind::WRITER_BOUND,
                                {},
                                {}});
    scada::tagd::log_retain_problem(
        logger(), RetainProblem{"io", "still being written",
                                RetainProblemKind::UNSAVED_AT_EXIT,
                                "io, web, ...", "1700", 3});
    const std::vector<std::string> records = delivered();
    ASSERT_EQ(records.size(), 4U);
    EXPECT_TRUE(has(records[0], "MESSAGE=retain file ignored"));
    EXPECT_TRUE(has(records[0], "PRIORITY=4"));
    EXPECT_TRUE(has(records[0], "SUBJECT=io.bin"));
    EXPECT_TRUE(has(records[0], "MESSAGE_ID=586ec48106f748609721eef8957c56d5"));
    EXPECT_TRUE(has(records[1], "MESSAGE=retain file not written"));
    EXPECT_TRUE(has(records[1], "PRIORITY=4"));
    EXPECT_TRUE(has(records[1], "DETAIL=cannot write: no space"));
    EXPECT_TRUE(has(records[1], "MESSAGE_ID=974151cda6fa452a998a576b2bb09bce"));
    EXPECT_TRUE(has(records[2],
                    "MESSAGE=retain writer did not finish within its bound"));
    EXPECT_TRUE(has(records[2], "PRIORITY=3"));
    EXPECT_TRUE(has(records[2], "MESSAGE_ID=0031dfe3391142b0b9fa995b07115e10"));
    EXPECT_TRUE(has(records[3], "MESSAGE=values unsaved at exit"));
    EXPECT_TRUE(has(records[3], "PRIORITY=3"));
    EXPECT_TRUE(has(records[3], "SUBJECT=io"));
    EXPECT_TRUE(has(records[3], "UNSAVED_OWNERS=io, web, ..."));
    EXPECT_TRUE(has(records[3], "UNSAVED_COUNT=3"));
    EXPECT_TRUE(has(records[3], "KEPT_WRITTEN_TIME_NS=1700"));
    EXPECT_TRUE(has(records[3], "MESSAGE_ID=7cf5c1c8535f44f6a297db538666d7a3"));
}

// Decision D-62: the keeper's report of a RETAIN_NOW change that reached
// the disk after 1 s (RetainNowLate in retain_keeper_test) is a WARNING
// with its own MESSAGE_ID.
TEST_F(ReportLog, ALateRetainNowWriteIsAWarningWithItsMessageId) {
    scada::tagd::log_retain_problem(
        logger(),
        RetainProblem{"io",
                      "a retain_now change reached the disk after 1700 ms, "
                      "more than 1000 ms",
                      RetainProblemKind::RETAIN_NOW_LATE,
                      {},
                      {}});
    const std::vector<std::string> records = delivered();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_TRUE(
        has(records[0], "MESSAGE=retain_now change reached the disk late"));
    EXPECT_TRUE(has(records[0], "PRIORITY=4"));
    EXPECT_TRUE(has(records[0], "SUBJECT=io"));
    EXPECT_TRUE(has(records[0],
                    "DETAIL=a retain_now change reached the disk "
                    "after 1700 ms, more than 1000 ms"));
    EXPECT_TRUE(has(records[0], "MESSAGE_ID=51a87b54784a4a21bd45dfb59069996d"));
}

TEST_F(ReportLog, AProcessStillAttachedAtTheTimeoutHasItsMessageId) {
    scada::tag::RetiredGeneration retired;
    retired.generation = 3;
    retired.removed = true;
    retired.timed_out = true;
    retired.still_attached = {"io (pid 42)"};
    scada::tagd::log_still_attached(logger(), retired);
    const std::vector<std::string> records = delivered();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_TRUE(has(records[0],
                    "MESSAGE=process still attached to a retired generation"));
    EXPECT_TRUE(has(records[0], "GENERATION=3"));
    EXPECT_TRUE(has(records[0], "PROCESS=io (pid 42)"));
    EXPECT_TRUE(has(records[0], "MESSAGE_ID=349b6814c6cc4a2fafa5d3d1af16e7a7"));
}

TEST_F(ReportLog, AnUnusableRetainDirectoryHasItsMessageId) {
    scada::tagd::log_retain_directory_unusable(
        logger(), "/var/lib/scada/retain", "PERMISSION_DENIED", "mode 0777");
    const std::vector<std::string> records = delivered();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_TRUE(has(records[0], "MESSAGE=the retain directory is not usable"));
    EXPECT_TRUE(has(records[0], "PRIORITY=2"));
    EXPECT_TRUE(has(records[0], "DIRECTORY=/var/lib/scada/retain"));
    EXPECT_TRUE(has(records[0], "STATUS=PERMISSION_DENIED"));
    EXPECT_TRUE(has(records[0], "MESSAGE_ID=8df1e064b8464d59ad5fa42e9a3070e4"));
}

// The IDs are all different (D-47: a new meaning takes a new ID).
TEST(ReportLogIds, EveryMessageIdIsDistinct) {
    using namespace scada::tagd;
    std::vector<std::string_view> ids = {MESSAGE_ID_PROCESS_DOWN,
                                         MESSAGE_ID_PROCESS_UP,
                                         MESSAGE_ID_HEARTBEAT_LATE,
                                         MESSAGE_ID_HEARTBEAT_LATE_BEFORE,
                                         MESSAGE_ID_PROCESS_RESTARTED,
                                         MESSAGE_ID_QUEUE_RECREATED,
                                         MESSAGE_ID_QUEUE_WRITE_SEQ_FELL,
                                         MESSAGE_ID_PROCESS_NOT_ATTACHED,
                                         MESSAGE_ID_RECORDS_UNCONSUMED,
                                         MESSAGE_ID_RETAIN_FILE_REJECTED,
                                         MESSAGE_ID_RETAIN_WRITE_FAILED,
                                         MESSAGE_ID_RETAIN_DIRECTORY_UNUSABLE,
                                         MESSAGE_ID_STILL_ATTACHED_AT_TIMEOUT,
                                         MESSAGE_ID_RETAIN_WRITER_BOUND,
                                         MESSAGE_ID_VALUES_UNSAVED_AT_EXIT,
                                         MESSAGE_ID_MOVED_VALUES_INCOMPLETE,
                                         MESSAGE_ID_RETAIN_NOW_LATE};
    std::ranges::sort(ids);
    EXPECT_EQ(std::ranges::adjacent_find(ids), ids.end());
}

}  // namespace
