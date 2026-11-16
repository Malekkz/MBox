#ifndef SCADA_TAGD_REPORT_LOG_H
#define SCADA_TAGD_REPORT_LOG_H

// The log records of the liveness check of scada-tagd, each with a fixed
// journald MESSAGE_ID (decision D-47, issue #61). Private to libscada-tagd;
// the IDs and fields are documented in its README.
//
// The IDs and fields are a provisional diagnostic input for the alarm
// service of a later phase, not an authority: any local process can write
// a journal record with the same fields.

#include "late_log.h"

#include "scada/common/log.h"
#include "scada/tag/generation_host.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace scada::tagd {

// MESSAGE_ID of each record kind: 128 bits as 32 lower-case hexadecimal
// digits, the journal's form. Never changed once released; a new meaning
// takes a new ID.
inline constexpr std::string_view MESSAGE_ID_PROCESS_DOWN =
    "51ad850aac4848fab6eade0000eaa868";
inline constexpr std::string_view MESSAGE_ID_PROCESS_UP =
    "3e293a9e793b4fa08fafb02b5f16ae7f";
inline constexpr std::string_view MESSAGE_ID_HEARTBEAT_LATE =
    "31aad0d613b645d39a83e7978c4531ec";
inline constexpr std::string_view MESSAGE_ID_HEARTBEAT_LATE_BEFORE =
    "c4de2438d3574ca38f45771de9254e3a";
inline constexpr std::string_view MESSAGE_ID_PROCESS_RESTARTED =
    "f35dfeb8e8124cc88b219cabf6947b00";
// A queue file created anew inside a generation: on an events queue, the
// record of possibly lost audit records (issue #53, decision D-49).
inline constexpr std::string_view MESSAGE_ID_QUEUE_RECREATED =
    "330f9ce1bb0547c098562d406ce2f774";
// A queue's `write_seq` fell in the same file (decision D-51).
inline constexpr std::string_view MESSAGE_ID_QUEUE_WRITE_SEQ_FELL =
    "d673f795274a49d1882153ae03003b15";
inline constexpr std::string_view MESSAGE_ID_PROCESS_NOT_ATTACHED =
    "0f57ce3d6dbc4891abd0e55e240f04f9";
inline constexpr std::string_view MESSAGE_ID_RECORDS_UNCONSUMED =
    "42c8e35948cb467eb6d894d3bb0476a8";
// Retain problems, one ID each (decision D-59, point 3).
inline constexpr std::string_view MESSAGE_ID_RETAIN_FILE_REJECTED =
    "586ec48106f748609721eef8957c56d5";
inline constexpr std::string_view MESSAGE_ID_RETAIN_WRITE_FAILED =
    "974151cda6fa452a998a576b2bb09bce";
inline constexpr std::string_view MESSAGE_ID_RETAIN_DIRECTORY_UNUSABLE =
    "8df1e064b8464d59ad5fa42e9a3070e4";
inline constexpr std::string_view MESSAGE_ID_STILL_ATTACHED_AT_TIMEOUT =
    "349b6814c6cc4a2fafa5d3d1af16e7a7";
inline constexpr std::string_view MESSAGE_ID_RETAIN_WRITER_BOUND =
    "0031dfe3391142b0b9fa995b07115e10";
inline constexpr std::string_view MESSAGE_ID_VALUES_UNSAVED_AT_EXIT =
    "7cf5c1c8535f44f6a297db538666d7a3";

// Logs records of late heartbeats: `heartbeat late` at WARNING, or, for a
// baseline, `heartbeat late before this check` at INFO.
void log_late(scada::Logger& logger, std::span<const LateRecord> records);

// Logs, for a retired generation removed or abandoned, each queue that
// still held records (decision D-54): WARNING, `audit records left
// unconsumed in a removed generation` for an events queue, `records left
// unconsumed ...` for the others, with GENERATION, QUEUE, RECORDS (a count
// or "unknown") and, for a known count, FIRST_SEQ and END_SEQ.
void log_unconsumed(scada::Logger& logger,
                    const scada::tag::RetiredGeneration& retired);

// Logs one retain problem with the MESSAGE_ID of its kind (decision D-59):
// `retain file ignored` (WARNING), `retain file not written` (WARNING),
// `retain writer did not finish within its bound` (ERROR) or `values
// unsaved at exit` (ERROR), with SUBJECT and DETAIL.
void log_retain_problem(scada::Logger& logger,
                        const scada::tag::RetainProblem& problem);

// Logs, for a retired generation removed after its timeout, each process
// still attached to it: WARNING, `process still attached to a retired
// generation`, with GENERATION and PROCESS ("<name> (pid <pid>)").
void log_still_attached(scada::Logger& logger,
                        const scada::tag::RetiredGeneration& retired);

// Logs that the retain directory is not usable at the start: CRITICAL,
// `the retain directory is not usable`, with DIRECTORY, STATUS and DETAIL.
void log_retain_directory_unusable(scada::Logger& logger,
                                   std::string_view directory,
                                   std::string_view status,
                                   std::string_view detail);

// Logs what one liveness check made at `now_mono_ns` found: each process
// not attached at the first check of a new generation, every change
// of a `down` flag, every restart, late heartbeats after the rate limit of
// `late_log`, every change of a queue's state, and every swept entry. Changes
// and restarts are not rate limited.
void log_report(scada::Logger& logger, const scada::tag::CheckReport& report,
                LateLog& late_log, std::int64_t now_mono_ns);

}  // namespace scada::tagd

#endif  // SCADA_TAGD_REPORT_LOG_H
