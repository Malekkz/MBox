#include "scada/driver/channel_engine.h"

#include "scada/common/static_string.h"
#include "scada/common/status.h"
#include "scada/driver/compiled.h"
#include "scada/driver/diagnostics.h"
#include "scada/driver/exchange.h"
#include "scada/driver/failure.h"
#include "scada/driver/plan.h"
#include "scada/driver/point_rules.h"
#include "scada/driver/protocol.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/sample_processing.h"
#include "scada/driver/scheduler.h"
#include "scada/driver/tag_value.h"
#include "scada/driver/timing.h"
#include "scada/driver/transport.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace scada::driver {

namespace {

constexpr std::int64_t NEVER = std::numeric_limits<std::int64_t>::max();

// A probe waits for its device's timeout, but at most 1 s (7.4).
constexpr std::int64_t PROBE_TIMEOUT_MAX_NS = 1'000'000'000;

std::int64_t later(std::int64_t time, std::int64_t interval) noexcept {
    if (interval <= 0) {
        return time;
    }
    return time > NEVER - interval ? NEVER : time + interval;
}

Framing framing_of(TransportKind kind) noexcept {
    switch (kind) {
        case TransportKind::SERIAL:
            return Framing::SERIAL;
        case TransportKind::RTU_OVER_TCP:
            return Framing::SERIAL_OVER_STREAM;
        case TransportKind::TCP:
            break;
    }
    return Framing::STREAM;
}

std::vector<FailureTracker::Device> tracked_devices(
    const CompiledChannel& channel) {
    std::vector<FailureTracker::Device> out;
    out.reserve(channel.devices.size());
    for (const CompiledDevice& device : channel.devices) {
        const bool polls = std::any_of(
            device.plan.by_group.begin(), device.plan.by_group.end(),
            [](const std::vector<std::uint32_t>& group) {
                return !group.empty();
            });
        out.push_back({.unit_id = device.unit,
                       .own_unit = device.unit == CompiledDevice::OWN_UNIT,
                       .rules = device.failure,
                       .enabled = device.enabled,
                       .has_poll_group = polls});
    }
    return out;
}

std::uint32_t lost_after(const CompiledChannel& channel) noexcept {
    // Read time-outs close only a `tcp` connection (7.3).
    return channel.transport == TransportKind::TCP
               ? channel.connection.lost_after_timeouts
               : 0;
}

Status invalid(StaticString what) noexcept {
    return {StatusCode::INVALID_ARGUMENT, what};
}

// When a point next goes STALE, or a held value BAD (7.5, D-131), after
// `now_ns`; NEVER without `max_age`. The worker wakes for it even when
// idle. `check_stale` compares strictly: the first time past counts.
std::int64_t stale_due(const PointRules& rules, const PointState& state,
                       std::int64_t now_ns) noexcept {
    if (rules.max_age_ns <= 0) {
        return NEVER;
    }
    std::int64_t next = NEVER;
    const std::int64_t confirmed =
        later(state.confirmed_mono_ns, rules.max_age_ns + 1);
    if (confirmed > now_ns) {
        next = confirmed;
    }
    if (state.holding) {
        const std::int64_t held =
            later(state.valid_mono_ns, rules.max_age_ns + 1);
        if (held > now_ns) {
            next = std::min(next, held);
        }
    }
    return next;
}

// The compiled keys the engine relies on (CS refuses the rest; checked once
// here, Q13): a back-off and a deadline above 0, a sane line.
Status check_keys(const CompiledChannel& channel) noexcept {
    const ConnectionRules& connection = channel.connection;
    if (connection.reconnect_min_ns <= 0 ||
        connection.reconnect_max_ns < connection.reconnect_min_ns ||
        connection.connect_timeout_ns <= 0) {
        return invalid("reconnect or connect_timeout not above 0");
    }
    // CS: `baud` 300 to 4,000,000 (0 is a stream), `turnaround` 0 to 1 s.
    constexpr std::uint32_t MIN_BIT_RATE = 300;
    constexpr std::uint32_t MAX_BIT_RATE = 4'000'000;
    constexpr std::int64_t MAX_TURNAROUND_NS = 1'000'000'000;
    const LineTiming& line = channel.line;
    if (line.bits_per_char == 0 || line.turnaround_ns < 0 ||
        line.turnaround_ns > MAX_TURNAROUND_NS ||
        (line.bit_rate != 0 &&
         (line.bit_rate < MIN_BIT_RATE || line.bit_rate > MAX_BIT_RATE))) {
        return invalid(
            "a line without bits per character, a bit rate outside 300 to "
            "4,000,000, or a turnaround outside 0 to 1 s");
    }
    for (const CompiledDevice& device : channel.devices) {
        const FailureRules& rules = device.failure;
        if (rules.timeout_ns <= 0 || rules.write_timeout_ns <= 0 ||
            rules.backoff_ns <= 0 || rules.backoff_max_ns < rules.backoff_ns ||
            rules.startup_grace_ns < 0 || rules.demote_after == 0) {
            return invalid("a device's failure keys out of range");
        }
    }
    return {};
}

}  // namespace

namespace {

std::vector<DeviceCaps> caps_of(const CompiledChannel& channel,
                                const FrameProtocol& protocol) {
    std::vector<DeviceCaps> caps;
    caps.reserve(channel.devices.size());
    for (const CompiledDevice& device : channel.devices) {
        caps.push_back(protocol.device_caps(device));
    }
    return caps;
}

}  // namespace

