#include "scada/common/log.h"

#include "log_internal.h"
#include "scada/common/clock.h"
#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>

#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace scada {

namespace {

using log_internal::BoundedText;
using log_internal::UINT64_DIGITS;

// How often an interrupted system call is repeated before giving up, so that
// a storm of signals cannot hold the worker in a loop.
constexpr int MAX_INTERRUPT_RETRIES = 3;

// Longest wait for a stream to become writable.
constexpr int STREAM_WRITE_TIMEOUT_MS = 100;

// A line of the stream sink fits one atomic pipe write (PIPE_BUF on Linux).
constexpr std::size_t STREAM_LINE_BYTES = 4096;

// Largest journal datagram: every field in the length-prefixed form
// "KEY\n<8-byte length>value\n" with all parts at their limits.
constexpr std::size_t JOURNAL_FIELD_OVERHEAD = 1 + 8 + 1;
constexpr std::size_t JOURNAL_DATAGRAM_BYTES = 2048;
constexpr std::string_view MESSAGE_KEY = "MESSAGE";
constexpr std::string_view PRIORITY_KEY = "PRIORITY";
constexpr std::string_view IDENTIFIER_KEY = "SYSLOG_IDENTIFIER";

constexpr std::size_t journal_field_bytes(std::size_t key,
                                          std::size_t value) noexcept {
    return key + JOURNAL_FIELD_OVERHEAD + value;
}
static_assert(journal_field_bytes(MESSAGE_KEY.size(), LOG_MAX_MESSAGE_BYTES) +
                      journal_field_bytes(PRIORITY_KEY.size(), 1) +
                      journal_field_bytes(IDENTIFIER_KEY.size(),
                                          LOG_MAX_COMPONENT_BYTES) +
                      journal_field_bytes(LOG_TIME_FIELD.size(),
                                          UINT64_DIGITS) +
                      LOG_MAX_FIELDS *
                          journal_field_bytes(LOG_MAX_KEY_BYTES,
                                              LOG_MAX_VALUE_BYTES) <=
                  JOURNAL_DATAGRAM_BYTES,
              "a record at the size limits must fit one journal datagram");

constexpr std::uint8_t priority_of(LogLevel level) noexcept {
    // The journal accepts 0 to 7; a value that is not an enumerator is
    // reported as the least severe instead of being trusted.
    constexpr std::uint8_t LEAST_SEVERE = 7;
    return std::min(static_cast<std::uint8_t>(level), LEAST_SEVERE);
}

constexpr std::string_view name_of(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::CRITICAL:
            return "CRITICAL";
        case LogLevel::ERROR:
            return "ERROR";
        case LogLevel::WARNING:
            return "WARNING";
        case LogLevel::NOTICE:
            return "NOTICE";
        case LogLevel::INFO:
            return "INFO";
        case LogLevel::DEBUG:
            return "DEBUG";
    }
    return "UNKNOWN";
}

std::uint64_t non_negative(std::int64_t value) noexcept {
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}

// Creates a sink, turning an allocation failure into a Status.
template <typename Sink, typename... Args>
Result<std::unique_ptr<LogSink>> make_sink(Args&&... args) noexcept {
    try {
        return std::unique_ptr<LogSink>(
            std::make_unique<Sink>(std::forward<Args>(args)...));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::RESOURCE_EXHAUSTED,
                      "out of memory creating a log sink");
    }
}

// ---------------------------------------------------------------- journal

void append_journal_field(BoundedText& out, std::string_view key,
                          std::string_view value) noexcept {
    out.append(key);
    if (value.find('\n') == std::string_view::npos) {
        out.append('=');
        out.append(value);
    } else {
        // A value with a line break is sent with an explicit length.
        out.append('\n');
        out.append_le64(value.size());
        out.append(value);
    }
    out.append('\n');
}

class JournalSink final : public LogSink {
public:
    explicit JournalSink(Fd socket) noexcept : socket_(std::move(socket)) {}

