#include "scada/driver/scheduler.h"

#include "scada/common/static_string.h"
#include "scada/common/status.h"
#include "scada/driver/compiled.h"
#include "scada/driver/plan.h"
#include "scada/driver/timing.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <vector>

namespace scada::driver {

namespace {

constexpr std::int64_t NEVER = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t EARLIEST = std::numeric_limits<std::int64_t>::min();

// a + b for a time and an interval of at least 0 (`check`), saturating.
std::int64_t later(std::int64_t time, std::int64_t interval) noexcept {
    if (interval <= 0) {
        return time;
    }
    return time > NEVER - interval ? NEVER : time + interval;
}

// now - since, 0 when the clock is before `since`, saturating (E9).
std::int64_t elapsed(std::int64_t now, std::int64_t since) noexcept {
    if (now <= since) {
        return 0;
    }
    if (since < 0 && now > NEVER + since) {
        return NEVER;
    }
    return now - since;
}

Status invalid(StaticString what) noexcept {
    return {StatusCode::INVALID_ARGUMENT, what};
}

bool valid_pacing(const Pacing& pacing) noexcept {
    return pacing.min_request_interval_ns >= 0 &&
           pacing.min_write_interval_ns >= 0;
}

// True when `request` follows a chained request in some poll group: the
// last request of a chain.
bool ends_a_chain(const ReadPlan& plan, std::uint32_t request) noexcept {
    for (const std::vector<std::uint32_t>& group : plan.by_group) {
        for (std::size_t index = 1; index < group.size(); ++index) {
            if (group[index] == request &&
                plan.requests[group[index - 1]].chained) {
                return true;
            }
        }
    }
    return false;
}

// A protocol's own plan must keep 6.5 and 6.6 too: a keep-alive or probe
// never reads a side effect, nor one request of a chain (a value without
// its companion).
Status check_extras(const ReadPlan& plan) noexcept {
    for (const std::optional<std::uint32_t>& extra :
         {plan.keepalive, plan.probe}) {
        if (extra.has_value() &&
            (plan.requests[*extra].side_effect ||
             plan.requests[*extra].chained || ends_a_chain(plan, *extra))) {
            return invalid(
                "keep-alive or probe reads a side effect or part of a chain");
        }
    }
    return {};
}

// Each request's poll group and split form.
Status check_requests(const ReadPlan& plan, std::size_t groups) noexcept {
    const std::size_t size = plan.requests.size();
    for (std::size_t index = 0; index < size; ++index) {
        const Request& request = plan.requests[index];
        // A point in no poll group is read by the identity cycle only.
        const bool identity_only =
            request.poll_group == NO_POLL_GROUP &&
            std::find(plan.identity.begin(), plan.identity.end(), index) !=
                plan.identity.end();
        if (request.poll_group >= groups && !identity_only) {
            return invalid("request of a poll group the channel does not have");
        }
        for (const std::uint32_t part : request.split) {
            if (part >= size) {
                return invalid(
                    "split form names a request the plan does not hold");
            }
            if (!plan.requests[part].split.empty()) {
                return invalid("a split part with a split form of its own");
            }
        }
    }
    return {};
}

Status check_plan(const ReadPlan& plan, std::size_t groups) noexcept {
    const std::size_t size = plan.requests.size();
    if (plan.by_group.size() != groups) {
        return invalid("plan without one request list per poll group");
    }
    for (std::size_t group = 0; group < groups; ++group) {
        for (const std::uint32_t index : plan.by_group[group]) {
            if (index >= size || plan.requests[index].poll_group != group) {
                return invalid("plan names a request it does not hold");
            }
        }
    }
    if (Status status = check_requests(plan, groups); !status.ok()) {
        return status;
    }
    if ((plan.keepalive.has_value() && *plan.keepalive >= size) ||
        (plan.probe.has_value() && *plan.probe >= size)) {
        return invalid("keep-alive or probe the plan does not hold");
    }
    // D-145: the identity cycle never reads a side effect (6.6).
    for (const std::uint32_t index : plan.identity) {
        if (index >= size) {
            return invalid("identity cycle names a request it does not hold");
        }
        if (plan.requests[index].side_effect ||
            plan.requests[index].exclusive) {
            return invalid("identity cycle reads a side effect");
        }
    }
    return check_extras(plan);
}

// Where the next request of a group's cycle is: a position in the group's
// list, and inside a split form.
struct Position {
    std::size_t index = 0;
    std::size_t part = 0;
    bool in_split = false;
    std::uint32_t request = 0;
};

// The next request of a cycle from `group`'s current position, skipping
// requests refused for good or resting (G8) and expanding split forms.
bool find_next(const std::vector<std::uint32_t>& list, const ReadPlan& plan,
               const std::vector<bool>& split, const std::vector<bool>& absent,
               const std::vector<bool>& resting, std::size_t next,
               std::size_t next_split, Position& out) noexcept {
    for (std::size_t index = next; index < list.size(); ++index) {
        const std::uint32_t request = list[index];
        if (split[request]) {
            const std::vector<std::uint32_t>& parts =
                plan.requests[request].split;
            const std::size_t from = index == next ? next_split : 0;
            for (std::size_t part = from; part < parts.size(); ++part) {
                if (!absent[parts[part]] && !resting[request] &&
                    !resting[parts[part]]) {
                    out = Position{.index = index,
                                   .part = part,
                                   .in_split = true,
                                   .request = parts[part]};
                    return true;
                }
            }
            continue;
        }
        if (!absent[request] && !resting[request]) {
            out = Position{.index = index, .request = request};
            return true;
        }
    }
    return false;
}

}  // namespace

Status Scheduler::check(const CompiledChannel& channel) noexcept {
    if (!valid_pacing(channel.pacing)) {
        return invalid("negative pacing interval");
    }
    if (std::any_of(channel.poll_groups.begin(), channel.poll_groups.end(),
                    [](const CompiledPollGroup& group) {
                        return group.period_ns <= 0;
                    })) {
        return invalid("poll group period not above 0");
    }
    for (const CompiledDevice& device : channel.devices) {
        if (!valid_pacing(device.pacing) || device.keepalive_ns < 0) {
            return invalid("negative pacing interval or keep-alive");
        }
        // A disabled device is checked too: a reload may enable it (E5).
        if (const Status status =
                check_plan(device.plan, channel.poll_groups.size());
            !status.ok()) {
            return status;
        }
    }
    return {};
}

Scheduler::Scheduler(const CompiledChannel& channel,
                     std::size_t max_pending_orders, std::int64_t now_ns)
    : channel_(&channel),
      status_(check(channel)),
      max_orders_(max_pending_orders),
      start_ns_(now_ns) {
    if (!status_.ok()) {
        return;  // no devices: `take` never chooses anything
    }
    orders_.reserve(max_pending_orders);
    devices_.resize(channel.devices.size());
    std::vector<std::uint32_t> keys;
    for (std::size_t index = 0; index < channel.devices.size(); ++index) {
        const CompiledDevice& device = channel.devices[index];
        DeviceRun& run = devices_[index];
        run.groups.resize(channel.poll_groups.size());
        for (GroupState& group : run.groups) {
            group.due_ns = now_ns;
        }
        run.split.assign(device.plan.requests.size(), false);
        run.absent.assign(device.plan.requests.size(), false);
        run.resting.assign(device.plan.requests.size(), false);
        run.never_rests.assign(device.plan.requests.size(), false);
        // OWN_UNIT is never shared: each such device is a unit of its own.
        const auto found =
            device.unit == CompiledDevice::OWN_UNIT
                ? keys.end()
                : std::find(keys.begin(), keys.end(), device.unit);
        run.unit = static_cast<std::uint32_t>(found - keys.begin());
        if (found == keys.end()) {
            keys.push_back(device.unit);
            units_.emplace_back();
        }
        Pacing& pacing = units_[run.unit].pacing;
        pacing.min_request_interval_ns =
            std::max(pacing.min_request_interval_ns,
                     device.pacing.min_request_interval_ns);
        pacing.min_write_interval_ns = std::max(
            pacing.min_write_interval_ns, device.pacing.min_write_interval_ns);
    }
}

std::int64_t Scheduler::allowed_from(std::uint32_t device,
                                     bool write) const noexcept {
    const UnitRun& unit = units_[devices_[device].unit];
    const Pacing& shared = channel_->pacing;
    std::int64_t from = EARLIEST;
    if (unit.has_request) {
        from = std::max(from, later(unit.last_request_ns,
                                    unit.pacing.min_request_interval_ns));
    }
    if (has_request_) {
        from = std::max(
            from, later(last_request_ns_, shared.min_request_interval_ns));
    }
    if (write && unit.has_write) {
        from = std::max(
            from, later(unit.last_write_ns, unit.pacing.min_write_interval_ns));
    }
    if (write && has_write_) {
        from =
            std::max(from, later(last_write_ns_, shared.min_write_interval_ns));
    }
    return from;
}

std::int64_t Scheduler::step_allowed_from(const OrderTicket& order,
                                          std::uint32_t step) const noexcept {
    // R1, R6: the operate follows its select at once, within the request
    // intervals only (the unit's and the channel's); R5: so does a cancel; a
    // READ_NOW is a read (F2). Each of them still counts as a write for the
    // write interval after it, but the READ_NOW.
    const bool operate = order.kind == OrderKind::SELECT_OPERATE && step > 0;
    const bool write = order.kind != OrderKind::READ_NOW &&
                       order.kind != OrderKind::CANCEL && !operate;
    return allowed_from(order.device, write);
}

void Scheduler::record(std::uint32_t device, bool write,
                       std::int64_t now_ns) noexcept {
    UnitRun& unit = units_[devices_[device].unit];
    unit.has_request = true;
    unit.last_request_ns = now_ns;
    has_request_ = true;
    last_request_ns_ = now_ns;
    if (write) {
        unit.has_write = true;
        unit.last_write_ns = now_ns;
        has_write_ = true;
        last_write_ns_ = now_ns;
    }
}

void Scheduler::save_undo(const Choice& choice, std::uint32_t group) noexcept {
    const DeviceRun& run = devices_[choice.device];
    undo_ = Undo{.last_request_ns = last_request_ns_,
                 .last_write_ns = last_write_ns_,
                 .chain_next = run.chain_next,
                 .chain_next_split = run.chain_next_split,
                 .identity_next = run.identity_next,
                 .identity_next_split = run.identity_next_split,
                 .choice = choice,
                 .unit = units_[run.unit],
                 .group_state = group < run.groups.size() ? run.groups[group]
                                                          : GroupState{},
                 .group = group,
                 .chain_group = run.chain_group,
                 .retry_request = run.retry_request,
                 .valid = true,
                 .has_request = has_request_,
                 .has_write = has_write_,
                 .chain = run.chain,
                 .retry = run.retry,
                 .probe_in_flight = probe_in_flight_,
                 .identity_cycle = run.identity_cycle,
                 .identity_due = run.identity_due,
                 .mode = run.mode};
}

bool Scheduler::peek(std::uint32_t device, std::uint32_t group,
                     std::uint32_t& request) const noexcept {
    const DeviceRun& run = devices_[device];
    const GroupState& state = run.groups[group];
    const ReadPlan& plan = channel_->devices[device].plan;
    Position position;
    if (!state.in_cycle ||
        !find_next(plan.by_group[group], plan, run.split, run.absent,
                   run.resting, state.next, state.next_split, position)) {
        return false;
    }
    request = position.request;
    return true;
}

void Scheduler::advance(std::uint32_t device, std::uint32_t group) noexcept {
    DeviceRun& run = devices_[device];
    GroupState& state = run.groups[group];
    const ReadPlan& plan = channel_->devices[device].plan;
    Position position;
    if (!find_next(plan.by_group[group], plan, run.split, run.absent,
                   run.resting, state.next, state.next_split, position)) {
        return;
    }
    if (position.in_split) {
        state.next = position.index;
        state.next_split = position.part + 1;
    } else {
        state.next = position.index + 1;
        state.next_split = 0;
    }
}

bool Scheduler::current_form(std::uint32_t device, std::uint32_t request,
                             std::uint32_t& form) const noexcept {
    const DeviceRun& run = devices_[device];
    if (run.split[request]) {
        for (const std::uint32_t part :
             channel_->devices[device].plan.requests[request].split) {
            if (!run.absent[part]) {
                form = part;
                return true;
            }
        }
        return false;
    }
    if (run.absent[request]) {
        return false;
    }
    form = request;
    return true;
}

bool Scheduler::unit_held_by_other(std::uint32_t device) const noexcept {
    const std::uint32_t holder = units_[devices_[device].unit].holder;
    return holder != NOBODY && holder != device;
}

bool Scheduler::unit_exclusive(std::uint32_t device) const noexcept {
    const std::uint32_t holder = units_[devices_[device].unit].holder;
    return holder != NOBODY && devices_[holder].order_active &&
           devices_[holder].order.exclusive;
}

namespace {

void update_holder(std::uint32_t& holder, std::uint32_t device,
                   bool holds) noexcept {
    if (holds) {
        holder = device;
    } else if (holder == device) {
        holder = std::numeric_limits<std::uint32_t>::max();
    }
}

}  // namespace

void Scheduler::drop_chain(std::uint32_t device) noexcept {
    DeviceRun& run = devices_[device];
    if (!run.chain) {
        return;
    }
    GroupState& state = run.groups[run.chain_group];
    state.next = run.chain_next;
    state.next_split = run.chain_next_split;
    run.chain = false;
    run.retry = false;  // the chain is read again whole
    update_holder(units_[run.unit].holder, device,
                  run.order_active && run.order.exclusive);
}

void Scheduler::end_order(std::uint32_t device) noexcept {
    DeviceRun& run = devices_[device];
    run.order_active = false;
    update_holder(units_[run.unit].holder, device, run.chain);
}

void Scheduler::complete_cycle(GroupState& state, std::int64_t period_ns,
                               std::int64_t now_ns) noexcept {
    // The next cycle is due a period after this one was, or now when it is
    // already late. A cycle that waited for pacing and ended after that
    // time missed its period because of pacing (11.1).
    // E4: the cycle ended when its last request was handed out, not at the
    // `take` that finds it complete.
    // A cycle due at EARLIEST (a device polled again, `set_polled`) is
    // followed a period after it started, not at once: else a returned
    // device is polled twice in a row, side-effect reads included (6.6;
    // review P1).
    const std::int64_t base =
        state.due_ns == EARLIEST ? state.cycle_start_ns : state.due_ns;
    const std::int64_t next_due = later(base, period_ns);
    state.limited = state.waited && state.last_sent_ns > next_due;
    state.in_cycle = false;
    state.due_ns = std::max(next_due, now_ns);
}

void Scheduler::start_due(std::int64_t now_ns) noexcept {
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        const CompiledDevice& compiled = channel_->devices[device];
        DeviceRun& run = devices_[device];
        if (!compiled.enabled || run.mode != DeviceMode::ALL) {
            continue;
        }
        for (std::uint32_t group = 0; group < run.groups.size(); ++group) {
            GroupState& state = run.groups[group];
            std::uint32_t request = 0;
            if (state.in_cycle && !peek(device, group, request)) {
                complete_cycle(state, channel_->poll_groups[group].period_ns,
                               now_ns);
                if (run.chain && run.chain_group == group) {
                    // A chain whose last requests are refused ends with its
                    // cycle (K2).
                    run.chain = false;
                    update_holder(units_[run.unit].holder, device,
                                  run.order_active && run.order.exclusive);
                }
            }
            if (state.in_cycle || state.due_ns > now_ns ||
                compiled.plan.by_group[group].empty()) {
                continue;
            }
            state.in_cycle = true;
            state.next = 0;
            state.next_split = 0;
            if (state.started) {
                *std::next(state.periods.begin(),
                           static_cast<std::ptrdiff_t>(state.period_head)) =
                    elapsed(now_ns, state.cycle_start_ns);
                state.period_head = (state.period_head + 1) % PERIODS;
                state.period_count = std::min(state.period_count + 1, PERIODS);
            }
            state.started = true;
            state.cycle_start_ns = now_ns;
            state.waited = false;
        }
    }
}

