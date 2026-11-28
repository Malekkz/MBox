// The channel engine's identity check and restart rules (driver interface
// contract 3.6, 7.6; decision D-147, ruling items 2, 3 and 6). The rest of
// the engine is in channel_engine.cpp.

#include "scada/driver/channel_engine.h"

#include "element.h"

#include "scada/driver/compiled.h"
#include "scada/driver/exchange.h"
#include "scada/driver/failure.h"
#include "scada/driver/identity_run.h"
#include "scada/driver/identity_setup.h"
#include "scada/driver/plan.h"
#include "scada/driver/protocol.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/sample_processing.h"
#include "scada/driver/scheduler.h"
#include "scada/driver/tag_value.h"
#include "scada/tag/types.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace scada::driver {

namespace {

constexpr auto NOT_INIT =
    static_cast<std::uint16_t>(tag::QualityFlag::NOT_INIT);

// Two devices of one physical unit: they share a unit key (3.4), and
// neither has a unit of its own.
bool same_unit(const CompiledDevice& left,
               const CompiledDevice& right) noexcept {
    return left.unit == right.unit && left.unit != CompiledDevice::OWN_UNIT;
}

}  // namespace

DeviceMode ChannelEngine::mode_of(std::uint32_t device) const noexcept {
    if (!channel_->devices[device].enabled) {
        return DeviceMode::OFF;
    }
    // MISMATCH is re-checked by the identity cycle alone (3.6); a device
    // whose check is pending reads nothing else (ruling item 2).
    return scheduler_mode(failures_.state(device),
                          failures_.identity_pending(device));
}

void ChannelEngine::start_identity_round(std::uint32_t device) noexcept {
    DeviceRun& run = devices_[device];
    if (!run.identity.has_value()) {
        return;
    }
    // The tracker's deadline: from the connect, recovery, restart or due
    // re-check that began the pending check (ruling item 2).
    run.identity->start(failures_.identity_deadline_ns(device));
    run.identity_answered = false;
    run.identity_round = true;
}

void ChannelEngine::identity_answered(const Choice& choice,
                                      const DecodeResult& decoded) noexcept {
    const std::uint32_t device = choice.device;
    DeviceRun& run = devices_[device];
    const Request& request =
        channel_->devices[device].plan.requests[choice.request];
    const std::int64_t now = clock_->monotonic_ns();
    if (choice.identity_keepalive) {
        // A keep-alive's answer proves the link alive only: no sample goes
        // to the check, no verdict, and a MISMATCH re-check keeps its due
        // time (the lead's ruling on the PR 177 delta check). A busy one is
        // no answer.
        (void)failures_.identity_ended(device,
                                       decoded.kind == DecodeResult::Kind::BUSY
                                           ? RequestEnd::NOT_COUNTED
                                           : RequestEnd::ANSWERED,
                                       now);
        return;
    }
    run.identity_answered = true;
    // An identity read neither adds to the run of time-outs nor empties it,
    // and resets no back-off (DI 7.3, ruling item 2).
    if (decoded.kind == DecodeResult::Kind::BUSY) {
        (void)failures_.identity_ended(device, RequestEnd::NOT_COUNTED, now);
        // Busy is no verdict: read again at once, at most `retries` times
        // (ruling item 3); the retry skips the identity floor, so the
        // count bounds it (review of PR 171).
        if (run.attempts < channel_->devices[device].failure.retries) {
            ++run.attempts;
            ++counts_.retries;
            scheduler_.retry(device, choice.request, now);
            return;
        }
    }
    run.attempts = 0;
    if (decoded.kind != DecodeResult::Kind::BUSY) {
        (void)failures_.identity_ended(device, RequestEnd::ANSWERED, now);
        const bool refused = decoded.kind == DecodeResult::Kind::SPLIT;
        if (refused && scheduler_.refused(device, choice.request)) {
            return;  // its split form follows
        }
        if (refused || decoded.kind == DecodeResult::Kind::ABSENT) {
            // An exception that the address does not exist, or ABSENT: a
            // mismatch, never a communication failure (ruling item 3).
            sink_count_ = 0;
            for (const std::uint32_t point : request.points) {
                if (sink_count_ < sink_storage_.size()) {
                    sink_storage_[sink_count_++] = DeliveredSample{
                        .point = point,
                        .sample = RawSample{.outcome = Outcome::ABSENT}};
                }
            }
        }
        if (identity_samples(device,
                             std::span(sink_storage_).first(sink_count_))) {
            // A new round, and the cycle read again unless it is still
            // running (PR 177 M1); no verdict on a round with nothing in it.
            restarted(device);
            return;
        }
    }
    if (!scheduler_.identity_running(device)) {
        identity_cycle_done(device);
    }
}