ChannelEngine::ChannelEngine(const CompiledChannel& channel,
                             const FrameProtocol& protocol,
                             Transport& transport, Clock& clock, TagSink& tags)
    : ChannelEngine(channel, protocol, transport, clock, tags,
                    caps_of(channel, protocol)) {}

ChannelEngine::ChannelEngine(const CompiledChannel& channel,
                             const FrameProtocol& protocol,
                             Transport& transport, Clock& clock, TagSink& tags,
                             std::span<const DeviceCaps> caps)
    : channel_(&channel),
      protocol_(&protocol),
      transport_(&transport),
      clock_(&clock),
      tags_(&tags),
      scheduler_(channel, channel.max_pending_writes, clock.monotonic_ns()),
      failures_(tracked_devices(channel), lost_after(channel)),
      devices_(channel.devices.size()),
      backoff_ns_(channel.connection.reconnect_min_ns) {
    status_ = scheduler_.status();
    if (status_.ok()) {
        status_ = check_keys(channel);
    }
    if (status_.ok() && caps.size() < channel.devices.size()) {
        status_ = invalid("fewer device caps than devices");
    }
    const std::int64_t now = clock.monotonic_ns();
    std::size_t frame = 0;
    std::size_t request_points = 0;
    std::size_t device_points = 0;
    std::size_t companions = 0;
    for (std::size_t index = 0; index < channel.devices.size(); ++index) {
        const CompiledDevice& device = channel.devices[index];
        DeviceRun& run = devices_[index];
        run.max_frame = index < caps.size() ? caps[index].max_frame_bytes : 0;
        if (device.enabled && run.max_frame == 0 && status_.ok()) {
            status_ = invalid("a device's protocol gives no frame size");
        }
        frame = std::max(frame, run.max_frame);
        run.points.resize(device.points.size());
        for (std::size_t point = 0; point < device.points.size(); ++point) {
            const CompiledPoint& compiled = device.points[point];
            PointRun& point_run = run.points[point];
            init_point_state(point_run.state, compiled.rules.tag_type);
            reset_point_state(point_run.state, compiled.rules.tag_type, now);
            point_run.polled = compiled.poll_group.has_value();
            companions = std::max(companions, compiled.rules.companions.size());
        }
        for (const Request& request : device.plan.requests) {
            request_points = std::max(request_points, request.points.size());
        }
        device_points = std::max(device_points, device.points.size());
        run.chain.reserve(device.points.size());
        run.rests.resize(device.plan.requests.size());
        run.published =
            device.enabled ? DeviceStatus::CONNECTING : DeviceStatus::DISABLED;
        tags.device_state(static_cast<std::uint32_t>(index), run.published);
    }
    tx_.resize(frame);
    rx_.resize(frame);
    sink_storage_.resize(std::max(request_points, device_points));
    batch_.reserve(device_points);
    companions_.reserve(companions);
}

void ChannelEngine::seed_point(std::uint32_t device, std::uint32_t point,
                               const TagValue& stored,
                               bool substituted) noexcept {
    if (device < devices_.size() && point < devices_[device].points.size()) {
        seed_point_state(devices_[device].points[point].state, stored,
                         substituted);
    }
}

std::int64_t ChannelEngine::step() noexcept {
    const std::int64_t now = clock_->monotonic_ns();
    const std::int64_t stale_due =
        std::min(check_staleness(now), check_verdicts(now));
    if (!status_.ok()) {
        return NEVER;
    }
    const std::int64_t own_deadline =
        later(own_close_ns_, channel_->connection.connect_timeout_ns);
    if (!connected_ && own_close_ && now > own_deadline) {
        // A close of our own not followed by a reconnect within
        // `connect_timeout`: the rules of a lost connection apply now,
        // whatever the connect does after (7.3, R-17; review P1).
        apply_loss();
    }
    if (!connected_) {
        std::int64_t next = std::min(try_connect(now), stale_due);
        if (own_close_) {
            next = std::min(next, later(own_deadline, 1));
        }
        return next;
    }
    if (transport_->closed_by_peer()) {
        lose(now);  // an idle close (7.3)
        return std::min(next_connect_ns_, stale_due);
    }
    sync_scheduler();
    if (now < quiet_until_ns_) {
        return quiet_until_ns_;  // a serial line's silence (7.2)
    }
    if (discard_before_send_ && !settle(now)) {
        return quiet_until_ns_;  // bytes came during the quiet time (G3)
    }
    const Choice choice = scheduler_.take(now);
    switch (choice.kind) {
        case Choice::Kind::IDLE:
            return stale_due;
        case Choice::Kind::WAIT:
            return std::min(choice.wake_ns, stale_due);
        case Choice::Kind::ORDER:
            // Orders come with the command slices; none is added yet.
            (void)scheduler_.not_sent();
            return now;
        case Choice::Kind::POLL:
        case Choice::Kind::KEEPALIVE:
        case Choice::Kind::PROBE:
            break;
    }
    exchange(choice);
    publish_states();
    return clock_->monotonic_ns();
}

bool ChannelEngine::settle(std::int64_t now_ns) noexcept {
    if (!transport_->discard_input()) {
        noisy_ = false;
        jammed_ = false;
        return true;
    }
    if (!noisy_) {
        noisy_ = true;
        noisy_since_ns_ = now_ns;
    }
    if (jammed_) {
        return true;  // jammed: counted once, sent all the same
    }
    if (now_ns - noisy_since_ns_ >= JAM_NS) {
        jammed_ = true;
        ++counts_.jams;
        return true;
    }
    quiet_until_ns_ = later(now_ns, quiet_time_ns(channel_->line));
    return false;
}