    [[nodiscard]] Status write(const LogRecord& record) noexcept override {
        std::array<char, JOURNAL_DATAGRAM_BYTES> storage{};
        BoundedText datagram(storage);

        std::array<char, UINT64_DIGITS> priority_storage{};
        BoundedText priority(priority_storage);
        priority.append_number(priority_of(record.level));
        std::array<char, UINT64_DIGITS> time_storage{};
        BoundedText time(time_storage);
        time.append_number(non_negative(record.time_ns));

        append_journal_field(datagram, MESSAGE_KEY, record.message);
        append_journal_field(datagram, PRIORITY_KEY, priority.view());
        append_journal_field(datagram, IDENTIFIER_KEY, record.component);
        append_journal_field(datagram, LOG_TIME_FIELD, time.view());
        for (const LogField& field : record.fields) {
            append_journal_field(datagram, field.key, field.value);
        }
        if (datagram.truncated()) {
            // Only possible for a record beyond the limits the Logger
            // enforces. A cut datagram would be malformed, so send nothing.
            return {StatusCode::OUT_OF_RANGE,
                    "log record too large for a journal datagram"};
        }
        return send(datagram.view());
    }

private:
    [[nodiscard]] Status send(std::string_view datagram) const noexcept {
        for (int attempt = 0; attempt <= MAX_INTERRUPT_RETRIES; ++attempt) {
            const ssize_t sent =
                ::send(socket_.get(), datagram.data(), datagram.size(),
                       MSG_NOSIGNAL | MSG_DONTWAIT);
            if (sent >= 0) {
                return {};
            }
            if (errno != EINTR) {
                break;
            }
        }
        if (errno == EAGAIN || errno == ENOBUFS || errno == ECONNREFUSED ||
            errno == ENOENT || errno == EINTR) {
            return {StatusCode::UNAVAILABLE,
                    "the journal does not accept records now"};
        }
        return {StatusCode::IO_ERROR, "sending to the journal failed"};
    }

    Fd socket_;
};

// ----------------------------------------------------------------- stream

// Appends `text` so that it cannot end the line or be mistaken for the
// structure around it: control characters become \xNN, a backslash is
// doubled, and inside a quoted value the quote is escaped too.
void append_escaped(BoundedText& out, std::string_view text,
                    bool quoted) noexcept {
    constexpr std::string_view HEX = "0123456789abcdef";
    constexpr unsigned FIRST_PRINTABLE = 0x20;
    constexpr unsigned DELETE = 0x7F;
    constexpr unsigned LOW_NIBBLE = 0x0F;
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < FIRST_PRINTABLE || byte == DELETE) {
            out.append("\\x");
            out.append(HEX.at(byte >> 4U));
            out.append(HEX.at(byte & LOW_NIBBLE));
        } else if (character == '\\' || (quoted && character == '"')) {
            out.append('\\');
            out.append(character);
        } else {
            out.append(character);
        }
    }
}

// Appends `time_ns` as UTC, for example 2026-11-13T12:00:00.123Z.
void append_utc_time(BoundedText& out, std::int64_t time_ns) noexcept {
    const std::uint64_t total_ns = non_negative(time_ns);
    const auto seconds = static_cast<std::time_t>(
        total_ns / static_cast<std::uint64_t>(NANOSECONDS_PER_SECOND));
    const std::uint64_t millis =
        total_ns % static_cast<std::uint64_t>(NANOSECONDS_PER_SECOND) /
        1'000'000U;
    constexpr std::uint64_t TM_YEAR_BASE = 1900;  // tm_year counts from 1900
    std::tm parts{};
    // On failure `parts` stays zero and a recognisably wrong date is printed.
    static_cast<void>(::gmtime_r(&seconds, &parts));
    const auto field = [](int value) {
        return static_cast<std::uint64_t>(std::max(value, 0));
    };
    out.append_number(field(parts.tm_year) + TM_YEAR_BASE, 4);
    out.append('-');
    out.append_number(field(parts.tm_mon) + 1, 2);
    out.append('-');
    out.append_number(field(parts.tm_mday), 2);
    out.append('T');
    out.append_number(field(parts.tm_hour), 2);
    out.append(':');
    out.append_number(field(parts.tm_min), 2);
    out.append(':');
    out.append_number(field(parts.tm_sec), 2);
    out.append('.');
    out.append_number(millis, 3);
    out.append('Z');
}

class StreamSink final : public LogSink {
public:
    explicit StreamSink(int descriptor) noexcept : fd_(descriptor) {}

