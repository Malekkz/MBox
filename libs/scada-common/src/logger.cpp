#include "scada/common/log.h"

#include "log_internal.h"
#include "scada/common/clock.h"
#include "scada/common/panic.h"
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

namespace log_internal {

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

    const std::span<FieldSize, LOG_MAX_FIELDS> sizes(slot.fields);
    slot.field_count = 0;
    for (const LogField& field : fields) {
        if (slot.field_count == sizes.size()) {
            break;
        }
        if (!is_valid_log_key(field.key)) {
            continue;
        }
        const std::string_view value =
            truncate_utf8(field.value, LOG_MAX_VALUE_BYTES);
        text.append(field.key);
        text.append(value);
        // In range: the loop ends at the array's size (issue #181).
        sizes[slot.field_count] = {static_cast<std::uint16_t>(field.key.size()),
                                   static_cast<std::uint16_t>(value.size())};
        ++slot.field_count;
    }
}

std::string_view read_slot(const Slot& slot,
                           std::array<LogField, LOG_MAX_FIELDS>& fields,
                           std::size_t& field_count) noexcept {
    const std::string_view text(slot.text.data(), slot.text.size());
    const std::span<const FieldSize, LOG_MAX_FIELDS> sizes(slot.fields);
    const std::span<LogField, LOG_MAX_FIELDS> out(fields);
    field_count = std::min(slot.field_count, sizes.size());
    std::size_t offset = slot.message_size;
    for (std::size_t index = 0; index < field_count; ++index) {
        const FieldSize size = sizes[index];
        out[index] = {text.substr(offset, size.key),
                      text.substr(offset + size.key, size.value)};
        offset += static_cast<std::size_t>(size.key) + size.value;
    }
    return text.substr(0, slot.message_size);
}

}  // namespace log_internal

namespace {

using log_internal::BoundedText;
using log_internal::FieldSize;
using log_internal::RecordRing;
using log_internal::Slot;
using log_internal::truncate_utf8;

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

// The counts a Logger and its queues share: atomics, so that reading them
// takes no lock (D-148, PR 182 M1), and shared, so that a queue that
// outlives its Logger still has them.
struct LogCounts {
    // Records the LogQueues dropped because they were full.
    std::atomic<std::uint64_t> queue_dropped{0};
    // Records accepted, by `Logger::log` or a push, and not yet handed to
    // the sink (or discarded by the level filter).
    std::atomic<std::uint64_t> pending{0};
};

// A LogQueue: its ring (log_internal::RecordRing), its name for the drop
// notice, its counts, and its one producer.
class LogQueue::Impl {
public:
    Impl(std::size_t capacity, std::string name,
         std::shared_ptr<LogCounts> counts)
        : ring_(capacity), name_(std::move(name)), counts_(std::move(counts)) {}

