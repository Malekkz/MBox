// Tests of the log sinks against real descriptors: a pipe for the stream
// sink and a datagram socket standing in for the journal.

#include "check.h"
#include "log_internal.h"
#include "scada/common/clock.h"
#include "scada/common/fd.h"
#include "scada/common/log.h"
#include "scada/common/result.h"
#include "scada/common/status.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using scada::Fd;
using scada::LogField;
using scada::LogLevel;
using scada::LogRecord;
using scada::LogSink;
using scada::Result;
using scada::Status;
using scada::StatusCode;
using scada::test::Checker;

// Longest time a test waits for data that must arrive.
constexpr int WAIT_MS = 30'000;

// 2023-11-14T22:13:20.123456789Z
constexpr std::int64_t FIXED_TIME_NS = 1'700'000'000'123'456'789;

struct Pipe {
    Fd reader;
    Fd writer;
};

Pipe make_pipe() {
    std::array<int, 2> ends{-1, -1};
    if (::pipe(ends.data()) != 0) {
        return {};
    }
    return {Fd(ends[0]), Fd(ends[1])};
}

// Reads what is available on `descriptor`, waiting until something is. Returns
// an empty string if nothing arrives in time.
std::string read_available(int descriptor) {
    constexpr std::size_t BUFFER_BYTES = 8192;
    pollfd waiting{descriptor, POLLIN, 0};
    if (::poll(&waiting, 1, WAIT_MS) <= 0) {
        return {};
    }
    std::array<char, BUFFER_BYTES> buffer{};
    const ssize_t count = ::read(descriptor, buffer.data(), buffer.size());
    return count > 0
               ? std::string(buffer.data(), static_cast<std::size_t>(count))
               : std::string();
}

// A datagram socket bound in a private temporary directory, playing the
// journal. Removes the socket and the directory when destroyed.
class FakeJournal {
public:
    FakeJournal() {
        // A name no other process or earlier journal of this test uses.
        // create_directory fails if the directory exists, so a name that
        // was taken after all leaves the journal not ready.
        static int instances = 0;
        ++instances;
        std::string name = "scada-log-";
        name.append(std::to_string(::getpid()));
        name.append("-").append(std::to_string(instances));
        const std::filesystem::path directory =
            std::filesystem::temp_directory_path() / name;
        std::error_code failed;
        if (!std::filesystem::create_directory(directory, failed)) {
            return;
        }
        directory_ = directory.string();
        path_ = directory_ + "/socket";

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path_.size() >= sizeof(address.sun_path)) {
            return;
        }
        std::copy(path_.begin(), path_.end(), std::begin(address.sun_path));
        Fd socket(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,scada-banned): the socket interface takes every address type through a sockaddr pointer
        const auto* generic = reinterpret_cast<const sockaddr*>(&address);
        if (socket.valid() &&
            ::bind(socket.get(), generic, sizeof(address)) == 0) {
            socket_ = std::move(socket);
        }
    }

    ~FakeJournal() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    FakeJournal(const FakeJournal&) = delete;
    FakeJournal& operator=(const FakeJournal&) = delete;
    FakeJournal(FakeJournal&&) = delete;
    FakeJournal& operator=(FakeJournal&&) = delete;

    [[nodiscard]] bool ready() const { return socket_.valid(); }
    [[nodiscard]] const std::string& path() const { return path_; }
    [[nodiscard]] const std::string& directory() const { return directory_; }
    [[nodiscard]] std::string receive() const {
        return read_available(socket_.get());
    }

private:
    std::string directory_;
    std::string path_;
    Fd socket_;
};

// A sink that always fails or always succeeds, and counts its calls. The
// counter is owned by the test.
class CountingSink final : public LogSink {
public:
    CountingSink(int& calls, bool fails) : calls_(calls), fails_(fails) {}

    [[nodiscard]] Status write(const LogRecord& /*record*/) noexcept override {
        ++calls_;
        return fails_ ? Status(StatusCode::IO_ERROR, "sink failed") : Status();
    }

private:
    int& calls_;
    bool fails_;
};

