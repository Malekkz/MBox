#include "scada/tagd/tagd.h"
#include "scada/tagd/suspend.h"

#include "scada/common/clock.h"
#include "scada/common/fd.h"
#include "scada/common/log.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/service/notify.h"
#include "scada/tag/generation_host.h"
#include "scada/tag/liveness.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/tag_set_json.h"

#include <fcntl.h>
#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal functions are declared here, not in <csignal>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace scada::tagd {

namespace {

using scada::service::Notifier;
using scada::service::NotifySettings;
using scada::tag::CheckReport;
using scada::tag::DownReason;
using scada::tag::GenerationHost;
using scada::tag::LateHeartbeats;
using scada::tag::ProcessChange;
using scada::tag::QueueChange;
using scada::tag::QueueEvent;
using scada::tag::SweptEntry;
using scada::tag::TagSet;

constexpr std::string_view USAGE =
    "Usage: scada-tagd [--tag-set <file>] [--base-dir <directory>]\n"
    "       scada-tagd --help\n"
    "\n"
    "Creates the shared-memory tag database from the tag-set file and watches\n"
    "the liveness of every process in it. Runs until stopped.\n"
    "\n"
    "At start it reuses the current generation if the tag set is unchanged,\n"
    "so that a restart does not disturb the other services. Otherwise it\n"
    "creates the next generation and switches to it. It then checks every\n"
    "heartbeat_check_ms (from the tag-set file, 250 ms by default) which\n"
    "processes are down and which reliable queues are broken (cursor and\n"
    "write_seq out of range) or missing, and logs every change. It warns\n"
    "when the system can suspend or was suspended, which a unit must not.\n"
    "\n"
    "Options:\n"
    "  --tag-set <file>        Tag-set file (JSON). It must hold a process\n"
    "                          named tagd whose uid is the user this program\n"
    "                          runs as. Default: /etc/scada/tag-set.json\n"
    "  --base-dir <directory>  Directory of the tag database. It must exist,\n"
    "                          be owned by the user this program runs as and\n"
    "                          have mode 2750. Default: /dev/shm/scada\n"
    "  --help                  Show this text and exit.\n"
    "\n"
    "Signals:\n"
    "  SIGTERM, SIGINT  Stop and exit with code 0. The generation stays.\n"
    "\n"
    "Environment (set by systemd):\n"
    "  NOTIFY_SOCKET    Socket for readiness and watchdog messages.\n"
    "  WATCHDOG_USEC    Watchdog interval in microseconds; the watchdog is\n"
    "                   fed four times in it, from the loop that checks\n"
    "                   liveness.\n"
    "  WATCHDOG_PID     Process the watchdog applies to.\n"
    "\n"
    "Exit codes: 0 stopped on request, 1 error, 2 wrong arguments.\n";

constexpr std::int64_t NANOSECONDS_PER_MILLISECOND = 1'000'000;
constexpr std::int64_t NANOSECONDS_PER_MICROSECOND = 1'000;

// A process writes its heartbeat at least this many times within its
// heartbeat timeout (contract, section 10.1).
constexpr std::uint32_t HEARTBEATS_PER_TIMEOUT = 3;

// The watchdog is fed this many times within its interval: often enough
// that one late round does not trip it, and no more often, so that the
// service manager is not sent a message in every round.
constexpr std::int64_t FEEDS_PER_WATCHDOG_INTERVAL = 4;

// Bytes read from the tag-set file in one call.
constexpr std::size_t READ_CHUNK = std::size_t{64} * 1024;

struct Options {
    std::string tag_set_path{DEFAULT_TAG_SET_PATH};
    std::string base_dir{scada::tag::DEFAULT_BASE_DIR};
    bool help = false;
};

// Parses the command line. Nothing if it is not valid.
std::optional<Options> parse_options(std::span<const std::string_view> args) {
    Options options;
    bool tag_set_given = false;
    bool base_dir_given = false;
    if (args.size() == 1 && args.front() == "--help") {
        options.help = true;
        return options;
    }
    // Every other option is followed by its value, which is not empty.
    constexpr std::size_t PAIR = 2;
    if (args.size() % PAIR != 0) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < args.size(); i += PAIR) {
        const std::string_view arg = args[i];
        const std::string_view value = args[i + 1];
        if (value.empty()) {
            return std::nullopt;
        }
        if (arg == "--tag-set" && !tag_set_given) {
            options.tag_set_path = value;
            tag_set_given = true;
        } else if (arg == "--base-dir" && !base_dir_given) {
            options.base_dir = value;
            base_dir_given = true;
        } else {
            return std::nullopt;
        }
    }
    return options;
}

