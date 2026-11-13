// Tests of the Logger: queueing, limits, and behaviour with a stalled sink.

#include "scada/common/log.h"
#include "scada/common/clock.h"
#include "scada/common/result.h"
#include "scada/common/status.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using scada::LogField;
using scada::Logger;
using scada::LogLevel;
using scada::LogRecord;
using scada::Result;
using scada::Status;
using scada::StatusCode;

// Longest time a test waits for something that must happen.
constexpr std::chrono::seconds WAIT{30};

// Queue capacity of the tests that do not care about it.
constexpr std::size_t CAPACITY = 64;

// What the sink saw, shared between the test and the logger's worker thread.
// It lives in the test, so it outlives the logger and can be inspected after
// the logger is destroyed.
class Capture {
public:
    // Called by the sink for each record, with its text form
    // "<priority>|<component>|<message>|KEY=value...". Blocks while stalled.
    [[nodiscard]] Status add(std::string text) {
        std::unique_lock lock(mutex_);
        ++entered_;
        changed_.notify_all();
        changed_.wait(lock, [this] { return !stalled_; });
        records_.push_back(std::move(text));
        changed_.notify_all();
        return failing_ ? Status(StatusCode::IO_ERROR, "sink failed")
                        : Status();
    }

    [[nodiscard]] std::vector<std::string> snapshot() {
        const std::scoped_lock lock(mutex_);
        return records_;
    }

    // Waits until `wanted` calls of the sink have started.
    [[nodiscard]] bool wait_for_entered(int wanted) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, WAIT,
                                 [&] { return entered_ >= wanted; });
    }

    // While stalled, the sink blocks inside `add`.
    void set_stalled(bool value) {
        const std::scoped_lock lock(mutex_);
        stalled_ = value;
        changed_.notify_all();
    }

    // While failing, the sink reports an error for every record.
    void set_failing(bool value) {
        const std::scoped_lock lock(mutex_);
        failing_ = value;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> records_;
    int entered_ = 0;
    bool stalled_ = false;
    bool failing_ = false;
};

class CaptureSink final : public scada::LogSink {
public:
    explicit CaptureSink(Capture& capture) : capture_(capture) {}

    [[nodiscard]] Status write(const LogRecord& record) noexcept override {
        std::string text = std::to_string(static_cast<int>(record.level));
        text.append("|").append(record.component);
        text.append("|").append(record.message);
        for (const LogField& field : record.fields) {
            text.append("|").append(field.key).append("=").append(field.value);
        }
        return capture_.add(std::move(text));
    }

private:
    Capture& capture_;  // owned by the test, which outlives the logger
};

Result<Logger> make_logger(Capture& capture, std::size_t capacity = CAPACITY,
                           LogLevel min_level = LogLevel::INFO) {
    return Logger::create(std::make_unique<CaptureSink>(capture),
                          {"unit-test", min_level, capacity});
}

// Returns the outcome of creating a logger with the given options.
StatusCode create_code(std::string_view component, std::size_t capacity) {
    Capture capture;
    return Logger::create(std::make_unique<CaptureSink>(capture),
                          {component, LogLevel::INFO, capacity})
        .status()
        .code();
}

TEST(Logger, CreateRejectsANullSink) {
    EXPECT_EQ(Logger::create(nullptr, {"unit-test"}).status().code(),
              StatusCode::INVALID_ARGUMENT);
}

