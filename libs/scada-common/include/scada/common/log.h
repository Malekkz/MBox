#ifndef SCADA_COMMON_LOG_H
#define SCADA_COMMON_LOG_H

#include "scada/common/realtime.h"
#include "scada/common/result.h"
#include "scada/common/status.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <string_view>

// Structured logging (reference 14.6).
//
// A record is a severity, a message and up to LOG_MAX_FIELDS key/value
// fields. `Logger::log` copies the record into a bounded queue and returns; a
// worker thread owned by the Logger hands queued records to a `LogSink`. The
// caller therefore never waits for the sink. If the queue is full the record
// is dropped and counted, and the count is reported as a record of its own
// once the sink accepts records again.
//
// `log` takes a mutex and reads the clock, so it is not allowed in
// real-time code (CLAUDE.md, "Real-time code"). A thread that must never wait
// for another (a real-time thread, or one that writes a heartbeat or a tag:
// driver interface contract 11.3) logs through a `LogQueue` of its own
// instead: a lock-free single-producer queue that the same worker thread
// drains.

namespace scada {

// Severity. The values are the syslog priorities, which the journal uses.
enum class LogLevel : std::uint8_t {
    CRITICAL = 2,  // the process cannot continue
    ERROR = 3,     // an operation failed
    WARNING = 4,   // something is wrong but was handled
    NOTICE = 5,    // normal but significant, such as start and stop
    INFO = 6,      // normal operation
    DEBUG = 7,     // detail for fault finding
};

// Size limits of one record, in bytes. Longer text is cut at a UTF-8
// character boundary. They bound the memory of the queue and keep every
// record within one journal datagram and one atomic pipe write.
inline constexpr std::size_t LOG_MAX_MESSAGE_BYTES = 384;
inline constexpr std::size_t LOG_MAX_KEY_BYTES = 32;
inline constexpr std::size_t LOG_MAX_VALUE_BYTES = 96;
inline constexpr std::size_t LOG_MAX_FIELDS = 8;
inline constexpr std::size_t LOG_MAX_COMPONENT_BYTES = 64;

// Queue capacity limits, in records.
inline constexpr std::size_t LOG_DEFAULT_QUEUE_CAPACITY = 256;
inline constexpr std::size_t LOG_MAX_QUEUE_CAPACITY = 65536;

// Name of the field in which the journal sink stores the time the record was
// created; the journal's own timestamp is the later time of delivery.
inline constexpr std::string_view LOG_TIME_FIELD = "SCADA_TIME_NS";

// One key/value field of a record. Both views are borrowed for the duration
// of the call they are passed to.
struct LogField {
    std::string_view key;
    std::string_view value;
};

// Returns true if `key` may be used as a field key: 1 to LOG_MAX_KEY_BYTES
// characters, the first A-Z and the rest A-Z, 0-9 or underscore (the
// journal's rule), and not one of the keys the sinks write themselves
// (MESSAGE, PRIORITY, SYSLOG_IDENTIFIER, SCADA_TIME_NS).
[[nodiscard]] bool is_valid_log_key(std::string_view key) noexcept;

// A record as a sink receives it. All views are valid only during the call
// to `LogSink::write`; a sink that keeps data must copy it. The Logger
// guarantees the size limits above and valid keys; message and values are
// otherwise arbitrary bytes and must be treated as untrusted.
struct LogRecord {
    LogLevel level = LogLevel::INFO;
    std::int64_t time_ns = 0;  // CLOCK_REALTIME when `log` was called
    std::string_view component;
    std::string_view message;
    std::span<const LogField> fields;
};

// Destination of log records. `write` is called from the Logger's worker
// thread only, one call at a time.
class LogSink {
public:
    virtual ~LogSink() = default;
    LogSink(const LogSink&) = delete;
    LogSink& operator=(const LogSink&) = delete;
    LogSink(LogSink&&) = delete;
    LogSink& operator=(LogSink&&) = delete;

