#ifndef SCADA_DRIVER_CHANNEL_ENGINE_H
#define SCADA_DRIVER_CHANNEL_ENGINE_H

// The exchanges of one channel worker (driver interface contract 6.2, 7.1
// to 7.5): connects with the `reconnect` back-off, takes the scheduler's
// choices, sends each request over the transport and waits for its answer
// with the two-stage deadline of 7.1, retries, classifies the outcome,
// counts failures toward demotion, probes demoted devices, applies the
// closing rules of 7.3 and the quiet time of 7.2, and turns every answer
// into tag writes through the processing of contract 5 (process_sample),
// with STALE checked at every transaction boundary (7.5).
//
// Orders (writes, select and operate, READ_NOW) come with the command
// slices; this engine sends none yet. Identity checks and device restarts
// (7.6) come with slice 5.

#include "scada/common/realtime.h"
#include "scada/common/status.h"
#include "scada/driver/compiled.h"
#include "scada/driver/exchange.h"
#include "scada/driver/failure.h"
#include "scada/driver/protocol.h"
#include "scada/driver/sample_processing.h"
#include "scada/driver/scheduler.h"
#include "scada/driver/transport.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace scada::driver {

// Where the engine's tag writes go: the channel's tag owner (5.1, 3.5).
class TagSink {
public:
    TagSink() = default;
    TagSink(const TagSink&) = delete;
    TagSink& operator=(const TagSink&) = delete;
    TagSink(TagSink&&) = delete;
    TagSink& operator=(TagSink&&) = delete;
    virtual ~TagSink() = default;

    // Point `point` of `device` is written as `sample` says (its own tag
    // when `write_tag`, then its bits).
    SCADA_RT virtual void write_point(
        std::uint32_t device, std::uint32_t point,
        const ProcessedSample& sample) noexcept = 0;

    // `sys.dev.<device>.state` (3.5) changed.
    SCADA_RT virtual void device_state(std::uint32_t device,
                                       DeviceStatus state) noexcept = 0;
};

// Counts of the engine, for the diagnostics of 11.1 and for the tests.
struct EngineCounts {
    std::uint64_t requests = 0;      // frames sent
    std::uint64_t timeouts = 0;      // attempts with no first byte in time
    std::uint64_t broken = 0;        // broken answers
    std::uint64_t retries = 0;       // attempts beyond the first
    std::uint64_t connects = 0;      // successful connects
    std::uint64_t losses = 0;        // connections lost (all devices failed)
    std::uint64_t own_closes = 0;    // closes the driver made itself (7.3)
    std::uint64_t resumed = 0;       // own closes followed by a quick reconnect
    std::uint64_t stale_frames = 0;  // answers of earlier requests dropped
};

class ChannelEngine {
public:
    // The answer to wait for when nothing is to be done until an order
    // arrives.
    static constexpr std::int64_t NEVER =
        std::numeric_limits<std::int64_t>::max();

    // Allocates every buffer, sized for `channel`, which must outlive the
    // engine, as must `protocol`, `transport`, `clock` and `tags`. Every
    // enabled device starts CONNECTING, its points not initialised. A
    // channel the scheduler refuses (Scheduler::check), or a device whose
    // protocol gives no frame size, gives an engine whose `status` is that
    // error and which never connects.
    ChannelEngine(const CompiledChannel& channel, const FrameProtocol& protocol,
                  Transport& transport, Clock& clock, TagSink& tags);

    [[nodiscard]] Status status() const noexcept { return status_; }

    // One transaction boundary: checks staleness, connects when due, or
    // takes the next choice and runs its exchange to the end (the transport
    // waits). Returns the monotonic time of the next step: now, a later
    // time to wait for (pacing, a due group, the back-off, the quiet time,
    // a point's `max_age`), or NEVER. Never allocates.
    SCADA_RT std::int64_t step() noexcept;