bool Scheduler::drop_unit_chain(std::uint32_t device,
                                std::uint32_t& chain_device) noexcept {
    const std::uint32_t unit = devices_[device].unit;
    for (std::uint32_t other = 0; other < devices_.size(); ++other) {
        if (devices_[other].unit == unit && devices_[other].chain) {
            drop_chain(other);
            chain_device = other;
            return true;  // one chain at most: a chain holds its unit
        }
    }
    return false;
}

Choice Scheduler::take_order_step(std::uint32_t device,
                                  std::int64_t now_ns) noexcept {
    DeviceRun& run = devices_[device];
    Choice choice{.kind = Choice::Kind::ORDER,
                  .device = device,
                  .order = run.order.id,
                  .step = run.order_step};
    // D2: no order step between the requests of a chain, an identity
    // cycle's included (D-145).
    choice.chain_dropped = drop_unit_chain(device, choice.chain_device) ||
                           drop_unit_identity(device, choice.chain_device);
    update_holder(units_[run.unit].holder, device, run.order.exclusive);
    save_undo(choice, 0);
    record(device, run.order.kind != OrderKind::READ_NOW, now_ns);
    ++run.order_step;
    if (run.order_step >= run.order.steps) {
        end_order(device);
    }
    return choice;
}

Choice Scheduler::take_poll(std::uint32_t device, std::uint32_t group,
                            std::int64_t now_ns) noexcept {
    DeviceRun& run = devices_[device];
    std::uint32_t request = 0;
    (void)peek(device, group, request);
    const Choice choice{
        .kind = Choice::Kind::POLL, .device = device, .request = request};
    save_undo(choice, group);
    if (!run.chain) {
        // Where a chain starting here is read again from (R2).
        run.chain_next = run.groups[group].next;
        run.chain_next_split = run.groups[group].next_split;
    }
    advance(device, group);
    record(device, false, now_ns);
    run.groups[group].last_sent_ns = now_ns;
    run.chain = channel_->devices[device].plan.requests[request].chained;
    run.chain_group = group;
    update_holder(units_[run.unit].holder, device,
                  run.chain || (run.order_active && run.order.exclusive));
    return choice;
}