// Reads the whole file at `path`, which must be a regular file of at most
// MAX_TAG_SET_JSON_BYTES bytes. On an error `detail` says what failed.
Result<std::string> read_file(const std::string& path, std::string& detail) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open has no other form
    const Fd file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    struct stat info {};
    if (!file.valid() || ::fstat(file.get(), &info) != 0) {
        detail = "cannot open " + path + ": " +
                 std::generic_category().message(errno);
        return Status(
            errno == ENOENT ? StatusCode::NOT_FOUND : StatusCode::IO_ERROR,
            "cannot open the tag-set file");
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) >
            scada::tag::MAX_TAG_SET_JSON_BYTES) {
        detail = path + " is not a regular file of at most " +
                 std::to_string(scada::tag::MAX_TAG_SET_JSON_BYTES) + " bytes";
        return Status(StatusCode::INVALID_ARGUMENT,
                      "the tag-set file is not usable");
    }
    std::string text;
    std::array<char, READ_CHUNK> chunk{};
    for (;;) {
        const ssize_t got = ::read(file.get(), chunk.data(), chunk.size());
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            detail = "cannot read " + path + ": " +
                     std::generic_category().message(errno);
            return Status(StatusCode::IO_ERROR, "cannot read the tag-set file");
        }
        if (got == 0) {
            return text;
        }
        // In range: read returns at most the size of the chunk.
        text.append(chunk.data(), static_cast<std::size_t>(got));
        // The file may have grown since fstat.
        if (text.size() > scada::tag::MAX_TAG_SET_JSON_BYTES) {
            detail = path + " is too large";
            return Status(StatusCode::INVALID_ARGUMENT,
                          "the tag-set file is not usable");
        }
    }
}

// What ended one wait of the main loop.
enum class Wake : std::uint8_t { CHECK_DUE, STOP, ERROR };

// Waits for one of the signals in `signals`, which must be blocked, for at
// most `timeout_ns`.
Wake wait_for_signal(const sigset_t& signals,
                     std::int64_t timeout_ns) noexcept {
    timespec limit{};
    limit.tv_sec =
        static_cast<std::time_t>(timeout_ns / NANOSECONDS_PER_SECOND);
    // Below one second, which fits every `long`.
    limit.tv_nsec = static_cast<long>(timeout_ns % NANOSECONDS_PER_SECOND);
    if (::sigtimedwait(&signals, nullptr, &limit) < 0) {
        // EAGAIN is the timeout. EINTR means an unrelated signal interrupted
        // the wait; checking early is harmless, checking late is not.
        return (errno == EAGAIN || errno == EINTR) ? Wake::CHECK_DUE
                                                   : Wake::ERROR;
    }
    return Wake::STOP;
}

// Logs a queue file created anew inside the generation (issue #53,
// condition C13): the system log keeps what `scada_queue_stats`, which is
// per process and lost at a restart, cannot. The records of the old file
// from the consumer's cursor up to its last `write_seq` are lost unless
// the consumer still holds the old file and takes them; hence "at most".
// On an events queue they are audit records.
void log_recreated(Logger& logger, const QueueChange& queue) {
    const std::uint64_t from = queue.cursor_known ? queue.cursor : 0;
    const std::uint64_t to = queue.old_write_seq;
    const std::uint64_t lost = to > from ? to - from : 0;
    std::string reason = "followed by the consumer";
    if (!queue.base_seq.has_value()) {
        reason = "no base (made by an older build): the queue stops";
    } else if (*queue.base_seq < to) {
        reason = "base below the old write_seq: the queue stops";
    }
    const std::string base =
        queue.base_seq.has_value() ? std::to_string(*queue.base_seq) : "none";
    const std::string old_written = std::to_string(to);
    const std::string cursor =
        queue.cursor_known ? std::to_string(queue.cursor) : "unknown";
    const std::string lost_at_most = std::to_string(lost);
    const std::string range =
        "[" + std::to_string(from) + ", " + std::to_string(to) + ")";
    const std::array<LogField, 7> fields{{
        {"QUEUE", queue.queue},
        {"BASE_SEQ", base},
        {"OLD_WRITE_SEQ", old_written},
        {"CURSOR", cursor},
        {"LOST_AT_MOST", lost_at_most},
        {"LOST_IDS", range},
        {"REASON", reason},
    }};
    const bool events = queue.queue.starts_with("events.");
    logger.log(LogLevel::WARNING,
               events ? "queue file created anew; audit records may be lost"
                      : "queue file created anew; records may be lost",
               fields);
}

