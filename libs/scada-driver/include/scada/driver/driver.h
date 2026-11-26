#ifndef SCADA_DRIVER_DRIVER_H
#define SCADA_DRIVER_DRIVER_H

// A driver process (driver interface contract 2.4, 2.6, 9.1, 10; decision
// D-142): the main thread and one worker thread per channel. A protocol's
// executable builds the compiled configuration and the transports, and
// `main` only calls `Driver::run` (CLAUDE.md: a library and a thin
// executable).
//
// The main thread blocks the stop signals and takes them from a signalfd,
// attaches to the tag database, learns the owner of each channel from its
// first tag and starts one worker per channel, which writes every tag of
// its owner at once (9.1). It then resolves the tags, checks that every tag
// of a channel has the channel's owner and that no two channels share one,
// hands each worker its handles, reports ready to the service manager, and
// then writes the heartbeat and the service manager's watchdog
// notification, and checks the workers' progress counters (the channel
// watchdog, 2.6). A worker runs its ChannelEngine, waiting between steps on
// its connection (POLLRDHUP, a stream only) and on its stop eventfd, each
// wait at most a third of the channel's stall limit; the stop eventfd also
// ends the transport's own waits (DI 10). Each worker also writes the
// system tags of its channel and devices (11.1, D-144). The command thread
// comes with the command slices (6, 7), the workers' log queues with slice
// 5b-3.

#include "scada/common/log.h"
#include "scada/common/status.h"
#include "scada/driver/compiled.h"
#include "scada/driver/protocol.h"
#include "scada/driver/transport.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace scada::driver {

// Exit statuses of a driver process.
inline constexpr int EXIT_OK = 0;     // stopped on request
inline constexpr int EXIT_ERROR = 1;  // could not start or lost the database
inline constexpr int EXIT_USAGE = 2;  // a malformed command line
// A worker stopped moving its progress counter (2.6). Distinct, so that the
// service manager's record of the exit tells a stall from an error.
inline constexpr int EXIT_STALL = 4;

// The least stall limit (2.6).
inline constexpr std::int64_t MIN_STALL_LIMIT_NS = 1'000'000'000;

// The stop budget (DI 10): from a stop signal, or a failed start, to the
// detach. Every wait of a worker ends at the stop (a read in flight and a
// connect are abandoned), so a worker that has not reached its boundary
// within it is stuck: the run then ends without joining it (D-142). Below
// every service manager's stop time the installed unit sets (#105).
inline constexpr std::int64_t STOP_BUDGET_NS = 2'000'000'000;

// The longest heartbeat period DriverSettings accepts.
inline constexpr std::int64_t MAX_HEARTBEAT_PERIOD_NS = 10'000'000'000;

// The stall limit of a channel (2.6): three times the longest single wait
// its worker can be in, the largest request deadline of its enabled
// devices or `connect_timeout`, whichever is longer, and at least 1 s. A
// request deadline is 7.1's two stages at their longest: a request of the
// device's largest frame (`DeviceCaps::max_frame_bytes`) on the wire, the
// larger of `timeout` and `write_timeout`, an answer of the largest frame,
// and the margin. Saturates.
[[nodiscard]] std::int64_t stall_limit_ns(
    const CompiledChannel& channel, const FrameProtocol& protocol) noexcept;

// The tag of a point (CS 12.4): `<device>.<point>`.
[[nodiscard]] std::string point_tag_name(const CompiledDevice& device,
                                         const CompiledPoint& point);

// Builds the connection of a channel: a TcpTransport or a SerialTransport
// from its keys (connections.h), or a test's own. Called on the main thread
// before the workers start; null refuses the channel, and the run fails.
using TransportFactory = std::function<std::unique_ptr<Transport>(
    const CompiledChannel& channel, const Clock& clock)>;

inline constexpr std::int64_t DEFAULT_HEARTBEAT_PERIOD_NS = 100'000'000;

struct DriverSettings {
    // `drv-<protocol>` (CS 12.2): the process name in the tag set.
    std::string process;
    // The tag database (TD 4), the retain directory (TD 12) and the user id
    // of scada-tagd; empty and -1 for the defaults of `scada_tag_attach`.
    std::string base_dir;
    std::string retain_dir;
    std::int64_t tagd_uid = -1;
    // The period of the heartbeat and of the watchdog check; well below the
    // process's heartbeat timeout in the tag set. Above 0 and at most
    // MAX_HEARTBEAT_PERIOD_NS.
    std::int64_t heartbeat_period_ns = DEFAULT_HEARTBEAT_PERIOD_NS;
};

// Blocks the stop signals the main thread takes from its signalfd
// (SIGTERM, SIGINT, SIGHUP) on the calling thread (DI 2.4). `main` calls it
// first, before any thread starts, the logger's included: a thread inherits
// the mask of its creator, and a stop signal sent to the process must reach
// only the signalfd, never a thread that would die of it. SIGPIPE is
// blocked as well and never taken, so that a write to a closed standard
// error or socket fails with EPIPE instead of ending the process. Every
// other signal keeps its default: SIGABRT and the fault signals end the
// process at once with a core (review M8). INTERNAL if the mask cannot be
// set.
[[nodiscard]] Status block_stop_signals() noexcept;

// How a run ended.
enum class RunEnd : std::uint8_t {
    STOPPED = 1,  // a stop signal; every worker joined, detached
    FAILED,       // could not start, or the generation retires (slice 8)
    STALLED,      // a worker stalled, or did not stop within the budget
};

// The exit status of a run that ended so: EXIT_OK, EXIT_ERROR, EXIT_STALL.
[[nodiscard]] int exit_status(RunEnd end) noexcept;

class Driver {
public:
    // `config`, `protocol`, `clock` and `logger` are borrowed and must
    // outlive the Driver; `logger` may be null (nothing is logged). A
    // disabled channel gets a worker that owns and writes its tags (9.1)
    // and is never connected (CS 4.6). `settings` is checked here: a run of
    // a Driver whose settings are not valid fails at once.
    Driver(const CompiledConfig& config, const FrameProtocol& protocol,
           TransportFactory transports, Clock& clock, DriverSettings settings,
           Logger* logger);
    ~Driver();
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;
    Driver(Driver&&) = delete;
    Driver& operator=(Driver&&) = delete;

    // Runs the process on the calling thread, which must be its main
    // thread and have called `block_stop_signals` before any thread
    // started, until SIGTERM or SIGINT, a failure or a stall. SIGHUP, the
    // reload of 12, is taken and ignored until slice 8. A second call fails
    // at once. Every end but a stall takes the bounded stop: the workers are
    // asked to stop (a read in flight and a connect are abandoned, DI 10),
    // the heartbeat and the stall check go on, and they are joined within
    // STOP_BUDGET_NS. On a stall (2.6): logged with the channel's name and
    // flushed, the other workers stopped, the heartbeat and the watchdog
    // notification stopped, and STALLED returned. Whenever `joined()` is
    // false after the run, a thread still runs: the caller, `main`, must end
    // the process at once with `std::_Exit(exit_status(end))` (coding
    // standard 7) and not destroy the Driver.
    [[nodiscard]] RunEnd run() noexcept;

    // True when no thread of the Driver runs any more, so that destroying
    // it is safe and waits for nothing.
    [[nodiscard]] bool joined() const noexcept;

    // Why the run failed, after FAILED, and the same with the names and
    // numbers involved, for a person.
    [[nodiscard]] const Status& failure() const noexcept { return failure_; }
    [[nodiscard]] const std::string& failure_detail() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    Status failure_;
};

}  // namespace scada::driver

#endif  // SCADA_DRIVER_DRIVER_H