void ChannelEngine::identity_failed(const Choice& choice, End end) noexcept {
    const std::uint32_t device = choice.device;
    DeviceRun& run = devices_[device];
    const std::int64_t now = clock_->monotonic_ns();
    run.attempts = 0;
    // A time-out, a broken frame or a gateway's failure: a communication
    // failure, which counts toward demotion and gives no verdict (ruling
    // items 2, 3). A frame not sent counts as a failed request.
    (void)end;
    (void)failures_.identity_ended(device, RequestEnd::FAILED, now);
    if (choice.identity_keepalive) {
        return;  // no part of a cycle: the cycle's times stay
    }
    if (!scheduler_.identity_running(device)) {
        identity_cycle_done(device);
    }
}

bool ChannelEngine::identity_samples(
    std::uint32_t device, std::span<const DeliveredSample> samples) noexcept {
    DeviceRun& run = devices_[device];
    const CompiledDevice& compiled = channel_->devices[device];
    const SampleTimes times{.receive_time_ns = clock_->realtime_ns(),
                            .receive_mono_ns = clock_->monotonic_ns()};
    // Every restart rule sees the whole answer, so that each keeps its
    // baseline when one fires (PR 177 M2); what the check saw of an answer
    // that shows a restart is dropped with the round.
    bool restart = false;
    for (const DeliveredSample& sample : samples) {
        if (sample.point >= run.scratch.size()) {
            continue;
        }
        // Processed apart from the point's own state, and no tag written:
        // the samples of the other points of the block are dropped.
        process_sample(compiled.points[sample.point].rules,
                       run.scratch[sample.point], sample.sample, {}, times,
                       processed_);
        if (run.identity.has_value() && !restart) {
            run.identity->sample(sample.point, sample.sample.outcome,
                                 processed_.rule_valid, processed_.rule_value);
        }
        restart = watch_restart(device, sample.point, processed_) || restart;
    }
    return restart;
}

void ChannelEngine::identity_cycle_done(std::uint32_t device) noexcept {
    DeviceRun& run = devices_[device];
    // A retry still to be sent ends the cycle later; a dropped one does
    // not hold it (PR 177 delta, item 2).
    if (!run.identity.has_value() || !run.identity_round ||
        scheduler_.retry_pending(device)) {
        return;
    }
    const CompiledDevice& compiled = channel_->devices[device];
    const std::int64_t now = clock_->monotonic_ns();
    const IdentityVerdict verdict = run.identity->verdict(now);
    const bool was_mismatch = failures_.state(device) == DeviceStatus::MISMATCH;
    switch (verdict.kind) {
        case IdentityVerdict::Kind::NONE: {
            // Not yet: the cycle again, the deadline kept; the scheduler
            // floors it at one identity period after the last (PR 171 M3),
            // or at the fastest group's period when nothing answered (the
            // lead's ruling on 5b-2b).
            run.identity->start(run.identity->deadline_ns());
            scheduler_.identity_again(
                device, now,
                run.identity_answered
                    ? Scheduler::IdentityFloor::IDENTITY_PERIOD
                    : Scheduler::IdentityFloor::FASTEST_PERIOD);
            run.identity_answered = false;
            return;
        }
        case IdentityVerdict::Kind::MATCH:
            keep_identity_text(device);
            run.identity_ok = true;
            run.identity_round = false;
            // Out of MISMATCH through INITIALISING only: a match makes no
            // tag GOOD, reads do (ruling item 3).
            failures_.identity_passed(device);
            if (was_mismatch) {
                note_event(device, DeviceEvent::MATCH, 0, 0,
                           identity_text(device));
            }
            return;
        case IdentityVerdict::Kind::MISMATCH:
            break;
    }
    keep_identity_text(device);
    run.identity_ok = false;
    std::array<char, TAG_STRING_MAX_BYTES> value{};
    const std::size_t length = run.identity->check_text(verdict.check, value);
    note_event(device, DeviceEvent::MISMATCH,
               static_cast<std::uint8_t>(verdict.reason), verdict.check,
               std::string_view(value.data(), length));
    if (!compiled.refuse_on_mismatch) {
        // `warn`: the device runs, `identity_ok` false (ruling item 3).
        failures_.identity_passed(device);
        run.identity_round = false;
        return;
    }
    if (!was_mismatch) {
        // Every point tag BAD without COMM_FAIL or STALE, NOT_INIT kept,
        // the value kept; the state forgets every value from before, and
        // the write comes before the state's (ruling item 3).
        withhold_points(device, 0, true);
    }
    // Checked again at the constant `backoff`, a failed re-check too
    // (DI 3.6, PR 171 M5); its deadline runs from its due time.
    failures_.identity_mismatched(device, now);
    start_identity_round(device);
    scheduler_.identity_again(device, failures_.probe_due_ns(device));
}

bool ChannelEngine::watch_restart(std::uint32_t device, std::uint32_t point,
                                  const ProcessedSample& sample) noexcept {
    DeviceRun& run = devices_[device];
    return run.restart.has_value() && sample.rule_valid &&
           run.restart->sample(point, sample.rule_value);
}