    [[nodiscard]] Status write(const LogRecord& record) noexcept override {
        std::array<char, STREAM_LINE_BYTES> storage{};
        // The last byte is kept for the line break, so that a cut line still
        // ends properly.
        BoundedText line(std::span<char>(storage).first(storage.size() - 1));
        append_utc_time(line, record.time_ns);
        line.append(' ');
        line.append(name_of(record.level));
        line.append(' ');
        line.append(record.component);
        line.append(": ");
        append_escaped(line, record.message, false);
        for (const LogField& field : record.fields) {
            line.append(' ');
            line.append(field.key);
            line.append("=\"");
            append_escaped(line, field.value, true);
            line.append('"');
        }
        const std::size_t size = line.view().size();
        storage.at(size) = '\n';
        return write_line(std::string_view(storage.data(), size + 1));
    }

private:
    [[nodiscard]] Status write_line(std::string_view line) const noexcept {
        for (int attempt = 0; attempt <= MAX_INTERRUPT_RETRIES; ++attempt) {
            pollfd waiting{fd_, POLLOUT, 0};
            const int ready = ::poll(&waiting, 1, STREAM_WRITE_TIMEOUT_MS);
            if (ready == 0) {
                return {StatusCode::TIMEOUT, "log stream is not writable"};
            }
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return {StatusCode::IO_ERROR, "polling the log stream failed"};
            }
            const ssize_t written = ::write(fd_, line.data(), line.size());
            if (written >= 0) {
                // A line is not completed after a short write: the rest
                // could block, and half a line is of no more use than none.
                return static_cast<std::size_t>(written) == line.size()
                           ? Status()
                           : Status(StatusCode::IO_ERROR,
                                    "short write to the log stream");
            }
            if (errno == EAGAIN) {
                return {StatusCode::UNAVAILABLE, "log stream is full"};
            }
            if (errno != EINTR) {
                return {StatusCode::IO_ERROR, "writing the log stream failed"};
            }
        }
        return {StatusCode::UNAVAILABLE,
                "writing the log stream was interrupted"};
    }

    int fd_;  // borrowed; see make_stream_sink
};

// --------------------------------------------------------------- fallback

class FallbackSink final : public LogSink {
public:
    FallbackSink(std::unique_ptr<LogSink> primary,
                 std::unique_ptr<LogSink> secondary) noexcept
        : primary_(std::move(primary)), secondary_(std::move(secondary)) {}

    [[nodiscard]] Status write(const LogRecord& record) noexcept override {
        if (primary_->write(record).ok()) {
            return {};
        }
        return secondary_->write(record);
    }

private:
    std::unique_ptr<LogSink> primary_;
    std::unique_ptr<LogSink> secondary_;
};

}  // namespace

Result<std::unique_ptr<LogSink>> make_journal_sink(
    std::string_view socket_path) noexcept {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::span<char> path(address.sun_path);
    // The path must leave room for the terminator. A NUL inside it would
    // silently select another (or an abstract) socket.
    if (socket_path.empty() || socket_path.size() >= path.size() ||
        socket_path.find('\0') != std::string_view::npos) {
        return Status(StatusCode::INVALID_ARGUMENT,
                      "journal socket path is empty or too long");
    }
    std::copy(socket_path.begin(), socket_path.end(), path.begin());

    Fd socket(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!socket.valid()) {
        return Status(StatusCode::RESOURCE_EXHAUSTED,
                      "cannot create the journal socket");
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,scada-banned): the socket interface takes every address type through a sockaddr pointer
    const auto* generic = reinterpret_cast<const sockaddr*>(&address);
    if (::connect(socket.get(), generic, sizeof(address)) != 0) {
        return Status(StatusCode::UNAVAILABLE,
                      "no journal listens on the socket");
    }
    return make_sink<JournalSink>(std::move(socket));
}

Result<std::unique_ptr<LogSink>> make_stream_sink(int descriptor) noexcept {
    if (descriptor < 0) {
        return Status(StatusCode::INVALID_ARGUMENT,
                      "log stream descriptor is negative");
    }
    return make_sink<StreamSink>(descriptor);
}

Result<std::unique_ptr<LogSink>> make_fallback_sink(
    std::unique_ptr<LogSink> primary,
    std::unique_ptr<LogSink> secondary) noexcept {
    if (!primary || !secondary) {
        return Status(StatusCode::INVALID_ARGUMENT, "log sink is null");
    }
    return make_sink<FallbackSink>(std::move(primary), std::move(secondary));
}

Result<std::unique_ptr<LogSink>> make_default_sink() noexcept {
    // The service manager sets JOURNAL_STREAM when it connects the standard
    // streams to the journal. Its value is checked against the descriptor,
    // because a child process inherits the variable even after redirecting
    // the stream elsewhere. Read once at start-up, before threads change the
    // environment.
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once at start-up, see above
    const char* const journal_stream = std::getenv("JOURNAL_STREAM");
    if (journal_stream != nullptr &&
        log_internal::is_journal_stream(journal_stream, STDERR_FILENO)) {
        auto journal = make_journal_sink();
        auto fallback = make_stream_sink(STDERR_FILENO);
        if (journal.ok() && fallback.ok()) {
            return make_fallback_sink(std::move(journal).value(),
                                      std::move(fallback).value());
        }
    }
    return make_stream_sink(STDERR_FILENO);
}

}  // namespace scada
