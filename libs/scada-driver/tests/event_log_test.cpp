// A worker's log of its devices' identity and restart events (driver
// interface contract 11.3; D-147, D-148): the records, their rate limit,
// the summaries and the stop's flush, through a real Logger and LogQueue.

#include "../src/event_log.h"

#include "channel_builder.h"
#include "scada/common/log.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/driver/channel_engine.h"
#include "scada/driver/compiled.h"
#include "scada/driver/identity_run.h"
#include "scada/driver/plan.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::driver::detail {
namespace {

using test::make_channel;
using test::PointSpec;

constexpr std::int64_t PERIOD = 10;
constexpr std::int64_t WALL = 1'000;
constexpr std::chrono::seconds WAIT{30};

// The records a sink received, as "MESSAGE|KEY=value|...". A stalled sink
// holds the Logger's thread in its write, so that a test fills a queue.
class Lines {
public:
    void add(std::string line) {
        std::unique_lock lock(mutex_);
        ++entered_;
        changed_.notify_all();
        changed_.wait(lock, [this] { return !stalled_; });
        lines_.push_back(std::move(line));
    }
    [[nodiscard]] std::vector<std::string> snapshot() {
        const std::scoped_lock lock(mutex_);
        return lines_;
    }
    void set_stalled(bool stalled) {
        const std::scoped_lock lock(mutex_);
        stalled_ = stalled;
        changed_.notify_all();
    }
    // True once `count` writes have begun, within WAIT.
    bool wait_for_entered(std::size_t count) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, WAIT, [&] { return entered_ >= count; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool stalled_ = false;
    std::size_t entered_ = 0;
    std::vector<std::string> lines_;
};

class LinesSink final : public LogSink {
public:
    explicit LinesSink(Lines& lines) : lines_(lines) {}
    [[nodiscard]] Status write(const LogRecord& record) noexcept override {
        std::string line(record.message);
        for (const LogField& field : record.fields) {
            line.append("|").append(field.key).append("=").append(field.value);
        }
        lines_.add(std::move(line));
        return {};
    }

private:
    Lines& lines_;
};

// The sum of the COUNTs of the records of `message_id` of device `device`.
std::uint64_t count_of(const std::vector<std::string>& lines,
                       std::string_view message_id, std::string_view device) {
    const std::string record = "|MESSAGE_ID=" + std::string(message_id) + "|";
    const std::string name = "|DEVICE=" + std::string(device) + "|";
    std::uint64_t sum = 0;
    for (const std::string& line : lines) {
        const std::size_t count = line.find("|COUNT=");
        if (line.find(record) != std::string::npos &&
            line.find(name) != std::string::npos &&
            count != std::string::npos) {
            sum += std::stoull(
                line.substr(count + std::string_view("|COUNT=").size()));
        }
    }
    return sum;
}

// Two devices, d1 and d2, on channel line1.
CompiledChannel two_devices() {
    return make_channel({{.name = "d1",
                          .unit_id = 1,
                          .points = {PointSpec{.first = 0, .group = 0}}},
                         {.name = "d2",
                          .unit_id = 2,
                          .points = {PointSpec{.first = 0, .group = 0}}}},
                        ReadRules{.max_units = 4});
}

EventRecord mismatch(std::uint64_t count, std::string_view value) {
    EventRecord record;
    record.count = count;
    record.reason = static_cast<std::uint8_t>(MismatchReason::VALUE);
    record.check = 0;
    record.length = value.size();
    std::copy(value.begin(), value.end(), record.text.begin());
    record.has_details = true;
    return record;
}

// A logger with a capture sink and one worker queue of `capacity`.
class Logged : public ::testing::Test {
protected:
    static constexpr std::size_t CAPACITY = 16;
    bool start(std::size_t capacity = CAPACITY) {
        Result<Logger> made = Logger::create(
            std::make_unique<LinesSink>(lines_), {.component = "unit-test"});
        if (!made.ok()) {
            return false;
        }
        logger_ = std::make_unique<Logger>(std::move(made).value());
        Result<std::shared_ptr<LogQueue>> queue = logger_->make_queue(capacity);
        if (!queue.ok()) {
            return false;
        }
        queue_ = queue.value();
        log_ = std::make_unique<EventLog>(channel_, queue_, PERIOD);
        return true;
    }
    std::vector<std::string> lines() {
        EXPECT_TRUE(logger_->flush(WAIT).ok());
        return lines_.snapshot();
    }
    EventLog& log() { return *log_; }
    Logger& logger() { return *logger_; }
    LogQueue& queue() { return *queue_; }
    // Holds the Logger's thread in the sink's write of a record of its own,
    // so that nothing leaves the queue until `release`.
    bool hold() {
        lines_.set_stalled(true);
        logger_->log(LogLevel::INFO, "held");
        return lines_.wait_for_entered(1);
    }
    void release() { lines_.set_stalled(false); }
    // A queue of 1; one restart logged and one held for each device; the
    // Logger's thread held in the sink. False if any step failed.
    bool held_with_one_each();

private:
    CompiledChannel channel_ = two_devices();
    Lines lines_;
    std::unique_ptr<Logger> logger_;
    std::shared_ptr<LogQueue> queue_;
    std::unique_ptr<EventLog> log_;
};

constexpr std::string_view FIRST_MISMATCH =
    "identity mismatch|MESSAGE_ID=driver-identity-mismatch|CHANNEL=line1|"
    "DEVICE=d1|REASON=value|CHECK=0|VALUE=8|COUNT=1";

TEST_F(Logged, TheFirstGoesAtOnceWithItsDetails) {
    if (!start()) {
        FAIL() << "no logger";
    }
    log().add(0, DeviceEvent::MISMATCH, mismatch(1, "8"), 0, WALL);
    EXPECT_EQ(lines(), std::vector<std::string>{std::string(FIRST_MISMATCH)});
}

TEST_F(Logged, LaterOnesAreSummedAfterThePeriodWithoutDetails) {
    if (!start()) {
        FAIL() << "no logger";
    }
    log().add(0, DeviceEvent::MISMATCH, mismatch(1, "8"), 0, WALL);
    log().add(0, DeviceEvent::MISMATCH, mismatch(2, "9"), 1, WALL);
    log().add(0, DeviceEvent::MISMATCH, mismatch(3, "9"), 2, WALL);
    log().due(PERIOD - 1, WALL);
    EXPECT_EQ(lines().size(), 1U);
    log().due(PERIOD, WALL);
    EXPECT_EQ(lines(), (std::vector<std::string>{
                           std::string(FIRST_MISMATCH),
                           "identity mismatch|MESSAGE_ID=driver-identity-"
                           "mismatch|CHANNEL=line1|DEVICE=d1|COUNT=5"}));
}

TEST_F(Logged, EachDeviceAndKindIsLimitedApart) {
    if (!start()) {
        FAIL() << "no logger";
    }
    EventRecord restart;
    restart.count = 1;
    restart.has_details = true;
    log().add(0, DeviceEvent::MISMATCH, mismatch(1, "8"), 0, WALL);
    log().add(1, DeviceEvent::MISMATCH, mismatch(1, "8"), 0, WALL);
    log().add(0, DeviceEvent::RESTART, restart, 0, WALL);
    const EventRecord matched = mismatch(1, "7");
    log().add(0, DeviceEvent::MATCH, matched, 0, WALL);
    const std::vector<std::string> got = lines();
    ASSERT_EQ(got.size(), 4U);
    EXPECT_NE(got[1].find("DEVICE=d2"), std::string::npos);
    EXPECT_EQ(got[2],
              "device restart|MESSAGE_ID=driver-device-restart|CHANNEL=line1|"
              "DEVICE=d1|COUNT=1");
    EXPECT_EQ(got[3],
              "identity matched|MESSAGE_ID=driver-identity-matched|CHANNEL="
              "line1|DEVICE=d1|IDENTITY=7|COUNT=1");
}

TEST_F(Logged, TheStopFlushesEveryHeldCount) {
    if (!start()) {
        FAIL() << "no logger";
    }
    EventRecord restart;
    restart.count = 1;
    log().add(1, DeviceEvent::RESTART, restart, 0, WALL);
    restart.count = 2;
    log().add(1, DeviceEvent::RESTART, restart, 1, WALL);
    EXPECT_TRUE(log().flush(WALL, false));
    EXPECT_TRUE(log().flush(WALL, false));  // nothing held any more
    EXPECT_EQ(lines(), (std::vector<std::string>{
                           "device restart|MESSAGE_ID=driver-device-restart|"
                           "CHANNEL=line1|DEVICE=d2|COUNT=1",
                           "device restart|MESSAGE_ID=driver-device-restart|"
                           "CHANNEL=line1|DEVICE=d2|COUNT=2"}));
}

TEST_F(Logged, NoDetailsLeaveReasonCheckAndValueOut) {
    if (!start()) {
        FAIL() << "no logger";
    }
    EventRecord bare;
    bare.count = 1;
    log().add(0, DeviceEvent::MISMATCH, bare, 0, WALL);
    EXPECT_EQ(lines(), (std::vector<std::string>{
                           "identity mismatch|MESSAGE_ID=driver-identity-"
                           "mismatch|CHANNEL=line1|DEVICE=d1|COUNT=1"}));
}

TEST_F(Logged, AFullQueueDropsAndCounts) {
    // A queue of 1 record whose worker logs 3 devices' worth at once: the
    // drops are counted in the Logger's `dropped`, which `log_dropped`
    // shows (D-148).
    if (!start(1)) {
        FAIL() << "no logger";
    }
    EventRecord restart;
    restart.count = 1;
    constexpr int ROUNDS = 50;
    for (int round = 0; round < ROUNDS; ++round) {
        log().add(0, DeviceEvent::RESTART, restart, round * PERIOD, WALL);
        log().add(1, DeviceEvent::RESTART, restart, round * PERIOD, WALL);
    }
    (void)lines();
    EXPECT_GT(logger().dropped(), 0U);
}

// PR 182 M2: a record the full queue refused gives its count to the next
// summary, so that the counts add up to the events.
TEST_F(Logged, ARefusedRecordsCountGoesInTheNextSummary) {
    if (!start(1) || !hold()) {
        FAIL() << "no logger";
    }
    EventRecord restart;
    restart.count = 1;
    log().add(0, DeviceEvent::RESTART, restart, 0, WALL);  // takes the room
    restart.count = 2;
    log().add(1, DeviceEvent::RESTART, restart, 0, WALL);  // refused
    EXPECT_EQ(queue().dropped(), 1U);
    release();
    EXPECT_EQ(count_of(lines(), "driver-device-restart", "d2"), 0U);
    log().due(PERIOD - 1, WALL);
    EXPECT_EQ(count_of(lines(), "driver-device-restart", "d2"), 0U);
    log().due(PERIOD, WALL);
    const std::vector<std::string> got = lines();
    EXPECT_EQ(count_of(got, "driver-device-restart", "d1"), 1U);
    EXPECT_EQ(count_of(got, "driver-device-restart", "d2"), 2U);
}

// A summary due while the queue is full stays due, and goes at the next
// call with room: not a period later, and not dropped.
TEST_F(Logged, ADueSummaryWaitsForRoom) {
    if (!start(1)) {
        FAIL() << "no logger";
    }
    EventRecord restart;
    restart.count = 1;
    log().add(0, DeviceEvent::RESTART, restart, 0, WALL);
    log().add(0, DeviceEvent::RESTART, restart, 1, WALL);  // held
    (void)lines();
    if (!hold()) {
        FAIL() << "the sink was not entered";
    }
    log().add(1, DeviceEvent::RESTART, restart, PERIOD, WALL);  // the room
    log().due(PERIOD, WALL);  // d1's summary stays due
    EXPECT_EQ(queue().dropped(), 0U);
    release();
    EXPECT_EQ(count_of(lines(), "driver-device-restart", "d1"), 1U);
    log().due(PERIOD + 1, WALL);
    EXPECT_EQ(count_of(lines(), "driver-device-restart", "d1"), 2U);
}

bool Logged::held_with_one_each() {
    if (!start(1)) {
        return false;
    }
    EventRecord restart;
    restart.count = 1;
    for (std::size_t device = 0; device < 2; ++device) {
        log().add(device, DeviceEvent::RESTART, restart, 0, WALL);
        (void)lines();
        log().add(device, DeviceEvent::RESTART, restart, 1, WALL);
    }
    return hold();
}

// The restart counts of d1 and d2 in `lines`.
std::array<std::uint64_t, 2> restarts_of(
    const std::vector<std::string>& lines) {
    return {count_of(lines, "driver-device-restart", "d1"),
            count_of(lines, "driver-device-restart", "d2")};
}

// The stop's flush logs while there is room and says when it must wait.
TEST_F(Logged, TheStopsFlushWaitsForRoom) {
    ASSERT_TRUE(held_with_one_each());
    // One goes in, one waits, and nothing is dropped.
    const bool waits = !log().flush(WALL, false) && !log().flush(WALL, false) &&
                       queue().dropped() == 0;
    release();
    (void)lines();
    const bool done = log().flush(WALL, false);
    EXPECT_TRUE(waits);
    EXPECT_TRUE(done);
    EXPECT_EQ(restarts_of(lines()), (std::array<std::uint64_t, 2>{2, 2}));
}

// Out of time, the last flush drops what does not fit, and counts it.
TEST_F(Logged, TheLastFlushDropsWhatDoesNotFit) {
    ASSERT_TRUE(held_with_one_each());
    const bool done = log().flush(WALL, true);
    release();
    // Nothing held any more.
    const bool empty = log().flush(WALL, false);
    EXPECT_TRUE(done && empty);
    EXPECT_EQ(restarts_of(lines()), (std::array<std::uint64_t, 2>{2, 1}));
    EXPECT_EQ(queue().dropped(), 1U);
    EXPECT_EQ(logger().dropped(), 1U);
}

// PR 182 delta, item 2: a stop that runs out of time drops what is still
// held, so that the held counts reach `log_dropped` as drops.
TEST_F(Logged, AStopOutOfTimeDropsAndCountsWhatIsLeft) {
    ASSERT_TRUE(held_with_one_each());
    int waits = 0;
    flush_at_stop(log(), WALL, 3, [&waits] { ++waits; });
    const std::uint64_t dropped = queue().dropped();
    release();
    EXPECT_EQ(waits, 3);
    EXPECT_EQ(dropped, 1U);
    EXPECT_EQ(restarts_of(lines()), (std::array<std::uint64_t, 2>{2, 1}));
    EXPECT_EQ(logger().dropped(), 1U);
}

// A wait that makes room lets every held count go, and nothing is dropped.
TEST_F(Logged, AStopWhoseWaitMakesRoomLosesNothing) {
    ASSERT_TRUE(held_with_one_each());
    int waits = 0;
    flush_at_stop(log(), WALL, 3, [&] {
        ++waits;
        release();
        (void)lines();
    });
    EXPECT_EQ(waits, 1);
    EXPECT_EQ(restarts_of(lines()), (std::array<std::uint64_t, 2>{2, 2}));
    EXPECT_EQ(logger().dropped(), 0U);
}

TEST(EventLog, AnEventOfNoDeviceOrNoQueueIsIgnored) {
    const CompiledChannel channel = two_devices();
    EventLog without(channel, nullptr, PERIOD);
    without.add(0, DeviceEvent::MISMATCH, mismatch(1, "8"), 0, WALL);
    EXPECT_TRUE(without.flush(WALL, false));
    EXPECT_EQ(reason_text(static_cast<std::uint8_t>(MismatchReason::ABSENT)),
              "absent");
    EXPECT_EQ(
        reason_text(static_cast<std::uint8_t>(MismatchReason::UNREADABLE)),
        "unreadable");
    EXPECT_EQ(reason_text(99), "unknown");
}

}  // namespace
}  // namespace scada::driver::detail
