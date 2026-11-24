#include "scada/driver/channel_engine.h"

#include "scada/common/static_string.h"
#include "scada/common/status.h"
#include "scada/driver/compiled.h"
#include "scada/driver/exchange.h"
#include "scada/driver/failure.h"
#include "scada/driver/plan.h"
#include "scada/driver/point_rules.h"
#include "scada/driver/protocol.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/sample_processing.h"
#include "scada/driver/scheduler.h"
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

}  // namespace

ChannelEngine::ChannelEngine(const CompiledChannel& channel,
                             const FrameProtocol& protocol,
                             Transport& transport, Clock& clock, TagSink& tags)
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
    const std::int64_t now = clock.monotonic_ns();
    std::size_t frame = 0;
    std::size_t request_points = 0;
    std::size_t device_points = 0;
    std::size_t companions = 0;
    for (std::size_t index = 0; index < channel.devices.size(); ++index) {
        const CompiledDevice& device = channel.devices[index];
        DeviceRun& run = devices_[index];
        run.max_frame = protocol.device_caps(device).max_frame_bytes;
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

std::int64_t ChannelEngine::step() noexcept {
    const std::int64_t now = clock_->monotonic_ns();
    const std::int64_t stale_due = check_staleness(now);
    if (!status_.ok()) {
        return NEVER;
    }
    if (!connected_) {
        return std::min(try_connect(now), stale_due);
    }
    sync_scheduler(now);
    if (now < quiet_until_ns_) {
        return quiet_until_ns_;  // a serial line's quiet time (7.2)
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

std::int64_t ChannelEngine::try_connect(std::int64_t now_ns) noexcept {
    if (now_ns < next_connect_ns_) {
        return next_connect_ns_;
    }
    const ConnectionRules& rules = channel_->connection;
    const IoStatus status =
        transport_->connect(later(now_ns, rules.connect_timeout_ns));
    const std::int64_t now = clock_->monotonic_ns();
    if (status == IoStatus::OK) {
        connected_ = true;
        ever_connected_ = true;
        ++counts_.connects;
        channel_state_.reset();
        // A close the driver made itself, reconnected within
        // `connect_timeout` of it: states, counts and tags are kept (7.3).
        if (own_close_ &&
            now <= later(own_close_ns_, rules.connect_timeout_ns)) {
            failures_.resumed();
            ++counts_.resumed;
        } else {
            failures_.connected(now);
        }
        own_close_ = false;
        answered_since_connect_ = false;
        quiet_until_ns_ = 0;
        scheduler_.reconnect();
        publish_states();
        return now;
    }
    if (own_close_) {
        // Not back within `connect_timeout`: the rules of a lost connection
        // apply (R-17).
        own_close_ = false;
        failures_.lost();
        scheduler_.reconnect();
        ++counts_.losses;
    }
    // CONNECTING with COMM_FAIL (conformance test 16).
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        if (channel_->devices[device].enabled) {
            fail_points(device);
        }
    }
    next_connect_ns_ = later(now, backoff_ns_);
    backoff_ns_ = std::min(backoff_ns_ > rules.reconnect_max_ns / 2
                               ? rules.reconnect_max_ns
                               : backoff_ns_ * 2,
                           rules.reconnect_max_ns);
    publish_states();
    return next_connect_ns_;
}

void ChannelEngine::lose(std::int64_t now_ns) noexcept {
    transport_->close(false);
    connected_ = false;
    own_close_ = false;
    ++counts_.losses;
    failures_.lost();
    scheduler_.reconnect();
    // Every device is CONNECTING now: `publish_states` writes their points
    // BAD with COMM_FAIL, all at once (conformance test 16).
    for (DeviceRun& run : devices_) {
        run.chain.clear();
        run.attempts = 0;
    }
    const ConnectionRules& rules = channel_->connection;
    next_connect_ns_ = later(now_ns, backoff_ns_);
    backoff_ns_ = std::min(backoff_ns_ > rules.reconnect_max_ns / 2
                               ? rules.reconnect_max_ns
                               : backoff_ns_ * 2,
                           rules.reconnect_max_ns);
    publish_states();
}

void ChannelEngine::close_own(std::int64_t now_ns) noexcept {
    transport_->close(true);
    connected_ = false;
    ++counts_.own_closes;
    own_close_ = true;
    own_close_ns_ = now_ns;
    for (DeviceRun& run : devices_) {
        run.chain.clear();  // a chain cut by the close publishes nothing
        run.attempts = 0;
    }
    // At once, but a second close before any valid answer waits for the
    // back-off (7.3).
    if (answered_since_connect_) {
        next_connect_ns_ = now_ns;
        return;
    }
    const ConnectionRules& rules = channel_->connection;
    next_connect_ns_ = later(now_ns, backoff_ns_);
    backoff_ns_ = std::min(backoff_ns_ > rules.reconnect_max_ns / 2
                               ? rules.reconnect_max_ns
                               : backoff_ns_ * 2,
                           rules.reconnect_max_ns);
}

void ChannelEngine::sync_scheduler(std::int64_t /*now_ns*/) noexcept {
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        if (!channel_->devices[device].enabled) {
            continue;
        }
        scheduler_.set_polled(device, failures_.polled(device));
        if (failures_.state(device) == DeviceStatus::DEMOTED) {
            scheduler_.set_probe(device, failures_.probe_due_ns(device));
        } else {
            scheduler_.clear_probe(device);
        }
    }
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
    const IoStatus status =
        transport_->send(std::span(tx_).first(encoded.length));
    ++counts_.requests;
    if (status == IoStatus::CLOSED) {
        return End::CLOSED;
    }
    if (status != IoStatus::OK) {
        return End::SEND_FAILED;
    }
    std::int64_t timeout = device.failure.timeout_ns;
    if (choice.kind == Choice::Kind::PROBE) {
        timeout = std::min(timeout, PROBE_TIMEOUT_MAX_NS);
    }
    std::size_t frame_bytes = 0;
    const End end = wait_answer(request, sent, encoded.length,
                                encoded.expected_max, timeout, frame_bytes);
    if (end != End::ANSWER) {
        return end;
    }
    SampleSink sink(sink_storage_);
    decoded = protocol_->decode(device, run.protocol, request, sent,
                                std::span(rx_).first(frame_bytes), sink);
    sink_count_ = sink.samples().size();
    return decoded.kind == DecodeResult::Kind::BROKEN ? End::BROKEN
                                                      : End::ANSWER;
}

ChannelEngine::End ChannelEngine::wait_answer(
    const Request& request, const Attempt& sent, std::size_t request_bytes,
    std::size_t expected_max, std::int64_t timeout_ns,
    std::size_t& frame_bytes) noexcept {
    const LineTiming& line = channel_->line;
    std::int64_t deadline = first_byte_deadline_ns(line, clock_->monotonic_ns(),
                                                   request_bytes, timeout_ns);
    std::size_t have = 0;
    bool first_byte = false;
    for (;;) {
        if (have >= rx_.size()) {
            return End::BROKEN;  // more than `max_frame_bytes` (7.1)
        }
        std::size_t got = 0;
        const IoStatus status =
            transport_->receive(std::span(rx_).subspan(have), deadline, got);
        if (status == IoStatus::CLOSED) {
            return End::CLOSED;
        }
        if (status != IoStatus::OK || got == 0) {
            return have == 0 && !first_byte ? End::TIMEOUT : End::BROKEN;
        }
        if (!first_byte) {
            first_byte = true;
            deadline =
                answer_deadline_ns(line, clock_->monotonic_ns(), expected_max);
        }
        have += got;
        switch (scan(request, sent, have, frame_bytes)) {
            case Scan::MORE:
                break;
            case Scan::COMPLETE:
                return End::ANSWER;
            case Scan::BROKEN:
                return End::BROKEN;
        }
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
    DecodeResult decoded;
    const End end = attempt(choice, decoded);
    const std::int64_t now = clock_->monotonic_ns();
    switch (end) {
        case End::NOT_SENT:
            (void)scheduler_.not_sent();
            return;
        case End::CLOSED:
            // Every device of the channel at once, no retries (7.3).
            lose(now);
            return;
        case End::SEND_FAILED:
            close_own(now);  // a frame not taken whole (7.3)
            return;
        case End::ANSWER:
            answered(choice, decoded);
            return;
        case End::TIMEOUT:
        case End::BROKEN:
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
    const bool probe = choice.kind == Choice::Kind::PROBE;
    // Any valid answer, an exception included: the back-off starts from
    // `min` again (7.3) and the run of time-outs is empty.
    answered_since_connect_ = true;
    backoff_ns_ = channel_->connection.reconnect_min_ns;
    failures_.answer_seen();
    if (decoded.kind == DecodeResult::Kind::UNREACHABLE) {
        // A gateway that cannot reach the unit: a failure of that unit only
        // (7.4, annex A), not of the line.
        failed(choice, End::ANSWER);
        return;
    }
    if (probe) {
        // The device answered: it is alive, busy or not (7.4).
        failures_.probe_ended(device, true, now);
        scheduler_.probe_done();
        run.attempts = 0;
        run.busy = 0;
        return;
    }
    if (decoded.kind == DecodeResult::Kind::BUSY) {
        // Busy neither fails nor answers, up to `busy_retries` times in a
        // row; then it counts as failed (CS 4.6).
        ++run.busy;
        RequestEnd end = RequestEnd::NOT_COUNTED;
        if (run.busy > compiled.failure.busy_retries) {
            run.busy = 0;
            end = RequestEnd::FAILED;
        }
        (void)failures_.request_ended(device, end, now);
        if (!request.chained) {
            run.chain.clear();
        }
        return;
    }
    run.attempts = 0;
    run.busy = 0;
    (void)failures_.request_ended(device, RequestEnd::ANSWERED, now);
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
    if (end == End::ANSWER) {
        return;  // a valid frame: only its unit failed
    }
    const std::int64_t now = clock_->monotonic_ns();
    if (channel_->transport == TransportKind::TCP) {
        // A broken frame on a stream, or a lost side-effect read, closes
        // the connection (7.3).
        if (end == End::BROKEN || request.side_effect) {
            close_own(now);
        }
        return;
    }
    // A serial line, or serial frames over a stream: discard input and keep
    // the quiet time before the next request to any device (7.2, 7.3).
    transport_->discard_input();
    quiet_until_ns_ = later(now, margin_ns(channel_->line));
}

void ChannelEngine::failed(const Choice& choice, End end) noexcept {
    const std::uint32_t device = choice.device;
    DeviceRun& run = devices_[device];
    const CompiledDevice& compiled = channel_->devices[device];
    const Request& request = compiled.plan.requests[choice.request];
    const bool probe = choice.kind == Choice::Kind::PROBE;
    if (end == End::TIMEOUT) {
        ++counts_.timeouts;
    } else if (end == End::BROKEN) {
        ++counts_.broken;
    }
    after_bad_frame(request, end);
    const std::int64_t now = clock_->monotonic_ns();
    // A read is tried again within its group's turn, never a side-effect
    // one, never a probe, never over a closed connection (7.1, 6.6).
    if (connected_ && !probe && !request.side_effect &&
        run.attempts < compiled.failure.retries) {
        ++run.attempts;
        ++counts_.retries;
        scheduler_.retry(device, choice.request);
        return;
    }
    run.attempts = 0;
    if (!request.chained) {
        run.chain.clear();  // a chain whose end failed publishes nothing
    }
    if (probe) {
        failures_.probe_ended(device, false, now);
        scheduler_.probe_done();
        return;
    }
    if (end == End::TIMEOUT) {
        const FailureTracker::TimeoutEnd timed_out =
            failures_.read_timed_out(device, now);
        if (timed_out.lost && connected_) {
            close_own(now);  // the run spans every live unit (7.3, D-139)
        }
        return;
    }
    (void)failures_.request_ended(device, RequestEnd::FAILED, now);
}

void ChannelEngine::deliver(const Choice& choice,
                            std::span<const DeliveredSample> samples) noexcept {
    DeviceRun& run = devices_[choice.device];
    const Request& request =
        channel_->devices[choice.device].plan.requests[choice.request];
    if (choice.kind == Choice::Kind::POLL &&
        (request.chained || !run.chain.empty())) {
        // A chain of companions is published when its last request is in
        // (5.5): held until then.
        for (const DeliveredSample& sample : samples) {
            if (run.chain.size() < run.chain.capacity()) {
                run.chain.push_back(sample);
            }
        }
        if (request.chained) {
            return;
        }
        process(choice.device, run.chain);
        run.chain.clear();
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
    batch_.clear();
    for (const DeliveredSample& sample : samples) {
        if (sample.point < run.points.size() &&
            batch_.size() < batch_.capacity()) {
            batch_.push_back(sample);
        }
    }
    // Points without companions first, so that a dependent point sees what
    // its companions' own processing gave them (5.5, D-131).
    std::stable_partition(
        batch_.begin(), batch_.end(), [&](const DeliveredSample& sample) {
            return compiled.points[sample.point].rules.companions.empty();
        });
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
        tags_->write_point(device, sample.point, processed_);
    }
    run.comm_failed = false;
}

}  // namespace scada::driver
