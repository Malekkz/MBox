#include "scada/driver/failure.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace scada::driver {

namespace {

std::int64_t later(std::int64_t time, std::int64_t interval) noexcept {
    constexpr std::int64_t MAX = std::numeric_limits<std::int64_t>::max();
    return time > MAX - interval ? MAX : time + interval;
}

// A probe back-off doubled, up to `backoff_max` (7.4).
std::int64_t doubled(std::int64_t backoff_ns,
                     const FailureRules& rules) noexcept {
    return std::min(backoff_ns > rules.backoff_max_ns / 2 ? rules.backoff_max_ns
                                                          : backoff_ns * 2,
                    rules.backoff_max_ns);
}

}  // namespace

FailureTracker::FailureTracker(std::vector<Device> devices,
                               std::uint32_t lost_after_timeouts)
    : devices_(std::move(devices)),
      runs_(devices_.size()),
      unit_index_(devices_.size()) {
    std::vector<std::uint32_t> units;
    for (std::size_t index = 0; index < devices_.size(); ++index) {
        const auto found = devices_[index].own_unit
                               ? units.end()
                               : std::find(units.begin(), units.end(),
                                           devices_[index].unit_id);
        unit_index_[index] = static_cast<std::uint32_t>(found - units.begin());
        if (found == units.end()) {
            units.push_back(devices_[index].unit_id);
        }
        runs_[index].state = devices_[index].enabled ? DeviceStatus::CONNECTING
                                                     : DeviceStatus::DISABLED;
        runs_[index].backoff_ns = devices_[index].rules.backoff_ns;
    }
    counts_.assign(units.size(), 0);
    run_units_.assign(units.size(), false);
    lost_after_ = lost_after_timeouts;
}

std::uint32_t& FailureTracker::count_of(std::uint32_t device) noexcept {
    return counts_[unit_index_[device]];
}

std::uint32_t FailureTracker::count_of(std::uint32_t device) const noexcept {
    return counts_[unit_index_[device]];
}

void FailureTracker::connected(std::int64_t now_ns) noexcept {
    // A reconnect keeps failure counts and demotion (7.3; PR 137 review,
    // P1 and Q2): a dead device is not polled again, nor its cost paid
    // again, because the connection came back.
    answer_seen();
    for (std::size_t index = 0; index < devices_.size(); ++index) {
        if (!devices_[index].enabled) {
            continue;
        }
        Run& run = runs_[index];
        run.state =
            run.demoted ? DeviceStatus::DEMOTED : DeviceStatus::INITIALISING;
        run.grace_until_ns =
            later(now_ns, devices_[index].rules.startup_grace_ns);
        // A demoted device is checked when a probe recovers it.
        run.identity_pending = false;
        if (!run.demoted) {
            begin_check(static_cast<std::uint32_t>(index), now_ns);
        }
    }
}

void FailureTracker::lost() noexcept {
    for (std::size_t index = 0; index < devices_.size(); ++index) {
        if (devices_[index].enabled) {
            runs_[index].state = DeviceStatus::CONNECTING;  // `demoted` kept
        }
    }
}

void FailureTracker::resumed() noexcept {
    answer_seen();
}

FailureTracker::TimeoutEnd FailureTracker::read_timed_out(
    std::uint32_t device, std::int64_t now_ns) noexcept {
    if (device >= devices_.size()) {
        return {};
    }
    if (add_to_run(device)) {
        return {.lost = true};  // not counted toward demotion (D-139)
    }
    return {.demoted = request_ended(device, RequestEnd::FAILED, now_ns)};
}

bool FailureTracker::add_to_run(std::uint32_t device) noexcept {
    if (lost_after_ == 0) {
        return false;
    }
    if (run_length_ < std::numeric_limits<std::uint32_t>::max()) {
        ++run_length_;
    }
    run_units_[unit_index_[device]] = true;
    if (run_length_ < lost_after_) {
        return false;
    }
    // Every eligible unit must be in the run (with none eligible, the run
    // alone decides, as the contract's rule reads).
    for (std::uint32_t other = 0; other < devices_.size(); ++other) {
        // A device whose identity check is pending reads only its identity,
        // which adds nothing to the run: not eligible (review S8).
        if (!polled(other) || !devices_[other].has_poll_group ||
            runs_[other].identity_pending) {
            continue;
        }
        if (!run_units_[unit_index_[other]]) {
            return false;
        }
    }
    return true;
}