bool Scheduler::choose_exclusive_step(std::int64_t now_ns, Choice& out,
                                      std::int64_t& wake) noexcept {
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        const DeviceRun& run = devices_[device];
        if (!run.order_active || !run.order.exclusive) {
            continue;
        }
        const std::int64_t from = step_allowed_from(run.order, run.order_step);
        if (from <= now_ns) {
            out = take_order_step(device, now_ns);
            return true;
        }
        wake = std::min(wake, from);
    }
    return false;
}

bool Scheduler::choose_order(std::int64_t now_ns, Choice& out,
                             std::int64_t& wake) noexcept {
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        DeviceRun& run = devices_[device];
        run.order_blocked = run.order_active || unit_exclusive(device);
    }
    // New orders in arrival order; per device, in its own arrival order.
    for (std::size_t index = 0; index < orders_.size(); ++index) {
        const OrderTicket order = orders_[index];
        DeviceRun& run = devices_[order.device];
        if (run.order_blocked) {
            continue;
        }
        const std::int64_t from = step_allowed_from(order, 0);
        if (from > now_ns) {
            wake = std::min(wake, from);
            run.order_blocked = true;
            continue;
        }
        orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(index));
        // A sequence of several steps holds its unit (6.6, D1); a one-step
        // order ends with its step.
        run.order = order;
        run.order.exclusive = order.exclusive ||
                              order.kind == OrderKind::SELECT_OPERATE ||
                              order.kind == OrderKind::WRITE_LIST;
        run.order_active = true;
        run.order_step = 0;
        out = take_order_step(order.device, now_ns);
        return true;
    }
    return false;
}

