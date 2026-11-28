// Tests of LogQueue: a thread's own lock-free queue, drained by the
// Logger's worker (driver interface contract 11.3).

#include "log_capture.h"
#include "scada/common/log.h"
#include "scada/common/result.h"
#include "scada/common/status.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using scada::Logger;
using scada::LogLevel;
using scada::LogQueue;
using scada::Result;
using scada::StatusCode;
using scada::log_test::Capture;
using scada::log_test::make_logger;
using scada::log_test::WAIT;

constexpr std::int64_t TIME_NS = 1'000;
constexpr std::size_t SIZE = 8;

// A logger with a capture sink and one queue.
class Queued : public ::testing::Test {
protected:
    // Makes the logger and a queue of `size`; false if either failed.
    bool start(std::size_t size = SIZE,
               std::size_t logger_capacity = scada::log_test::CAPACITY) {
        Result<Logger> made = make_logger(capture_, logger_capacity);
        if (!made.ok()) {
            return false;
        }
        logger_ = std::make_unique<Logger>(std::move(made).value());
        queue_ = make_queue(size);
        return queue_ != nullptr;
    }
    LogQueue* make_queue(std::size_t size) {
        Result<std::shared_ptr<LogQueue>> queue = logger_->make_queue(size);
        if (!queue.ok()) {
            return nullptr;
        }
        held_.push_back(queue.value());
        return held_.back().get();
    }
    Capture& capture() { return capture_; }
    Logger& logger() { return *logger_; }
    LogQueue& queue() { return *queue_; }
    // Destroys the logger, which delivers what it holds.
    void end_logger() { logger_.reset(); }

private:
    Capture capture_;
    std::unique_ptr<Logger> logger_;
    std::vector<std::shared_ptr<LogQueue>> held_;  // the producers' share
    LogQueue* queue_ = nullptr;
};

TEST_F(Queued, MakeQueueChecksTheCapacity) {
    if (!start(1)) {
        FAIL() << "logger or queue not made";
    }
    EXPECT_EQ(queue().capacity(), 1U);
    EXPECT_EQ(logger().make_queue(0).status().code(), StatusCode::OUT_OF_RANGE);
    EXPECT_EQ(
        logger().make_queue(scada::LOG_MAX_QUEUE_CAPACITY + 1).status().code(),
        StatusCode::OUT_OF_RANGE);
}

TEST_F(Queued, AMovedFromLoggerMakesNoQueue) {
    if (!start()) {
        FAIL() << "logger or queue not made";
    }
    const Logger target = std::move(logger());
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from state is what is tested
    EXPECT_EQ(logger().make_queue(SIZE).status().code(),
              StatusCode::FAILED_PRECONDITION);
}

TEST_F(Queued, PushedRecordsAreDeliveredInOrderWithoutAWake) {
    if (!start()) {
        FAIL() << "logger or queue not made";
    }
    queue().push(LogLevel::INFO, TIME_NS, "started");
    queue().push(LogLevel::WARNING, TIME_NS, "link down",
                 {{"PEER", "ps2"}, {"ATTEMPT", "3"}});
    // Below min_level: queued, then discarded by the worker.
    queue().push(LogLevel::DEBUG, TIME_NS, "detail");
    queue().push(LogLevel::ERROR, TIME_NS, "write failed",
                 {{"bad key", "x"}, {"CODE", "17"}});
    // Nothing wakes the worker: it finds them at its poll.
    EXPECT_TRUE(logger().flush(WAIT).ok());
    const std::vector<std::string> expected{
        "6|unit-test|started",
        "4|unit-test|link down|PEER=ps2|ATTEMPT=3",
        "3|unit-test|write failed|CODE=17",
    };
    EXPECT_EQ(capture().snapshot(), expected);
    EXPECT_EQ(logger().dropped(), 0U);
}

// Pushes `count` records "queued" numbered from 0; returns how many the
// queue took.
std::size_t push_numbered(LogQueue& queue, std::size_t count) {
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (queue.push(LogLevel::INFO, TIME_NS, "queued",
                       {{"N", std::to_string(i)}})) {
            ++accepted;
        }
    }
    return accepted;
}

