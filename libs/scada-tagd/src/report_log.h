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
inline constexpr std::string_view MESSAGE_ID_PROCESS_NOT_ATTACHED =
    "0f57ce3d6dbc4891abd0e55e240f04f9";

// Logs records of late heartbeats: `heartbeat late` at WARNING, or, for a
// baseline, `heartbeat late before this check` at INFO.
void log_late(scada::Logger& logger, std::span<const LateRecord> records);

// Logs what one liveness check made at `now_mono_ns` found: each process
// not attached at the first check of a new generation, every change
// of a `down` flag, every restart, late heartbeats after the rate limit of
// `late_log`, every change of a queue's state, and every swept entry. Changes
// and restarts are not rate limited.
void log_report(scada::Logger& logger, const scada::tag::CheckReport& report,
                LateLog& late_log, std::int64_t now_mono_ns);

}  // namespace scada::tagd

#endif  // SCADA_TAGD_REPORT_LOG_H