    // Delivers one record. Should return within a bounded time, with an
    // error if the destination does not accept the record; a sink that
    // blocks delays later records and `Logger` destruction, but never the
    // callers of `Logger::log`. Must not throw.
    [[nodiscard]] virtual Status write(const LogRecord& record) noexcept = 0;

protected:
    LogSink() = default;
};

// Returns a sink that sends each record as one datagram in the journal's
// native protocol to the socket at `socket_path`. Fields: MESSAGE, PRIORITY,
// SYSLOG_IDENTIFIER (the component), SCADA_TIME_NS and the record's own
// fields. Sending never blocks: if the journal does not take the datagram,
// `write` returns UNAVAILABLE.
//
// Fails with INVALID_ARGUMENT if the path is empty or too long for a socket
// address, and UNAVAILABLE if no journal listens there.
[[nodiscard]] Result<std::unique_ptr<LogSink>> make_journal_sink(
    std::string_view socket_path = "/run/systemd/journal/socket") noexcept;

// Returns a sink that writes each record as one text line to `descriptor`:
//   <UTC time> <LEVEL> <component>: <message> KEY="value" ...
// Control characters, and quotes inside values, are escaped, so a record
// cannot forge a second line. A line is at most 4096 bytes. `write` waits at
// most 100 ms for the descriptor to become writable and then returns
// TIMEOUT.
//
// The descriptor is borrowed: the caller keeps it open for the life of the
// sink. Fails with INVALID_ARGUMENT if `descriptor` is negative.
[[nodiscard]] Result<std::unique_ptr<LogSink>> make_stream_sink(
    int descriptor) noexcept;

// Returns a sink that writes to `primary` and, whenever that fails, writes
// the same record to `secondary` and returns its result. Fails with
// INVALID_ARGUMENT if either is null.
[[nodiscard]] Result<std::unique_ptr<LogSink>> make_fallback_sink(
    std::unique_ptr<LogSink> primary,
    std::unique_ptr<LogSink> secondary) noexcept;

// Returns the sink a service should use. If the service manager connected
// the standard error stream to the journal (it then sets JOURNAL_STREAM) and
// the journal socket can be reached, records go to the journal with the
// standard error stream as fallback. Otherwise, for example when started
// from a terminal or a test, records go to the standard error stream.
[[nodiscard]] Result<std::unique_ptr<LogSink>> make_default_sink() noexcept;

struct LoggerOptions {
    // Name of the logging process or library, for example "scada-tagd": 1 to
    // LOG_MAX_COMPONENT_BYTES characters from A-Z, a-z, 0-9, '.', '_', '-'.
    // Copied by `Logger::create`.
    std::string_view component;
    // Records less severe than this are discarded by `log`.
    LogLevel min_level = LogLevel::INFO;
    // Records the queue holds, 1 to LOG_MAX_QUEUE_CAPACITY. Each takes about
    // 1.5 KiB, allocated once by `Logger::create`.
    std::size_t queue_capacity = LOG_DEFAULT_QUEUE_CAPACITY;
};

// The polling period of a Logger's worker thread, in milliseconds: a push
// into a `LogQueue` cannot wake the worker (a wake is a system call), so the
// worker looks at its queues at least this often. A record pushed into a
// queue reaches the sink within about this time plus the sink's own time.
inline constexpr std::int64_t LOG_QUEUE_POLL_MS = 20;

// One thread's queue of records, made by `Logger::make_queue` and drained by
// that Logger's worker thread. Exactly one thread may push into a queue at a
// time. The Logger and the producer share it: a producer that outlives the
// Logger (a stalled thread that is never joined) pushes into a queue that
// nobody drains, and its records are dropped and counted, never written to
// freed memory. A moved Logger keeps its queues.
class LogQueue {
public:
    LogQueue(const LogQueue&) = delete;
    LogQueue& operator=(const LogQueue&) = delete;
    LogQueue(LogQueue&&) = delete;
    LogQueue& operator=(LogQueue&&) = delete;
    ~LogQueue();

    // Copies one record into the queue and returns true, or, if the queue is
    // full, drops it, counts it (see `dropped`) and returns false.
    // `time_ns` is the record's time on CLOCK_REALTIME, read by the caller,
    // which may read it once for many records. The record is otherwise
    // treated as by `Logger::log`, but records below the Logger's
    // `min_level` are queued and discarded by the worker. Allocates nothing,
    // takes no lock and makes no system call; one producer thread only.
    SCADA_RT bool push(LogLevel level, std::int64_t time_ns,
                       std::string_view message,
                       std::span<const LogField> fields = {}) noexcept;

    // Same, for fields written in place.
    SCADA_RT bool push(LogLevel level, std::int64_t time_ns,
                       std::string_view message,
                       std::initializer_list<LogField> fields) noexcept;

    // True if a push now would drop its record. For the producer, which can
    // wait for room instead of dropping (the driver's stop, D-148): only the
    // Logger's thread takes records out, so a false answer holds until the
    // producer's next push. Takes no lock and makes no system call.
    SCADA_RT [[nodiscard]] bool full() const noexcept;

