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
// (7.6) come with slice 5b-2. The counts behind the system tags (11.1) are
// kept here and published by the worker (D-146).

#include "scada/common/realtime.h"
#include "scada/common/status.h"
#include "scada/driver/compiled.h"
#include "scada/driver/diagnostics.h"
#include "scada/driver/exchange.h"
#include "scada/driver/failure.h"
#include "scada/driver/identity.h"
#include "scada/driver/identity_run.h"
#include "scada/driver/protocol.h"
#include "scada/driver/sample_processing.h"
#include "scada/driver/scheduler.h"
#include "scada/driver/tag_value.h"
#include "scada/driver/transport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
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

    // `sys.drv.<channel>.state` (11.1) changed. It is called before a
    // connect is begun, so a sink that wants CONNECTING seen while the
    // connect blocks wakes its readers here. Called only from the exchange,
    // which is not real-time (DI 2.5). A sink without the tag ignores it.
    virtual void channel_link(ChannelLink /*link*/) noexcept {}
};

// Counts of the engine, for the diagnostics of 11.1 and for the tests.
struct EngineCounts {
    std::uint64_t requests = 0;      // frames sent
    std::uint64_t timeouts = 0;      // attempts with no answer in time
    std::uint64_t broken = 0;        // broken answers
    std::uint64_t retries = 0;       // attempts beyond the first
    std::uint64_t connects = 0;      // successful connects
    std::uint64_t refused = 0;       // connects the peer refused
    std::uint64_t losses = 0;        // connections lost (all devices failed)
    std::uint64_t own_closes = 0;    // closes the driver made itself (7.3)
    std::uint64_t resumed = 0;       // own closes followed by a quick reconnect
    std::uint64_t stale_frames = 0;  // frames of other requests dropped
    std::uint64_t chain_overflows = 0;  // chains too long for their buffer
    std::uint64_t jams = 0;   // serial lines never quiet before a send (G3)
    std::uint64_t rests = 0;  // dead blocks of live devices put to rest (G8)
    std::uint64_t early_verdicts = 0;  // COMM_FAIL before the retries (J2)
};

// An identity or restart event of a device, for the log (D-147): noted by
// the engine, handed to the main thread through the worker's event slot.
enum class DeviceEvent : std::uint8_t {
    MISMATCH = 0,  // a check failed (with `refuse` or `warn`)
    MATCH,         // a device in MISMATCH matched again
    RESTART,       // a restart of the device's unit was detected
};
inline constexpr std::size_t DEVICE_EVENTS = 3;

// The events of one kind since they were last taken: how many, and the last
// one's details.
struct EventRecord {
    std::uint64_t count = 0;
    std::int64_t time_ns = 0;  // the wall clock at the last one
    // MISMATCH: the MismatchReason and the first failing check.
    std::uint8_t reason = 0;
    std::uint32_t check = 0;
    // MISMATCH: the check's value; MATCH: the identity text.
    std::array<char, TAG_STRING_MAX_BYTES> text{};
    std::size_t length = 0;
};

class ChannelEngine {
public:
    // The answer to wait for when nothing is to be done until an order
    // arrives.
    static constexpr std::int64_t NEVER =
        std::numeric_limits<std::int64_t>::max();
    // How long quiet times may keep finding bytes before a send: then the
    // line counts as jammed, one is counted in `jams`, and requests are sent
    // again without waiting until a discard finds the line quiet (G3, J6).
    // 5 s at any bit rate and `turnaround`: longer than the longest answer
    // at 1,200 bit/s (255 bytes: 2.3 s).
    static constexpr std::int64_t JAM_NS = 5'000'000'000;

    // Allocates every buffer, sized for `channel`, which must outlive the
    // engine, as must `protocol`, `transport`, `clock` and `tags`. Every
    // enabled device starts CONNECTING, its points not initialised. A
    // channel the scheduler refuses (Scheduler::check), keys out of range (a
    // back-off or time-out not above 0, CS refuses them), or a device whose
    // protocol gives no frame size, gives an engine whose `status` is that
    // error and which never connects.
    ChannelEngine(const CompiledChannel& channel, const FrameProtocol& protocol,
                  Transport& transport, Clock& clock, TagSink& tags);
    // The same with every device's caps given, one per device of
    // `channel`, in order: `FrameProtocol::device_caps` runs on the main
    // thread (DI 2.3), and the worker builds its engine with what the main
    // thread asked. Fewer caps than devices gives an engine whose `status`
    // is INVALID_ARGUMENT.
    ChannelEngine(const CompiledChannel& channel, const FrameProtocol& protocol,
                  Transport& transport, Clock& clock, TagSink& tags,
                  std::span<const DeviceCaps> caps);