void test_stream_format(Checker& checker) {
    const Pipe pipe = make_pipe();
    Result<std::unique_ptr<LogSink>> sink =
        scada::make_stream_sink(pipe.writer.get());
    checker.expect(sink.ok(), "stream: sink created");
    if (!sink.ok()) {
        return;
    }
    const std::array<LogField, 2> fields{LogField{"PEER", "ps2"},
                                         LogField{"ATTEMPT", "3"}};
    checker.expect(sink.value()
                       ->write({LogLevel::WARNING, FIXED_TIME_NS, "scada-test",
                                "link down", fields})
                       .ok(),
                   "stream: write succeeds");
    checker.expect(read_available(pipe.reader.get()) ==
                       "2023-11-14T22:13:20.123Z WARNING scada-test: link down "
                       "PEER=\"ps2\" ATTEMPT=\"3\"\n",
                   "stream: line format");

    checker.expect(sink.value()
                       ->write({LogLevel::INFO, 0, "scada-test", "plain", {}})
                       .ok(),
                   "stream: write without fields succeeds");
    checker.expect(read_available(pipe.reader.get()) ==
                       "1970-01-01T00:00:00.000Z INFO scada-test: plain\n",
                   "stream: record without fields, time zero");
}

void test_stream_escapes(Checker& checker) {
    const Pipe pipe = make_pipe();
    Result<std::unique_ptr<LogSink>> sink =
        scada::make_stream_sink(pipe.writer.get());
    if (!sink.ok()) {
        checker.expect(false, "escape: sink created");
        return;
    }
    // An attacker-controlled name tries to forge a second log line and to
    // close the quoted value early.
    const std::array<LogField, 1> fields{
        LogField{"NAME", "x\" FORGED=\"1\n\x7f\\"}};
    const std::string_view message("bad\nERROR forged\tline \x01 caf\xC3\xA9");
    checker.expect(sink.value()
                       ->write({LogLevel::ERROR, FIXED_TIME_NS, "scada-test",
                                message, fields})
                       .ok(),
                   "escape: write succeeds");
    checker.expect(
        read_available(pipe.reader.get()) ==
            "2023-11-14T22:13:20.123Z ERROR scada-test: "
            "bad\\x0aERROR forged\\x09line \\x01 caf\xC3\xA9 "
            "NAME=\"x\\\" FORGED=\\\"1\\x0a\\x7f\\\\\"\n",
        "escape: control characters, quotes and backslashes are escaped");
}

void test_stream_line_limit(Checker& checker) {
    const Pipe pipe = make_pipe();
    Result<std::unique_ptr<LogSink>> sink =
        scada::make_stream_sink(pipe.writer.get());
    if (!sink.ok()) {
        checker.expect(false, "line limit: sink created");
        return;
    }
    // Larger than the Logger would ever pass; the sink must still bound it.
    constexpr std::size_t HUGE_BYTES = 20'000;
    constexpr std::size_t LINE_LIMIT = 4096;
    const std::string huge(HUGE_BYTES, '\n');
    checker.expect(
        sink.value()->write({LogLevel::INFO, 0, "scada-test", huge, {}}).ok(),
        "line limit: write succeeds");
    const std::string line = read_available(pipe.reader.get());
    checker.expect(line.size() == LINE_LIMIT, "line limit: line is 4096 bytes");
    checker.expect(!line.empty() && line.back() == '\n' &&
                       line.find('\n') == line.size() - 1,
                   "line limit: the cut line still ends with one line break");
}

void test_stream_stalled(Checker& checker) {
    const Pipe pipe = make_pipe();
    Result<std::unique_ptr<LogSink>> sink =
        scada::make_stream_sink(pipe.writer.get());
    if (!sink.ok()) {
        checker.expect(false, "stream stalled: sink created");
        return;
    }
    // Fill the pipe; nobody reads it. A pipe reports itself writable while
    // one page is free, so a write of one page after that never blocks.
    constexpr std::size_t PAGE_BYTES = 4096;
    constexpr int MAX_PAGES = 100'000;
    const std::array<char, PAGE_BYTES> page{};
    pollfd writable{pipe.writer.get(), POLLOUT, 0};
    int pages = 0;
    while (pages < MAX_PAGES && ::poll(&writable, 1, 0) == 1 &&
           ::write(pipe.writer.get(), page.data(), page.size()) > 0) {
        ++pages;
    }
    checker.expect(pages > 0 && pages < MAX_PAGES,
                   "stream stalled: pipe filled");

    constexpr std::int64_t BOUND_SECONDS = 10;
    const std::int64_t start = scada::monotonic_ns();
    const Status status =
        sink.value()->write({LogLevel::INFO, 0, "scada-test", "stuck", {}});
    const std::int64_t elapsed = scada::monotonic_ns() - start;
    checker.expect(status.code() == StatusCode::TIMEOUT,
                   "stream stalled: write gives up with TIMEOUT");
    // The sink waits 100 ms; the margin covers a slow or emulated machine.
    checker.expect(elapsed < BOUND_SECONDS * scada::NANOSECONDS_PER_SECOND,
                   "stream stalled: write returned within the bound");
}