void ChannelEngine::next_connect_after_backoff(std::int64_t now_ns) noexcept {
    const ConnectionRules& rules = channel_->connection;
    next_connect_ns_ = later(now_ns, backoff_ns_);
    backoff_ns_ = std::min(backoff_ns_ > rules.reconnect_max_ns / 2
                               ? rules.reconnect_max_ns
                               : backoff_ns_ * 2,
                           rules.reconnect_max_ns);
}

std::int64_t ChannelEngine::try_connect(std::int64_t now_ns) noexcept {
    if (now_ns < next_connect_ns_) {
        return next_connect_ns_;
    }
    const ConnectionRules& rules = channel_->connection;
    set_link(ChannelLink::CONNECTING);
    const IoStatus status =
        transport_->connect(later(now_ns, rules.connect_timeout_ns));
    const std::int64_t now = clock_->monotonic_ns();
    if (status == IoStatus::ABANDONED) {
        // A stop gave up the connect: no failure, no back-off (DI 10).
        return now;
    }
    if (status == IoStatus::OK) {
        if (own_close_ &&
            now <= later(own_close_ns_, rules.connect_timeout_ns)) {
            // Back within `connect_timeout` of a close of our own: states,
            // counts and tags are kept (7.3, R-17).
            failures_.resumed();
            ++counts_.resumed;
        } else {
            if (own_close_) {
                apply_loss();  // back, but too late (review P1)
            }
            failures_.connected(now);  // keeps counts and demotion
        }
        own_close_ = false;
        connected_ = true;
        set_link(ChannelLink::CONNECTED);
        ++counts_.connects;
        channel_state_.reset();
        answered_since_connect_ = false;
        quiet_until_ns_ = 0;
        discard_before_send_ = false;
        noisy_ = false;
        jammed_ = false;
        carry_ = 0;
        for (DeviceRun& run : devices_) {
            drop_chain(run);  // a chain never spans a connect (P9)
            // `reconnect` ends every rest: dead blocks are tried again.
            std::fill(run.rests.begin(), run.rests.end(), RequestRest{});
            run.any_resting = false;
        }
        scheduler_.reconnect();
        sync_scheduler();
        publish_states();
        return now;
    }
    set_link(ChannelLink::DISCONNECTED);
    if (status == IoStatus::REFUSED) {
        ++counts_.refused;
    }
    if (!own_close_) {
        // CONNECTING with COMM_FAIL (conformance test 16).
        for (std::uint32_t device = 0; device < devices_.size(); ++device) {
            if (channel_->devices[device].enabled) {
                fail_points(device);
            }
        }
    }
    next_connect_after_backoff(now);
    publish_states();
    return next_connect_ns_;
}

void ChannelEngine::apply_loss() noexcept {
    own_close_ = false;
    ++counts_.losses;
    failures_.lost();
    scheduler_.reconnect();
    for (DeviceRun& run : devices_) {
        drop_chain(run);
        run.attempts = 0;
        run.verdict_pending = false;
    }
    // Every device is CONNECTING: `publish_states` writes their points BAD
    // with COMM_FAIL, all at once (conformance test 16).
    publish_states();
}

void ChannelEngine::lose(std::int64_t now_ns) noexcept {
    transport_->close(false);
    connected_ = false;
    set_link(ChannelLink::DISCONNECTED);
    carry_ = 0;
    apply_loss();
    next_connect_after_backoff(now_ns);
}

void ChannelEngine::close_own(std::int64_t now_ns, bool lost) noexcept {
    transport_->close(true);
    connected_ = false;
    set_link(ChannelLink::DISCONNECTED);
    carry_ = 0;
    ++counts_.own_closes;
    for (DeviceRun& run : devices_) {
        drop_chain(run);  // a chain cut by the close publishes nothing
        run.attempts = 0;
        run.verdict_pending = false;
    }
    if (lost) {
        // The run of time-outs found the connection lost (7.3, R-15): its
        // rules apply at once, and the values go bad (review P1, D-139).
        apply_loss();
    } else {
        own_close_ = true;
        own_close_ns_ = now_ns;
    }
    // At once, but a second close before any valid answer waits for the
    // back-off (7.3).
    if (answered_since_connect_) {
        next_connect_ns_ = now_ns;
        return;
    }
    next_connect_after_backoff(now_ns);
}

void ChannelEngine::sync_scheduler() noexcept {
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        const CompiledDevice& compiled = channel_->devices[device];
        if (!compiled.enabled) {
            continue;
        }
        scheduler_.set_polled(device, failures_.polled(device));
        DeviceRun& run = devices_[device];
        if (run.any_resting) {
            // A resting block is polled again at the end of its back-off;
            // a further failure rests it again, for twice as long (G8).
            const std::int64_t now = clock_->monotonic_ns();
            run.any_resting = false;
            for (std::uint32_t request = 0; request < run.rests.size();
                 ++request) {
                RequestRest& rest = run.rests[request];
                if (rest.resting && now >= rest.until_ns) {
                    rest.resting = false;
                    scheduler_.rest(device, request, false);
                }
                run.any_resting = run.any_resting || rest.resting;
            }
        }
        // One probe for every demoted device of a unit (Q5).
        if (failures_.probes_for_unit(device)) {
            scheduler_.set_probe(
                device, failures_.probe_due_ns(device),
                std::min(compiled.failure.timeout_ns, PROBE_TIMEOUT_MAX_NS));
        } else {
            scheduler_.clear_probe(device);
        }
    }
}

