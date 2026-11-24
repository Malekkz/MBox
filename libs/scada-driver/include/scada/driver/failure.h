#ifndef SCADA_DRIVER_FAILURE_H
#define SCADA_DRIVER_FAILURE_H

// Device states and failure accounting (driver interface contract 3.5, 7.4).
// A device is demoted after `demote_after` consecutive failed requests,
// counted across poll groups, once per request after its retries; devices
// that share a unit id share one count. A demoted device is probed every
// `backoff`, doubling to `backoff_max`; a probe that succeeds recovers it.
// Pure logic over an injected clock; the worker applies it at each
// transaction boundary.

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

// The failure keys of a device (CS 4.6), with their defaults.
struct FailureRules {
    std::int64_t timeout_ns = 1'000'000'000;
    std::uint8_t retries = 2;
    std::uint8_t busy_retries = 3;
    std::uint8_t demote_after = 3;
    std::int64_t backoff_ns = 10'000'000'000;
    std::int64_t backoff_max_ns = 300'000'000'000;
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
    };
    explicit FailureTracker(std::vector<Device> devices);

    // A connection was made at `now_ns`: every enabled device starts its
    // grace and is INITIALISING; counts start from 0 (7.4).
    void connected(std::int64_t now_ns) noexcept;

    // The connection is lost: every enabled device is CONNECTING (7.3).
    void lost() noexcept;

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

    // Index into units_ of each device.
    [[nodiscard]] std::uint32_t& count_of(std::uint32_t device) noexcept;
    [[nodiscard]] std::uint32_t count_of(std::uint32_t device) const noexcept;

    std::vector<Device> devices_;
    std::vector<Run> runs_;
    std::vector<std::uint32_t> unit_index_;  // per device
    std::vector<std::uint32_t> counts_;      // per distinct unit id
};

}  // namespace scada::driver

#endif  // SCADA_DRIVER_FAILURE_H
