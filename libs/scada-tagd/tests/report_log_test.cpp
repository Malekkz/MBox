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

}  // namespace