// The records of the loss test: the first, the `kept` queued ones, then
// the notice of `lost`.
std::vector<std::string> after_loss(std::size_t kept, std::size_t lost) {
    std::vector<std::string> expected{"6|unit-test|first"};
    for (std::size_t i = 0; i < kept; ++i) {
        expected.push_back("6|unit-test|queued|N=" + std::to_string(i));
    }
    expected.push_back(
        "4|unit-test|log records dropped: the queue was full|DROPPED=" +
        std::to_string(lost));
    return expected;
}

constexpr std::size_t EXTRA = 3;

// The worker blocks inside the sink with a record of its own queue, so it
// takes nothing from the LogQueue while SIZE + EXTRA records are pushed;
// returns how many the queue took, or 0 if the set-up failed.
std::size_t overfill_while_stalled(Capture& capture, Logger& logger,
                                   LogQueue& queue) {
    logger.log(LogLevel::INFO, "first");
    if (!capture.wait_for_entered(1)) {
        return 0;
    }
    return push_numbered(queue, SIZE + EXTRA);
}

TEST_F(Queued, AFullQueueDropsAndCounts) {
    capture().set_stalled(true);
    if (!start()) {
        FAIL() << "logger or queue not made";
    }
    EXPECT_EQ(overfill_while_stalled(capture(), logger(), queue()), SIZE);
    EXPECT_EQ(queue().dropped(), EXTRA);
    EXPECT_EQ(logger().dropped(), EXTRA);
    capture().set_stalled(false);
}

TEST_F(Queued, FullSaysWhetherAPushWouldDrop) {
    capture().set_stalled(true);
    ASSERT_TRUE(start());
    logger().log(LogLevel::INFO, "first");
    ASSERT_TRUE(capture().wait_for_entered(1));
    const bool room_before_the_last =
        push_numbered(queue(), SIZE - 1) == SIZE - 1 && !queue().full();
    const bool full_after_it = push_numbered(queue(), 1) == 1 && queue().full();
    // Asking drops nothing.
    const bool none_dropped = queue().dropped() == 0;
    capture().set_stalled(false);
    const bool drained = logger().flush(WAIT).ok();
    EXPECT_TRUE(room_before_the_last && full_after_it && none_dropped);
    EXPECT_TRUE(drained && !queue().full());
}

TEST_F(Queued, TheLossIsReportedAfterTheKeptRecords) {
    capture().set_stalled(true);
    if (!start()) {
        FAIL() << "logger or queue not made";
    }
    EXPECT_EQ(overfill_while_stalled(capture(), logger(), queue()), SIZE);
    capture().set_stalled(false);
    EXPECT_TRUE(logger().flush(WAIT).ok());
    EXPECT_EQ(capture().snapshot(), after_loss(SIZE, EXTRA));
    // Room again once drained.
    EXPECT_TRUE(queue().push(LogLevel::INFO, TIME_NS, "again"));
}

TEST_F(Queued, ANamedQueuesLossNamesIt) {
    // PR 182 S2: the notice says which thread or channel lost records.
    capture().set_stalled(true);
    if (!start()) {
        FAIL() << "logger or queue not made";
    }
    Result<std::shared_ptr<LogQueue>> named = logger().make_queue(1, "line1");
    if (!named.ok()) {
        FAIL() << "named queue not made";
    }
    logger().log(LogLevel::INFO, "first");
    ASSERT_TRUE(capture().wait_for_entered(1));
    EXPECT_TRUE(named.value()->push(LogLevel::INFO, TIME_NS, "kept"));
    EXPECT_FALSE(named.value()->push(LogLevel::INFO, TIME_NS, "lost"));
    capture().set_stalled(false);
    EXPECT_TRUE(logger().flush(WAIT).ok());
    EXPECT_EQ(capture().snapshot(),
              (std::vector<std::string>{
                  "6|unit-test|first", "6|unit-test|kept",
                  "4|unit-test|log records dropped: the queue was "
                  "full|QUEUE=line1|DROPPED=1"}));
}