bool Scheduler::polls_allowed(std::uint32_t device) const noexcept {
    return channel_->devices[device].enabled &&
           devices_[device].mode == DeviceMode::ALL &&
           !unit_exclusive(device) && !unit_held_by_other(device);
}

Choice Scheduler::take_retry(std::uint32_t device,
                             std::int64_t now_ns) noexcept {
    DeviceRun& run = devices_[device];
    const std::uint32_t group =
        channel_->devices[device].plan.requests[run.retry_request].poll_group;
    const Choice choice{.kind = Choice::Kind::POLL,
                        .device = device,
                        .request = run.retry_request};
    save_undo(choice, group);
    record(device, false, now_ns);
    run.groups[group].last_sent_ns = now_ns;
    run.retry = false;
    return choice;
}

// One order (DI 6.2 step 3): a chain in progress first, then priority, then
// due time, device order. A retry is its device's next poll, due when the
// failure was seen (Q1).
void Scheduler::consider(const PollPick& pick, PollKey& best_key,
                         PollPick& best, bool& found) const noexcept {
    const DeviceRun& run = devices_[pick.device];
    const std::int64_t due =
        pick.retry ? run.retry_due_ns : run.groups[pick.group].due_ns;
    const PollKey key{!chain_open(pick),
                      channel_->poll_groups[pick.group].priority, due,
                      pick.device};
    if (!found || key < best_key) {
        best = pick;
        best_key = key;
        found = true;
    }
}

void Scheduler::consider_groups(std::uint32_t device, std::int64_t now_ns,
                                std::int64_t& wake, PollKey& best_key,
                                PollPick& best, bool& found) noexcept {
    DeviceRun& run = devices_[device];
    for (std::uint32_t group = 0; group < run.groups.size(); ++group) {
        GroupState& state = run.groups[group];
        std::uint32_t request = 0;
        // Nothing else to the unit inside a chain.
        if ((run.chain && group != run.chain_group) ||
            !peek(device, group, request)) {
            if (!state.in_cycle &&
                !channel_->devices[device].plan.by_group[group].empty()) {
                wake = std::min(wake, state.due_ns);
            }
            continue;
        }
        const std::int64_t from = allowed_from(device, false);
        if (from > now_ns) {
            state.waited = true;
            wake = std::min(wake, from);
            continue;
        }
        consider(PollPick{.device = device, .group = group}, best_key, best,
                 found);
    }
}

bool Scheduler::choose_poll(std::int64_t now_ns, Choice& out,
                            std::int64_t& wake) noexcept {
    bool found = false;
    PollPick best;
    PollKey best_key{};
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        const DeviceRun& run = devices_[device];
        if (!polls_allowed(device)) {
            continue;
        }
        if (!run.retry) {
            consider_groups(device, now_ns, wake, best_key, best, found);
            continue;
        }
        const std::int64_t from = allowed_from(device, false);
        if (from > now_ns) {
            wake = std::min(wake, from);
            continue;
        }
        const std::uint32_t group = channel_->devices[device]
                                        .plan.requests[run.retry_request]
                                        .poll_group;
        consider(PollPick{.device = device, .group = group, .retry = true},
                 best_key, best, found);
    }
    if (!found) {
        return false;
    }
    out = best.retry ? take_retry(best.device, now_ns)
                     : take_poll(best.device, best.group, now_ns);
    return true;
}

bool Scheduler::choose_keepalive(std::int64_t now_ns, Choice& out,
                                 std::int64_t& wake) noexcept {
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        const CompiledDevice& compiled = channel_->devices[device];
        const DeviceRun& run = devices_[device];
        std::uint32_t form = 0;
        if (!compiled.enabled || compiled.keepalive_ns <= 0 ||
            !compiled.plan.keepalive.has_value() ||
            run.mode != DeviceMode::ALL || run.retry ||
            units_[run.unit].holder != NOBODY || run.order_active ||
            !current_form(device, *compiled.plan.keepalive, form) ||
            run.resting[*compiled.plan.keepalive] || run.resting[form]) {
            // A resting request is a dead block: no keep-alive of it (K5).
            continue;
        }
        // Idle is the unit's silence: any request to it keeps it alive.
        const UnitRun& unit = units_[run.unit];
        const std::int64_t idle_since =
            unit.has_request ? unit.last_request_ns : start_ns_;
        const std::int64_t due = later(idle_since, compiled.keepalive_ns);
        const std::int64_t from = std::max(due, allowed_from(device, false));
        if (from > now_ns) {
            wake = std::min(wake, from);
            continue;
        }
        out = Choice{
            .kind = Choice::Kind::KEEPALIVE, .device = device, .request = form};
        save_undo(out, 0);
        record(device, false, now_ns);
        return true;
    }
    return false;
}

