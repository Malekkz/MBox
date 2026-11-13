// Tests of the Logger: queueing, limits, and behaviour with a stalled sink.

#include "check.h"
#include "scada/common/clock.h"
#include "scada/common/log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using scada::LogField;
using scada::Logger;
using scada::LoggerOptions;
using scada::LogLevel;
using scada::LogRecord;
using scada::Result;
using scada::Status;
using scada::StatusCode;

// Longest time a test waits for something that must happen.
constexpr std::chrono::seconds WAIT{30};

// What the sink saw, shared between the test and the logger's worker thread.
// It lives in the test, so it outlives the logger and can be inspected after
// the logger is destroyed.
struct Capture {
    // Text form of one record: "<priority>|<component>|<message>|KEY=value...".
    std::vector<std::string> snapshot() {
        const std::scoped_lock lock(mutex);
        return records;
    }

    bool wait_for_records(std::size_t wanted) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, WAIT,
                                [&] { return records.size() >= wanted; });
    }

    bool wait_for_entered(int wanted) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, WAIT, [&] { return entered >= wanted; });
    }

    void set_stalled(bool value) {
        const std::scoped_lock lock(mutex);
        stalled = value;
        changed.notify_all();
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> records;
    int entered = 0;       // calls to write() that have started
    bool stalled = false;  // write() blocks while this is true
    bool failing = false;  // write() reports an error
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

        std::unique_lock lock(capture_.mutex);
        ++capture_.entered;
        capture_.changed.notify_all();
        capture_.changed.wait(lock, [this] { return !capture_.stalled; });
        capture_.records.push_back(std::move(text));
        capture_.changed.notify_all();
        return capture_.failing ? Status(StatusCode::IO_ERROR, "sink failed")
                                : Status();
    }

private:
    Capture& capture_;  // owned by the test, which outlives the logger
};

Result<Logger> make_logger(Capture& capture, std::size_t capacity = 64,
                           LogLevel min_level = LogLevel::INFO) {
    return Logger::create(std::make_unique<CaptureSink>(capture),
                          {"unit-test", min_level, capacity});
}

void test_create_validates(scada::test::Checker& checker) {
    Capture capture;
    const auto code_for = [&capture](std::string_view component,
                                     std::size_t capacity) {
        return Logger::create(std::make_unique<CaptureSink>(capture),
                              {component, LogLevel::INFO, capacity})
            .status()
            .code();
    };
    checker.expect(Logger::create(nullptr, {"unit-test"}).status().code() ==
                       StatusCode::INVALID_ARGUMENT,
                   "create: null sink rejected");
    checker.expect(code_for("", 8) == StatusCode::INVALID_ARGUMENT,
                   "create: empty component rejected");
    checker.expect(code_for("bad name", 8) == StatusCode::INVALID_ARGUMENT,
                   "create: component with a space rejected");
    checker.expect(code_for("line\nbreak", 8) == StatusCode::INVALID_ARGUMENT,
                   "create: component with a line break rejected");
    checker.expect(code_for(std::string(65, 'a'), 8) ==
                       StatusCode::INVALID_ARGUMENT,
                   "create: component of 65 bytes rejected");
    checker.expect(code_for(std::string(64, 'a'), 8) == StatusCode::OK,
                   "create: component of 64 bytes accepted");
    checker.expect(code_for("unit-test", 0) == StatusCode::OUT_OF_RANGE,
                   "create: capacity 0 rejected");
    checker.expect(code_for("unit-test", scada::LOG_MAX_QUEUE_CAPACITY + 1) ==
                       StatusCode::OUT_OF_RANGE,
                   "create: capacity above the maximum rejected");
}