void ChannelEngine::cut_chain(std::uint32_t device, bool more) noexcept {
    DeviceRun& run = devices_[device];
    // Only the COMM_FAIL samples of failed parts are kept, in place and
    // without allocation; nothing a part read is published (P9, K4).
    const auto failed_end = std::partition(
        run.chain.begin(), run.chain.end(), [](const DeliveredSample& sample) {
            return sample.sample.outcome == Outcome::COMM_FAIL;
        });
    run.chain.erase(failed_end, run.chain.end());
    if (more) {
        run.chain_cut = true;  // the parts that follow are read, not published
        return;
    }
    process(device, run.chain);
    drop_chain(run);
}

void ChannelEngine::drop_chain(DeviceRun& run) noexcept {
    run.chain.clear();
    run.chain_overflow = false;
    run.chain_cut = false;
}

void ChannelEngine::publish_states() noexcept {
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        DeviceRun& run = devices_[device];
        const DeviceStatus state = failures_.state(device);
        if (state == run.published) {
            continue;
        }
        run.published = state;
        tags_->device_state(device, state);
        if (state == DeviceStatus::DEMOTED ||
            state == DeviceStatus::CONNECTING) {
            // What a chain held from before is never published (P9).
            drop_chain(run);
            fail_points(device);  // BAD with COMM_FAIL (7.4, 7.3)
        }
    }
}

void ChannelEngine::fail_points(std::uint32_t device) noexcept {
    DeviceRun& run = devices_[device];
    if (run.comm_failed) {
        return;
    }
    run.comm_failed = true;
    const CompiledDevice& compiled = channel_->devices[device];
    const SampleTimes times{.receive_time_ns = clock_->realtime_ns(),
                            .receive_mono_ns = clock_->monotonic_ns()};
    const RawSample failure{.outcome = Outcome::COMM_FAIL};
    for (std::uint32_t point = 0; point < run.points.size(); ++point) {
        if (!run.points[point].polled) {
            continue;
        }
        process_sample(compiled.points[point].rules, run.points[point].state,
                       failure, {}, times, processed_);
        tags_->write_point(device, point, processed_);
    }
}

void ChannelEngine::fail_request(const Choice& choice) noexcept {
    // The points of a request that failed go bad (5.2, annex A; review P2),
    // and in a chain the points that depend on them too (5.5, review G1).
    const Request& request =
        channel_->devices[choice.device].plan.requests[choice.request];
    sink_count_ = 0;
    for (const std::uint32_t point : request.points) {
        if (sink_count_ < sink_storage_.size()) {
            sink_storage_[sink_count_++] = DeliveredSample{
                .point = point,
                .sample = RawSample{.outcome = Outcome::COMM_FAIL}};
        }
    }
    deliver(choice, std::span(sink_storage_).first(sink_count_));
}

std::int64_t ChannelEngine::check_verdicts(std::int64_t now_ns) noexcept {
    std::int64_t next = NEVER;
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        DeviceRun& run = devices_[device];
        if (!run.verdict_pending) {
            continue;
        }
        if (now_ns < run.verdict_ns) {
            next = std::min(next, run.verdict_ns);
            continue;
        }
        // Retries still waiting for their turn: the points go bad now, and the
        // retries go on as attempts to recover (J2). Demotion still counts the
        // request once, when its attempts end.
        run.verdict_pending = false;
        fail_points_of(
            device,
            channel_->devices[device].plan.requests[run.verdict_request]);
        ++counts_.early_verdicts;
    }
    return next;
}

std::int64_t ChannelEngine::check_staleness(std::int64_t now_ns) noexcept {
    std::int64_t next = NEVER;
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        const CompiledDevice& compiled = channel_->devices[device];
        if (!compiled.enabled) {
            continue;
        }
        DeviceRun& run = devices_[device];
        for (std::uint32_t point = 0; point < run.points.size(); ++point) {
            const PointRules& rules = compiled.points[point].rules;
            PointState& state = run.points[point].state;
            if (!run.points[point].polled) {
                continue;
            }
            if (check_stale(rules, state, now_ns, processed_)) {
                tags_->write_point(device, point, processed_);
            }
            next = std::min(next, stale_due(rules, state, now_ns));
        }
    }
    return next;
}

