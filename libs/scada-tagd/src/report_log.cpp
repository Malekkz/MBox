#include "report_log.h"

#include "late_log.h"

#include "scada/common/log.h"
#include "scada/tag/generation_host.h"
#include "scada/tag/liveness.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace scada::tagd {

namespace {

using scada::LogField;
using scada::LogLevel;
using scada::tag::DownReason;
using scada::tag::ProcessChange;
using scada::tag::QueueChange;
using scada::tag::QueueEvent;
using scada::tag::Restarts;
using scada::tag::SweptEntry;

constexpr std::int64_t NANOSECONDS_PER_MILLISECOND = 1'000'000;
constexpr std::int64_t NANOSECONDS_PER_MICROSECOND = 1'000;

void log_restarts(scada::Logger& logger, std::span<const Restarts> restarts) {
    for (const Restarts& restart : restarts) {
        // The texts live until the end of the iteration: a field borrows.
        const std::string added = std::to_string(restart.added);
        const std::string unseen = std::to_string(restart.unseen);
        const std::string total = std::to_string(restart.total);
        const std::array<LogField, 5> fields{{
            {"PROCESS", restart.process},
            {"RESTARTS", added},
            {"RESTARTS_UNSEEN", unseen},
            {"STARTS_TOTAL", total},
            {"MESSAGE_ID", MESSAGE_ID_PROCESS_RESTARTED},
        }};
        // WARNING when a life came and went between two checks: readers saw
        // the process down and no `process down` record says so. NOTICE
        // when the restart is the one logged as `process down` and `process
        // up`.
        logger.log(restart.unseen > 0 ? LogLevel::WARNING : LogLevel::NOTICE,
                   "process restarted", fields);
    }
}

// Logs a queue file created anew inside the generation (issue #53,
// condition C13): the system log keeps what `scada_queue_stats`, which is
// per process and lost at a restart, cannot. The records of the old file
// from the consumer's cursor up to its last `write_seq` are lost unless
// the consumer still holds the old file and takes them; hence "at most".
// On an events queue they are audit records.
void log_recreated(scada::Logger& logger, const QueueChange& queue) {
    const std::uint64_t from = queue.cursor_known ? queue.cursor : 0;
    const std::uint64_t until = queue.old_write_seq;
    const std::uint64_t lost = until > from ? until - from : 0;
    std::string reason = "followed by the consumer";
    if (!queue.base_seq.has_value()) {
        reason = "no base (made by an older build): the queue stops";
    } else if (*queue.base_seq < until) {
        reason = "base below the old write_seq: the queue stops";
    }
    const std::string base =
        queue.base_seq.has_value() ? std::to_string(*queue.base_seq) : "none";
    const std::string old_written = std::to_string(until);
    const std::string cursor =
        queue.cursor_known ? std::to_string(queue.cursor) : "unknown";
    const std::string lost_at_most = std::to_string(lost);
    const std::string range =
        "[" + std::to_string(from) + ", " + std::to_string(until) + ")";
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

}  // namespace

void log_late(scada::Logger& logger, std::span<const LateRecord> records) {
    for (const LateRecord& late : records) {
        // A late heartbeat is a moment in which readers saw the process
        // down (BAD with OWNER_DOWN, a gap on its rings), also when no
        // check fell into it. WARNING also when a change of the same
        // process was logged in the same check: the gap length is only
        // here. What the first look at a region finds is a baseline, logged
        // at INFO. The time is in microseconds, the unit of journald's own
        // monotonic stamps. The texts live until the end of the iteration:
        // a field borrows.
        const std::string added = std::to_string(late.added);
        const std::string total = std::to_string(late.total);
        const std::string longest =
            std::to_string(late.longest_gap_ns / NANOSECONDS_PER_MILLISECOND);
        const std::string last = std::to_string(late.last_late_mono_ns /
                                                NANOSECONDS_PER_MICROSECOND);
        const std::string checks = std::to_string(late.checks);
        const std::array<LogField, 7> fields{{
            {"PROCESS", late.process},
            {"LATE_BEATS", added},
            {"LATE_BEATS_TOTAL", total},
            {"LONGEST_GAP_MS", longest},
            {"LAST_LATE_MONO_US", last},
            {"CHECKS", checks},
            {"MESSAGE_ID", late.baseline ? MESSAGE_ID_HEARTBEAT_LATE_BEFORE
                                         : MESSAGE_ID_HEARTBEAT_LATE},
        }};
        if (late.baseline) {
            // Counted before this scada-tagd looked; not new.
            logger.log(LogLevel::INFO, "heartbeat late before this check",
                       fields);
        } else {
            logger.log(LogLevel::WARNING, "heartbeat late", fields);
        }
    }
}

void log_report(scada::Logger& logger, const scada::tag::CheckReport& report,
                LateLog& late_log, std::int64_t now_mono_ns) {
    for (const ProcessChange& change : report.changes) {
        if (change.reason == DownReason::NONE) {
            logger.log(LogLevel::NOTICE, "process up",
                       {{"PROCESS", change.process},
                        {"MESSAGE_ID", MESSAGE_ID_PROCESS_UP}});
        } else {
            logger.log(LogLevel::WARNING, "process down",
                       {{"PROCESS", change.process},
                        {"REASON", scada::tag::to_string(change.reason)},
                        {"MESSAGE_ID", MESSAGE_ID_PROCESS_DOWN}});
        }
    }
    log_restarts(logger, report.restarts);
    log_late(logger, late_log.admit(report.late, now_mono_ns));
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

}  // namespace scada::tagd