bool Scheduler::choose_identity(std::int64_t now_ns, Choice& out,
                                std::int64_t& wake) noexcept {
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        DeviceRun& run = devices_[device];
        const CompiledDevice& compiled = channel_->devices[device];
        if (!compiled.enabled || run.mode != DeviceMode::IDENTITY ||
            run.order_active || unit_exclusive(device) ||
            unit_held_by_other(device)) {
            continue;
        }
        const ReadPlan& plan = compiled.plan;
        if (run.retry) {
            const std::int64_t from = allowed_from(device, false);
            if (from > now_ns) {
                wake = std::min(wake, from);
                continue;
            }
            out = Choice{.kind = Choice::Kind::IDENTITY,
                         .device = device,
                         .request = run.retry_request};
            save_undo(out, 0);
            record(device, false, now_ns);
            run.retry = false;
            update_holder(units_[run.unit].holder, device,
                          plan.requests[run.retry_request].chained);
            return true;
        }
        if (!run.identity_cycle) {
            if (!run.identity_due) {
                continue;
            }
            if (run.identity_due_ns > now_ns) {
                wake = std::min(wake, run.identity_due_ns);
                continue;
            }
        }
        Position position;
        const std::size_t next = run.identity_cycle ? run.identity_next : 0;
        const std::size_t next_split =
            run.identity_cycle ? run.identity_next_split : 0;
        if (!find_next(plan.identity, plan, run.split, run.absent,
                       run.never_rests, next, next_split, position)) {
            // The cycle is complete, or has nothing to read: the engine
            // asks for the next with `identity_again`.
            run.identity_cycle = false;
            run.identity_due = false;
            update_holder(units_[run.unit].holder, device, false);
            continue;
        }
        const std::int64_t from = allowed_from(device, false);
        if (from > now_ns) {
            wake = std::min(wake, from);
            continue;
        }
        out = Choice{.kind = Choice::Kind::IDENTITY,
                     .device = device,
                     .request = position.request};
        save_undo(out, 0);
        advance_identity(device, position.index, position.part,
                         position.in_split, position.request);
        record(device, false, now_ns);
        return true;
    }
    return false;
}

void Scheduler::advance_identity(std::uint32_t device, std::size_t index,
                                 std::size_t part, bool in_split,
                                 std::uint32_t sent) noexcept {
    DeviceRun& run = devices_[device];
    const ReadPlan& plan = channel_->devices[device].plan;
    run.identity_due = false;
    run.identity_next = in_split ? index : index + 1;
    run.identity_next_split = in_split ? part + 1 : 0;
    // The cycle ends with its last request, so that `identity_again` may
    // ask for the next at once.
    Position after;
    run.identity_cycle =
        find_next(plan.identity, plan, run.split, run.absent, run.never_rests,
                  run.identity_next, run.identity_next_split, after);
    // A chain holds its unit to its end: nothing between its parts.
    update_holder(units_[run.unit].holder, device,
                  run.identity_cycle && plan.requests[sent].chained);
}

bool Scheduler::drop_unit_identity(std::uint32_t device,
                                   std::uint32_t& chain_device) noexcept {
    const std::uint32_t unit = devices_[device].unit;
    const std::uint32_t holder = units_[unit].holder;
    if (holder == NOBODY || holder == device) {
        return false;
    }
    DeviceRun& run = devices_[holder];
    if (run.mode != DeviceMode::IDENTITY || !run.identity_cycle ||
        run.order_active) {
        return false;
    }
    // In a chain of the identity cycle: read again from its start.
    run.identity_next = 0;
    run.identity_next_split = 0;
    run.retry = false;
    update_holder(units_[unit].holder, holder, false);
    chain_device = holder;
    return true;
}

bool Scheduler::due_work_pending() const noexcept {
    if (!orders_.empty()) {
        return true;
    }
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        const DeviceRun& run = devices_[device];
        if (run.order_active) {
            return true;
        }
        if (run.mode == DeviceMode::IDENTITY &&
            channel_->devices[device].enabled &&
            (run.retry || run.identity_cycle)) {
            return true;
        }
        if (run.mode != DeviceMode::ALL || !channel_->devices[device].enabled) {
            continue;
        }
        if (run.retry || run.chain) {
            return true;
        }
        for (std::uint32_t group = 0; group < run.groups.size(); ++group) {
            std::uint32_t request = 0;
            if (peek(device, group, request)) {
                return true;  // even one waiting for its spacing (D-138)
            }
        }
    }
    return false;
}

std::int64_t Scheduler::next_poll_due_ns() const noexcept {
    std::int64_t next = NEVER;
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        const DeviceRun& run = devices_[device];
        if (run.mode == DeviceMode::IDENTITY && run.identity_due &&
            channel_->devices[device].enabled) {
            next = std::min(next, run.identity_due_ns);
        }
        if (run.mode != DeviceMode::ALL || !channel_->devices[device].enabled) {
            continue;
        }
        for (std::uint32_t group = 0; group < run.groups.size(); ++group) {
            if (!channel_->devices[device].plan.by_group[group].empty()) {
                next = std::min(next, run.groups[group].due_ns);
            }
        }
    }
    return next;
}

bool Scheduler::choose_floor_probe(std::int64_t now_ns, Choice& out,
                                   std::int64_t& wake) noexcept {
    if (probe_in_flight_ || !orders_.empty()) {
        return false;
    }
    // A chain of companions in progress anywhere on the channel goes first
    // (DI 6.2 step 3): a floor probe between its parts would read the value
    // and its companion a probe time-out apart (review Q1).
    for (const DeviceRun& run : devices_) {
        if (run.chain) {
            return false;
        }
    }
    // The oldest probe past its floor, of a device whose unit is free.
    bool found = false;
    std::uint32_t chosen = 0;
    std::int64_t oldest_floor = NEVER;
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        const DeviceRun& run = devices_[device];
        if (!channel_->devices[device].enabled || !run.probe ||
            units_[run.unit].holder != NOBODY) {
            continue;
        }
        const std::int64_t floor =
            later(run.probe_due_ns, run.probe_timeout_ns * PROBE_FLOOR);
        const std::int64_t from = std::max(floor, allowed_from(device, false));
        if (from > now_ns) {
            wake = std::min(wake, std::max(from, floor_release_ns_));
            continue;
        }
        if (floor < oldest_floor) {
            found = true;
            chosen = device;
            oldest_floor = floor;
        }
    }
    if (!found) {
        return false;
    }
    // At most one probe past its floor in any PROBE_FLOOR time-outs of line
    // time after the last one (the lead's ruling N1): floor probes cost the
    // live devices at most one probe time-out in every PROBE_FLOOR + 1,
    // whatever the number of dead devices and their groups.
    if (now_ns < floor_release_ns_) {
        wake = std::min(wake, floor_release_ns_);
        return false;
    }
    const std::int64_t timeout_ns = devices_[chosen].probe_timeout_ns;
    if (!take_probe(chosen, now_ns, timeout_ns, false, out)) {
        return false;
    }
    floor_release_ns_ = later(now_ns, timeout_ns * (PROBE_FLOOR + 1));
    return true;
}

