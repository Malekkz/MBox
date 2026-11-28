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

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
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

// The records a sink received, as "MESSAGE|KEY=value|...".
class Lines {
public:
    void add(std::string line) {
        const std::scoped_lock lock(mutex_);
        lines_.push_back(std::move(line));
    }
    [[nodiscard]] std::vector<std::string> snapshot() {
        const std::scoped_lock lock(mutex_);
        return lines_;
    }

private:
    std::mutex mutex_;
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
    bool start(std::size_t capacity = 16) {
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
    EventRecord matched = mismatch(1, "7");
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
    log().flush(WALL);
    log().flush(WALL);  // nothing held any more
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
    for (int round = 0; round < 50; ++round) {
        log().add(0, DeviceEvent::RESTART, restart, round * PERIOD, WALL);
        log().add(1, DeviceEvent::RESTART, restart, round * PERIOD, WALL);
    }
    (void)lines();
    EXPECT_GT(logger().dropped(), 0U);
}

TEST(EventLog, AnEventOfNoDeviceOrNoQueueIsIgnored) {
    const CompiledChannel channel = two_devices();
    EventLog without(channel, nullptr, PERIOD);
    without.add(0, DeviceEvent::MISMATCH, mismatch(1, "8"), 0, WALL);
    without.flush(WALL);
    EXPECT_EQ(reason_text(static_cast<std::uint8_t>(MismatchReason::ABSENT)),
              "absent");
    EXPECT_EQ(
        reason_text(static_cast<std::uint8_t>(MismatchReason::UNREADABLE)),
        "unreadable");
    EXPECT_EQ(reason_text(99), "unknown");
}

}  // namespace
}  // namespace scada::driver::detail