    // Takes `stored`, the value point `point` of device `device` holds in
    // the tag database after the writes of 9.1, as its last value
    // (seed_point_state, review M4). `substituted`: the stored value carries
    // SUBSTITUTED (review R1 of #157). Out of range is ignored. Called once,
    // before the first step.
    void seed_point(std::uint32_t device, std::uint32_t point,
                    const TagValue& stored, bool substituted = false) noexcept;

    [[nodiscard]] Status status() const noexcept { return status_; }

    // One transaction boundary: checks staleness, applies the rules of a
    // lost connection when a close's `connect_timeout` has passed, connects
    // when due, learns of a peer's close, or takes the next choice and runs
    // its exchange to the end. Returns the monotonic time of the next step:
    // now, a later time to wait for (pacing, a due group, the back-off, the
    // quiet time, a point's `max_age`), or NEVER. Never allocates. Not
    // real-time: the transport blocks up to its deadlines and makes system
    // calls (DI 2.5).
    std::int64_t step() noexcept;

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
    [[nodiscard]] ChannelLink link() const noexcept { return link_; }

    // The system tag counts of device `device` (11.1, D-146), an index of
    // the channel's devices; every count 0 for an index out of range.
    [[nodiscard]] const DeviceCounts& device_counts(
        std::uint32_t device) const noexcept;
    // `sys.dev.<device>.response_ms`; 0 for an index out of range.
    [[nodiscard]] double response_ms(std::uint32_t device) const noexcept;
    // `sys.dev.<device>.points_absent`: its points ABSENT now; 0 for an
    // index out of range.
    [[nodiscard]] std::uint64_t points_absent(
        std::uint32_t device) const noexcept;
    // `sys.dev.<device>.identity_ok` (3.6, 11.1; D-147): the last verdict
    // of the identity check, kept while a re-check is pending; none before
    // the first, nor for a device without checks or out of range.
    [[nodiscard]] std::optional<bool> identity_ok(
        std::uint32_t device) const noexcept;
    // `sys.dev.<device>.identity`: the values the last verdict's round read,
    // cleaned and joined (D-147); empty before the first verdict.
    [[nodiscard]] std::string_view identity_text(
        std::uint32_t device) const noexcept;
    // `sys.dev.<device>.restarts` (7.6, 11.1): the restarts detected since
    // the attach; 0 for an index out of range.
    [[nodiscard]] std::uint64_t restarts(std::uint32_t device) const noexcept;
    // The events of `kind` of `device` since the last take, into `out`, and
    // forgets them; false when there were none or the device is out of
    // range. The worker hands them to the main thread's log (D-147).
    SCADA_RT bool take_event(std::uint32_t device, DeviceEvent kind,
                             EventRecord& out) noexcept;
    // `sys.drv.<channel>.requests_per_s`: frames sent over the last 10 s.
    [[nodiscard]] double requests_per_s(std::int64_t now_ns) noexcept {
        return request_rate_.per_second(now_ns);
    }

private:
    // How one attempt ended.
    enum class End : std::uint8_t {
        ANSWER = 1,   // a complete, decoded answer: `decoded`
        TIMEOUT,      // no byte of its answer by the first deadline
        BROKEN,       // a broken frame, an overflow, or the answer deadline
        CLOSED,       // the peer closed the connection, or it failed
        PARTIAL,      // the frame was not taken whole
        NOT_SENT,     // encode refused the request
        UNREACHABLE,  // a gateway's answer that its unit did not respond
        ABANDONED,    // the wait was given up for a stop (DI 10, R-14)
    };

    // What the bytes received so far are.
    enum class Scan : std::uint8_t { MORE = 1, COMPLETE, BROKEN };

    struct PointRun {
        PointState state;
        bool polled = false;  // the point is in a poll group
    };

    // A planned request that keeps failing while its device answers others:
    // a dead block of a live device (review G8).
    struct RequestRest {
        std::uint8_t failures = 0;  // in a row, after retries
        bool resting = false;
        // The device's `answers` at this run's first failure.
        std::uint64_t answers_mark = 0;
        std::int64_t until_ns = 0;
        std::int64_t backoff_ns = 0;
    };