void test_delivers_in_order(scada::test::Checker& checker) {
    Capture capture;
    Result<Logger> logger = make_logger(capture);
    checker.expect(logger.ok(), "deliver: logger created");
    if (!logger.ok()) {
        return;
    }
    logger.value().log(LogLevel::INFO, "started");
    logger.value().log(LogLevel::WARNING, "link down",
                       {{"PEER", "ps2"}, {"ATTEMPT", "3"}});
    const std::array<LogField, 1> fields{LogField{"CODE", "17"}};
    logger.value().log(LogLevel::ERROR, "write failed", fields);

    checker.expect(logger.value().flush(WAIT).ok(), "deliver: flush succeeds");
    const std::vector<std::string> expected{
        "6|unit-test|started",
        "4|unit-test|link down|PEER=ps2|ATTEMPT=3",
        "3|unit-test|write failed|CODE=17",
    };
    checker.expect(capture.snapshot() == expected,
                   "deliver: records arrive complete and in order");
    checker.expect(logger.value().dropped() == 0 &&
                       logger.value().sink_failures() == 0,
                   "deliver: nothing dropped or refused");
}

void test_level_filter(scada::test::Checker& checker) {
    Capture capture;
    Result<Logger> logger = make_logger(capture, 64, LogLevel::WARNING);
    if (!logger.ok()) {
        checker.expect(false, "filter: logger created");
        return;
    }
    logger.value().log(LogLevel::DEBUG, "debug");
    logger.value().log(LogLevel::INFO, "info");
    logger.value().log(LogLevel::NOTICE, "notice");
    logger.value().log(LogLevel::WARNING, "warning");
    logger.value().log(LogLevel::ERROR, "error");
    logger.value().log(LogLevel::CRITICAL, "critical");
    checker.expect(logger.value().flush(WAIT).ok(), "filter: flush succeeds");
    const std::vector<std::string> expected{
        "4|unit-test|warning", "3|unit-test|error", "2|unit-test|critical"};
    checker.expect(capture.snapshot() == expected,
                   "filter: only records at or above min_level pass");
}

void test_limits(scada::test::Checker& checker) {
    Capture capture;
    Result<Logger> logger = make_logger(capture);
    if (!logger.ok()) {
        checker.expect(false, "limits: logger created");
        return;
    }
    // 1: message one byte too long is cut to the limit.
    logger.value().log(LogLevel::INFO,
                       std::string(scada::LOG_MAX_MESSAGE_BYTES + 1, 'm'));
    // 2: a two-byte character straddling the limit is removed whole.
    logger.value().log(
        LogLevel::INFO,
        std::string(scada::LOG_MAX_MESSAGE_BYTES - 1, 'm') + "\xC3\xA9");
    // 3: value too long; invalid and reserved keys omitted.
    logger.value().log(
        LogLevel::INFO, "fields",
        {{"GOOD", std::string(scada::LOG_MAX_VALUE_BYTES + 10, 'v')},
         {"lower", "x"},
         {"", "x"},
         {"MESSAGE", "forged"},
         {"PRIORITY", "0"},
         {"WITH SPACE", "x"},
         {"LINE\nBREAK", "x"},
         {"ALSO_GOOD_2", "y"}});
    // 4: more fields than the limit.
    std::vector<LogField> many(scada::LOG_MAX_FIELDS + 3, LogField{"K", "v"});
    logger.value().log(LogLevel::INFO, "many", many);
    // 5: everything at its maximum still fits the queue slot.
    const std::string long_key(scada::LOG_MAX_KEY_BYTES, 'K');
    const std::string long_value(scada::LOG_MAX_VALUE_BYTES, 'v');
    std::vector<LogField> full(scada::LOG_MAX_FIELDS,
                               LogField{long_key, long_value});
    logger.value().log(LogLevel::INFO,
                       std::string(scada::LOG_MAX_MESSAGE_BYTES, 'm'), full);

    checker.expect(logger.value().flush(WAIT).ok(), "limits: flush succeeds");
    const std::vector<std::string> records = capture.snapshot();
    checker.expect(records.size() == 5, "limits: five records");
    if (records.size() != 5) {
        return;
    }
    const std::string prefix = "6|unit-test|";
    checker.expect(
        records[0] ==
            prefix + std::string(scada::LOG_MAX_MESSAGE_BYTES, 'm'),
        "limits: long message cut to the limit");
    checker.expect(
        records[1] ==
            prefix + std::string(scada::LOG_MAX_MESSAGE_BYTES - 1, 'm'),
        "limits: message cut at a character boundary");
    checker.expect(
        records[2] == prefix + "fields|GOOD=" +
                          std::string(scada::LOG_MAX_VALUE_BYTES, 'v') +
                          "|ALSO_GOOD_2=y",
        "limits: long value cut, invalid and reserved keys omitted");
    checker.expect(
        static_cast<std::size_t>(
            std::count(records[3].begin(), records[3].end(), '=')) ==
            scada::LOG_MAX_FIELDS,
        "limits: fields beyond the maximum omitted");
    std::string full_text =
        prefix + std::string(scada::LOG_MAX_MESSAGE_BYTES, 'm');
    for (std::size_t i = 0; i < scada::LOG_MAX_FIELDS; ++i) {
        full_text.append("|").append(long_key).append("=").append(long_value);
    }
    checker.expect(records[4] == full_text,
                   "limits: a record at every maximum is delivered intact");
}