void test_stream_rejects_bad_descriptor(Checker& checker) {
    checker.expect(scada::make_stream_sink(-1).status().code() ==
                       StatusCode::INVALID_ARGUMENT,
                   "stream: negative descriptor rejected");
}

void test_journal_datagram(Checker& checker) {
    const FakeJournal journal;
    checker.expect(journal.ready(), "journal: fake journal ready");
    Result<std::unique_ptr<LogSink>> sink =
        scada::make_journal_sink(journal.path());
    checker.expect(sink.ok(), "journal: sink created");
    if (!journal.ready() || !sink.ok()) {
        return;
    }

    const std::array<LogField, 2> fields{LogField{"PEER", "ps2"},
                                         LogField{"ATTEMPT", "3"}};
    checker.expect(sink.value()
                       ->write({LogLevel::WARNING, FIXED_TIME_NS, "scada-test",
                                "link down", fields})
                       .ok(),
                   "journal: write succeeds");
    checker.expect(journal.receive() ==
                       "MESSAGE=link down\n"
                       "PRIORITY=4\n"
                       "SYSLOG_IDENTIFIER=scada-test\n"
                       "SCADA_TIME_NS=1700000000123456789\n"
                       "PEER=ps2\n"
                       "ATTEMPT=3\n",
                   "journal: datagram in the native protocol");

    // A value with a line break uses the length-prefixed form, so it cannot
    // inject a field of its own.
    const std::array<LogField, 1> multiline{
        LogField{"DETAIL", "a\nPRIORITY=0"}};
    checker.expect(sink.value()
                       ->write({LogLevel::ERROR, FIXED_TIME_NS, "scada-test",
                                "two\nlines", multiline})
                       .ok(),
                   "journal: multi-line write succeeds");
    // The length is 8 bytes, least significant first: 9 and 12 here.
    constexpr std::size_t LENGTH_PADDING = 7;
    std::string expected = "MESSAGE\n\x09";
    expected.append(LENGTH_PADDING, '\0');
    expected.append(
        "two\nlines\n"
        "PRIORITY=3\n"
        "SYSLOG_IDENTIFIER=scada-test\n"
        "SCADA_TIME_NS=1700000000123456789\n"
        "DETAIL\n\x0c");
    expected.append(LENGTH_PADDING, '\0');
    expected.append("a\nPRIORITY=0\n");
    checker.expect(journal.receive() == expected,
                   "journal: values with line breaks are length-prefixed");
}

void test_journal_oversized_record(Checker& checker) {
    const FakeJournal journal;
    Result<std::unique_ptr<LogSink>> sink =
        scada::make_journal_sink(journal.path());
    if (!journal.ready() || !sink.ok()) {
        checker.expect(false, "journal oversized: set up");
        return;
    }
    const std::string huge(5000, 'x');
    checker.expect(
        sink.value()
                ->write({LogLevel::INFO, 0, "scada-test", huge, {}})
                .code() == StatusCode::OUT_OF_RANGE,
        "journal: a record beyond the datagram size is refused, not cut");
}

void test_journal_stalled(Checker& checker) {
    const FakeJournal journal;
    Result<std::unique_ptr<LogSink>> sink =
        scada::make_journal_sink(journal.path());
    if (!journal.ready() || !sink.ok()) {
        checker.expect(false, "journal stalled: set up");
        return;
    }
    // The fake journal never reads. Its receive queue fills, and from then
    // on the sink must refuse instead of waiting.
    constexpr int ATTEMPTS = 100'000;
    constexpr std::int64_t BOUND_SECONDS = 30;
    Status status;
    int accepted = 0;
    const std::int64_t start = scada::monotonic_ns();
    while (status.ok() && accepted < ATTEMPTS) {
        status = sink.value()->write(
            {LogLevel::INFO, 0, "scada-test", "nobody reads this", {}});
        ++accepted;
    }
    const std::int64_t elapsed = scada::monotonic_ns() - start;
    checker.expect(status.code() == StatusCode::UNAVAILABLE,
                   "journal stalled: write is refused with UNAVAILABLE");
    checker.expect(elapsed < BOUND_SECONDS * scada::NANOSECONDS_PER_SECOND,
                   "journal stalled: no write waited for the journal");
}