    // Members in the order that packs them best.
    struct DeviceRun {
        std::uint64_t answers = 0;  // its polls that answered
        std::size_t max_frame = 0;  // `DeviceCaps::max_frame_bytes`
        // The request whose first attempt timed out, retries pending: its
        // points are written COMM_FAIL at `verdict_ns` if they are still
        // pending then (review J2).
        std::int64_t verdict_ns = 0;
        std::vector<PointRun> points;
        std::vector<RequestRest> rests;  // per planned request
        // Samples of a chain of companions until its last request (5.5).
        std::vector<DeliveredSample> chain;
        std::uint32_t attempts = 0;  // failed attempts of the request
        std::uint32_t verdict_request = 0;
        bool any_resting = false;
        DeviceState protocol;     // the protocol's own state (2.2)
        DeviceCounts diag;        // its system tag counts (11.1)
        ResponseWindow response;  // its times to the first byte
        DeviceStatus published = DeviceStatus::DISABLED;
        bool verdict_pending = false;
        bool comm_failed = false;     // its points were written COMM_FAIL
        bool chain_overflow = false;  // the chain did not fit: not published
        // A part of the chain was busy: the rest of the chain is read, and
        // nothing of it published (P9).
        bool chain_cut = false;
        // The identity check (3.6) and the restart rules (7.6; D-147).
        std::optional<IdentityRun> identity;  // with `identify`
        std::optional<RestartWatch> restart;  // with `restart_indication`
        // Per point: the processing of identity and restart samples read in
        // an identity round, apart from the point's own state and tag (no
        // point tag is written while the check is pending, ruling item 2).
        std::vector<PointState> scratch;
        bool identity_round = false;      // a round is in progress
        bool identity_retrying = false;   // a retry of an identity read waits
        std::optional<bool> identity_ok;  // the last verdict
        std::array<char, TAG_STRING_MAX_BYTES> identity_text{};
        std::size_t identity_length = 0;
        std::uint64_t restarts = 0;
        // Per DeviceEvent: since the worker last took them.
        std::array<EventRecord, DEVICE_EVENTS> events{};
    };

    // The exchange and the connection: not real-time, since they reach the
    // transport (DI 2.5); the processing they call is.
    std::int64_t try_connect(std::int64_t now_ns) noexcept;
    void lose(std::int64_t now_ns) noexcept;
    void apply_loss() noexcept;
    void close_own(std::int64_t now_ns, bool lost) noexcept;
    void next_connect_after_backoff(std::int64_t now_ns) noexcept;
    End attempt(const Choice& choice, DecodeResult& decoded) noexcept;
    End wait_answer(const Request& request, const Attempt& sent,
                    std::size_t request_bytes, std::size_t expected_max,
                    std::int64_t timeout_ns, std::size_t& have,
                    std::size_t& frame_bytes) noexcept;
    void exchange(const Choice& choice) noexcept;
    void answered(const Choice& choice, const DecodeResult& decoded) noexcept;
    void failed(const Choice& choice, End end) noexcept;
    void after_bad_frame(const Request& request, End end) noexcept;
    // A poll request failed after its retries and was counted: it rests for
    // its own back-off when it failed `demote_after` times in a row while
    // its device answered other requests in between (G8).
    void rest_if_dead_block(const Choice& choice) noexcept;

    // A busy part cut the chain of `device`: only the COMM_FAIL samples of
    // its failed parts are kept, and written at once unless `more` parts
    // follow, which are read but not published (P9, K4).
    SCADA_RT void cut_chain(std::uint32_t device, bool more) noexcept;
    // Empties a device's chain: what it held is never published.
    SCADA_RT static void drop_chain(DeviceRun& run) noexcept;
    // Before a send after a quiet time: true when the line is quiet (or
    // jammed); false when bytes were found, with the quiet time restarted.
    bool settle(std::int64_t now_ns) noexcept;
    SCADA_RT void sync_scheduler() noexcept;
    // The link changed: kept and told to the sink. Not real-time.
    void set_link(ChannelLink link) noexcept;
    // Counts the answer `decoded` of device `device` (11.1, D-146).
    SCADA_RT void count_answer(DeviceRun& run,
                               const DecodeResult& decoded) noexcept;
    SCADA_RT void publish_states() noexcept;
    SCADA_RT void fail_points(std::uint32_t device) noexcept;
    // The points of `request` are written BAD with COMM_FAIL now.
    SCADA_RT void fail_points_of(std::uint32_t device,
                                 const Request& request) noexcept;
    // The points of the choice's request go bad with COMM_FAIL. In a chain
    // of companions they are part of the chain, so that its dependents see
    // their companion's outcome when it is processed (5.5, review G1).
    SCADA_RT void fail_request(const Choice& choice) noexcept;
    // Returns when a point next goes STALE, or NEVER.
    SCADA_RT std::int64_t check_staleness(std::int64_t now_ns) noexcept;
    // Writes COMM_FAIL to the points of a request whose retries still wait
    // for their turn when its verdict falls due (J2); returns the next such
    // time, or NEVER.
    SCADA_RT std::int64_t check_verdicts(std::int64_t now_ns) noexcept;
    SCADA_RT Scan scan(const Request& request, const Attempt& sent,
                       std::size_t& have, std::size_t& frame_bytes) noexcept;
    SCADA_RT void deliver(const Choice& choice,
                          std::span<const DeliveredSample> samples) noexcept;
    SCADA_RT void process(std::uint32_t device,
                          std::span<const DeliveredSample> samples) noexcept;
    // `samples` into `batch_` in the order `process` takes them: restart
    // rules' points, then points without companions, then the rest.
    SCADA_RT void order_batch(
        std::uint32_t device,
        std::span<const DeliveredSample> samples) noexcept;
    // The companions of `batch_[index]` into `companions_`, as the points
    // before it in the batch gave them.
    SCADA_RT void gather_companions(std::uint32_t device,
                                    std::size_t index) noexcept;
    // The identity check and restart rules of a device at construction:
    // their point check, the scratch states of an identity round, the run
    // and the watch (D-147).
    void set_up_identity(std::uint32_t device, std::int64_t now_ns);