// Acceptance criterion of issue P0-06: logging never blocks the caller for
// more than a bounded time, shown with a stalled sink.
void test_stalled_sink(scada::test::Checker& checker) {
    constexpr std::size_t CAPACITY = 8;
    constexpr std::size_t EXTRA = 5;
    // Far above what a call needs, far below "blocked": the sink stays
    // stalled for the whole measurement, so a call that waited for it would
    // never return at all.
    constexpr std::int64_t BOUND_NS = scada::NANOSECONDS_PER_SECOND;

    Capture capture;
    capture.stalled = true;
    Result<Logger> logger = make_logger(capture, CAPACITY);
    if (!logger.ok()) {
        checker.expect(false, "stalled: logger created");
        return;
    }

    // The worker takes the first record and blocks inside the sink; from
    // then on the queue is empty and nothing is delivered.
    logger.value().log(LogLevel::INFO, "first");
    checker.expect(capture.wait_for_entered(1), "stalled: sink is stalled");

    std::int64_t longest_ns = 0;
    for (std::size_t i = 0; i < CAPACITY + EXTRA; ++i) {
        const std::int64_t start = scada::monotonic_ns();
        logger.value().log(LogLevel::INFO, "queued",
                           {{"N", std::to_string(i)}});
        longest_ns = std::max(longest_ns, scada::monotonic_ns() - start);
    }
    // Reaching this line at all shows that no call waited for the sink.
    checker.expect(longest_ns < BOUND_NS,
                   "stalled: every log call returned within the bound");
    checker.expect(logger.value().dropped() == EXTRA,
                   "stalled: exactly the records beyond the capacity dropped");
    checker.expect(capture.snapshot().empty(),
                   "stalled: nothing delivered while the sink is stalled");
    checker.expect(logger.value().flush(std::chrono::milliseconds(20)).code() ==
                       StatusCode::TIMEOUT,
                   "stalled: flush reports a timeout instead of waiting");

    capture.set_stalled(false);
    checker.expect(logger.value().flush(WAIT).ok(),
                   "stalled: queue delivered after the sink recovers");
    std::vector<std::string> expected{"6|unit-test|first"};
    for (std::size_t i = 0; i < CAPACITY; ++i) {
        expected.push_back("6|unit-test|queued|N=" + std::to_string(i));
    }
    expected.emplace_back(
        "4|unit-test|log records dropped: the queue was full|DROPPED=5");
    checker.expect(capture.snapshot() == expected,
                   "stalled: oldest records kept, then the loss is reported");
}