void test_journal_unreachable(Checker& checker) {
    const FakeJournal journal;
    checker.expect(scada::make_journal_sink(journal.directory() + "/missing")
                           .status()
                           .code() == StatusCode::UNAVAILABLE,
                   "journal: no socket at the path gives UNAVAILABLE");
    checker.expect(scada::make_journal_sink("").status().code() ==
                       StatusCode::INVALID_ARGUMENT,
                   "journal: empty path rejected");
    checker.expect(
        scada::make_journal_sink(std::string(sizeof(sockaddr_un), 'a'))
                .status()
                .code() == StatusCode::INVALID_ARGUMENT,
        "journal: path longer than a socket address rejected");
    std::string abstract(1, '\0');
    abstract.append("abstract");
    checker.expect(scada::make_journal_sink(abstract).status().code() ==
                       StatusCode::INVALID_ARGUMENT,
                   "journal: path with a NUL rejected");
}

void test_fallback(Checker& checker) {
    const LogRecord record{LogLevel::INFO, 0, "scada-test", "message", {}};
    {
        int primary_calls = 0;
        int secondary_calls = 0;
        Result<std::unique_ptr<LogSink>> sink = scada::make_fallback_sink(
            std::make_unique<CountingSink>(primary_calls, false),
            std::make_unique<CountingSink>(secondary_calls, false));
        checker.expect(sink.ok() && sink.value()->write(record).ok(),
                       "fallback: write succeeds through the primary");
        checker.expect(primary_calls == 1 && secondary_calls == 0,
                       "fallback: secondary unused while the primary works");
    }
    {
        int primary_calls = 0;
        int secondary_calls = 0;
        Result<std::unique_ptr<LogSink>> sink = scada::make_fallback_sink(
            std::make_unique<CountingSink>(primary_calls, true),
            std::make_unique<CountingSink>(secondary_calls, false));
        checker.expect(sink.ok() && sink.value()->write(record).ok(),
                       "fallback: write succeeds through the secondary");
        checker.expect(primary_calls == 1 && secondary_calls == 1,
                       "fallback: secondary used when the primary fails");
    }
    {
        int calls = 0;
        Result<std::unique_ptr<LogSink>> sink = scada::make_fallback_sink(
            std::make_unique<CountingSink>(calls, true),
            std::make_unique<CountingSink>(calls, true));
        checker.expect(sink.ok() && sink.value()->write(record).code() ==
                                        StatusCode::IO_ERROR,
                       "fallback: error when both fail");
    }
    {
        int calls = 0;
        checker.expect(
            scada::make_fallback_sink(
                nullptr, std::make_unique<CountingSink>(calls, false))
                        .status()
                        .code() == StatusCode::INVALID_ARGUMENT &&
                scada::make_fallback_sink(
                    std::make_unique<CountingSink>(calls, false), nullptr)
                        .status()
                        .code() == StatusCode::INVALID_ARGUMENT,
            "fallback: null sinks rejected");
    }
}

// A stalled journal with the stream as fallback: the record still arrives,
// on the stream.
void test_journal_falls_back_to_stream(Checker& checker) {
    const FakeJournal journal;
    const Pipe pipe = make_pipe();
    Result<std::unique_ptr<LogSink>> journal_sink =
        scada::make_journal_sink(journal.path());
    Result<std::unique_ptr<LogSink>> stream_sink =
        scada::make_stream_sink(pipe.writer.get());
    if (!journal.ready() || !journal_sink.ok() || !stream_sink.ok()) {
        checker.expect(false, "journal fallback: set up");
        return;
    }
    // Fill the journal's queue through the sink itself.
    constexpr int ATTEMPTS = 100'000;
    Status status;
    for (int i = 0; status.ok() && i < ATTEMPTS; ++i) {
        status = journal_sink.value()->write(
            {LogLevel::INFO, 0, "scada-test", "fill", {}});
    }
    checker.expect(!status.ok(), "journal fallback: journal is stalled");

    Result<std::unique_ptr<LogSink>> sink = scada::make_fallback_sink(
        std::move(journal_sink).value(), std::move(stream_sink).value());
    checker.expect(
        sink.ok() &&
            sink.value()
                ->write({LogLevel::ERROR, 0, "scada-test", "not lost", {}})
                .ok(),
        "journal fallback: write succeeds");
    checker.expect(read_available(pipe.reader.get()) ==
                       "1970-01-01T00:00:00.000Z ERROR scada-test: not lost\n",
                   "journal fallback: the record went to the stream");
}

