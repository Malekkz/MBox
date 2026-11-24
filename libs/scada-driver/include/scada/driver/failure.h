#ifndef SCADA_DRIVER_FAILURE_H
#define SCADA_DRIVER_FAILURE_H

// Device states and failure accounting (driver interface contract 3.5, 7.4).
// A device is demoted after `demote_after` consecutive failed requests,
// counted across poll groups, once per request after its retries; devices
// that share a unit id share one count. A demoted device is probed every
// `backoff`, doubling to `backoff_max`; a probe that succeeds recovers it.
// On `tcp`, a run of consecutive read timeouts that spans every live unit
// is a lost connection (7.3, R-15). Pure logic over an injected clock; the
// worker applies it at each transaction boundary.

#include "scada/common/realtime.h"

#include <cstdint>
#include <vector>

namespace scada::driver {

// `sys.dev.<device>.state` (3.5).
enum class DeviceState : std::uint8_t {
    DISABLED = 0,
    CONNECTING = 1,
    INITIALISING = 2,
    RUNNING = 3,
    DEMOTED = 4,
    MISMATCH = 5,
    REFUSING = 6,
};

inline constexpr std::int64_t DEFAULT_TIMEOUT_NS = 1'000'000'000;   // 1 s
inline constexpr std::int64_t DEFAULT_BACKOFF_NS = 10'000'000'000;  // 10 s
inline constexpr std::int64_t DEFAULT_BACKOFF_MAX_NS =
    300'000'000'000;  // 300 s

// The failure keys of a device (CS 4.6), with their defaults.
struct FailureRules {
    std::int64_t timeout_ns = DEFAULT_TIMEOUT_NS;
    std::uint8_t retries = 2;
    std::uint8_t busy_retries = 3;
    std::uint8_t demote_after = 3;
    std::int64_t backoff_ns = DEFAULT_BACKOFF_NS;
    std::int64_t backoff_max_ns = DEFAULT_BACKOFF_MAX_NS;
    std::int64_t startup_grace_ns = 0;
};

// How one request ended, after its retries (7.4).
enum class RequestEnd : std::uint8_t {
    ANSWERED = 1,  // any valid answer: samples, an exception, ABSENT, ...
    FAILED,        // a timeout, a broken answer, a gateway without its unit
    NOT_COUNTED,   // busy: neither a failure nor an answer
};

// The failure state of the devices of one channel.
class FailureTracker {
public:
    // One entry per device: its unit id and rules. Allocates.
    struct Device {
        std::uint32_t unit_id = 0;
        FailureRules rules;
        bool enabled = true;
        bool has_poll_group = true;  // at least one point in a poll group
    };
    // `lost_after_timeouts` is the channel's key (CS) on `tcp`, and 0 where
    // read timeouts never close the connection (serial, `rtu_over_tcp`).
    explicit FailureTracker(std::vector<Device> devices,
                            std::uint32_t lost_after_timeouts = 0);

    // A connection was made at `now_ns`: every enabled device starts its
    // grace and is INITIALISING; counts start from 0 (7.4).
    void connected(std::int64_t now_ns) noexcept;

    // The connection is lost: every enabled device is CONNECTING (7.3).
    void lost() noexcept;

    // A close the driver made itself was followed by a reconnect within
    // `connect_timeout` (7.3): states, counts and grace are kept; only the
    // run of consecutive timeouts is emptied.
    void resumed() noexcept;

    // What a read time-out after its retries did.
    struct TimeoutEnd {
        bool lost = false;     // the connection is lost (7.3)
        bool demoted = false;  // some device was demoted (7.4)
    };

    // A planned read of a poll group or a keep-alive of `device` timed out
    // after its retries at `now_ns` (7.3, 7.4). First it adds one to the run
    // of consecutive time-outs and records its unit id; the connection is
    // lost when the run holds at least `lost_after_timeouts` requests and
    // its unit ids include every eligible one (those with a device
    // INITIALISING or RUNNING that has a poll group; with none eligible, the
    // run's length alone decides). A time-out that loses the connection is
    // not counted toward demotion (the lead's ruling, D-139); any other is
    // counted as `request_ended` FAILED does. Identity reads, READ_NOW and
    // probes are not passed here. With the rule off, only the count.
    SCADA_RT TimeoutEnd read_timed_out(std::uint32_t device,
                                       std::int64_t now_ns) noexcept;

    // Any valid answer from any unit, an exception answer included, empties
    // the run (7.3).
    SCADA_RT void answer_seen() noexcept;

    // The requests in the run of consecutive timeouts.
    [[nodiscard]] std::uint32_t timeout_run() const noexcept {
        return run_length_;
    }

    // The end of a polled request of `device` at `now_ns`. A failure
    // outside the device's grace adds one to its unit's count; an answer
    // empties it. Every device of the unit whose own `demote_after` the
    // count reaches is demoted. An answer makes an INITIALISING device
    // RUNNING. Returns true when some device was demoted by this call.
    SCADA_RT bool request_ended(std::uint32_t device, RequestEnd end,
                                std::int64_t now_ns) noexcept;

    // The end of a probe of a demoted `device` (7.4): an answer recovers it
    // to INITIALISING with its back-off reset; a failure doubles its
    // back-off, up to `backoff_max`.
    SCADA_RT void probe_ended(std::uint32_t device, bool answered,
                              std::int64_t now_ns) noexcept;

    [[nodiscard]] DeviceState state(std::uint32_t device) const noexcept;
    // When a demoted device's next probe is due.
    [[nodiscard]] std::int64_t probe_due_ns(
        std::uint32_t device) const noexcept;
    // The consecutive failures counted for the device's unit id.
    [[nodiscard]] std::uint32_t failures(std::uint32_t device) const noexcept;
    // The device is polled: INITIALISING or RUNNING.
    [[nodiscard]] bool polled(std::uint32_t device) const noexcept;

private:
    struct Run {
        DeviceState state = DeviceState::CONNECTING;
        std::int64_t grace_until_ns = 0;
        std::int64_t backoff_ns = 0;
        std::int64_t probe_due_ns = 0;
    };

    // Adds the time-out of `device` to the run; true when the connection is
    // lost.
    bool add_to_run(std::uint32_t device) noexcept;

    // Index into units_ of each device.
    [[nodiscard]] std::uint32_t& count_of(std::uint32_t device) noexcept;
    [[nodiscard]] std::uint32_t count_of(std::uint32_t device) const noexcept;

    std::vector<Device> devices_;
    std::vector<Run> runs_;
    std::vector<std::uint32_t> unit_index_;  // per device
    std::vector<std::uint32_t> counts_;      // per distinct unit id
    std::uint32_t lost_after_ = 0;
    std::uint32_t run_length_ = 0;
    std::vector<bool> run_units_;  // per distinct unit id: in the run
};

}  // namespace scada::driver

#endif  // SCADA_DRIVER_FAILURE_H