// Writes what one liveness check changed to the log.
void log_report(Logger& logger, const CheckReport& report) {
    for (const ProcessChange& change : report.changes) {
        if (change.reason == DownReason::NONE) {
            logger.log(LogLevel::NOTICE, "process up",
                       {{"PROCESS", change.process}});
        } else {
            logger.log(LogLevel::WARNING, "process down",
                       {{"PROCESS", change.process},
                        {"REASON", scada::tag::to_string(change.reason)}});
        }
    }
    for (const LateHeartbeats& late : report.late) {
        // A late heartbeat is a moment in which readers saw the process
        // down (BAD with OWNER_DOWN, a gap on its rings), also when no
        // check fell into it. WARNING also when a change of the same
        // process was logged above: the gap length is only here. What the
        // first look at a region finds is a baseline, logged at INFO.
        // The texts live until the end of the iteration: a field borrows.
        const std::string added = std::to_string(late.added);
        const std::string total = std::to_string(late.total);
        const std::string longest =
            std::to_string(late.longest_gap_ns / NANOSECONDS_PER_MILLISECOND);
        const std::string last = std::to_string(late.last_late_mono_ns);
        const std::array<LogField, 5> fields{{
            {"PROCESS", late.process},
            {"LATE_BEATS", added},
            {"LATE_BEATS_TOTAL", total},
            {"LONGEST_GAP_MS", longest},
            {"LAST_LATE_MONO_NS", last},
        }};
        if (late.baseline) {
            // Counted before this scada-tagd looked; not new.
            logger.log(LogLevel::INFO, "heartbeat late before this check",
                       fields);
        } else {
            logger.log(LogLevel::WARNING, "heartbeat late", fields);
        }
    }
    for (const QueueChange& queue : report.queues) {
        // A broken queue refuses every post and delivers nothing until the
        // next generation (contract 9.2): actions that need it are refused.
        // Each is logged once per change, not at every check.
        const std::string cursor = std::to_string(queue.cursor);
        const std::string written = std::to_string(queue.write_seq);
        const std::string capacity = std::to_string(queue.capacity);
        const std::array<LogField, 5> fields{{
            {"QUEUE", queue.queue},
            {"CURSOR", cursor},
            {"WRITE_SEQ", written},
            {"CAPACITY", capacity},
            {"REASON", queue.replaced ? "replaced" : "out of range"},
        }};
        switch (queue.event) {
            case QueueEvent::BROKEN:
                logger.log(LogLevel::WARNING, "queue broken", fields);
                break;
            case QueueEvent::NO_LONGER_BROKEN:
                logger.log(LogLevel::NOTICE, "queue no longer broken",
                           std::span(fields).first(4));
                break;
            case QueueEvent::MISSING:
                logger.log(LogLevel::WARNING, "queue file missing",
                           std::span(fields).first(1));
                break;
            case QueueEvent::FOUND:
                logger.log(LogLevel::NOTICE, "queue file found again",
                           std::span(fields).first(1));
                break;
            case QueueEvent::RECREATED:
                log_recreated(logger, queue);
                break;
        }
    }
    for (const SweptEntry& entry : report.swept) {
        // The name is outside input; the sinks escape it and cut it.
        if (entry.removed) {
            logger.log(LogLevel::WARNING,
                       "removed an entry that does not belong in the "
                       "generation directory",
                       {{"ENTRY", entry.name}});
        } else {
            logger.log(LogLevel::ERROR,
                       "cannot remove an entry that does not belong in the "
                       "generation directory",
                       {{"ENTRY", entry.name}});
        }
    }
}