bool Scheduler::choose_probe(std::int64_t now_ns, Choice& out,
                             std::int64_t& wake) noexcept {
    // One probe of the channel at a time, and only in time no due request
    // needs (7.4), so that dead devices never starve the live ones.
    if (probe_in_flight_ || due_work_pending()) {
        return false;
    }
    const std::int64_t next_poll = next_poll_due_ns();
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        const CompiledDevice& compiled = channel_->devices[device];
        const DeviceRun& run = devices_[device];
        if (!compiled.enabled || !run.probe ||
            units_[run.unit].holder != NOBODY) {
            continue;
        }
        const std::int64_t from =
            std::max(run.probe_due_ns, allowed_from(device, false));
        if (from > now_ns) {
            wake = std::min(wake, from);
            continue;
        }
        // Q4, G7: a poll that falls due within the probe's time-out ends
        // the probe's wait then, if the window is worth a probe; unless the
        // probe has waited PROBE_FLOOR time-outs past its due time.
        const std::int64_t floor =
            later(run.probe_due_ns, run.probe_timeout_ns * PROBE_FLOOR);
        std::int64_t wait_ns = run.probe_timeout_ns;
        bool windowed = false;
        if (now_ns >= floor) {
            // Past its floor: `choose_floor_probe`'s, under its spacing.
            continue;
        }
        if (next_poll < later(now_ns, wait_ns)) {
            // The window ends before the quiet time a serial line keeps
            // after a time-out, so that the poll goes out when due.
            const std::int64_t quiet = channel_->transport == TransportKind::TCP
                                           ? 0
                                           : margin_ns(channel_->line);
            const std::int64_t window = next_poll - now_ns - quiet;
            // One windowed probe per due time (J1): a device slower than
            // the window is still found by the floor probe.
            if (window < std::min(wait_ns, PROBE_MIN_WINDOW_NS) ||
                !compiled.plan.probe.has_value() || run.windowed_tried) {
                wake = std::min(wake, floor);
                continue;
            }
            wait_ns = window;
            windowed = true;
        }
        if (take_probe(device, now_ns, wait_ns, windowed, out)) {
            return true;
        }
    }
    return false;
}

bool Scheduler::take_probe(std::uint32_t device, std::int64_t now_ns,
                           std::int64_t wait_ns, bool windowed,
                           Choice& out) noexcept {
    DeviceRun& run = devices_[device];
    const CompiledDevice& compiled = channel_->devices[device];
    if (!compiled.plan.probe.has_value() && !compiled.plan.identity.empty()) {
        // D-145: a device with an identity cycle and no probe request is
        // probed by that cycle; it goes on only if the device recovers.
        const ReadPlan& plan = compiled.plan;
        Position position;
        if (!find_next(plan.identity, plan, run.split, run.absent,
                       run.never_rests, 0, 0, position)) {
            return false;
        }
        out = Choice{.kind = Choice::Kind::PROBE,
                     .device = device,
                     .request = position.request,
                     .cycle = true,
                     .timeout_ns = wait_ns};
        save_undo(out, 0);
        set_mode(device, DeviceMode::IDENTITY);
        advance_identity(device, position.index, position.part,
                         position.in_split, position.request);
        record(device, false, now_ns);
        probe_in_flight_ = true;
        return true;
    }
    if (!compiled.plan.probe.has_value()) {
        // R8: no request may serve as a probe (all side-effect or
        // chained): one whole normal poll cycle of the device is.
        if (!start_probe_cycle(device, now_ns, out)) {
            return false;
        }
        probe_in_flight_ = true;
        return true;
    }
    out = Choice{.kind = Choice::Kind::PROBE,
                 .device = device,
                 .request = *compiled.plan.probe,
                 .timeout_ns = wait_ns,
                 .windowed = windowed};
    run.windowed_tried = run.windowed_tried || windowed;
    save_undo(out, 0);
    record(device, false, now_ns);
    probe_in_flight_ = true;
    return true;
}

bool Scheduler::chain_open(const PollPick& pick) const noexcept {
    const DeviceRun& run = devices_[pick.device];
    return run.chain ||
           (pick.retry && in_chain(pick.device, run.retry_request));
}

std::optional<std::uint32_t> Scheduler::first_group(
    std::uint32_t device) const noexcept {
    const ReadPlan& plan = channel_->devices[device].plan;
    std::optional<std::uint32_t> first;
    for (std::uint32_t group = 0; group < plan.by_group.size(); ++group) {
        if (!plan.by_group[group].empty() &&
            (!first.has_value() ||
             channel_->poll_groups[group].priority <
                 channel_->poll_groups[*first].priority)) {
            first = group;
        }
    }
    return first;
}

bool Scheduler::start_probe_cycle(std::uint32_t device, std::int64_t now_ns,
                                  Choice& out) noexcept {
    DeviceRun& run = devices_[device];
    // The group of the highest priority that has requests opens the cycle;
    // every group of the device is due at once, as when it is polled again.
    const std::optional<std::uint32_t> first = first_group(device);
    if (!first.has_value()) {
        return false;
    }
    run.mode = DeviceMode::ALL;
    for (GroupState& state : run.groups) {
        state = GroupState{.due_ns = now_ns};
    }
    start_due(now_ns);
    out = take_poll(device, *first, now_ns);
    out.kind = Choice::Kind::PROBE;
    out.cycle = true;
    return true;
}

