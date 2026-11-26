#ifndef SCADA_DRIVER_DIAGNOSTICS_H
#define SCADA_DRIVER_DIAGNOSTICS_H

// The counts behind the system tags of a channel and its devices (driver
// interface contract 11.1, decision D-144). The engine keeps them; the
// worker publishes them. Nothing here allocates.

#include "scada/common/realtime.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace scada::driver {

// `sys.drv.<channel>.state`: 0 disconnected, 1 connecting, 2 connected.
enum class ChannelLink : std::uint8_t {
    DISCONNECTED = 0,  // no connection, none being made (also the back-off)
    CONNECTING = 1,    // a connect is under way
    CONNECTED = 2,
};

// The counters of one device since attach (11.1, D-144).
struct DeviceCounts {
    std::uint64_t requests = 0;    // frames sent to it
    std::uint64_t errors = 0;      // answers that are errors (D-144)
    std::uint64_t timeouts = 0;    // attempts with no answer in time
    std::uint64_t exceptions = 0;  // error answers with a protocol code
    std::uint64_t refusals = 0;    // busy and gateway answers
    std::uint64_t sentinels = 0;   // NO_VALUE samples and markers
    std::uint64_t invalid = 0;     // DEVICE_INVALID samples, non-finite values
    std::int64_t last_ok_time_ns = 0;  // ns UTC of the last valid answer; 0
                                       // before the first
    std::int64_t last_status = 0;      // raw code of the last error answer
};

// The average time to the first byte of a device's last RESPONSE_SAMPLES
// answered requests (`response_ms`, 11.1).
class ResponseWindow {
public:
    static constexpr std::size_t RESPONSE_SAMPLES = 100;

    // One answered request, `duration_ns` from the send to its first byte;
    // a negative time counts as 0.
    SCADA_RT void add(std::int64_t duration_ns) noexcept;

    // The average in ms; 0 before the first answer.
    [[nodiscard]] SCADA_RT double average_ms() const noexcept;

private:
    std::array<std::int64_t, RESPONSE_SAMPLES> samples_{};
    std::size_t next_ = 0;
    std::size_t count_ = 0;
};

// Events over the last RATE_SECONDS whole seconds of the monotonic clock
// (`requests_per_s`, 11.1): one bucket per second, the current one
// included, so the rate is the count of the window over RATE_SECONDS.
class RateWindow {
public:
    static constexpr std::int64_t RATE_SECONDS = 10;

    SCADA_RT void add(std::int64_t now_ns) noexcept;

    // Events per second over the window that ends at `now_ns`.
    [[nodiscard]] SCADA_RT double per_second(std::int64_t now_ns) noexcept;

private:
    // Empties the buckets of the seconds that passed since the last call.
    SCADA_RT void advance(std::int64_t now_ns) noexcept;

    std::array<std::uint64_t, RATE_SECONDS> buckets_{};
    std::int64_t second_ = 0;  // the second of the newest bucket
    bool started_ = false;
};

}  // namespace scada::driver

#endif  // SCADA_DRIVER_DIAGNOSTICS_H