void test_default_sink(Checker& checker) {
    // Whatever the environment, a usable sink must come back.
    checker.expect(scada::make_default_sink().ok(),
                   "default sink: always available");
}

void test_journal_stream_detection(Checker& checker) {
    using scada::log_internal::is_journal_stream;
    const Pipe pipe = make_pipe();
    struct stat info {};
    checker.expect(::fstat(pipe.writer.get(), &info) == 0,
                   "journal stream: fstat");
    const std::string device = std::to_string(info.st_dev);
    const std::string inode = std::to_string(info.st_ino);
    const std::string other_device = std::to_string(info.st_dev + 1);
    const std::string other_inode = std::to_string(info.st_ino + 1);
    const int descriptor = pipe.writer.get();
    // Joins two parts with a separator between them.
    const auto join = [](std::string_view left, std::string_view separator,
                         std::string_view right) {
        std::string text(left);
        text.append(separator).append(right);
        return text;
    };

    checker.expect(is_journal_stream(join(device, ":", inode), descriptor),
                   "journal stream: matching device and inode");
    checker.expect(
        !is_journal_stream(join(device, ":", other_inode), descriptor),
        "journal stream: other inode");
    checker.expect(
        !is_journal_stream(join(other_device, ":", inode), descriptor),
        "journal stream: other device");
    checker.expect(!is_journal_stream(join(device, ":", inode), -1),
                   "journal stream: invalid descriptor");
    const std::string valid = join(device, ":", inode);
    for (const std::string& malformed : std::vector<std::string>{
             "", ":", device, join(device, ":", ""), join("", ":", inode),
             join(valid, ":", ""), join(valid, " ", ""), join(" ", "", valid),
             join(device, ";", inode), join("-", "", valid),
             join(device, ":0x", inode),
             join("99999999999999999999999", ":", inode)}) {
        checker.expect(!is_journal_stream(malformed, descriptor),
                       "journal stream: malformed value rejected");
    }
}

void test_parse_decimal(Checker& checker) {
    using scada::log_internal::parse_decimal;
    constexpr std::uint64_t FORTY_TWO = 42;
    checker.expect(parse_decimal("0") == 0U, "decimal: zero");
    checker.expect(parse_decimal("0042") == FORTY_TWO,
                   "decimal: leading zeros");
    checker.expect(parse_decimal("18446744073709551615") == UINT64_MAX,
                   "decimal: largest 64-bit value");
    checker.expect(!parse_decimal("18446744073709551616"),
                   "decimal: one above the largest value");
    checker.expect(!parse_decimal("99999999999999999999"),
                   "decimal: 20 digits that overflow");
    checker.expect(!parse_decimal(""), "decimal: empty");
    checker.expect(!parse_decimal("-1"), "decimal: sign");
    checker.expect(!parse_decimal("+1"), "decimal: plus sign");
    checker.expect(!parse_decimal("12a"), "decimal: trailing letter");
    checker.expect(!parse_decimal(" 12"), "decimal: leading space");
}

void test_truncate_utf8(Checker& checker) {
    using scada::log_internal::truncate_utf8;
    constexpr std::size_t LIMIT = 5;
    checker.expect(truncate_utf8("abc", 3) == "abc", "truncate: fits");
    checker.expect(truncate_utf8("abcd", 3) == "abc", "truncate: plain cut");
    checker.expect(truncate_utf8("abc", 0).empty(), "truncate: to nothing");
    // U+00E9 (2 bytes), U+20AC (3 bytes), U+1F600 (4 bytes).
    checker.expect(truncate_utf8("a\xC3\xA9", 2) == "a",
                   "truncate: inside a 2-byte character");
    checker.expect(truncate_utf8("a\xE2\x82\xAC", 3) == "a",
                   "truncate: inside a 3-byte character");
    checker.expect(truncate_utf8("a\xF0\x9F\x98\x80", 4) == "a",
                   "truncate: inside a 4-byte character");
    checker.expect(truncate_utf8("a\xC3\xA9z", 3) == "a\xC3\xA9",
                   "truncate: after a whole character");
    // Not UTF-8: only continuation bytes. The cut moves back 3 bytes at
    // most and never below zero.
    checker.expect(
        truncate_utf8("\x80\x80\x80\x80\x80\x80", LIMIT).size() == LIMIT - 3,
        "truncate: invalid text is cut within 3 bytes");
    checker.expect(truncate_utf8("\x80\x80\x80", 1).empty(),
                   "truncate: invalid text, cut reaches the start");
}