void FailureTracker::answer_seen() noexcept {
    run_length_ = 0;
    std::fill(run_units_.begin(), run_units_.end(), false);
}

bool FailureTracker::request_ended(std::uint32_t device, RequestEnd end,
                                   std::int64_t now_ns) noexcept {
    if (device >= devices_.size() || !polled(device)) {
        return false;
    }
    Run& run = runs_[device];
    if (end == RequestEnd::NOT_COUNTED) {
        return false;
    }
    if (end == RequestEnd::ANSWERED) {
        count_of(device) = 0;
        if (!run.identity_pending) {
            run.state = DeviceStatus::RUNNING;  // never before its check (M4)
        }
        // Only a poll that answered resets the probe back-off (Q3): a unit
        // that answers probes but no poll keeps its growing back-off.
        run.backoff_ns = devices_[device].rules.backoff_ns;
        run.probe_recovered = false;
        return false;
    }
    return count_failure(device, now_ns);
}

bool FailureTracker::count_failure(std::uint32_t device,
                                   std::int64_t now_ns) noexcept {
    if (now_ns < runs_[device].grace_until_ns) {
        return false;  // within startup_grace: not counted (7.4)
    }
    const std::uint32_t count = ++count_of(device);
    bool demoted = false;
    // Every device of the same unit whose failures count and whose limit
    // the shared count reached is demoted together.
    for (std::uint32_t other = 0; other < devices_.size(); ++other) {
        if (unit_index_[other] != unit_index_[device] || !counted(other) ||
            count < devices_[other].rules.demote_after) {
            continue;
        }
        Run& demote = runs_[other];
        if (demote.probe_recovered) {
            demote.backoff_ns =
                doubled(demote.backoff_ns, devices_[other].rules);
            demote.probe_recovered = false;
        }
        demote.state = DeviceStatus::DEMOTED;
        demote.demoted = true;
        demote.probe_due_ns = later(now_ns, demote.backoff_ns);
        demoted = true;
    }
    return demoted;
}

void FailureTracker::probe_ended(std::uint32_t device, bool answered,
                                 std::int64_t now_ns) noexcept {
    if (device >= devices_.size() ||
        runs_[device].state != DeviceStatus::DEMOTED) {
        return;
    }
    // One probe serves every demoted device of the unit: they share the
    // failure count (3.4; Q5).
    if (answered) {
        count_of(device) = 0;
    }
    for (std::uint32_t other = 0; other < devices_.size(); ++other) {
        Run& run = runs_[other];
        if (unit_index_[other] != unit_index_[device] ||
            run.state != DeviceStatus::DEMOTED) {
            continue;
        }
        const FailureRules& rules = devices_[other].rules;
        if (answered) {
            run.state = DeviceStatus::INITIALISING;
            run.demoted = false;
            run.probe_recovered = true;
            begin_check(other, now_ns);
            continue;
        }
        run.backoff_ns = doubled(run.backoff_ns, rules);
        run.probe_due_ns = later(now_ns, run.backoff_ns);
    }
}

bool FailureTracker::probes_for_unit(std::uint32_t device) const noexcept {
    if (device >= devices_.size() ||
        runs_[device].state != DeviceStatus::DEMOTED) {
        return false;
    }
    for (std::uint32_t other = 0; other < device; ++other) {
        if (unit_index_[other] == unit_index_[device] &&
            runs_[other].state == DeviceStatus::DEMOTED) {
            return false;
        }
    }
    return true;
}

DeviceStatus FailureTracker::state(std::uint32_t device) const noexcept {
    return device < runs_.size() ? runs_[device].state : DeviceStatus::DISABLED;
}

std::int64_t FailureTracker::probe_due_ns(std::uint32_t device) const noexcept {
    return device < runs_.size() ? runs_[device].probe_due_ns : 0;
}