    [[nodiscard]] const FailureTracker& failures() const noexcept {
        return failures_;
    }
    [[nodiscard]] const Scheduler& scheduler() const noexcept {
        return scheduler_;
    }
    [[nodiscard]] const EngineCounts& counts() const noexcept {
        return counts_;
    }
    [[nodiscard]] bool connected() const noexcept { return connected_; }

private:
    // How one attempt ended.
    enum class End : std::uint8_t {
        ANSWER = 1,   // a complete, decoded answer: `decoded`
        TIMEOUT,      // no first byte by the first deadline
        BROKEN,       // a broken frame, an overflow, or the answer deadline
        CLOSED,       // the peer closed the connection
        SEND_FAILED,  // the frame was not taken whole
        NOT_SENT,     // encode refused the request
    };

    // What the bytes received so far are.
    enum class Scan : std::uint8_t { MORE = 1, COMPLETE, BROKEN };

    struct PointRun {
        PointState state;
        bool polled = false;  // the point is in a poll group
    };

    struct DeviceRun {
        std::vector<PointRun> points;
        DeviceState protocol;       // the protocol's own state (2.2)
        std::size_t max_frame = 0;  // `DeviceCaps::max_frame_bytes`
        DeviceStatus published = DeviceStatus::DISABLED;
        std::uint32_t attempts = 0;  // failed attempts of the request
        std::uint32_t busy = 0;      // busy answers in a row
        bool comm_failed = false;    // its points were written COMM_FAIL
        // Samples of a chain of companions until its last request (5.5).
        std::vector<DeliveredSample> chain;
    };

    SCADA_RT std::int64_t try_connect(std::int64_t now_ns) noexcept;
    SCADA_RT void lose(std::int64_t now_ns) noexcept;
    SCADA_RT void close_own(std::int64_t now_ns) noexcept;
    SCADA_RT void sync_scheduler(std::int64_t now_ns) noexcept;
    SCADA_RT void publish_states() noexcept;
    SCADA_RT void fail_points(std::uint32_t device) noexcept;
    // Returns when a point next goes STALE, or NEVER.
    SCADA_RT std::int64_t check_staleness(std::int64_t now_ns) noexcept;

    SCADA_RT End attempt(const Choice& choice, DecodeResult& decoded) noexcept;
    SCADA_RT End wait_answer(const Request& request, const Attempt& sent,
                             std::size_t request_bytes,
                             std::size_t expected_max, std::int64_t timeout_ns,
                             std::size_t& frame_bytes) noexcept;
    SCADA_RT Scan scan(const Request& request, const Attempt& sent,
                       std::size_t& have, std::size_t& frame_bytes) noexcept;
    SCADA_RT void exchange(const Choice& choice) noexcept;
    SCADA_RT void answered(const Choice& choice,
                           const DecodeResult& decoded) noexcept;
    SCADA_RT void failed(const Choice& choice, End end) noexcept;
    SCADA_RT void after_bad_frame(const Request& request, End end) noexcept;
    SCADA_RT void deliver(const Choice& choice,
                          std::span<const DeliveredSample> samples) noexcept;
    SCADA_RT void process(std::uint32_t device,
                          std::span<const DeliveredSample> samples) noexcept;

    const CompiledChannel* channel_;
    const FrameProtocol* protocol_;
    Transport* transport_;
    Clock* clock_;
    TagSink* tags_;
    Status status_;
    Scheduler scheduler_;
    FailureTracker failures_;
    ChannelState channel_state_;
    std::vector<DeviceRun> devices_;
    std::vector<std::byte> tx_;
    std::vector<std::byte> rx_;
    std::vector<DeliveredSample> sink_storage_;
    std::size_t sink_count_ = 0;          // samples of the last decoded answer
    std::vector<DeliveredSample> batch_;  // samples processed together
    std::vector<CompanionInput> companions_;
    ProcessedSample processed_;
    EngineCounts counts_;
    bool connected_ = false;
    bool ever_connected_ = false;
    // A close the driver made itself, and when (7.3, R-17).
    bool own_close_ = false;
    std::int64_t own_close_ns_ = 0;
    bool answered_since_connect_ = false;
    std::int64_t backoff_ns_ = 0;
    std::int64_t next_connect_ns_ = 0;
    std::int64_t quiet_until_ns_ = 0;
};

}  // namespace scada::driver

#endif  // SCADA_DRIVER_CHANNEL_ENGINE_H