TEST_F(Queued, TheCountsAndAPushTakeNoLock) {
    // DI 11.3, PR 182 M1: a thread that writes a heartbeat reads the drop
    // counts and pushes, and none of that takes the Logger's mutex.
    if (!start(1)) {
        FAIL() << "logger or queue not made";
    }
    const std::uint64_t before = logger().caller_locks();
    (void)queue().push(LogLevel::INFO, TIME_NS, "one");
    (void)queue().push(LogLevel::INFO, TIME_NS, "two");  // full, or not
    EXPECT_GE(logger().dropped(), 0U);
    (void)logger().pending();
    (void)logger().sink_failures();
    (void)queue().dropped();
    EXPECT_EQ(logger().caller_locks(), before);
    // The probe counts: `log` takes it.
    logger().log(LogLevel::INFO, "direct");
    EXPECT_EQ(logger().caller_locks(), before + 1);
}

TEST_F(Queued, PendingCountsWhatIsNotYetDelivered) {
    capture().set_stalled(true);
    if (!start()) {
        FAIL() << "logger or queue not made";
    }
    logger().log(LogLevel::INFO, "first");
    ASSERT_TRUE(capture().wait_for_entered(1));
    EXPECT_EQ(push_numbered(queue(), 3), 3U);
    // The first is in the sink; the three wait.
    EXPECT_EQ(logger().pending(), 4U);
    capture().set_stalled(false);
    EXPECT_TRUE(logger().flush(WAIT).ok());
    EXPECT_EQ(logger().pending(), 0U);
}

TEST(LoggerPolling, ALoggerWithoutQueuesDoesNotPoll) {
    // PR 182 S3: with no LogQueue the worker sleeps until a record comes;
    // with one, it looks at least every LOG_QUEUE_POLL_MS. The interval is
    // waited out on a condition that never holds: absence takes time to
    // show.
    static constexpr std::chrono::milliseconds INTERVAL{300};
    Capture capture;
    Result<Logger> made = make_logger(capture);
    if (!made.ok()) {
        FAIL() << "no logger";
    }
    Logger& logger = made.value();
    const auto wait_out = [] {
        std::mutex mutex;
        std::condition_variable never;
        std::unique_lock lock(mutex);
        (void)never.wait_for(lock, INTERVAL, [] { return false; });
    };
    const std::uint64_t idle = logger.worker_rounds();
    wait_out();
    EXPECT_LE(logger.worker_rounds() - idle, 1U);
    const Result<std::shared_ptr<LogQueue>> queue = logger.make_queue(4);
    ASSERT_TRUE(queue.ok());
    const std::uint64_t polling = logger.worker_rounds();
    wait_out();
    EXPECT_GE(logger.worker_rounds() - polling, 5U);
}

TEST_F(Queued, TheDestructorDeliversQueuedRecords) {
    if (!start()) {
        FAIL() << "logger or queue not made";
    }
    EXPECT_EQ(push_numbered(queue(), SIZE), SIZE);
    end_logger();
    EXPECT_EQ(capture().snapshot().size(), SIZE);
}

TEST_F(Queued, AQueueOutlivesItsLogger) {
    // A producer that outlives the Logger (a stalled thread never joined)
    // still pushes into its share: dropped and counted once full, never
    // into freed memory.
    if (!start(2)) {
        FAIL() << "logger or queue not made";
    }
    end_logger();
    EXPECT_TRUE(queue().push(LogLevel::INFO, TIME_NS, "one"));
    EXPECT_TRUE(queue().push(LogLevel::INFO, TIME_NS, "two"));
    EXPECT_FALSE(queue().push(LogLevel::INFO, TIME_NS, "three"));
    EXPECT_EQ(queue().dropped(), 1U);
}

TEST_F(Queued, AQueueOutlivesAMoveOfItsLogger) {
    if (!start()) {
        FAIL() << "logger or queue not made";
    }
    Logger moved = std::move(logger());
    EXPECT_TRUE(queue().push(LogLevel::INFO, TIME_NS, "after move"));
    EXPECT_TRUE(moved.flush(WAIT).ok());
    EXPECT_EQ(capture().snapshot(),
              std::vector<std::string>{"6|unit-test|after move"});
}

