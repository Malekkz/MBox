#ifndef SCADA_COMMON_TESTS_LOG_CAPTURE_H
#define SCADA_COMMON_TESTS_LOG_CAPTURE_H

// A sink that records what a Logger delivers, for the logger tests.

#include "scada/common/log.h"
#include "scada/common/result.h"
#include "scada/common/status.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace scada::log_test {

using scada::LogField;
using scada::Logger;
using scada::LogLevel;
using scada::LogRecord;
using scada::Result;
using scada::Status;
using scada::StatusCode;

// Longest time a test waits for something that must happen.
inline constexpr std::chrono::seconds WAIT{30};

// Queue capacity of the tests that do not care about it.
inline constexpr std::size_t CAPACITY = 64;

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

inline Result<Logger> make_logger(Capture& capture,
                                  std::size_t capacity = CAPACITY,
                                  LogLevel min_level = LogLevel::INFO) {
    return Logger::create(std::make_unique<CaptureSink>(capture),
                          {"unit-test", min_level, capacity});
}

}  // namespace scada::log_test

#endif  // SCADA_COMMON_TESTS_LOG_CAPTURE_H
