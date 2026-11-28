#include "scada/driver/identity_setup.h"

#include "scada/driver/compiled.h"
#include "scada/driver/failure.h"
#include "scada/driver/identity.h"
#include "scada/driver/plan.h"
#include "scada/driver/scheduler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::driver {

namespace {

// The identity check reads its round in about one period of its slowest
// group; three of them are three rounds (ruling item 2).
constexpr std::int64_t ROUNDS = 3;

std::int64_t times_rounds(std::int64_t period_ns) noexcept {
    return period_ns > NEVER_NS / ROUNDS ? NEVER_NS : period_ns * ROUNDS;
}

}  // namespace

std::int64_t identity_wait_ns(
    std::int64_t startup_grace_ns, const ReadPlan& plan,
    std::span<const CompiledPollGroup> groups) noexcept {
    std::int64_t identity_period = 0;
    for (const std::uint32_t request : plan.identity) {
        const std::uint32_t group = plan.requests[request].poll_group;
        if (group < groups.size()) {
            identity_period =
                std::max(identity_period, groups[group].period_ns);
        }
    }
    if (identity_period == 0) {
        // Identity requests in no group: the device's slowest group.
        for (std::size_t group = 0;
             group < plan.by_group.size() && group < groups.size(); ++group) {
            if (!plan.by_group[group].empty()) {
                identity_period =
                    std::max(identity_period, groups[group].period_ns);
            }
        }
    }
    return std::max(startup_grace_ns, times_rounds(identity_period));
}

std::int64_t full_period_ns(
    std::span<const CompiledPollGroup> groups) noexcept {
    if (groups.empty()) {
        return NEVER_NS;
    }
    return std::max_element(groups.begin(), groups.end(),
                            [](const CompiledPollGroup& left,
                               const CompiledPollGroup& right) {
                                return left.period_ns < right.period_ns;
                            })
        ->period_ns;
}

bool reads_identity(const CompiledDevice& device,
                    std::uint32_t point) noexcept {
    return std::any_of(device.identify.begin(), device.identify.end(),
                       [&](const IdentityCheck& check) {
                           return check.point == point;
                       }) ||
           std::any_of(
               device.restart_indication.begin(),
               device.restart_indication.end(),
               [&](const RestartRule& rule) { return rule.point == point; });
}

DeviceMode scheduler_mode(DeviceStatus state, bool identity_pending) noexcept {
    switch (state) {
        case DeviceStatus::MISMATCH:
            return DeviceMode::IDENTITY;
        case DeviceStatus::INITIALISING:
        case DeviceStatus::RUNNING:
            return identity_pending ? DeviceMode::IDENTITY : DeviceMode::ALL;
        case DeviceStatus::DISABLED:
        case DeviceStatus::CONNECTING:
        case DeviceStatus::DEMOTED:
        case DeviceStatus::REFUSING:
            return DeviceMode::OFF;
    }
    return DeviceMode::OFF;
}

}  // namespace scada::driver