void test_key_validation(Checker& checker) {
    using scada::is_valid_log_key;
    checker.expect(is_valid_log_key("A"), "key: one letter");
    checker.expect(is_valid_log_key("TAG_NAME_2"),
                   "key: letters, digit, underscore");
    checker.expect(is_valid_log_key(std::string(scada::LOG_MAX_KEY_BYTES, 'K')),
                   "key: longest allowed");
    checker.expect(
        !is_valid_log_key(std::string(scada::LOG_MAX_KEY_BYTES + 1, 'K')),
        "key: one byte too long");
    checker.expect(!is_valid_log_key(""), "key: empty");
    checker.expect(!is_valid_log_key("_PID"), "key: leading underscore");
    checker.expect(!is_valid_log_key("2ND"), "key: leading digit");
    checker.expect(!is_valid_log_key("lower"), "key: lower case");
    checker.expect(!is_valid_log_key("A=B"), "key: equals sign");
    checker.expect(!is_valid_log_key("A\nB"), "key: line break");
    checker.expect(!is_valid_log_key("MESSAGE"), "key: MESSAGE is reserved");
    checker.expect(!is_valid_log_key("PRIORITY"), "key: PRIORITY is reserved");
    checker.expect(!is_valid_log_key("SYSLOG_IDENTIFIER"),
                   "key: SYSLOG_IDENTIFIER is reserved");
    checker.expect(!is_valid_log_key("SCADA_TIME_NS"),
                   "key: SCADA_TIME_NS is reserved");
}

void test_bounded_text(Checker& checker) {
    using scada::log_internal::BoundedText;
    constexpr std::size_t SMALL = 8;
    constexpr std::size_t WIDE = 32;
    constexpr std::uint64_t SEVEN = 7;
    constexpr std::uint64_t LONGER = 12345;
    constexpr std::size_t TOO_WIDE = 99;
    constexpr std::uint64_t PATTERN = 0x0102030405060708U;

    std::array<char, SMALL> storage{};
    BoundedText text(storage);
    text.append("abc");
    text.append('d');
    checker.expect(text.view() == "abcd" && !text.truncated(),
                   "bounded: appends");
    text.append("efghij");
    checker.expect(text.view() == "abcdefgh" && text.truncated(),
                   "bounded: cut at the end of the storage");
    text.append('k');
    text.append_number(SEVEN);
    checker.expect(text.view() == "abcdefgh", "bounded: full stays unchanged");

    std::array<char, WIDE> wide{};
    BoundedText numbers(wide);
    numbers.append_number(0);
    numbers.append(' ');
    numbers.append_number(SEVEN, 3);
    numbers.append(' ');
    numbers.append_number(LONGER, 2);
    checker.expect(numbers.view() == "0 007 12345", "bounded: numbers");

    BoundedText largest(wide);
    largest.append_number(UINT64_MAX, TOO_WIDE);
    checker.expect(largest.view() == "18446744073709551615",
                   "bounded: largest number, width capped at 20");

    BoundedText bytes(wide);
    bytes.append_le64(PATTERN);
    checker.expect(
        bytes.view() == std::string_view("\x08\x07\x06\x05\x04\x03\x02\x01",
                                         sizeof(PATTERN)),
        "bounded: 64-bit value, least significant byte first");
}

}  // namespace

int main() {
    Checker checker;
    test_stream_format(checker);
    test_stream_escapes(checker);
    test_stream_line_limit(checker);
    test_stream_stalled(checker);
    test_stream_rejects_bad_descriptor(checker);
    test_journal_datagram(checker);
    test_journal_oversized_record(checker);
    test_journal_stalled(checker);
    test_journal_unreachable(checker);
    test_fallback(checker);
    test_journal_falls_back_to_stream(checker);
    test_default_sink(checker);
    test_journal_stream_detection(checker);
    test_parse_decimal(checker);
    test_truncate_utf8(checker);
    test_key_validation(checker);
    test_bounded_text(checker);
    return checker.exit_code();
}