void ChannelEngine::restarted(std::uint32_t device) noexcept {
    const std::int64_t now = clock_->monotonic_ns();
    const CompiledDevice& compiled = channel_->devices[device];
    for (std::uint32_t other = 0; other < devices_.size(); ++other) {
        const CompiledDevice& peer = channel_->devices[other];
        if (!peer.enabled || (other != device && !same_unit(compiled, peer))) {
            continue;
        }
        DeviceRun& peer_run = devices_[other];
        ++peer_run.restarts;
        note_event(other, DeviceEvent::RESTART, 0, 0, {});
        if (failures_.state(other) != DeviceStatus::RUNNING) {
            continue;  // INITIALISING already: counted, nothing else
        }
        // The writes of 9.1 for its point tags: NOT_INIT, no source time.
        withhold_points(other, NOT_INIT, false);
        scheduler_.abort_order(other);
        drop_chain(peer_run);
        peer_run.identity_round = false;
    }
    // The tracker re-arms the grace once until a full period passes without
    // a restart of the unit (ruling item 6).
    failures_.restarted(device, now);
    // A device already in IDENTITY (its check pending, or MISMATCH) starts
    // its round again and is read again (PR 177 M1): without this, a
    // restart seen in the last read of a cycle left it unread for ever. A
    // device that was RUNNING enters IDENTITY at the next sync_scheduler,
    // which starts its round.
    for (std::uint32_t other = 0; other < devices_.size(); ++other) {
        const CompiledDevice& peer = channel_->devices[other];
        const DeviceRun& peer_run = devices_[other];
        if (!peer.enabled || !peer_run.identity_round ||
            (other != device && !same_unit(compiled, peer))) {
            continue;
        }
        start_identity_round(other);
        if (!scheduler_.identity_running(other)) {
            scheduler_.identity_again(other, now,
                                      Scheduler::IdentityFloor::FASTEST_PERIOD);
        }
    }
}

void ChannelEngine::withhold_points(std::uint32_t device, std::uint16_t flags,
                                    bool keep_not_init) noexcept {
    DeviceRun& run = devices_[device];
    const CompiledDevice& compiled = channel_->devices[device];
    for (std::uint32_t point = 0; point < run.points.size(); ++point) {
        PointState& state = run.points[point].state;
        const std::uint16_t written =
            keep_not_init ? static_cast<std::uint16_t>(state.flags & NOT_INIT)
                          : flags;
        // MISMATCH keeps the value's source time; a restart writes none.
        const std::int64_t source = keep_not_init ? state.source_time_ns : 0;
        withhold_point(compiled.points[point].rules, state, written, source,
                       processed_);
        tags_->write_point(device, point, processed_);
    }
    // A later loss writes COMM_FAIL again (ruling item 3).
    run.comm_failed = false;
}

void ChannelEngine::keep_identity_text(std::uint32_t device) noexcept {
    DeviceRun& run = devices_[device];
    if (run.identity.has_value()) {
        run.identity_length = run.identity->text(run.identity_text);
    }
}

std::optional<bool> ChannelEngine::identity_ok(
    std::uint32_t device) const noexcept {
    return device < devices_.size() ? devices_[device].identity_ok
                                    : std::nullopt;
}

std::string_view ChannelEngine::identity_text(
    std::uint32_t device) const noexcept {
    if (device >= devices_.size()) {
        return {};
    }
    const DeviceRun& run = devices_[device];
    return {run.identity_text.data(), run.identity_length};
}

std::uint64_t ChannelEngine::restarts(std::uint32_t device) const noexcept {
    return device < devices_.size() ? devices_[device].restarts : 0;
}

void ChannelEngine::note_event(std::uint32_t device, DeviceEvent kind,
                               std::uint8_t reason, std::uint32_t check,
                               std::string_view text) noexcept {
    EventRecord& record = detail::element(devices_[device].events,
                                          static_cast<std::size_t>(kind));
    ++record.count;
    record.has_details = true;
    record.time_ns = clock_->realtime_ns();
    record.reason = reason;
    record.check = check;
    record.length = std::min(text.size(), record.text.size());
    std::copy_n(text.data(), record.length, record.text.data());
}

bool ChannelEngine::take_event(std::uint32_t device, DeviceEvent kind,
                               EventRecord& out) noexcept {
    if (device >= devices_.size()) {
        return false;
    }
    EventRecord& record = detail::element(devices_[device].events,
                                          static_cast<std::size_t>(kind));
    if (record.count == 0) {
        return false;
    }
    out = record;
    record.count = 0;
    return true;
}

void ChannelEngine::note_channel_event(ChannelEvent kind,
                                       CloseReason reason) noexcept {
    EventRecord& record =
        detail::element(channel_events_, static_cast<std::size_t>(kind));
    ++record.count;
    record.has_details = true;
    record.time_ns = clock_->realtime_ns();
    record.reason = static_cast<std::uint8_t>(reason);
}

bool ChannelEngine::take_channel_event(ChannelEvent kind,
                                       EventRecord& out) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    if (index >= channel_events_.size()) {
        return false;
    }
    EventRecord& record = detail::element(channel_events_, index);
    if (record.count == 0) {
        return false;
    }
    out = record;
    record.count = 0;
    return true;
}

}  // namespace scada::driver