    SCADA_RT bool push(LogLevel level, std::int64_t time_ns,
                       std::string_view message,
                       std::span<const LogField> fields) noexcept {
        check_producer();
        if (!ring_.push(level, time_ns, message, fields)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            counts_->queue_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        counts_->pending.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Worker thread: moves the oldest record into `out`; false when empty.
    bool pop(Slot& out) noexcept { return ring_.pop(out); }

    [[nodiscard]] bool empty() const noexcept { return ring_.empty(); }

    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return ring_.capacity();
    }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

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
    // One producer only (PR 182 S4): the first thread to push owns the
    // queue, and a push from another one is a programmer's error that would
    // corrupt the ring, so it ends the process.
    SCADA_RT void check_producer() noexcept {
        const std::thread::id self = std::this_thread::get_id();
        if (producer_.load(std::memory_order_relaxed) == self) {
            return;
        }
        std::thread::id none{};
        if (!producer_.compare_exchange_strong(none, self,
                                               std::memory_order_relaxed) &&
            none != self) {
            panic("a second thread pushed into a log queue");
        }
    }

    RecordRing ring_;
    std::string name_;
    std::shared_ptr<LogCounts> counts_;
    static_assert(std::atomic<std::thread::id>::is_always_lock_free);
    std::atomic<std::thread::id> producer_{};
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
          counts_(std::make_shared<LogCounts>()),
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
        caller_locks_.fetch_add(1, std::memory_order_relaxed);
        const std::scoped_lock lock(mutex_);
        if (count_ == ring_.size()) {
            ++dropped_unreported_;
            dropped_total_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::size_t tail = (head_ + count_) % ring_.size();
        log_internal::fill_slot(ring_[tail], level, now, message, fields);
        ++count_;
        counts_->pending.fetch_add(1, std::memory_order_relaxed);
        work_cv_.notify_one();
    }

    [[nodiscard]] Status flush(std::chrono::milliseconds timeout) noexcept {
        caller_locks_.fetch_add(1, std::memory_order_relaxed);
        std::unique_lock lock(mutex_);
        const bool idle =
            idle_cv_.wait_for(lock, timeout, [this] { return idle_locked(); });
        if (!idle) {
            return {StatusCode::TIMEOUT, "log queue not delivered in time"};
        }
        return {};
    }

    [[nodiscard]] Result<std::shared_ptr<LogQueue>> make_queue(
        std::size_t capacity, std::string_view name) noexcept {
        if (capacity == 0 || capacity > LOG_MAX_QUEUE_CAPACITY) {
            return Status(StatusCode::OUT_OF_RANGE,
                          "log queue capacity is out of range");
        }
        try {
            auto queue =
                std::make_shared<LogQueue>(std::make_unique<LogQueue::Impl>(
                    capacity, std::string(name), counts_));
            caller_locks_.fetch_add(1, std::memory_order_relaxed);
            const std::scoped_lock lock(mutex_);
            queues_.push_back(queue);
            // The worker polls from now on (S3: it does not while there is
            // no queue).
            work_cv_.notify_one();
            return queue;
        } catch (const std::bad_alloc&) {
            return Status(StatusCode::RESOURCE_EXHAUSTED,
                          "out of memory creating a log queue");
        }
    }

    // Lock-free: a heartbeat writer reads it (DI 11.3, PR 182 M1).
    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_total_.load(std::memory_order_relaxed) +
               counts_->queue_dropped.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t pending() const noexcept {
        return counts_->pending.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t caller_locks() const noexcept {
        return caller_locks_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t worker_rounds() const noexcept {
        return rounds_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t sink_failures() const noexcept {
        return sink_failures_.load(std::memory_order_relaxed);
    }

private:
    // Worker thread: takes records from its own queue and from every
    // LogQueue and hands them to the sink. It waits for a record of its own
    // queue, and, once a LogQueue exists, at most LOG_QUEUE_POLL_MS, since a
    // push into a LogQueue does not wake it; a Logger without queues never
    // polls (PR 182 S3). After a stop request it delivers what is queued
    // anywhere, then returns.
    void run(const std::stop_token& stop) noexcept {
        Slot slot;
        for (;;) {
            rounds_.fetch_add(1, std::memory_order_relaxed);
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
                const auto own = [this] {
                    return count_ > 0 || dropped_unreported_ > 0;
                };
                if (queues_.empty()) {
                    work_cv_.wait(lock, stop,
                                  [&] { return own() || !queues_.empty(); });
                } else {
                    work_cv_.wait_for(
                        lock, stop,
                        std::chrono::milliseconds(LOG_QUEUE_POLL_MS), own);
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
                counts_->pending.fetch_sub(1, std::memory_order_relaxed);
            } else if (dropped_count > 0) {
                deliver_drop_notice(dropped_count, {});
            }
            drain_queues(slot);
        }
    }

    // Worker thread: delivers every record the LogQueues hold now, and for
    // each queue that dropped records since its last notice, a notice that
    // names it (PR 182 S2).
    void drain_queues(Slot& slot) noexcept {
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
                counts_->pending.fetch_sub(1, std::memory_order_relaxed);
            }
            const std::uint64_t dropped_count = queue->take_unreported();
            if (dropped_count > 0) {
                deliver_drop_notice(dropped_count, queue->name());
            }
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
        std::array<LogField, LOG_MAX_FIELDS> fields{};
        std::size_t count = 0;
        const std::string_view message =
            log_internal::read_slot(slot, fields, count);
        write({slot.level, slot.time_ns, component_, message,
               std::span<const LogField>(fields).first(count)});
    }

    // `queue`: the name of the LogQueue that dropped them, none for the
    // Logger's own queue.
    void deliver_drop_notice(std::uint64_t dropped_count,
                             std::string_view queue) noexcept {
        std::array<char, log_internal::UINT64_DIGITS> digits{};
        BoundedText number(digits);
        number.append_number(dropped_count);
        const std::array<LogField, 2> fields{
            LogField{"QUEUE", queue}, LogField{"DROPPED", number.view()}};
        write({LogLevel::WARNING, realtime_ns(), component_,
               "log records dropped: the queue was full",
               queue.empty() ? std::span<const LogField>(fields).last(1)
                             : std::span<const LogField>(fields)});
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
    std::shared_ptr<LogCounts> counts_;  // shared with every LogQueue

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
    // Times a caller (not the worker) took `mutex_`: the probe of the tests
    // that show a path takes no lock (PR 182 M1).
    std::atomic<std::uint64_t> caller_locks_{0};
    // Rounds of the worker's loop: the probe of the test that a Logger
    // without queues does not poll (PR 182 S3).
    std::atomic<std::uint64_t> rounds_{0};

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
    std::size_t capacity, std::string_view name) noexcept {
    if (!impl_) {
        return Status(StatusCode::FAILED_PRECONDITION,
                      "the logger was moved from");
    }
    return impl_->make_queue(capacity, name);
}

std::uint64_t Logger::dropped() const noexcept {
    return impl_ ? impl_->dropped() : 0;
}

std::uint64_t Logger::pending() const noexcept {
    return impl_ ? impl_->pending() : 0;
}

std::uint64_t Logger::caller_locks() const noexcept {
    return impl_ ? impl_->caller_locks() : 0;
}

std::uint64_t Logger::worker_rounds() const noexcept {
    return impl_ ? impl_->worker_rounds() : 0;
}

std::uint64_t Logger::sink_failures() const noexcept {
    return impl_ ? impl_->sink_failures() : 0;
}

}  // namespace scada