    // The identity check and the restart rules (channel_engine_identity.cpp;
    // D-147). The mode the scheduler is to give `device` now.
    [[nodiscard]] SCADA_RT DeviceMode
    mode_of(std::uint32_t device) const noexcept;
    // A new round of `device`'s check, to end by the tracker's deadline of
    // its pending check (ruling item 2; identity_setup.h).
    SCADA_RT void start_identity_round(std::uint32_t device) noexcept;
    // An IDENTITY choice answered, or failed after its retries.
    void identity_answered(const Choice& choice,
                           const DecodeResult& decoded) noexcept;
    void identity_failed(const Choice& choice, End end) noexcept;
    // The samples of an identity read: those of identity and restart points
    // go to the check and the watch through `scratch`; the rest are
    // dropped. False when a restart fired: the rest of the answer is not
    // used.
    SCADA_RT bool identity_samples(
        std::uint32_t device,
        std::span<const DeliveredSample> samples) noexcept;
    // The verdict, once `device`'s identity cycle has no read left.
    void identity_cycle_done(std::uint32_t device) noexcept;
    // A valid sample of a restart rule's point; true when the device
    // restarted.
    SCADA_RT bool watch_restart(std::uint32_t device, std::uint32_t point,
                                const ProcessedSample& sample) noexcept;
    // A restart of `device`'s unit was detected (ruling item 6).
    void restarted(std::uint32_t device) noexcept;
    // Every point tag of `device` withheld with `flags` (withhold_point).
    SCADA_RT void withhold_points(std::uint32_t device, std::uint16_t flags,
                                  bool keep_not_init) noexcept;
    // Keeps the round's identity text as the device's (on a verdict).
    SCADA_RT void keep_identity_text(std::uint32_t device) noexcept;
    // Counts an event of `kind` of `device`, with `text` as its details.
    SCADA_RT void note_event(std::uint32_t device, DeviceEvent kind,
                             std::uint8_t reason, std::uint32_t check,
                             std::string_view text) noexcept;

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
    RateWindow request_rate_;
    ChannelLink link_ = ChannelLink::DISCONNECTED;
    // When the answer being waited for brought its first byte (11.1,
    // `response_ms`); set by `wait_answer`.
    std::int64_t first_byte_ns_ = 0;
    bool connected_ = false;
    // A close the driver made itself, and when (7.3, R-17): the rules of a
    // lost connection apply when `connect_timeout` passes without the
    // reconnect, whatever the connect does after.
    bool own_close_ = false;
    std::int64_t own_close_ns_ = 0;
    bool answered_since_connect_ = false;
    std::int64_t backoff_ns_ = 0;
    std::int64_t next_connect_ns_ = 0;
    std::int64_t quiet_until_ns_ = 0;
    // Input is discarded by `settle` at every step until the next send
    // (7.2: bytes that arrive during a quiet time are not the next answer).
    bool discard_before_send_ = false;
    // Quiet times keep finding bytes since `noisy_since_ns_`; `jammed_` once
    // that lasted JAM_NS.
    bool noisy_ = false;
    bool jammed_ = false;
    std::int64_t noisy_since_ns_ = 0;
    // Bytes a stream delivered after the last complete frame, kept at the
    // start of `rx_` for the next answer (Q12).
    std::size_t carry_ = 0;
};

}  // namespace scada::driver

#endif  // SCADA_DRIVER_CHANNEL_ENGINE_H