// Success if `records` holds thread `thread`'s `count` records in order.
::testing::AssertionResult in_order(const std::vector<std::string>& records,
                                    std::size_t thread, std::size_t count) {
    const std::string prefix =
        "6|unit-test|queued|THREAD=" + std::to_string(thread) + "|N=";
    std::size_t next = 0;
    for (const std::string& record : records) {
        if (record.starts_with(prefix)) {
            if (record != prefix + std::to_string(next)) {
                return ::testing::AssertionFailure()
                       << record << " where N=" << next << " was due";
            }
            ++next;
        }
    }
    if (next != count) {
        return ::testing::AssertionFailure()
               << "thread " << thread << ": " << next << " of " << count;
    }
    return ::testing::AssertionSuccess();
}

// Pushes `count` records into each queue from a thread of its own, and logs
// `count` through `logger` from one more thread; returns when all are done.
void run_producers(Logger& logger, const std::vector<LogQueue*>& queues,
                   std::size_t count) {
    std::vector<std::jthread> threads;
    threads.reserve(queues.size() + 1);
    for (std::size_t thread = 0; thread < queues.size(); ++thread) {
        threads.emplace_back([queue = queues[thread], thread, count] {
            for (std::size_t i = 0; i < count; ++i) {
                queue->push(LogLevel::INFO, TIME_NS, "queued",
                            {{"THREAD", std::to_string(thread)},
                             {"N", std::to_string(i)}});
            }
        });
    }
    // `logger` outlives the threads: they are joined before the return.
    threads.emplace_back([&logger, count] {
        for (std::size_t i = 0; i < count; ++i) {
            logger.log(LogLevel::INFO, "direct");
        }
    });
}

// Several threads, each with its own queue, while the Logger's own queue is
// used too: every record arrives once, each queue's in its order.
TEST_F(Queued, ConcurrentProducersEachKeepTheirOrder) {
    constexpr std::size_t THREADS = 4;
    constexpr std::size_t PER_THREAD = 500;
    if (!start(PER_THREAD, PER_THREAD)) {
        FAIL() << "logger or queue not made";
    }
    std::vector<LogQueue*> queues{&queue()};
    while (queues.size() < THREADS) {
        queues.push_back(make_queue(PER_THREAD));
    }
    if (std::find(queues.begin(), queues.end(), nullptr) != queues.end()) {
        FAIL() << "queue not made";
    }
    run_producers(logger(), queues, PER_THREAD);
    EXPECT_TRUE(logger().flush(WAIT).ok());
    const std::vector<std::string> records = capture().snapshot();
    EXPECT_EQ(records.size(), (THREADS + 1) * PER_THREAD);
    EXPECT_EQ(logger().dropped(), 0U);
    for (std::size_t thread = 0; thread < THREADS; ++thread) {
        EXPECT_TRUE(in_order(records, thread, PER_THREAD));
    }
}

// One producer against the draining worker through a small queue, for the
// thread sanitizer: the release stores and acquire loads of head and tail
// must order every slot's copy. Each record is delivered whole and in order,
// or dropped and counted; none is lost or torn.
TEST_F(Queued, OneProducerAndTheDrainUnderLoad) {
    constexpr std::size_t SMALL = 4;
    constexpr std::size_t PUSHES = 20'000;
    if (!start(SMALL)) {
        FAIL() << "logger or queue not made";
    }
    const std::size_t accepted = push_numbered(queue(), PUSHES);
    EXPECT_TRUE(logger().flush(WAIT).ok());
    EXPECT_EQ(accepted + queue().dropped(), PUSHES);
    std::size_t delivered = 0;
    std::size_t last = 0;
    bool ordered = true;
    for (const std::string& record : capture().snapshot()) {
        constexpr std::string_view PREFIX = "6|unit-test|queued|N=";
        if (!record.starts_with(PREFIX)) {
            continue;  // a drop notice
        }
        const std::size_t number = std::stoul(record.substr(PREFIX.size()));
        ordered = ordered && (delivered == 0 || number > last);
        last = number;
        ++delivered;
    }
    EXPECT_EQ(delivered, accepted);
    EXPECT_TRUE(ordered);
}

}  // namespace
