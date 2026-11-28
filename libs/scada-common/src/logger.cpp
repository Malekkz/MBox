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

// A single-producer, single-consumer ring. `tail_` is written by the
// producer only and `head_` by the Logger's worker only; each publishes the
// slots before it with release, and the other side reads it with acquire, so
// a slot is never read and written at once. The indexes count records since
// creation: at 2^64 records they would wrap, which no process lives to see.
class LogQueue::Impl {
public:
    explicit Impl(std::size_t capacity) : ring_(capacity) {}

    SCADA_RT bool push(LogLevel level, std::int64_t time_ns,
                       std::string_view message,
                       std::span<const LogField> fields) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail - head == ring_.size()) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        fill_slot(ring_[tail % ring_.size()], level, time_ns, message, fields);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Worker thread: moves the oldest record into `out`; false when empty.
    bool pop(Slot& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head == tail) {
            return false;
        }
        out = ring_[head % ring_.size()];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Any thread: true if no record waits. Exact for the worker; another
    // thread may see a record that is being taken.
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return ring_.size(); }

    // Drops already in a notice. Any thread may read it with the Logger's
    // mutex held; the worker writes it outside the mutex, so it is atomic.
    [[nodiscard]] std::uint64_t reported() const noexcept {
        return reported_.load(std::memory_order_relaxed);
    }

    // Worker thread only: drops counted since the last notice.
    std::uint64_t take_unreported() noexcept {
        const std::uint64_t now = dropped();
        return now - reported_.exchange(now, std::memory_order_relaxed);
    }

private:
    std::vector<Slot> ring_;  // fixed size; never grows
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
    // Statistics only, as the Logger's own counts.
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> reported_{0};  // written by the worker only
};

LogQueue::LogQueue(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

LogQueue::~LogQueue() = default;

bool LogQueue::push(LogLevel level, std::int64_t time_ns,
                    std::string_view message,
                    std::span<const LogField> fields) noexcept {
    return impl_->push(level, time_ns, message, fields);
}

bool LogQueue::push(LogLevel level, std::int64_t time_ns,
                    std::string_view message,
                    std::initializer_list<LogField> fields) noexcept {
    return impl_->push(
        level, time_ns, message,
        std::span<const LogField>(fields.begin(), fields.size()));
}

std::uint64_t LogQueue::dropped() const noexcept {
    return impl_->dropped();
}

std::size_t LogQueue::capacity() const noexcept {
    return impl_->capacity();
}

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
        const bool idle =
            idle_cv_.wait_for(lock, timeout, [this] { return idle_locked(); });
        if (!idle) {
            return {StatusCode::TIMEOUT, "log queue not delivered in time"};
        }
        return {};
    }

    [[nodiscard]] Result<std::shared_ptr<LogQueue>> make_queue(
        std::size_t capacity) noexcept {
        if (capacity == 0 || capacity > LOG_MAX_QUEUE_CAPACITY) {
            return Status(StatusCode::OUT_OF_RANGE,
                          "log queue capacity is out of range");
        }
        try {
            auto queue = std::make_shared<LogQueue>(
                std::make_unique<LogQueue::Impl>(capacity));
            const std::scoped_lock lock(mutex_);
            queues_.push_back(queue);
            return queue;
        } catch (const std::bad_alloc&) {
            return Status(StatusCode::RESOURCE_EXHAUSTED,
                          "out of memory creating a log queue");
        }
    }

    [[nodiscard]] std::uint64_t dropped() const noexcept {
        std::uint64_t total = dropped_total_.load(std::memory_order_relaxed);
        const std::scoped_lock lock(mutex_);
        for (const std::shared_ptr<LogQueue>& queue : queues_) {
            total += queue->impl_->dropped();
        }
        return total;
    }

    [[nodiscard]] std::uint64_t sink_failures() const noexcept {
        return sink_failures_.load(std::memory_order_relaxed);
    }

private:
    // Worker thread: takes records from its own queue and from every
    // LogQueue and hands them to the sink. It waits for a record of its own
    // queue or at most LOG_QUEUE_POLL_MS, since a push into a LogQueue does
    // not wake it. After a stop request it delivers what is queued anywhere,
    // then returns.
    void run(const std::stop_token& stop) noexcept {
        Slot slot;
        for (;;) {
            std::uint64_t dropped_count = 0;
            bool have_record = false;
            {
                std::unique_lock lock(mutex_);
                busy_ = false;
                if (idle_locked()) {
                    idle_cv_.notify_all();
                    if (stop.stop_requested()) {
                        return;
                    }
                }
                work_cv_.wait_for(
                    lock, stop, std::chrono::milliseconds(LOG_QUEUE_POLL_MS),
                    [this] { return count_ > 0 || dropped_unreported_ > 0; });
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
            } else if (dropped_count > 0) {
                deliver_drop_notice(dropped_count);
            }
            drain_queues(slot);
        }
    }

    // Worker thread: delivers every record the LogQueues hold now, then one
    // notice for the records they dropped since the last one.
    void drain_queues(Slot& slot) noexcept {
        std::uint64_t dropped_count = 0;
        for (std::size_t index = 0;; ++index) {
            LogQueue::Impl* queue = nullptr;
            {
                // The list only grows, and it holds each queue as long as
                // this Impl lives, so the pointer stays valid after the lock.
                const std::scoped_lock lock(mutex_);
                if (index == queues_.size()) {
                    break;
                }
                queue = queues_[index]->impl_.get();
            }
            // At most one ring's worth per round, so that a producer that
            // keeps pushing cannot hold the worker on one queue.
            for (std::size_t taken = 0;
                 taken < queue->capacity() && queue->pop(slot); ++taken) {
                if (slot.level <= min_level_) {
                    deliver(slot);
                }
            }
            dropped_count += queue->take_unreported();
        }
        if (dropped_count > 0) {
            deliver_drop_notice(dropped_count);
        }
    }

    // True when everything accepted so far was handed to the sink. Called
    // with `mutex_` held.
    [[nodiscard]] bool idle_locked() const noexcept {
        if (count_ > 0 || dropped_unreported_ > 0 || busy_) {
            return false;
        }
        return std::all_of(queues_.begin(), queues_.end(),
                           [](const std::shared_ptr<LogQueue>& queue) {
                               return queue->impl_->empty() &&
                                      queue->impl_->dropped() ==
                                          queue->impl_->reported();
                           });
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
    mutable std::mutex mutex_;
    std::condition_variable_any work_cv_;  // there is something to deliver
    std::condition_variable idle_cv_;      // everything accepted was delivered
    std::vector<Slot> ring_;               // fixed size; never grows
    std::size_t head_ = 0;                 // index of the oldest record
    std::size_t count_ = 0;                // records queued
    std::uint64_t dropped_unreported_ = 0;
    bool busy_ = false;  // the worker is inside a sink call
    // The LogQueues, in the order made. Only grows; drained by the worker.
    std::vector<std::shared_ptr<LogQueue>> queues_;

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

Result<std::shared_ptr<LogQueue>> Logger::make_queue(
    std::size_t capacity) noexcept {
    if (!impl_) {
        return Status(StatusCode::FAILED_PRECONDITION,
                      "the logger was moved from");
    }
    return impl_->make_queue(capacity);
}

std::uint64_t Logger::dropped() const noexcept {
    return impl_ ? impl_->dropped() : 0;
}

std::uint64_t Logger::sink_failures() const noexcept {
    return impl_ ? impl_->sink_failures() : 0;
}

}  // namespace scada