void test_destructor_delivers(scada::test::Checker& checker) {
    Capture capture;
    {
        Result<Logger> logger = make_logger(capture);
        if (!logger.ok()) {
            checker.expect(false, "destructor: logger created");
            return;
        }
        for (int i = 0; i < 20; ++i) {
            logger.value().log(LogLevel::INFO, "pending");
        }
    }
    checker.expect(capture.snapshot().size() == 20,
                   "destructor: queued records are delivered first");
}

void test_sink_failure_counted(scada::test::Checker& checker) {
    Capture capture;
    capture.failing = true;
    Result<Logger> logger = make_logger(capture);
    if (!logger.ok()) {
        checker.expect(false, "failure: logger created");
        return;
    }
    logger.value().log(LogLevel::INFO, "one");
    logger.value().log(LogLevel::INFO, "two");
    checker.expect(logger.value().flush(WAIT).ok(), "failure: flush succeeds");
    checker.expect(logger.value().sink_failures() == 2,
                   "failure: refused records are counted");
}

void test_moved_from(scada::test::Checker& checker) {
    Capture capture;
    Result<Logger> source = make_logger(capture);
    if (!source.ok()) {
        checker.expect(false, "move: logger created");
        return;
    }
    Logger target = std::move(source).value();
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from state is what is tested
    source.value().log(LogLevel::INFO, "discarded");
    checker.expect(source.value().flush(WAIT).ok() &&
                       source.value().dropped() == 0,
                   "move: a moved-from logger is inert");
    target.log(LogLevel::INFO, "kept");
    checker.expect(target.flush(WAIT).ok(), "move: flush succeeds");
    checker.expect(
        capture.snapshot() == std::vector<std::string>{"6|unit-test|kept"},
        "move: the target logs, the source discards");
}

void test_concurrent_callers(scada::test::Checker& checker) {
    constexpr int THREADS = 4;
    constexpr int PER_THREAD = 200;
    Capture capture;
    Result<Logger> logger = make_logger(capture, THREADS * PER_THREAD);
    if (!logger.ok()) {
        checker.expect(false, "concurrent: logger created");
        return;
    }
    {
        Logger& shared = logger.value();
        std::vector<std::jthread> threads;
        for (int t = 0; t < THREADS; ++t) {
            // `shared` outlives the threads: they are joined at the end of
            // this block.
            threads.emplace_back([&shared, t] {
                for (int i = 0; i < PER_THREAD; ++i) {
                    shared.log(LogLevel::INFO, "concurrent",
                               {{"THREAD", std::to_string(t)},
                                {"N", std::to_string(i)}});
                }
            });
        }
    }
    checker.expect(logger.value().flush(WAIT).ok(),
                   "concurrent: flush succeeds");
    const std::vector<std::string> records = capture.snapshot();
    checker.expect(records.size() == THREADS * PER_THREAD &&
                       logger.value().dropped() == 0,
                   "concurrent: every record delivered");
    // Per thread, records keep the order in which they were logged.
    bool ordered = true;
    for (int t = 0; t < THREADS; ++t) {
        const std::string prefix =
            "6|unit-test|concurrent|THREAD=" + std::to_string(t) + "|N=";
        int next = 0;
        for (const std::string& record : records) {
            if (record.starts_with(prefix)) {
                ordered = ordered &&
                          record == prefix + std::to_string(next);
                ++next;
            }
        }
        ordered = ordered && next == PER_THREAD;
    }
    checker.expect(ordered, "concurrent: per-thread order is kept");
}

}  // namespace

int main() {
    scada::test::Checker checker;
    test_create_validates(checker);
    test_delivers_in_order(checker);
    test_level_filter(checker);
    test_limits(checker);
    test_stalled_sink(checker);
    test_destructor_delivers(checker);
    test_sink_failure_counted(checker);
    test_moved_from(checker);
    test_concurrent_callers(checker);
    return checker.exit_code();
}