// Logs what a sample of the clocks showed about a system suspend (contract
// 3.4): the unit must not suspend, and a suspend makes write requests
// expire later in real time than their requesters meant.
void note_suspend(Logger& logger, SuspendWatch& watch) {
    const std::optional<SuspendFinding> finding =
        watch.observe(sample_clocks());
    if (!finding.has_value()) {
        return;
    }
    // Rounded up, so that a suspend shorter than 1 ms does not read as 0.
    const std::string suspended = std::to_string(
        (finding->suspended_ns + NANOSECONDS_PER_MILLISECOND - 1) /
        NANOSECONDS_PER_MILLISECOND);
    logger.log(LogLevel::WARNING,
               finding->at_start ? "the system was suspended since boot"
                                 : "the system was suspended",
               {{"SUSPENDED_MS", suspended}});
}

// The time between two watchdog messages, or nothing if no watchdog applies
// to this process.
std::optional<std::int64_t> feed_period_ns(
    const NotifySettings& settings) noexcept {
    if (!settings.watchdog_interval) {
        return std::nullopt;
    }
    // At least 1 ns: an interval below 4 microseconds is fed in every round.
    return std::max(std::int64_t{1}, settings.watchdog_interval->count() *
                                         NANOSECONDS_PER_MICROSECOND /
                                         FEEDS_PER_WATCHDOG_INTERVAL);
}

// The time between two rounds of the main loop: the liveness check
// interval, shortened so that the heartbeat of `proc.tagd` and the watchdog
// are written often enough.
std::int64_t loop_period_ns(const GenerationHost& host,
                            const NotifySettings& settings) noexcept {
    const std::uint32_t heartbeat_ms = std::max(
        std::uint32_t{1}, host.heartbeat_timeout_ms() / HEARTBEATS_PER_TIMEOUT);
    std::int64_t period =
        std::int64_t{std::min(host.heartbeat_check_ms(), heartbeat_ms)} *
        NANOSECONDS_PER_MILLISECOND;
    if (const std::optional<std::int64_t> feed = feed_period_ns(settings)) {
        period = std::min(period, *feed);
    }
    return period;
}

// Runs until a stop signal arrives. Returns the process exit code.
int serve(GenerationHost& host, Notifier& notifier,
          const NotifySettings& settings, const sigset_t& signals,
          Logger& logger, SuspendWatch& suspend) {
    const std::int64_t period = loop_period_ns(host, settings);
    const std::optional<std::int64_t> feed_period = feed_period_ns(settings);
    bool feeding = true;  // the last watchdog message was sent
    std::int64_t next_feed = monotonic_ns();
    std::int64_t next = monotonic_ns();
    for (;;) {
        const std::int64_t now = monotonic_ns();
        // The heartbeat, the check and the watchdog share this loop, so
        // that a hung loop stops all three (contract, sections 3.4, 10.1).
        host.heartbeat(now);
        log_report(logger, host.check(now));
        note_suspend(logger, suspend);
        if (feed_period.has_value() && now >= next_feed) {
            const bool fed = notifier.watchdog().ok();
            if (!fed && feeding) {
                // Keep running: if feeding stays broken the service manager
                // restarts the service, which is its job. Logged once, not
                // at every attempt.
                logger.log(LogLevel::ERROR, "cannot feed the watchdog");
            }
            feeding = fed;
            next_feed = now + *feed_period;
        }

        // The next round is due one period after this one was; a round
        // that came late does not shift the ones after it, and rounds that
        // were missed are not made up.
        const std::int64_t done = monotonic_ns();
        next = std::max(next + period, done);
        switch (wait_for_signal(signals, next - done)) {
            case Wake::CHECK_DUE:
                break;
            case Wake::STOP:
                host.stop();
                // Best effort: the process exits either way, and the
                // service manager sees the exit.
                static_cast<void>(notifier.stopping());
                logger.log(LogLevel::NOTICE, "stopped on request");
                return EXIT_OK;
            case Wake::ERROR:
                logger.log(LogLevel::CRITICAL, "waiting for signals failed");
                return EXIT_ERROR;
        }
    }
}

// Writes an error both to the log and to `err`, so that it is seen under a
// service manager and from a terminal.
void fail(Logger* logger, std::ostream& err, std::string_view what,
          const Status& status, std::string_view detail) {
    if (logger != nullptr) {
        logger->log(LogLevel::CRITICAL, what,
                    {{"STATUS", to_string(status.code())}, {"DETAIL", detail}});
    }
    err << "scada-tagd: " << what << ": " << status.message();
    if (!detail.empty()) {
        err << " (" << detail << ')';
    }
    err << '\n' << std::flush;
}

