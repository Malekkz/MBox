#include "scada/common/log.h"

#include "log_internal.h"
#include "scada/common/clock.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <new>
#include <stop_token>
#include <string>
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
    return std::all_of(component.begin(), component.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}

}  // namespace

struct Logger::Impl {
    Impl(std::unique_ptr<LogSink> sink_in, const LoggerOptions& options)
        : component(options.component),
          sink(std::move(sink_in)),
          min_level(options.min_level),
          ring(options.queue_capacity) {}

    // Worker thread: takes records from the queue and hands them to the
    // sink. After a stop request it delivers what is queued, then returns.
    void run(const std::stop_token& stop) noexcept;

    void deliver(const Slot& slot) noexcept;
    void deliver_drop_notice(std::uint64_t count) noexcept;
    void write(const LogRecord& record) noexcept;

    // Set once before the worker starts, read-only afterwards.
    const std::string component;
    const std::unique_ptr<LogSink> sink;  // used by the worker thread only
    const LogLevel min_level;

    // Protects the members below up to `busy`. Held only to copy one record
    // in or out, never during a sink call; that is what bounds `log`.
    std::mutex mutex;
    std::condition_variable_any work_cv;  // there is something to deliver
    std::condition_variable idle_cv;      // everything accepted was delivered
    std::vector<Slot> ring;               // fixed size; never grows
    std::size_t head = 0;                 // index of the oldest record
    std::size_t count = 0;                // records queued
    std::uint64_t dropped_unreported = 0;
    bool busy = false;  // the worker is inside a sink call

    // Statistics only, so relaxed ordering is enough: no other data depends
    // on the order in which these are observed.
    std::atomic<std::uint64_t> dropped_total{0};
    std::atomic<std::uint64_t> sink_failures{0};

    // Declared last: destroyed first, which stops and joins the worker while
    // everything it uses is still alive.
    std::jthread worker;
};

void Logger::Impl::run(const std::stop_token& stop) noexcept {
    Slot slot;
    for (;;) {
        std::uint64_t dropped = 0;
        bool have_record = false;
        {
            std::unique_lock lock(mutex);
            busy = false;
            if (count == 0 && dropped_unreported == 0) {
                idle_cv.notify_all();
            }
            // Returns false only if a stop was requested and nothing is left.
            if (!work_cv.wait(lock, stop, [this] {
                    return count > 0 || dropped_unreported > 0;
                })) {
                return;
            }
            if (count > 0) {
                slot = ring[head];
                head = (head + 1) % ring.size();
                --count;
                have_record = true;
            } else {
                dropped = std::exchange(dropped_unreported, 0);
            }
            busy = true;
        }
        if (have_record) {
            deliver(slot);
        } else {
            deliver_drop_notice(dropped);
        }
    }
}

void Logger::Impl::deliver(const Slot& slot) noexcept {
    const std::string_view text(slot.text.data(), slot.text.size());
    std::array<LogField, LOG_MAX_FIELDS> fields{};
    std::size_t offset = slot.message_size;
    for (std::size_t i = 0; i < slot.field_count; ++i) {
        const FieldSize size = slot.fields.at(i);
        fields.at(i) = {text.substr(offset, size.key),
                        text.substr(offset + size.key, size.value)};
        offset += static_cast<std::size_t>(size.key) + size.value;
    }
    write({slot.level, slot.time_ns, component,
           text.substr(0, slot.message_size),
           std::span<const LogField>(fields).first(slot.field_count)});
}

void Logger::Impl::deliver_drop_notice(std::uint64_t dropped) noexcept {
    std::array<char, 20> digits{};
    BoundedText number(digits);
    number.append_number(dropped);
    const std::array<LogField, 1> fields{LogField{"DROPPED", number.view()}};
    write({LogLevel::WARNING, realtime_ns(), component,
           "log records dropped: the queue was full", fields});
}

void Logger::Impl::write(const LogRecord& record) noexcept {
    if (!sink->write(record).ok()) {
        // Nowhere left to report it; `Logger::sink_failures` exposes it.
        sink_failures.fetch_add(1, std::memory_order_relaxed);
    }
}

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
        // The pointer is captured by value; the Impl has a stable address
        // and joins the thread in its destructor, so it outlives the thread.
        Impl* const self = impl.get();
        impl->worker = std::jthread(
            [self](const std::stop_token& stop) { self->run(stop); });
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
    if (!impl_ || level > impl_->min_level) {
        return;
    }
    const std::int64_t now = realtime_ns();
    const std::scoped_lock lock(impl_->mutex);
    if (impl_->count == impl_->ring.size()) {
        ++impl_->dropped_unreported;
        impl_->dropped_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::size_t tail = (impl_->head + impl_->count) % impl_->ring.size();
    fill_slot(impl_->ring[tail], level, now, message, fields);
    ++impl_->count;
    impl_->work_cv.notify_one();
}

void Logger::log(LogLevel level, std::string_view message,
                 std::initializer_list<LogField> fields) noexcept {
    log(level, message,
        std::span<const LogField>(fields.begin(), fields.size()));
}

Status Logger::flush(std::chrono::milliseconds timeout) noexcept {
    if (!impl_) {
        return {};
    }
    std::unique_lock lock(impl_->mutex);
    const bool idle = impl_->idle_cv.wait_for(lock, timeout, [this] {
        return impl_->count == 0 && impl_->dropped_unreported == 0 &&
               !impl_->busy;
    });
    if (!idle) {
        return {StatusCode::TIMEOUT, "log queue not delivered in time"};
    }
    return {};
}

std::uint64_t Logger::dropped() const noexcept {
    return impl_ ? impl_->dropped_total.load(std::memory_order_relaxed) : 0;
}

std::uint64_t Logger::sink_failures() const noexcept {
    return impl_ ? impl_->sink_failures.load(std::memory_order_relaxed) : 0;
}

}  // namespace scada
