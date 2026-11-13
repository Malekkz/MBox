#include "scada/common/log.h"

#include "log_internal.h"
#include "scada/common/clock.h"
#include "scada/common/result.h"
#include "scada/common/status.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace scada {

namespace {

using log_internal::BoundedText;
using log_internal::truncate_utf8;

struct FieldSize {
    std::uint16_t key = 0;
    std::uint16_t value = 0;
};

constexpr std::size_t SLOT_TEXT_BYTES =
    LOG_MAX_MESSAGE_BYTES +
    LOG_MAX_FIELDS * (LOG_MAX_KEY_BYTES + LOG_MAX_VALUE_BYTES);

// One queued record. `text` holds the message, then key and value of each
// field in turn, without separators; the sizes say where each part ends.
// Fixed size, so that queueing a record never allocates.
struct Slot {
    std::array<char, SLOT_TEXT_BYTES> text{};
    std::array<FieldSize, LOG_MAX_FIELDS> fields{};
    std::int64_t time_ns = 0;
    std::size_t message_size = 0;
    std::size_t field_count = 0;
    LogLevel level = LogLevel::INFO;
};

void fill_slot(Slot& slot, LogLevel level, std::int64_t time_ns,
               std::string_view message,
               std::span<const LogField> fields) noexcept {
    slot.level = level;
    slot.time_ns = time_ns;

    // The limits make every part fit: SLOT_TEXT_BYTES is their sum.
    BoundedText text(slot.text);
    const std::string_view kept_message =
        truncate_utf8(message, LOG_MAX_MESSAGE_BYTES);
    text.append(kept_message);
    slot.message_size = kept_message.size();

    slot.field_count = 0;
    for (const LogField& field : fields) {
        if (slot.field_count == LOG_MAX_FIELDS) {
            break;
        }
        if (!is_valid_log_key(field.key)) {
            continue;
        }
        const std::string_view value =
            truncate_utf8(field.value, LOG_MAX_VALUE_BYTES);
        text.append(field.key);
        text.append(value);
        slot.fields.at(slot.field_count) = {
            static_cast<std::uint16_t>(field.key.size()),
            static_cast<std::uint16_t>(value.size())};
        ++slot.field_count;
    }
}

bool is_valid_component(std::string_view component) noexcept {
    if (component.empty() || component.size() > LOG_MAX_COMPONENT_BYTES) {
        return false;
    }
    return std::all_of(component.begin(), component.end(), [](char character) {
        return (character >= 'A' && character <= 'Z') ||
               (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '.' ||
               character == '_' || character == '-';
    });
}

}  // namespace

// The state behind a Logger. It has a stable address, which the worker thread
// relies on.
class Logger::Impl {
public:
    Impl(std::unique_ptr<LogSink> sink, const LoggerOptions& options)
        : component_(options.component),
          sink_(std::move(sink)),
          min_level_(options.min_level),
          ring_(options.queue_capacity) {}

    // Starts the worker thread. Called once, right after construction. May
    // throw std::system_error.
    void start() {
        // `this` is captured by value: the Impl is never moved and joins the
        // thread in its destructor, so it outlives the thread.
        worker_ =
            std::jthread([this](const std::stop_token& stop) { run(stop); });
    }