    // Records this queue dropped because it was full, since creation. Any
    // thread.
    [[nodiscard]] std::uint64_t dropped() const noexcept;

    // Records it holds, 1 to LOG_MAX_QUEUE_CAPACITY.
    [[nodiscard]] std::size_t capacity() const noexcept;

    class Impl;  // opaque: only a Logger can make one
    explicit LogQueue(std::unique_ptr<Impl> impl) noexcept;

private:
    friend class Logger;
    std::unique_ptr<Impl> impl_;
};

// Accepts records from any thread and delivers them to one sink from its own
// worker thread. Move-only; a moved-from Logger discards every record.
class Logger {
public:
    // Creates a logger and starts its worker thread. Fails with
    // INVALID_ARGUMENT for a null sink or invalid options, and
    // RESOURCE_EXHAUSTED or UNAVAILABLE if memory or the thread cannot be
    // obtained.
    [[nodiscard]] static Result<Logger> create(
        std::unique_ptr<LogSink> sink, const LoggerOptions& options) noexcept;

    // Stops accepting records, delivers those already queued, and joins the
    // worker thread. Takes as long as the sink needs for the queued records.
    ~Logger();

    Logger(Logger&&) noexcept;
    Logger& operator=(Logger&&) noexcept;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Queues one record and returns without waiting for the sink. The time a
    // call takes is bounded by copying one record under a mutex that is never
    // held during sink I/O; it does not allocate. Thread-safe.
    //
    // The record is silently discarded if `level` is less severe than
    // `min_level`, and dropped and counted (see `dropped`) if the queue is
    // full. Text beyond the size limits is cut. A field with an invalid key
    // (see `is_valid_log_key`) and fields beyond LOG_MAX_FIELDS are omitted.
    // `message` and `fields` are copied; nothing is kept after the call.
    void log(LogLevel level, std::string_view message,
             std::span<const LogField> fields = {}) noexcept;

    // Same, for fields written in place:
    //   logger.log(LogLevel::INFO, "connected", {{"PEER", name}});
    void log(LogLevel level, std::string_view message,
             std::initializer_list<LogField> fields) noexcept;

    // Waits until every record accepted so far has been handed to the sink,
    // or `timeout` has passed. Returns OK or TIMEOUT. Thread-safe.
    [[nodiscard]] Status flush(std::chrono::milliseconds timeout) noexcept;

    // Makes a queue of `capacity` records for one thread (see `LogQueue`)
    // and returns it, shared with the Logger, whose worker drains it from
    // then on. `name` (a thread's or a channel's) is the QUEUE field of the
    // notice of its dropped records; empty: none. Allocates about 1.5 KiB per
    // record, so call it at set-up, not in real-time code. Fails with
    // OUT_OF_RANGE for a capacity of 0 or above LOG_MAX_QUEUE_CAPACITY,
    // RESOURCE_EXHAUSTED without memory, and FAILED_PRECONDITION on a
    // moved-from Logger. Thread-safe.
    [[nodiscard]] Result<std::shared_ptr<LogQueue>> make_queue(
        std::size_t capacity, std::string_view name = {}) noexcept;

    // Number of records dropped because a queue was full, since creation:
    // the Logger's own queue and every `LogQueue` of it. Lock-free, so that
    // a thread that writes a heartbeat may read it (DI 11.3).
    [[nodiscard]] std::uint64_t dropped() const noexcept;

    // Records accepted, by `log` or a push, and not yet handed to the sink:
    // after a `flush` that timed out, those still on their way. Lock-free.
    [[nodiscard]] std::uint64_t pending() const noexcept;

    // Times a caller took the Logger's mutex (`log`, `flush`,
    // `make_queue`), since creation: a probe for the tests that show a path
    // takes none. `dropped`, `pending`, `sink_failures` and a push take none.
    [[nodiscard]] std::uint64_t caller_locks() const noexcept;

    // Rounds of the worker thread's loop, since creation: a probe for the
    // test that a Logger without a LogQueue does not poll.
    [[nodiscard]] std::uint64_t worker_rounds() const noexcept;

    // Number of records the sink refused, since creation. Lock-free.
    [[nodiscard]] std::uint64_t sink_failures() const noexcept;

private:
    class Impl;
    explicit Logger(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace scada

#endif  // SCADA_COMMON_LOG_H
