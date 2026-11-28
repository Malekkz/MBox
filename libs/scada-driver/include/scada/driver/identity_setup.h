#ifndef SCADA_DRIVER_IDENTITY_SETUP_H
#define SCADA_DRIVER_IDENTITY_SETUP_H

// The values the engine gives the failure tracker and the scheduler for a
// device's identity check (driver interface contract 3.6, 7.6; decision
// D-147), as pure functions of the compiled plan and the tracker's state, so
// that the engine only calls them (review of PR 171).

#include "scada/common/realtime.h"
#include "scada/driver/compiled.h"
#include "scada/driver/failure.h"
#include "scada/driver/plan.h"
#include "scada/driver/scheduler.h"

#include <cstdint>
#include <span>

namespace scada::driver {

// How long a pending check may take before a value it cannot read is a
// mismatch (ruling item 2): max(`startup_grace_ns`, 3 periods of the longest
// group of the device's identity requests). For identity requests in no
// group, 3 periods of the device's longest group; `startup_grace_ns` alone
// when the device has no group at all. Saturates.
[[nodiscard]] std::int64_t identity_wait_ns(
    std::int64_t startup_grace_ns, const ReadPlan& plan,
    std::span<const CompiledPollGroup> groups) noexcept;

// How long a device runs without a restart before a restart re-arms the
// grace again (ruling item 6): the longest period of the channel's groups,
// so that every group of the device has had a full period; NEVER_NS for a
// channel without groups.
[[nodiscard]] std::int64_t full_period_ns(
    std::span<const CompiledPollGroup> groups) noexcept;

// Whether `point` of `device` is read by an identity check or a restart
// rule (3.6, 7.6): a protocol's plan step gives such a point to the planner
// as a `PlanPoint::identity`, in no poll group when the point has none.
[[nodiscard]] bool reads_identity(const CompiledDevice& device,
                                  std::uint32_t point) noexcept;

// What the scheduler reads of a device in tracker state `state` (D-147):
// - OFF for DISABLED, CONNECTING, DEMOTED and REFUSING (a demoted device is
//   reached by its probe alone);
// - IDENTITY for MISMATCH, and for INITIALISING or RUNNING while its check
//   is pending (`identity_pending`);
// - ALL otherwise.
[[nodiscard]] SCADA_RT DeviceMode
scheduler_mode(DeviceStatus state, bool identity_pending) noexcept;

// Whether a device in tracker state `state` takes a write, an order step
// or a fail-safe write: RUNNING only. INITIALISING (its identity check
// pending or a first poll not yet answered) and MISMATCH take none, their
// fail-safe writes included: the interim strict default of D-147 until the
// owner rules (the lead's instruction). The command and fail-safe slices
// call it before they queue a write.
[[nodiscard]] SCADA_RT bool accepts_writes(DeviceStatus state) noexcept;

}  // namespace scada::driver

#endif  // SCADA_DRIVER_IDENTITY_SETUP_H