std::uint32_t FailureTracker::failures(std::uint32_t device) const noexcept {
    return device < runs_.size() ? count_of(device) : 0;
}

bool FailureTracker::identity_ended(std::uint32_t device, RequestEnd end,
                                    std::int64_t now_ns) noexcept {
    if (device >= devices_.size() || !counted(device)) {
        return false;
    }
    switch (end) {
        case RequestEnd::ANSWERED:
            // Not RUNNING, no back-off reset, `probe_recovered` kept (DI 7.3).
            count_of(device) = 0;
            return false;
        case RequestEnd::FAILED:
            return count_failure(device, now_ns);
        case RequestEnd::NOT_COUNTED:
            return false;
    }
    return false;
}

void FailureTracker::identity_passed(std::uint32_t device) noexcept {
    if (device >= devices_.size()) {
        return;
    }
    Run& run = runs_[device];
    run.identity_pending = false;
    if (run.state == DeviceStatus::MISMATCH) {
        run.state = DeviceStatus::INITIALISING;  // RUNNING on a poll's answer
    }
}

void FailureTracker::identity_mismatched(std::uint32_t device,
                                         std::int64_t now_ns) noexcept {
    if (device >= devices_.size() ||
        (runs_[device].state != DeviceStatus::INITIALISING &&
         runs_[device].state != DeviceStatus::MISMATCH)) {
        return;
    }
    Run& run = runs_[device];
    run.state = DeviceStatus::MISMATCH;
    run.identity_pending = false;
    // At the constant `backoff` (DI 3.6), not the probe's grown one, after
    // the first mismatch and after every failed re-check (review M5). The
    // re-check is a check of its own, with its own deadline.
    run.probe_due_ns = later(now_ns, devices_[device].rules.backoff_ns);
    run.identity_since_ns = run.probe_due_ns;
}

void FailureTracker::begin_check(std::uint32_t device,
                                 std::int64_t now_ns) noexcept {
    Run& run = runs_[device];
    run.identity_pending = devices_[device].identify;
    run.identity_since_ns = now_ns;
}

std::int64_t FailureTracker::identity_since_ns(
    std::uint32_t device) const noexcept {
    return device < devices_.size() ? runs_[device].identity_since_ns : 0;
}

std::int64_t FailureTracker::identity_deadline_ns(
    std::uint32_t device) const noexcept {
    if (device >= devices_.size()) {
        return 0;
    }
    return later(runs_[device].identity_since_ns,
                 devices_[device].identity_wait_ns);
}

void FailureTracker::restarted(std::uint32_t device,
                               std::int64_t now_ns) noexcept {
    if (device >= devices_.size()) {
        return;
    }
    // The unit restarted: every device of it (D-147, ruling item 6). The
    // grace starts again once, until a full period of every group passes
    // without a restart of the unit.
    for (std::uint32_t other = 0; other < devices_.size(); ++other) {
        Run& run = runs_[other];
        if (unit_index_[other] != unit_index_[device]) {
            continue;
        }
        const bool rearm =
            run.last_restart_ns == NO_RESTART ||
            now_ns - run.last_restart_ns >= devices_[other].full_period_ns;
        run.last_restart_ns = now_ns;
        if (run.state != DeviceStatus::RUNNING) {
            continue;  // counted by the engine, nothing else (item 6)
        }
        run.state = DeviceStatus::INITIALISING;
        begin_check(other, now_ns);
        if (rearm) {
            run.grace_until_ns =
                later(now_ns, devices_[other].rules.startup_grace_ns);
        }
    }
}

bool FailureTracker::identity_pending(std::uint32_t device) const noexcept {
    return device < devices_.size() && runs_[device].identity_pending;
}

bool FailureTracker::counted(std::uint32_t device) const noexcept {
    return polled(device) || state(device) == DeviceStatus::MISMATCH;
}

bool FailureTracker::polled(std::uint32_t device) const noexcept {
    const DeviceStatus current = state(device);
    return current == DeviceStatus::INITIALISING ||
           current == DeviceStatus::RUNNING;
}

}  // namespace scada::driver