ChannelEngine::End ChannelEngine::attempt(const Choice& choice,
                                          DecodeResult& decoded) noexcept {
    const CompiledDevice& device = channel_->devices[choice.device];
    DeviceRun& run = devices_[choice.device];
    const Request& request = device.plan.requests[choice.request];
    Attempt sent{.framing = framing_of(channel_->transport)};
    TxBuffer out{.bytes = std::span(tx_).first(run.max_frame)};
    const EncodeResult encoded =
        protocol_->encode(device, channel_state_, request, sent, out);
    if (encoded.kind != EncodeResult::Kind::OK ||
        encoded.length > out.bytes.size()) {
        return End::NOT_SENT;
    }
    // `settle` dropped what came during the quiet time in this same step,
    // just before this send (7.2, G3).
    discard_before_send_ = false;
    if (transport_->closed_by_peer()) {
        return End::CLOSED;  // checked before a write (7.3)
    }
    const std::int64_t sent_ns = clock_->monotonic_ns();
    const IoStatus status =
        transport_->send(std::span(tx_).first(encoded.length));
    ++counts_.requests;
    ++run.diag.requests;
    request_rate_.add(sent_ns);
    if (status == IoStatus::PARTIAL) {
        return End::PARTIAL;
    }
    if (status != IoStatus::OK) {
        return End::CLOSED;  // nothing sent: the connection failed
    }
    std::int64_t timeout = device.failure.timeout_ns;
    if (choice.kind == Choice::Kind::PROBE) {
        // At most 1 s (7.4), and ended when the next poll falls due (G7).
        timeout = std::min(timeout, PROBE_TIMEOUT_MAX_NS);
        if (choice.timeout_ns > 0) {
            timeout = std::min(timeout, choice.timeout_ns);
        }
    }
    std::size_t have = 0;
    std::size_t frame_bytes = 0;
    const End end =
        wait_answer(request, sent, encoded.length, encoded.expected_max,
                    timeout, have, frame_bytes);
    if (end != End::ANSWER) {
        return end;
    }
    // A complete frame: its time to the first byte (`response_ms`). Bytes
    // carried from before the send count as 0.
    run.response.add(first_byte_ns_ - sent_ns);
    if (channel_->transport == TransportKind::SERIAL) {
        // The silence before the next request, from the last byte (7.2).
        quiet_until_ns_ =
            later(clock_->monotonic_ns(), inter_frame_gap_ns(channel_->line));
    }
    SampleSink sink(sink_storage_);
    decoded = protocol_->decode(device, run.protocol, request, sent,
                                std::span(rx_).first(frame_bytes), sink);
    sink_count_ = sink.samples().size();
    count_answer(run, decoded);
    // Bytes after the frame: on a stream the start of the next one, kept;
    // on a serial line noise, discarded by the quiet-time rule (Q12).
    if (have > frame_bytes && channel_->transport != TransportKind::SERIAL) {
        std::copy(rx_.begin() + static_cast<std::ptrdiff_t>(frame_bytes),
                  rx_.begin() + static_cast<std::ptrdiff_t>(have), rx_.begin());
        carry_ = have - frame_bytes;
    } else if (have > frame_bytes) {
        discard_before_send_ = true;
    }
    if (decoded.kind == DecodeResult::Kind::BROKEN) {
        return End::BROKEN;
    }
    return decoded.kind == DecodeResult::Kind::UNREACHABLE ? End::UNREACHABLE
                                                           : End::ANSWER;
}

ChannelEngine::End ChannelEngine::wait_answer(
    const Request& request, const Attempt& sent, std::size_t request_bytes,
    std::size_t expected_max, std::int64_t timeout_ns, std::size_t& have,
    std::size_t& frame_bytes) noexcept {
    const LineTiming& line = channel_->line;
    const std::int64_t first_deadline = first_byte_deadline_ns(
        line, clock_->monotonic_ns(), request_bytes, timeout_ns);
    // The latest the answer may end: begun at the last instant of the first
    // stage. A stale frame never moves the wait past it (P3).
    const std::int64_t latest =
        answer_deadline_ns(line, first_deadline, expected_max);
    std::int64_t deadline = first_deadline;
    bool started = false;  // bytes that may start this request's answer
    have = carry_;  // bytes a stream delivered after the last frame (Q12)
    carry_ = 0;
    first_byte_ns_ = clock_->monotonic_ns();
    for (;;) {
        if (have > 0) {
            switch (scan(request, sent, have, frame_bytes)) {
                case Scan::COMPLETE:
                    return End::ANSWER;
                case Scan::BROKEN:
                    return End::BROKEN;
                case Scan::MORE:
                    break;
            }
        }
        // Only bytes left after stale frames are dropped start the second
        // stage; when all were stale, the first deadline holds again.
        if (have == 0) {
            started = false;
            deadline = first_deadline;
        } else if (!started) {
            started = true;
            deadline = std::min(
                answer_deadline_ns(line, clock_->monotonic_ns(), expected_max),
                latest);
        }
        if (have >= rx_.size()) {
            return End::BROKEN;  // more than `max_frame_bytes` (7.1)
        }
        std::size_t got = 0;
        const IoStatus status =
            transport_->receive(std::span(rx_).subspan(have), deadline, got);
        if (status == IoStatus::ABANDONED) {
            return End::ABANDONED;
        }
        if (status == IoStatus::TIMEOUT) {
            return started ? End::BROKEN : End::TIMEOUT;
        }
        if (status != IoStatus::OK || got == 0) {
            // Closed or failed; OK with nothing read is a transport fault,
            // taken as a failed connection (Q8, Q14).
            return End::CLOSED;
        }
        if (have == 0) {
            // The first byte of this answer, stale frames dropped (11.1,
            // `response_ms`).
            first_byte_ns_ = clock_->monotonic_ns();
        }
        have += got;
    }
}

ChannelEngine::Scan ChannelEngine::scan(const Request& request,
                                        const Attempt& sent, std::size_t& have,
                                        std::size_t& frame_bytes) noexcept {
    while (have > 0) {
        const FrameState state =
            protocol_->frame_state(request, sent, std::span(rx_).first(have));
        if (state.kind == FrameState::Kind::NEED_MORE) {
            return Scan::MORE;
        }
        if (state.kind == FrameState::Kind::BROKEN || state.length == 0 ||
            state.length > have) {
            return Scan::BROKEN;
        }
        if (state.kind == FrameState::Kind::COMPLETE) {
            frame_bytes = state.length;
            return Scan::COMPLETE;
        }
        // STALE: an answer to an earlier request; drop it and wait on to the
        // same deadline (7.2, 7.3).
        ++counts_.stale_frames;
        std::copy(rx_.begin() + static_cast<std::ptrdiff_t>(state.length),
                  rx_.begin() + static_cast<std::ptrdiff_t>(have), rx_.begin());
        have -= state.length;
    }
    return Scan::MORE;
}