TEST(Logger, CreateChecksTheComponentName) {
    const std::string longest(scada::LOG_MAX_COMPONENT_BYTES, 'a');
    EXPECT_EQ(create_code("", CAPACITY), StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(create_code("bad name", CAPACITY), StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(create_code("line\nbreak", CAPACITY),
              StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(create_code(longest + "a", CAPACITY),
              StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(create_code(longest, CAPACITY), StatusCode::OK);
}

TEST(Logger, CreateChecksTheQueueCapacity) {
    EXPECT_EQ(create_code("unit-test", 0), StatusCode::OUT_OF_RANGE);
    EXPECT_EQ(create_code("unit-test", scada::LOG_MAX_QUEUE_CAPACITY + 1),
              StatusCode::OUT_OF_RANGE);
    EXPECT_EQ(create_code("unit-test", 1), StatusCode::OK);
    EXPECT_EQ(create_code("unit-test", scada::LOG_MAX_QUEUE_CAPACITY),
              StatusCode::OK);
}

TEST(Logger, DeliversInOrder) {
    Capture capture;
    Result<Logger> logger = make_logger(capture);
    EXPECT_TRUE(logger.ok()) << "deliver: logger created";
    if (!logger.ok()) {
        return;
    }
    logger.value().log(LogLevel::INFO, "started");
    logger.value().log(LogLevel::WARNING, "link down",
                       {{"PEER", "ps2"}, {"ATTEMPT", "3"}});
    const std::array<LogField, 1> fields{LogField{"CODE", "17"}};
    logger.value().log(LogLevel::ERROR, "write failed", fields);

    EXPECT_TRUE(logger.value().flush(WAIT).ok()) << "deliver: flush succeeds";
    const std::vector<std::string> expected{
        "6|unit-test|started",
        "4|unit-test|link down|PEER=ps2|ATTEMPT=3",
        "3|unit-test|write failed|CODE=17",
    };
    EXPECT_TRUE(capture.snapshot() == expected)
        << "deliver: records arrive complete and in order";
    EXPECT_TRUE(logger.value().dropped() == 0 &&
                logger.value().sink_failures() == 0)
        << "deliver: nothing dropped or refused";
}

TEST(Logger, LevelFilter) {
    Capture capture;
    Result<Logger> logger = make_logger(capture, CAPACITY, LogLevel::WARNING);
    if (!logger.ok()) {
        FAIL() << "filter: logger created";
    }
    logger.value().log(LogLevel::DEBUG, "debug");
    logger.value().log(LogLevel::INFO, "info");
    logger.value().log(LogLevel::NOTICE, "notice");
    logger.value().log(LogLevel::WARNING, "warning");
    logger.value().log(LogLevel::ERROR, "error");
    logger.value().log(LogLevel::CRITICAL, "critical");
    EXPECT_TRUE(logger.value().flush(WAIT).ok()) << "filter: flush succeeds";
    const std::vector<std::string> expected{
        "4|unit-test|warning", "3|unit-test|error", "2|unit-test|critical"};
    EXPECT_TRUE(capture.snapshot() == expected)
        << "filter: only records at or above min_level pass";
}

// Logs one record through a new logger and returns what the sink received
// for it, without the "<priority>|<component>|" prefix. Returns "<failed>" if
// the record did not arrive alone.
std::string deliver_one(std::string_view message,
                        std::span<const LogField> fields) {
    const std::string_view prefix = "6|unit-test|";
    Capture capture;
    Result<Logger> logger = make_logger(capture);
    if (!logger.ok()) {
        return "<failed>";
    }
    logger.value().log(LogLevel::INFO, message, fields);
    if (!logger.value().flush(WAIT).ok()) {
        return "<failed>";
    }
    const std::vector<std::string> records = capture.snapshot();
    if (records.size() != 1 || !records.front().starts_with(prefix)) {
        return "<failed>";
    }
    return records.front().substr(prefix.size());
}

TEST(Logger, LongMessageIsCutToTheLimit) {
    const std::string limit(scada::LOG_MAX_MESSAGE_BYTES, 'm');
    EXPECT_EQ(deliver_one(limit + "m", {}), limit);
}

TEST(Logger, MessageIsCutAtACharacterBoundary) {
    // A two-byte character straddling the limit is removed whole.
    const std::string before(scada::LOG_MAX_MESSAGE_BYTES - 1, 'm');
    EXPECT_EQ(deliver_one(before + "\xC3\xA9", {}), before);
}

TEST(Logger, LongValueIsCutAndBadKeysAreOmitted) {
    const std::string limit(scada::LOG_MAX_VALUE_BYTES, 'v');
    const std::string too_long = limit + "v";
    const std::vector<LogField> fields{
        {"GOOD", too_long},    {"lower", "x"},      {"", "x"},
        {"MESSAGE", "forged"}, {"PRIORITY", "0"},   {"WITH SPACE", "x"},
        {"LINE\nBREAK", "x"},  {"ALSO_GOOD_2", "y"}};
    EXPECT_EQ(deliver_one("fields", fields),
              "fields|GOOD=" + limit + "|ALSO_GOOD_2=y");
}

TEST(Logger, FieldsBeyondTheMaximumAreOmitted) {
    const std::vector<LogField> many(scada::LOG_MAX_FIELDS + 3,
                                     LogField{"K", "v"});
    const std::string record = deliver_one("many", many);
    EXPECT_EQ(
        static_cast<std::size_t>(std::count(record.begin(), record.end(), '=')),
        scada::LOG_MAX_FIELDS);
}

TEST(Logger, RecordAtEveryMaximumIsDeliveredIntact) {
    const std::string message(scada::LOG_MAX_MESSAGE_BYTES, 'm');
    const std::string key(scada::LOG_MAX_KEY_BYTES, 'K');
    const std::string value(scada::LOG_MAX_VALUE_BYTES, 'v');
    const std::vector<LogField> full(scada::LOG_MAX_FIELDS,
                                     LogField{key, value});
    std::string expected = message;
    for (std::size_t i = 0; i < scada::LOG_MAX_FIELDS; ++i) {
        expected.append("|").append(key).append("=").append(value);
    }
    EXPECT_EQ(deliver_one(message, full), expected);
}

// Logs `count` numbered records and returns the longest time one call took,
// in nanoseconds.
std::int64_t longest_log_call_ns(Logger& logger, std::size_t count) {
    std::int64_t longest_ns = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::int64_t start = scada::monotonic_ns();
        logger.log(LogLevel::INFO, "queued", {{"N", std::to_string(i)}});
        longest_ns = std::max(longest_ns, scada::monotonic_ns() - start);
    }
    return longest_ns;
}

// The stalled-sink scenario: the sink blocks, more records are logged than
// the queue holds, then the sink recovers.
constexpr std::size_t STALL_QUEUE_SIZE = 8;
constexpr std::size_t STALL_EXTRA = 5;

struct StallOutcome {
    bool sink_stalled = false;         // the worker blocked inside the sink
    std::int64_t longest_call_ns = 0;  // slowest log call during the stall
    std::uint64_t dropped = 0;
    std::size_t delivered_during_stall = 0;
    StatusCode flush_during_stall = StatusCode::OK;
    StatusCode flush_after_recovery = StatusCode::INTERNAL;
    std::vector<std::string> records;  // everything the sink received
};

StallOutcome run_stalled_sink_scenario() {
    constexpr std::chrono::milliseconds SHORT_WAIT{20};
    StallOutcome outcome;
    Capture capture;
    capture.set_stalled(true);
    Result<Logger> logger = make_logger(capture, STALL_QUEUE_SIZE);
    if (!logger.ok()) {
        return outcome;
    }
    // The worker takes the first record and blocks inside the sink; from
    // then on the queue is empty and nothing is delivered.
    logger.value().log(LogLevel::INFO, "first");
    outcome.sink_stalled = capture.wait_for_entered(1);

    outcome.longest_call_ns =
        longest_log_call_ns(logger.value(), STALL_QUEUE_SIZE + STALL_EXTRA);
    outcome.dropped = logger.value().dropped();
    outcome.delivered_during_stall = capture.snapshot().size();
    outcome.flush_during_stall = logger.value().flush(SHORT_WAIT).code();

    capture.set_stalled(false);
    outcome.flush_after_recovery = logger.value().flush(WAIT).code();
    outcome.records = capture.snapshot();
    return outcome;
}

// Acceptance criterion of issue P0-06: logging never blocks the caller for
// more than a bounded time, shown with a stalled sink.
TEST(Logger, StalledSinkNeverBlocksTheCaller) {
    // Far above what a call needs, far below "blocked": the sink stays
    // stalled for the whole measurement, so a call that waited for it would
    // never return at all, and the scenario would not finish.
    constexpr std::int64_t BOUND_NS = scada::NANOSECONDS_PER_SECOND;
    const StallOutcome outcome = run_stalled_sink_scenario();
    ASSERT_TRUE(outcome.sink_stalled);
    EXPECT_LT(outcome.longest_call_ns, BOUND_NS);
    // Exactly the records beyond the capacity are dropped.
    EXPECT_EQ(outcome.dropped, STALL_EXTRA);
    EXPECT_EQ(outcome.delivered_during_stall, 0U);
    // flush reports a timeout instead of waiting for the sink.
    EXPECT_EQ(outcome.flush_during_stall, StatusCode::TIMEOUT);
}

TEST(Logger, StalledSinkGetsTheQueueAndTheLossReportAfterRecovery) {
    const StallOutcome outcome = run_stalled_sink_scenario();
    ASSERT_TRUE(outcome.sink_stalled);
    EXPECT_EQ(outcome.flush_after_recovery, StatusCode::OK);
    // The oldest records are kept, then the loss is reported.
    std::vector<std::string> expected{"6|unit-test|first"};
    for (std::size_t i = 0; i < STALL_QUEUE_SIZE; ++i) {
        expected.push_back("6|unit-test|queued|N=" + std::to_string(i));
    }
    expected.push_back(
        "4|unit-test|log records dropped: the queue was full|DROPPED=" +
        std::to_string(STALL_EXTRA));
    EXPECT_EQ(outcome.records, expected);
}

TEST(Logger, DestructorDelivers) {
    constexpr std::size_t PENDING = 20;
    Capture capture;
    {
        Result<Logger> logger = make_logger(capture);
        if (!logger.ok()) {
            FAIL() << "destructor: logger created";
        }
        for (std::size_t i = 0; i < PENDING; ++i) {
            logger.value().log(LogLevel::INFO, "pending");
        }
    }
    EXPECT_TRUE(capture.snapshot().size() == PENDING)
        << "destructor: queued records are delivered first";
}

TEST(Logger, SinkFailureCounted) {
    Capture capture;
    capture.set_failing(true);
    Result<Logger> logger = make_logger(capture);
    if (!logger.ok()) {
        FAIL() << "failure: logger created";
    }
    logger.value().log(LogLevel::INFO, "one");
    logger.value().log(LogLevel::INFO, "two");
    EXPECT_TRUE(logger.value().flush(WAIT).ok()) << "failure: flush succeeds";
    EXPECT_TRUE(logger.value().sink_failures() == 2)
        << "failure: refused records are counted";
}

TEST(Logger, MovedFrom) {
    Capture capture;
    Result<Logger> source = make_logger(capture);
    if (!source.ok()) {
        FAIL() << "move: logger created";
    }
    Logger target = std::move(source).value();
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from state is what is tested
    source.value().log(LogLevel::INFO, "discarded");
    EXPECT_TRUE(source.value().flush(WAIT).ok() &&
                source.value().dropped() == 0)
        << "move: a moved-from logger is inert";
    target.log(LogLevel::INFO, "kept");
    EXPECT_TRUE(target.flush(WAIT).ok()) << "move: flush succeeds";
    EXPECT_TRUE(capture.snapshot() ==
                std::vector<std::string>{"6|unit-test|kept"})
        << "move: the target logs, the source discards";
}

TEST(Logger, ConcurrentCallers) {
    constexpr std::size_t THREADS = 4;
    constexpr std::size_t PER_THREAD = 200;
    Capture capture;
    Result<Logger> logger = make_logger(capture, THREADS * PER_THREAD);
    if (!logger.ok()) {
        FAIL() << "concurrent: logger created";
    }
    {
        Logger& shared = logger.value();
        std::vector<std::jthread> threads;
        threads.reserve(THREADS);
        for (std::size_t thread = 0; thread < THREADS; ++thread) {
            // `shared` outlives the threads: they are joined at the end of
            // this block.
            threads.emplace_back([&shared, thread] {
                for (std::size_t i = 0; i < PER_THREAD; ++i) {
                    shared.log(LogLevel::INFO, "concurrent",
                               {{"THREAD", std::to_string(thread)},
                                {"N", std::to_string(i)}});
                }
            });
        }
    }
    EXPECT_TRUE(logger.value().flush(WAIT).ok())
        << "concurrent: flush succeeds";
    const std::vector<std::string> records = capture.snapshot();
    EXPECT_TRUE(records.size() == THREADS * PER_THREAD &&
                logger.value().dropped() == 0)
        << "concurrent: every record delivered";
    // Per thread, records keep the order in which they were logged.
    bool ordered = true;
    for (std::size_t thread = 0; thread < THREADS; ++thread) {
        const std::string prefix =
            "6|unit-test|concurrent|THREAD=" + std::to_string(thread) + "|N=";
        std::size_t next = 0;
        for (const std::string& record : records) {
            if (record.starts_with(prefix)) {
                ordered = ordered && record == prefix + std::to_string(next);
                ++next;
            }
        }
        ordered = ordered && next == PER_THREAD;
    }
    EXPECT_TRUE(ordered) << "concurrent: per-thread order is kept";
}

}  // namespace