int start(const Options& options, std::ostream& err) {
    std::string detail;
    const Result<std::string> text = read_file(options.tag_set_path, detail);
    if (!text.ok()) {
        fail(nullptr, err, "cannot read the tag set", text.status(), detail);
        return EXIT_ERROR;
    }
    const Result<TagSet> tag_set =
        scada::tag::parse_tag_set_json(text.value(), detail);
    if (!tag_set.ok()) {
        fail(nullptr, err, "the tag-set file is not valid", tag_set.status(),
             detail);
        return EXIT_ERROR;
    }

    const Result<NotifySettings> settings =
        scada::service::parse_notify_environment(
            scada::service::read_notify_environment(), ::getpid());
    if (!settings.ok()) {
        fail(nullptr, err, "invalid environment", settings.status(), "");
        return EXIT_ERROR;
    }

    // The signals are blocked and then collected synchronously, so no signal
    // handler and no global flag is needed. They are blocked before the
    // first thread is started: a thread inherits the mask, and a stop
    // signal delivered to a thread that has not blocked it would end the
    // process without the stop being handled.
    sigset_t signals{};
    if (::sigemptyset(&signals) != 0 || ::sigaddset(&signals, SIGTERM) != 0 ||
        ::sigaddset(&signals, SIGINT) != 0 ||
        ::pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        fail(nullptr, err, "cannot block signals",
             Status(StatusCode::INTERNAL, "the signal mask was refused"), "");
        return EXIT_ERROR;
    }

    Result<std::unique_ptr<LogSink>> sink = make_default_sink();
    if (!sink.ok()) {
        fail(nullptr, err, "cannot open the log", sink.status(), "");
        return EXIT_ERROR;
    }
    Result<Logger> logger =
        Logger::create(std::move(sink).value(), {.component = "scada-tagd"});
    if (!logger.ok()) {
        fail(nullptr, err, "cannot start the log", logger.status(), "");
        return EXIT_ERROR;
    }

    Result<GenerationHost> host = GenerationHost::start(
        tag_set.value(),
        {.base_dir = options.base_dir, .tagd_uid = ::geteuid()}, detail);
    if (!host.ok()) {
        fail(&logger.value(), err, "cannot start the tag database",
             host.status(), detail);
        return EXIT_ERROR;
    }
    // Contract 3.4: the unit must not suspend. Whether the sleep targets
    // are masked cannot be seen from here. Most kernels offer sleep states
    // even when they are, so what the kernel offers is INFO, logged once;
    // a measured suspend (below, and in every round) is the WARNING.
    if (const std::string states =
            read_sleep_states(std::string(SLEEP_STATES_PATH));
        !states.empty()) {
        logger.value().log(LogLevel::INFO,
                           "the kernel offers sleep states; the unit must "
                           "not suspend",
                           {{"STATES", states}});
    }
    SuspendWatch suspend;
    note_suspend(logger.value(), suspend);
    for (const std::string& note : host.value().notes()) {
        logger.value().log(LogLevel::NOTICE, note);
    }
    const std::string generation = std::to_string(host.value().generation());
    logger.value().log(
        LogLevel::NOTICE,
        host.value().reused() ? "generation reused" : "generation created",
        {{"GENERATION", generation}});

    Notifier notifier(settings.value());
    if (!notifier.ready().ok()) {
        fail(&logger.value(), err,
             "cannot report readiness to the service manager",
             Status(StatusCode::IO_ERROR, "the notification was not sent"), "");
        return EXIT_ERROR;
    }
    // Best effort: the status line only helps the person looking.
    static_cast<void>(notifier.set_status_text("Generation " + generation));
    return serve(host.value(), notifier, settings.value(), signals,
                 logger.value(), suspend);
}

}  // namespace

int run(std::span<const std::string_view> args, std::ostream& out,
        std::ostream& err) {
    const std::optional<Options> options = parse_options(args);
    if (!options.has_value()) {
        err << "scada-tagd: unexpected arguments\n" << USAGE;
        return EXIT_USAGE;
    }
    if (options->help) {
        out << USAGE;
        return EXIT_OK;
    }
    return start(*options, err);
}

}  // namespace scada::tagd