    void log(LogLevel level, std::string_view message,
             std::span<const LogField> fields) noexcept {
        if (level > min_level_) {
            return;
        }
        const std::int64_t now = realtime_ns();
        const std::scoped_lock lock(mutex_);
        if (count_ == ring_.size()) {
            ++dropped_unreported_;
            dropped_total_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::size_t tail = (head_ + count_) % ring_.size();
        fill_slot(ring_[tail], level, now, message, fields);
        ++count_;
        work_cv_.notify_one();
    }

    [[nodiscard]] Status flush(std::chrono::milliseconds timeout) noexcept {
        std::unique_lock lock(mutex_);
        const bool idle = idle_cv_.wait_for(lock, timeout, [this] {
            return count_ == 0 && dropped_unreported_ == 0 && !busy_;
        });
        if (!idle) {
            return {StatusCode::TIMEOUT, "log queue not delivered in time"};
        }
        return {};
    }

    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_total_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t sink_failures() const noexcept {
        return sink_failures_.load(std::memory_order_relaxed);
    }

private:
    // Worker thread: takes records from the queue and hands them to the
    // sink. After a stop request it delivers what is queued, then returns.
    void run(const std::stop_token& stop) noexcept {
        Slot slot;
        for (;;) {
            std::uint64_t dropped_count = 0;
            bool have_record = false;
            {
                std::unique_lock lock(mutex_);
                busy_ = false;
                if (count_ == 0 && dropped_unreported_ == 0) {
                    idle_cv_.notify_all();
                }
                // Returns false only if a stop was requested and nothing is
                // left to deliver.
                if (!work_cv_.wait(lock, stop, [this] {
                        return count_ > 0 || dropped_unreported_ > 0;
                    })) {
                    return;
                }
                if (count_ > 0) {
                    slot = ring_[head_];
                    head_ = (head_ + 1) % ring_.size();
                    --count_;
                    have_record = true;
                } else {
                    dropped_count = std::exchange(dropped_unreported_, 0);
                }
                busy_ = true;
            }
            if (have_record) {
                deliver(slot);
            } else {
                deliver_drop_notice(dropped_count);
            }
        }
    }

    void deliver(const Slot& slot) noexcept {
        const std::string_view text(slot.text.data(), slot.text.size());
        std::array<LogField, LOG_MAX_FIELDS> fields{};
        std::size_t offset = slot.message_size;
        for (std::size_t i = 0; i < slot.field_count; ++i) {
            const FieldSize size = slot.fields.at(i);
            fields.at(i) = {text.substr(offset, size.key),
                            text.substr(offset + size.key, size.value)};
            offset += static_cast<std::size_t>(size.key) + size.value;
        }
        write({slot.level, slot.time_ns, component_,
               text.substr(0, slot.message_size),
               std::span<const LogField>(fields).first(slot.field_count)});
    }

    void deliver_drop_notice(std::uint64_t dropped_count) noexcept {
        std::array<char, log_internal::UINT64_DIGITS> digits{};
        BoundedText number(digits);
        number.append_number(dropped_count);
        const std::array<LogField, 1> fields{
            LogField{"DROPPED", number.view()}};
        write({LogLevel::WARNING, realtime_ns(), component_,
               "log records dropped: the queue was full", fields});
    }

    void write(const LogRecord& record) noexcept {
        if (!sink_->write(record).ok()) {
            // Nowhere left to report it; `Logger::sink_failures` exposes it.
            sink_failures_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Set by the constructor, read-only afterwards.
    std::string component_;
    std::unique_ptr<LogSink> sink_;  // used by the worker thread only
    LogLevel min_level_;

    // Protects the members below up to `busy_`. Held only to copy one record
    // in or out, never during a sink call; that is what bounds `log`.
    std::mutex mutex_;
    std::condition_variable_any work_cv_;  // there is something to deliver
    std::condition_variable idle_cv_;      // everything accepted was delivered
    std::vector<Slot> ring_;               // fixed size; never grows
    std::size_t head_ = 0;                 // index of the oldest record
    std::size_t count_ = 0;                // records queued
    std::uint64_t dropped_unreported_ = 0;
    bool busy_ = false;  // the worker is inside a sink call

    // Statistics only, so relaxed ordering is enough: no other data depends
    // on the order in which these are observed.
    std::atomic<std::uint64_t> dropped_total_{0};
    std::atomic<std::uint64_t> sink_failures_{0};

    // Declared last: destroyed first, which stops and joins the worker while
    // everything it uses is still alive.
    std::jthread worker_;
};

Result<Logger> Logger::create(std::unique_ptr<LogSink> sink,
                              const LoggerOptions& options) noexcept {
    if (!sink) {
        return Status(StatusCode::INVALID_ARGUMENT, "log sink is null");
    }
    if (!is_valid_component(options.component)) {
        return Status(StatusCode::INVALID_ARGUMENT,
                      "log component name is invalid");
    }
    if (options.queue_capacity == 0 ||
        options.queue_capacity > LOG_MAX_QUEUE_CAPACITY) {
        return Status(StatusCode::OUT_OF_RANGE,
                      "log queue capacity is out of range");
    }
    try {
        auto impl = std::make_unique<Impl>(std::move(sink), options);
        impl->start();
        return Logger(std::move(impl));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::RESOURCE_EXHAUSTED,
                      "out of memory creating the logger");
    } catch (const std::system_error&) {
        return Status(StatusCode::UNAVAILABLE,
                      "cannot start the log worker thread");
    }
}

Logger::Logger(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Logger::~Logger() = default;
Logger::Logger(Logger&&) noexcept = default;
Logger& Logger::operator=(Logger&&) noexcept = default;

void Logger::log(LogLevel level, std::string_view message,
                 std::span<const LogField> fields) noexcept {
    if (impl_) {
        impl_->log(level, message, fields);
    }
}

void Logger::log(LogLevel level, std::string_view message,
                 std::initializer_list<LogField> fields) noexcept {
    log(level, message,
        std::span<const LogField>(fields.begin(), fields.size()));
}

Status Logger::flush(std::chrono::milliseconds timeout) noexcept {
    return impl_ ? impl_->flush(timeout) : Status();
}

std::uint64_t Logger::dropped() const noexcept {
    return impl_ ? impl_->dropped() : 0;
}

std::uint64_t Logger::sink_failures() const noexcept {
    return impl_ ? impl_->sink_failures() : 0;
}

}  // namespace scada