void ChannelEngine::exchange(const Choice& choice) noexcept {
    if (choice.chain_dropped) {
        drop_chain(devices_[choice.chain_device]);  // publishes nothing
    }
    DecodeResult decoded;
    const End end = attempt(choice, decoded);
    const std::int64_t now = clock_->monotonic_ns();
    switch (end) {
        case End::NOT_SENT:
            // The protocol cannot encode it: the request ends failed
            // without I/O, and the engine moves on (Q11).
            if (choice.kind == Choice::Kind::PROBE) {
                failures_.probe_ended(choice.device, false, now);
                scheduler_.probe_done();
            } else {
                fail_request(choice);
            }
            return;
        case End::CLOSED:
            // Every device of the channel at once, no retries (7.3).
            lose(now);
            return;
        case End::ABANDONED:
            // A stop gave up the read in flight: nothing is counted and
            // nothing written, its points keep their values (DI 9.2, 10,
            // R-14). The worker stops at this boundary.
            return;
        case End::PARTIAL:
            if (channel_->transport == TransportKind::SERIAL) {
                failed(choice, End::TIMEOUT);  // as a time-out (7.2, Q7)
            } else {
                close_own(now, false);  // a frame not taken whole (7.3)
            }
            return;
        case End::ANSWER:
            answered(choice, decoded);
            return;
        case End::TIMEOUT:
        case End::BROKEN:
        case End::UNREACHABLE:
            failed(choice, end);
            return;
    }
}

void ChannelEngine::answered(const Choice& choice,
                             const DecodeResult& decoded) noexcept {
    const std::uint32_t device = choice.device;
    DeviceRun& run = devices_[device];
    const CompiledDevice& compiled = channel_->devices[device];
    const Request& request = compiled.plan.requests[choice.request];
    const std::int64_t now = clock_->monotonic_ns();
    run.attempts = 0;
    run.verdict_pending = false;
    if (choice.kind == Choice::Kind::PROBE) {
        // The device answered, busy or not: it is alive (7.4). A probe
        // neither adds to the run of time-outs nor empties it, nor resets
        // the reconnect back-off (7.3; the lead's ruling R10).
        failures_.probe_ended(device, true, now);
        scheduler_.probe_done();
        if (choice.cycle && decoded.kind == DecodeResult::Kind::BUSY) {
            // A busy part cuts its chain, as a poll's does (H4).
            cut_chain(device, request.chained);
        } else if (choice.cycle) {
            // A whole poll cycle as a probe (R8): published as a poll.
            deliver(choice, std::span(sink_storage_).first(sink_count_));
        }
        return;
    }
    // A valid answer to a poll: the reconnect back-off starts from `min`
    // again, and the run of time-outs is empty (7.3).
    answered_since_connect_ = true;
    backoff_ns_ = channel_->connection.reconnect_min_ns;
    failures_.answer_seen();
    if (decoded.kind == DecodeResult::Kind::BUSY) {
        // Busy never counts and is never retried (7.4, annex A, test 19;
        // the lead's ruling on D-12): the points keep their values, STALE
        // by `max_age`.
        (void)failures_.request_ended(device, RequestEnd::NOT_COUNTED, now);
        // A busy part cuts its chain: nothing it read is published, and the
        // parts that follow are read but not published (P9); its failed
        // parts still go bad (K4).
        cut_chain(device, request.chained);
        return;
    }
    (void)failures_.request_ended(device, RequestEnd::ANSWERED, now);
    if (choice.kind == Choice::Kind::POLL) {
        run.rests[choice.request] = RequestRest{};  // its block answers (G8)
        ++run.answers;
    }
    const bool refused = decoded.kind == DecodeResult::Kind::SPLIT;
    if ((refused && !scheduler_.refused(device, choice.request)) ||
        decoded.kind == DecodeResult::Kind::ABSENT) {
        // Not on this device: its points are ABSENT (5.2, 6.3).
        sink_count_ = 0;
        for (const std::uint32_t point : request.points) {
            if (sink_count_ < sink_storage_.size()) {
                sink_storage_[sink_count_++] = DeliveredSample{
                    .point = point,
                    .sample = RawSample{.outcome = Outcome::ABSENT}};
            }
        }
    } else if (refused) {
        return;  // its split form follows
    }
    deliver(choice, std::span(sink_storage_).first(sink_count_));
}

void ChannelEngine::after_bad_frame(const Request& request, End end) noexcept {
    if (end == End::UNREACHABLE) {
        return;  // a valid frame: only its unit failed
    }
    const std::int64_t now = clock_->monotonic_ns();
    if (channel_->transport == TransportKind::TCP) {
        // A broken frame on a stream, or a lost side-effect read, closes
        // the connection (7.3).
        if (end == End::BROKEN || request.side_effect) {
            close_own(now, false);
        }
        return;
    }
    // A serial line, or serial frames over a stream: discard input and keep
    // the quiet time before the next request to any device, and discard
    // again before that request (7.2, 7.3; review P6).
    (void)transport_->discard_input();
    discard_before_send_ = true;
    quiet_until_ns_ = later(now, quiet_time_ns(channel_->line));
}

