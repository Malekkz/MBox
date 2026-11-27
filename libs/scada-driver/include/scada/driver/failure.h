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

// `sys.dev.<device>.state` (3.5). Not `DeviceState` of 2.2 (exchange.h),
// which is a protocol's mutable state of a device.
enum class DeviceStatus : std::uint8_t {
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
    // `write_timeout`: the compile step gives it `timeout`'s value when it
    // is not set (CS 4.11).
    std::int64_t write_timeout_ns = DEFAULT_TIMEOUT_NS;
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
        // A unit of its own (`CompiledDevice::OWN_UNIT`): shares its count
        // with no other device, whatever `unit_id` says (review H5).
        bool own_unit = false;
        FailureRules rules;
        bool enabled = true;
        bool has_poll_group = true;  // at least one point in a poll group
        bool identify = false;       // has identity checks (3.6; D-145)
    };
    // `lost_after_timeouts` is the channel's key (CS) on `tcp`, and 0 where
    // read timeouts never close the connection (serial, `rtu_over_tcp`).
    explicit FailureTracker(std::vector<Device> devices,
                            std::uint32_t lost_after_timeouts = 0);

    // A connection was made at `now_ns`: every enabled device starts its
    // grace and is INITIALISING, but a demoted one is DEMOTED again with its
    // probe schedule; failure counts are kept (7.3, 7.4). An INITIALISING
    // device with identity checks has its check pending (D-145).
    void connected(std::int64_t now_ns) noexcept;

    // The connection is lost: every enabled device is CONNECTING (7.3); a
    // demoted one stays demoted for the next connect.
    void lost() noexcept;

    // A close the driver made itself was followed by a reconnect within
    // `connect_timeout` (7.3): states, counts and grace are kept; only the
    // run of consecutive timeouts is emptied. A pending identity check stays
    // pending, and none is started (D-145).
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
    // count reaches is demoted; one that a probe recovered and that no poll
    // answered since has its back-off doubled first, up to `backoff_max`, so
    // that a unit that answers probes and no poll does not flap at the same
    // rate for ever (review G6). An answer makes an INITIALISING device
    // RUNNING. Returns true when some device was demoted by this call.
    SCADA_RT bool request_ended(std::uint32_t device, RequestEnd end,
                                std::int64_t now_ns) noexcept;

    // The end of a probe of a demoted `device` (7.4), for every demoted
    // device of its unit: an answer recovers them to INITIALISING, with
    // their identity check pending, and empties the count; a failure
    // doubles their back-off, up to `backoff_max`. The back-off is reset
    // only by a poll that answered.
    SCADA_RT void probe_ended(std::uint32_t device, bool answered,
                              std::int64_t now_ns) noexcept;

    // The end of an identity read of `device` at `now_ns`, after its
    // retries (D-145; DI 7.3). It neither adds to the run of consecutive
    // time-outs nor empties it. A failure outside the grace counts toward
    // the unit's demotion as a polled read's does (`request_ended`), so a
    // dead device is DEMOTED, never MISMATCH. An answer empties the unit's
    // count, but makes no device RUNNING and resets no back-off: a unit
    // that answers its identity and nothing else is not seen to recover.
    // For a device INITIALISING, RUNNING or MISMATCH; returns true when some
    // device was demoted.
    SCADA_RT bool identity_ended(std::uint32_t device, RequestEnd end,
                                 std::int64_t now_ns) noexcept;

    // The identity check of `device` matched (or failed with `warn`): it is
    // no longer pending; a device in MISMATCH is INITIALISING again, and
    // RUNNING only once a poll answers (D-145).
    SCADA_RT void identity_passed(std::uint32_t device) noexcept;

    // The identity check of an INITIALISING `device` failed with `refuse` at
    // `now_ns`: MISMATCH, its check due again after `backoff`, which does
    // not grow (DI 3.6).
    SCADA_RT void identity_mismatched(std::uint32_t device,
                                      std::int64_t now_ns) noexcept;

    // A restart of `device` was detected at `now_ns` (D-145): every RUNNING
    // device of its unit is INITIALISING again, with its identity check
    // pending and, when `rearm_grace`, its grace started again. A device
    // already INITIALISING, or in any other state, is left as it is.
    SCADA_RT void restarted(std::uint32_t device, std::int64_t now_ns,
                            bool rearm_grace) noexcept;

    // The identity check of `device` is pending: no verdict since the
    // connect, recovery or restart that started it.
    [[nodiscard]] bool identity_pending(std::uint32_t device) const noexcept;

    [[nodiscard]] DeviceStatus state(std::uint32_t device) const noexcept;
    // When a demoted device's next probe is due, or a MISMATCH device's
    // next identity check.
    [[nodiscard]] std::int64_t probe_due_ns(
        std::uint32_t device) const noexcept;
    // The consecutive failures counted for the device's unit id.
    [[nodiscard]] std::uint32_t failures(std::uint32_t device) const noexcept;
    // The device is polled: INITIALISING or RUNNING.
    [[nodiscard]] bool polled(std::uint32_t device) const noexcept;
    // The device is the demoted device that probes for its unit: the first
    // demoted one of the unit, in device order.
    [[nodiscard]] bool probes_for_unit(std::uint32_t device) const noexcept;

private:
    struct Run {
        DeviceStatus state = DeviceStatus::CONNECTING;
        std::int64_t grace_until_ns = 0;
        std::int64_t backoff_ns = 0;
        std::int64_t probe_due_ns = 0;
        bool demoted = false;  // kept across a lost connection
        // Recovered by a probe, and no poll answered since (review G6).
        bool probe_recovered = false;
        bool identity_pending = false;  // D-145
    };

    // Adds a failure of `device` outside its grace to its unit's count and
    // demotes every device of the unit whose limit it reaches; true when
    // one was.
    bool count_failure(std::uint32_t device, std::int64_t now_ns) noexcept;

    // INITIALISING, RUNNING or MISMATCH: a device whose failures count.
    [[nodiscard]] bool counted(std::uint32_t device) const noexcept;

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