Choice Scheduler::take(std::int64_t now_ns) noexcept {
    undo_.valid = false;
    start_due(now_ns);
    std::int64_t wake = NEVER;
    Choice out;
    // 1. The next step of an exclusive order in progress; 2. orders;
    // 3. retries and due poll groups; 4. keep-alive; 5. a probe (6.2).
    if (choose_exclusive_step(now_ns, out, wake) ||
        choose_order(now_ns, out, wake) ||
        choose_floor_probe(now_ns, out, wake) ||
        choose_identity(now_ns, out, wake) || choose_poll(now_ns, out, wake) ||
        choose_keepalive(now_ns, out, wake) ||
        choose_probe(now_ns, out, wake)) {
        return out;
    }
    if (wake == NEVER) {
        return Choice{.kind = Choice::Kind::IDLE};
    }
    return Choice{.kind = Choice::Kind::WAIT, .wake_ns = wake};
}

std::optional<std::uint64_t> Scheduler::not_sent() noexcept {
    if (!undo_.valid) {
        return std::nullopt;
    }
    undo_.valid = false;
    const Choice& choice = undo_.choice;
    DeviceRun& run = devices_[choice.device];
    const std::uint32_t holder = units_[run.unit].holder;
    units_[run.unit] = undo_.unit;
    units_[run.unit].holder = holder;
    has_request_ = undo_.has_request;
    last_request_ns_ = undo_.last_request_ns;
    has_write_ = undo_.has_write;
    last_write_ns_ = undo_.last_write_ns;
    if (choice.kind == Choice::Kind::ORDER) {
        // A one-step order ended with its step already: either way it is
        // ended now, and its outcome is the worker's.
        end_order(choice.device);
        return choice.order;
    }
    probe_in_flight_ = undo_.probe_in_flight;
    if (choice.kind == Choice::Kind::IDENTITY ||
        (choice.kind == Choice::Kind::PROBE && undo_.mode != run.mode)) {
        run.mode = undo_.mode;
        run.retry = undo_.retry;
        run.retry_request = undo_.retry_request;
        run.identity_cycle = undo_.identity_cycle;
        run.identity_due = undo_.identity_due;
        run.identity_next = undo_.identity_next;
        run.identity_next_split = undo_.identity_next_split;
        units_[run.unit].holder = undo_.unit.holder;
        return std::nullopt;
    }
    if (choice.kind == Choice::Kind::POLL) {
        run.retry = undo_.retry;
        run.retry_request = undo_.retry_request;
        run.groups[undo_.group] = undo_.group_state;
        run.chain = undo_.chain;
        run.chain_group = undo_.chain_group;
        run.chain_next = undo_.chain_next;
        run.chain_next_split = undo_.chain_next_split;
        update_holder(units_[run.unit].holder, choice.device,
                      run.chain || (run.order_active && run.order.exclusive));
    }
    return std::nullopt;
}

bool Scheduler::add_order(const OrderTicket& order) noexcept {
    const bool one_step = order.kind == OrderKind::WRITE ||
                          order.kind == OrderKind::CANCEL ||
                          order.kind == OrderKind::READ_NOW;
    const bool known =
        order.kind >= OrderKind::WRITE && order.kind <= OrderKind::READ_NOW;
    const bool two_steps = order.kind == OrderKind::SELECT_OPERATE;
    if (orders_.size() >= max_orders_ || order.device >= devices_.size() ||
        !channel_->devices[order.device].enabled || order.steps == 0 ||
        !known || (one_step && order.steps != 1) ||
        (two_steps && order.steps != 2)) {
        return false;
    }
    orders_.push_back(order);  // capacity reserved: never allocates
    return true;
}

bool Scheduler::remove_order(std::uint64_t order_id) noexcept {
    const auto found = std::find_if(
        orders_.begin(), orders_.end(),
        [&](const OrderTicket& order) { return order.id == order_id; });
    if (found == orders_.end()) {
        return false;
    }
    orders_.erase(found);
    return true;
}

void Scheduler::abort_order(std::uint32_t device) noexcept {
    if (device < devices_.size() && devices_[device].order_active) {
        end_order(device);
    }
}

bool Scheduler::refused(std::uint32_t device, std::uint32_t request) noexcept {
    if (device >= devices_.size()) {
        return false;
    }
    DeviceRun& run = devices_[device];
    const ReadPlan& plan = channel_->devices[device].plan;
    if (request >= plan.requests.size()) {
        return false;
    }
    const Request& refused_request = plan.requests[request];
    if (refused_request.split.empty() || run.split[request]) {
        run.absent[request] = true;
        return false;
    }
    run.split[request] = true;
    if (undo_.valid && undo_.choice.kind == Choice::Kind::KEEPALIVE &&
        undo_.choice.device == device) {
        return true;  // a keep-alive leaves the cycle as it is (K1)
    }
    if ((undo_.valid && undo_.choice.kind == Choice::Kind::IDENTITY &&
         undo_.choice.device == device) ||
        refused_request.poll_group >= run.groups.size()) {
        // Taken by the identity cycle (D-145): its split form next in it.
        const auto found =
            std::find(plan.identity.begin(), plan.identity.end(), request);
        if (found != plan.identity.end()) {
            run.identity_next =
                static_cast<std::size_t>(found - plan.identity.begin());
            run.identity_next_split = 0;
            run.identity_cycle = true;
        }
        return true;
    }
    // Send the split form next, in this cycle: rewind to the request.
    GroupState& state = run.groups[refused_request.poll_group];
    const std::vector<std::uint32_t>& list =
        plan.by_group[refused_request.poll_group];
    const auto found = std::find(list.begin(), list.end(), request);
    if (found != list.end()) {
        state.next = static_cast<std::size_t>(found - list.begin());
        state.next_split = 0;
        state.in_cycle = true;
    }
    return true;
}