void ChannelEngine::failed(const Choice& choice, End end) noexcept {
    const std::uint32_t device = choice.device;
    DeviceRun& run = devices_[device];
    const CompiledDevice& compiled = channel_->devices[device];
    const Request& request = compiled.plan.requests[choice.request];
    const bool probe = choice.kind == Choice::Kind::PROBE;
    if (end == End::BROKEN) {
        ++counts_.broken;
        ++run.diag.errors;  // a broken answer (D-146)
    } else {
        ++counts_.timeouts;
        if (end == End::TIMEOUT) {
            ++run.diag.timeouts;
        }
    }
    after_bad_frame(request, end);
    const std::int64_t now = clock_->monotonic_ns();
    // The lead's ruling on D-12: a time-out is retried, and on a serial
    // line a broken frame; an exception answer never (a gateway has waited
    // already); never a side-effect read, a probe, or over a closed
    // connection (7.1, 6.6).
    const bool retryable =
        end == End::TIMEOUT ||
        (end == End::BROKEN && channel_->transport != TransportKind::TCP);
    if (retryable && connected_ && !probe && !request.side_effect &&
        run.attempts < compiled.failure.retries) {
        if (run.attempts == 0 && (choice.kind == Choice::Kind::POLL ||
                                  choice.kind == Choice::Kind::KEEPALIVE)) {
            // The verdict does not wait for retries that wait for their
            // turn: one period of the group, or the retries' time-outs if
            // longer (J2), whether the first attempt was a poll or a
            // keep-alive (review M1).
            const std::int64_t period =
                channel_->poll_groups[request.poll_group].period_ns;
            run.verdict_pending = true;
            run.verdict_request = choice.request;
            run.verdict_ns =
                later(now, std::max(period, compiled.failure.timeout_ns *
                                                static_cast<std::int64_t>(
                                                    compiled.failure.retries)));
        }
        ++run.attempts;
        ++counts_.retries;
        scheduler_.retry(device, choice.request, now);
        return;
    }
    run.attempts = 0;
    run.verdict_pending = false;
    if (probe) {
        // A probe cut short to a window and not answered in it is no failed
        // probe: the device may only be slower than the window. The due
        // time stands, and the floor probe with the whole time-out follows
        // (review J1).
        if (!choice.windowed || end != End::TIMEOUT) {
            failures_.probe_ended(device, false, now);
        }
        scheduler_.probe_done();
        drop_chain(run);
        return;
    }
    // A failed part of a chain is part of the chain: the points that depend
    // on it go bad through their companions' outcome when the chain's last
    // request is in (5.5, test 8; G1). Its own points go bad now, not at the
    // chain's end (J2: the verdict does not wait for the other parts).
    if (request.chained) {
        fail_points_of(device, request);
    }
    fail_request(choice);
    if (end == End::BROKEN) {
        (void)failures_.request_ended(device, RequestEnd::FAILED, now);
        rest_if_dead_block(choice);
        return;
    }
    if (end == End::UNREACHABLE) {
        // The lead's revised ruling on D-12: the gateway's "did not
        // respond" counts toward its unit's demotion, without a retry, but
        // is a valid frame from the gateway: it proves the connection and
        // empties the run of time-outs like any valid answer (7.3).
        answered_since_connect_ = true;
        backoff_ns_ = channel_->connection.reconnect_min_ns;
        failures_.answer_seen();
        (void)failures_.request_ended(device, RequestEnd::FAILED, now);
        rest_if_dead_block(choice);
        return;
    }
    // A time-out counts toward demotion and adds to the run (7.3, 7.4).
    const FailureTracker::TimeoutEnd timed_out =
        failures_.read_timed_out(device, now);
    if (timed_out.lost && connected_) {
        close_own(now, true);  // the run spans every live unit (7.3)
        return;
    }
    rest_if_dead_block(choice);
}

void ChannelEngine::rest_if_dead_block(const Choice& choice) noexcept {
    if (choice.kind != Choice::Kind::POLL) {
        return;
    }
    const std::uint32_t device = choice.device;
    DeviceRun& run = devices_[device];
    const FailureRules& rules = channel_->devices[device].failure;
    RequestRest& rest = run.rests[choice.request];
    if (rest.failures == 0) {
        rest.answers_mark = run.answers;
    }
    if (rest.failures < UINT8_MAX) {
        ++rest.failures;
    }
    // The device answered other requests since this one began to fail: a
    // dead block of a live device, which demotion never ends (G8).
    // A part of a chain never rests: the chain is read whole every period,
    // and with J2 its points go bad on time (the lead's ruling K1).
    if (rest.failures < rules.demote_after ||
        run.answers == rest.answers_mark || !failures_.polled(device) ||
        scheduler_.in_chain(device, choice.request)) {
        return;
    }
    const std::int64_t now = clock_->monotonic_ns();
    rest.backoff_ns = rest.backoff_ns == 0
                          ? rules.backoff_ns
                          : std::min(rest.backoff_ns > rules.backoff_max_ns / 2
                                         ? rules.backoff_max_ns
                                         : rest.backoff_ns * 2,
                                     rules.backoff_max_ns);
    rest.until_ns = later(now, rest.backoff_ns);
    rest.resting = true;
    run.any_resting = true;
    scheduler_.rest(device, choice.request, true);
    ++counts_.rests;
}

