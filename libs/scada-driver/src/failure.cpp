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

}  // namespace

FailureTracker::FailureTracker(std::vector<Device> devices)
    : devices_(std::move(devices)),
      runs_(devices_.size()),
      unit_index_(devices_.size()) {
    std::vector<std::uint32_t> units;
    for (std::size_t index = 0; index < devices_.size(); ++index) {
        const auto found =
            std::find(units.begin(), units.end(), devices_[index].unit_id);
        unit_index_[index] = static_cast<std::uint32_t>(found - units.begin());
        if (found == units.end()) {
            units.push_back(devices_[index].unit_id);
        }
        runs_[index].state = devices_[index].enabled ? DeviceState::CONNECTING
                                                     : DeviceState::DISABLED;
    }
    counts_.assign(units.size(), 0);
}

std::uint32_t& FailureTracker::count_of(std::uint32_t device) noexcept {
    return counts_[unit_index_[device]];
}

std::uint32_t FailureTracker::count_of(std::uint32_t device) const noexcept {
    return counts_[unit_index_[device]];
}

void FailureTracker::connected(std::int64_t now_ns) noexcept {
    std::fill(counts_.begin(), counts_.end(), 0);
    for (std::size_t index = 0; index < devices_.size(); ++index) {
        if (!devices_[index].enabled) {
            continue;
        }
        Run& run = runs_[index];
        run.state = DeviceState::INITIALISING;
        run.grace_until_ns =
            later(now_ns, devices_[index].rules.startup_grace_ns);
        run.backoff_ns = devices_[index].rules.backoff_ns;
    }
}

void FailureTracker::lost() noexcept {
    for (std::size_t index = 0; index < devices_.size(); ++index) {
        if (devices_[index].enabled) {
            runs_[index].state = DeviceState::CONNECTING;
        }
    }
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
        run.state = DeviceState::RUNNING;
        return false;
    }
    if (now_ns < run.grace_until_ns) {
        return false;  // within startup_grace: not counted (7.4)
    }
    const std::uint32_t count = ++count_of(device);
    bool demoted = false;
    // Every polled device of the same unit whose limit the shared count
    // reached is demoted together.
    for (std::uint32_t other = 0; other < devices_.size(); ++other) {
        if (unit_index_[other] != unit_index_[device] || !polled(other) ||
            count < devices_[other].rules.demote_after) {
            continue;
        }
        Run& demote = runs_[other];
        demote.state = DeviceState::DEMOTED;
        demote.backoff_ns = devices_[other].rules.backoff_ns;
        demote.probe_due_ns = later(now_ns, demote.backoff_ns);
        demoted = true;
    }
    return demoted;
}

void FailureTracker::probe_ended(std::uint32_t device, bool answered,
                                 std::int64_t now_ns) noexcept {
    if (device >= devices_.size() ||
        runs_[device].state != DeviceState::DEMOTED) {
        return;
    }
    Run& run = runs_[device];
    const FailureRules& rules = devices_[device].rules;
    if (answered) {
        count_of(device) = 0;
        run.state = DeviceState::INITIALISING;
        run.backoff_ns = rules.backoff_ns;
        return;
    }
    run.backoff_ns = std::min(run.backoff_ns > rules.backoff_max_ns / 2
                                  ? rules.backoff_max_ns
                                  : run.backoff_ns * 2,
                              rules.backoff_max_ns);
    run.probe_due_ns = later(now_ns, run.backoff_ns);
}

DeviceState FailureTracker::state(std::uint32_t device) const noexcept {
    return device < runs_.size() ? runs_[device].state : DeviceState::DISABLED;
}

std::int64_t FailureTracker::probe_due_ns(std::uint32_t device) const noexcept {
    return device < runs_.size() ? runs_[device].probe_due_ns : 0;
}

std::uint32_t FailureTracker::failures(std::uint32_t device) const noexcept {
    return device < runs_.size() ? count_of(device) : 0;
}

bool FailureTracker::polled(std::uint32_t device) const noexcept {
    const DeviceState current = state(device);
    return current == DeviceState::INITIALISING ||
           current == DeviceState::RUNNING;
}

}  // namespace scada::driver