std::size_t Scheduler::reconnect(std::span<std::uint64_t> ended) noexcept {
    std::size_t count = 0;
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        DeviceRun& run = devices_[device];
        std::fill(run.split.begin(), run.split.end(), false);
        std::fill(run.absent.begin(), run.absent.end(), false);
        std::fill(run.resting.begin(), run.resting.end(), false);
        drop_chain(device);
        if (run.order_active) {
            if (count < ended.size()) {
                ended[count] = run.order.id;
            }
            ++count;
            end_order(device);
        }
        run.retry = false;
        for (GroupState& state : run.groups) {
            state.next_split = 0;
        }
        // An identity cycle in progress is read again from its start.
        if (run.identity_cycle) {
            run.identity_cycle = false;
            run.identity_due = run.mode == DeviceMode::IDENTITY;
            run.identity_due_ns = EARLIEST;
            run.identity_next = 0;
            run.identity_next_split = 0;
            update_holder(units_[run.unit].holder, device,
                          run.order_active && run.order.exclusive);
        }
    }
    probe_in_flight_ = false;
    undo_.valid = false;
    return count;
}

void Scheduler::set_mode(std::uint32_t device, DeviceMode mode) noexcept {
    if (device >= devices_.size() || devices_[device].mode == mode) {
        return;
    }
    DeviceRun& run = devices_[device];
    drop_chain(device);
    run.mode = mode;
    run.retry = false;
    run.identity_cycle = false;
    run.identity_next = 0;
    run.identity_next_split = 0;
    // An identity cycle due at once in IDENTITY, none otherwise.
    run.identity_due = mode == DeviceMode::IDENTITY;
    run.identity_due_ns = EARLIEST;
    update_holder(units_[run.unit].holder, device,
                  run.order_active && run.order.exclusive);
    for (GroupState& state : run.groups) {
        // Polled again: every group is due at once, and the outage is not
        // published as an achieved period.
        state = GroupState{.due_ns = EARLIEST};
    }
}

void Scheduler::identity_again(std::uint32_t device,
                               std::int64_t due_ns) noexcept {
    if (device >= devices_.size() ||
        devices_[device].mode != DeviceMode::IDENTITY ||
        devices_[device].identity_cycle) {
        return;
    }
    devices_[device].identity_due = true;
    devices_[device].identity_due_ns = due_ns;
}

void Scheduler::set_probe(std::uint32_t device, std::int64_t due_ns,
                          std::int64_t timeout_ns) noexcept {
    if (device < devices_.size()) {
        DeviceRun& run = devices_[device];
        if (!run.probe || run.probe_due_ns != due_ns) {
            run.windowed_tried = false;  // a new due time (J1)
        }
        devices_[device].probe = true;
        devices_[device].probe_due_ns = due_ns;
        devices_[device].probe_timeout_ns =
            std::max<std::int64_t>(timeout_ns, 0);
    }
}

bool Scheduler::in_chain(std::uint32_t device,
                         std::uint32_t request) const noexcept {
    if (device >= devices_.size()) {
        return false;
    }
    const ReadPlan& plan = channel_->devices[device].plan;
    if (request >= plan.requests.size()) {
        return false;
    }
    const Request& planned = plan.requests[request];
    if (planned.chained) {
        return true;
    }
    // The last request of a chain is not `chained`; the one before it is.
    if (planned.poll_group >= plan.by_group.size()) {
        return false;
    }
    const std::vector<std::uint32_t>& list = plan.by_group[planned.poll_group];
    const auto found = std::find(list.begin(), list.end(), request);
    return found != list.end() && found != list.begin() &&
           plan.requests[*(found - 1)].chained;
}

void Scheduler::rest(std::uint32_t device, std::uint32_t request,
                     bool resting) noexcept {
    // No request of a chain of companions rests, first, middle or last: a
    // chain is read whole every period, so that nothing of it is read and
    // thrown away or left GOOD and frozen (the lead's ruling K1).
    if (device >= devices_.size() ||
        request >= devices_[device].resting.size() ||
        in_chain(device, request)) {
        return;
    }
    devices_[device].resting[request] = resting;
}

void Scheduler::clear_probe(std::uint32_t device) noexcept {
    if (device < devices_.size()) {
        devices_[device].probe = false;
    }
}

void Scheduler::retry(std::uint32_t device, std::uint32_t request,
                      std::int64_t now_ns) noexcept {
    if (device >= devices_.size() || devices_[device].mode == DeviceMode::OFF ||
        request >= channel_->devices[device].plan.requests.size() ||
        channel_->devices[device].plan.requests[request].side_effect) {
        return;  // a side-effect read is never sent twice (6.6)
    }
    devices_[device].retry = true;
    devices_[device].retry_request = request;
    devices_[device].retry_due_ns = now_ns;
}

std::int64_t Scheduler::achieved_period_ns(std::uint32_t group,
                                           std::int64_t now_ns) const noexcept {
    std::int64_t longest = 0;
    for (const DeviceRun& run : devices_) {
        if (group >= run.groups.size() || !run.groups[group].started) {
            continue;
        }
        const GroupState& state = run.groups[group];
        std::int64_t value = 0;
        if (state.period_count > 0) {
            // Each period is at most the age of the process: no overflow.
            const std::int64_t sum = std::accumulate(
                state.periods.begin(),
                std::next(state.periods.begin(),
                          static_cast<std::ptrdiff_t>(state.period_count)),
                std::int64_t{0});
            value = sum / static_cast<std::int64_t>(state.period_count);
        }
        if (state.in_cycle || now_ns >= state.due_ns) {
            // A running or overdue cycle already longer counts (K4).
            value = std::max(value, elapsed(now_ns, state.cycle_start_ns));
        }
        longest = std::max(longest, value);
    }
    return longest;
}

bool Scheduler::pacing_limited(std::uint32_t device,
                               std::int64_t now_ns) const noexcept {
    if (device >= devices_.size()) {
        return false;
    }
    for (std::uint32_t group = 0; group < devices_[device].groups.size();
         ++group) {
        const GroupState& state = devices_[device].groups[group];
        const std::int64_t next_due =
            later(state.due_ns, channel_->poll_groups[group].period_ns);
        // A running cycle with no request left ended at its last send (the
        // `take` that closes it comes later); one with requests left is
        // still running at `now_ns`.
        std::uint32_t request = 0;
        const std::int64_t end =
            peek(device, group, request) ? now_ns : state.last_sent_ns;
        if (state.limited ||
            (state.in_cycle && state.waited && end > next_due)) {
            return true;
        }
    }
    return false;
}

}  // namespace scada::driver