void ChannelEngine::fail_points_of(std::uint32_t device,
                                   const Request& request) noexcept {
    sink_count_ = 0;
    for (const std::uint32_t point : request.points) {
        if (sink_count_ < sink_storage_.size()) {
            sink_storage_[sink_count_++] = DeliveredSample{
                .point = point,
                .sample = RawSample{.outcome = Outcome::COMM_FAIL}};
        }
    }
    process(device, std::span(sink_storage_).first(sink_count_));
}

void ChannelEngine::deliver(const Choice& choice,
                            std::span<const DeliveredSample> samples) noexcept {
    DeviceRun& run = devices_[choice.device];
    const Request& request =
        channel_->devices[choice.device].plan.requests[choice.request];
    const bool poll = choice.kind == Choice::Kind::POLL || choice.cycle;
    if (poll && (request.chained || !run.chain.empty() || run.chain_overflow ||
                 run.chain_cut)) {
        // A chain of companions is published when its last request is in
        // (5.5): held until then. A chain that does not fit its buffer, or
        // one with a failed part, is published not at all, never in part
        // (P9); an overflow is counted.
        for (const DeliveredSample& sample : samples) {
            if (run.chain.size() == run.chain.capacity()) {
                run.chain_overflow = true;
                break;
            }
            run.chain.push_back(sample);
        }
        if (request.chained) {
            return;
        }
        if (run.chain_cut) {
            cut_chain(choice.device, false);  // its end: the failures only
            return;
        }
        if (run.chain_overflow) {
            ++counts_.chain_overflows;
        } else {
            process(choice.device, run.chain);
        }
        drop_chain(run);
        return;
    }
    process(choice.device, samples);
}

void ChannelEngine::process(std::uint32_t device,
                            std::span<const DeliveredSample> samples) noexcept {
    DeviceRun& run = devices_[device];
    const CompiledDevice& compiled = channel_->devices[device];
    const SampleTimes times{.receive_time_ns = clock_->realtime_ns(),
                            .receive_mono_ns = clock_->monotonic_ns()};
    // Points without companions first, so that a dependent point sees what
    // its companions' own processing gave them (5.5, D-131): two passes
    // into the engine's own storage, nothing allocated (P8).
    batch_.clear();
    for (const bool dependent : {false, true}) {
        for (const DeliveredSample& sample : samples) {
            if (sample.point < run.points.size() &&
                compiled.points[sample.point].rules.companions.empty() !=
                    dependent &&
                batch_.size() < batch_.capacity()) {
                batch_.push_back(sample);
            }
        }
    }
    for (std::size_t index = 0; index < batch_.size(); ++index) {
        const DeliveredSample& sample = batch_[index];
        const PointRules& rules = compiled.points[sample.point].rules;
        companions_.clear();
        for (const std::uint32_t companion : rules.companions) {
            CompanionInput input{};
            for (std::size_t other = 0; other < index; ++other) {
                if (batch_[other].point == companion) {
                    const PointState& state = run.points[companion].state;
                    input =
                        CompanionInput{.outcome = batch_[other].sample.outcome,
                                       .raw = batch_[other].sample.raw,
                                       .mark = {.quality = state.quality,
                                                .flags = state.flags},
                                       .stale = false};
                }
            }
            if (companions_.size() < companions_.capacity()) {
                companions_.push_back(input);
            }
        }
        process_sample(rules, run.points[sample.point].state, sample.sample,
                       companions_, times, processed_);
        if (processed_.counts.sentinel) {
            ++run.diag.sentinels;
        }
        if (processed_.counts.invalid) {
            ++run.diag.invalid;
        }
        tags_->write_point(device, sample.point, processed_);
    }
    run.comm_failed = false;
}

void ChannelEngine::set_link(ChannelLink link) noexcept {
    if (link == link_) {
        return;
    }
    link_ = link;
    tags_->channel_link(link);
}

void ChannelEngine::count_answer(DeviceRun& run,
                                 const DecodeResult& decoded) noexcept {
    switch (decoded.kind) {
        case DecodeResult::Kind::SAMPLES:
        case DecodeResult::Kind::WRITE_OK:
            run.diag.last_ok_time_ns = clock_->realtime_ns();
            return;
        case DecodeResult::Kind::BUSY:
        case DecodeResult::Kind::UNREACHABLE:
            ++run.diag.refusals;
            break;
        case DecodeResult::Kind::SPLIT:
        case DecodeResult::Kind::ABSENT:
        case DecodeResult::Kind::DEVICE_INVALID:
        case DecodeResult::Kind::REFUSED:
            ++run.diag.exceptions;
            break;
        case DecodeResult::Kind::BROKEN:
            return;  // counted with the other broken answers, in `failed`
    }
    ++run.diag.errors;
    run.diag.last_status = decoded.raw;
}

const DeviceCounts& ChannelEngine::device_counts(
    std::uint32_t device) const noexcept {
    // No device: the counts of none (coding standard 4; review S5).
    static constexpr DeviceCounts NONE{};
    return device < devices_.size() ? devices_[device].diag : NONE;
}

double ChannelEngine::response_ms(std::uint32_t device) const noexcept {
    return device < devices_.size() ? devices_[device].response.average_ms()
                                    : 0.0;
}

std::uint64_t ChannelEngine::points_absent(
    std::uint32_t device) const noexcept {
    if (device >= devices_.size()) {
        return 0;
    }
    std::uint64_t absent = 0;
    for (const PointRun& point : devices_[device].points) {
        if (point.state.absent) {
            ++absent;
        }
    }
    return absent;
}

}  // namespace scada::driver
