#include "scada/driver/scheduler.h"

#include "scada/driver/compiled.h"
#include "scada/driver/plan.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <tuple>
#include <vector>

namespace scada::driver {

namespace {

constexpr std::int64_t NEVER = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t EARLIEST = std::numeric_limits<std::int64_t>::min();

// a + b for a time and an interval of at most a minute (CS 4.6), saturating.
std::int64_t later(std::int64_t time, std::int64_t interval) noexcept {
    return time > NEVER - interval ? NEVER : time + interval;
}

// Where the next request of a group's cycle is: a position in the group's
// list, and inside a split form.
struct Position {
    std::size_t index = 0;
    std::size_t part = 0;
    bool in_split = false;
    std::uint32_t request = 0;
};

}  // namespace

Scheduler::Scheduler(const CompiledChannel& channel,
                     std::size_t max_pending_orders, std::int64_t now_ns)
    : channel_(&channel),
      devices_(channel.devices.size()),
      max_orders_(max_pending_orders),
      start_ns_(now_ns) {
    orders_.reserve(max_pending_orders);
    for (std::size_t index = 0; index < channel.devices.size(); ++index) {
        const CompiledDevice& device = channel.devices[index];
        DeviceRun& run = devices_[index];
        run.groups.resize(channel.poll_groups.size());
        for (GroupState& group : run.groups) {
            group.due_ns = now_ns;
        }
        run.split.assign(device.plan.requests.size(), false);
        run.absent.assign(device.plan.requests.size(), false);
    }
}

std::int64_t Scheduler::allowed_from(std::uint32_t device,
                                     bool write) const noexcept {
    const DeviceRun& run = devices_[device];
    const Pacing& own = channel_->devices[device].pacing;
    const Pacing& shared = channel_->pacing;
    std::int64_t from = EARLIEST;
    if (run.has_request) {
        from = std::max(
            from, later(run.last_request_ns, own.min_request_interval_ns));
    }
    if (has_request_) {
        from = std::max(
            from, later(last_request_ns_, shared.min_request_interval_ns));
    }
    if (write && run.has_write) {
        from =
            std::max(from, later(run.last_write_ns, own.min_write_interval_ns));
    }
    if (write && has_write_) {
        from =
            std::max(from, later(last_write_ns_, shared.min_write_interval_ns));
    }
    return from;
}

void Scheduler::record(std::uint32_t device, bool write,
                       std::int64_t now_ns) noexcept {
    DeviceRun& run = devices_[device];
    run.has_request = true;
    run.last_request_ns = now_ns;
    has_request_ = true;
    last_request_ns_ = now_ns;
    if (write) {
        run.has_write = true;
        run.last_write_ns = now_ns;
        has_write_ = true;
        last_write_ns_ = now_ns;
    }
}

namespace {

// The next request of a cycle from `group`'s current position, skipping
// requests refused for good and expanding split forms.
bool find_next(const std::vector<std::uint32_t>& list, const ReadPlan& plan,
               const std::vector<bool>& split, const std::vector<bool>& absent,
               std::size_t next, std::size_t next_split,
               Position& out) noexcept {
    for (std::size_t index = next; index < list.size(); ++index) {
        const std::uint32_t request = list[index];
        if (split[request]) {
            const std::vector<std::uint32_t>& parts =
                plan.requests[request].split;
            const std::size_t from = index == next ? next_split : 0;
            for (std::size_t part = from; part < parts.size(); ++part) {
                if (!absent[parts[part]]) {
                    out = Position{.index = index,
                                   .part = part,
                                   .in_split = true,
                                   .request = parts[part]};
                    return true;
                }
            }
            continue;
        }
        if (!absent[request]) {
            out = Position{.index = index, .request = request};
            return true;
        }
    }
    return false;
}

}  // namespace

bool Scheduler::peek(std::uint32_t device, std::uint32_t group,
                     std::uint32_t& request) const noexcept {
    const DeviceRun& run = devices_[device];
    const GroupState& state = run.groups[group];
    const ReadPlan& plan = channel_->devices[device].plan;
    Position position;
    if (!state.in_cycle ||
        !find_next(plan.by_group[group], plan, run.split, run.absent,
                   state.next, state.next_split, position)) {
        return false;
    }
    request = position.request;
    return true;
}

void Scheduler::advance(std::uint32_t device, std::uint32_t group,
                        std::int64_t /*now_ns*/) noexcept {
    DeviceRun& run = devices_[device];
    GroupState& state = run.groups[group];
    const ReadPlan& plan = channel_->devices[device].plan;
    Position position;
    if (!find_next(plan.by_group[group], plan, run.split, run.absent,
                   state.next, state.next_split, position)) {
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

bool Scheduler::start_due(std::int64_t now_ns) noexcept {
    bool started = false;
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        const CompiledDevice& compiled = channel_->devices[device];
        if (!compiled.enabled) {
            continue;
        }
        DeviceRun& run = devices_[device];
        for (std::uint32_t group = 0; group < run.groups.size(); ++group) {
            GroupState& state = run.groups[group];
            std::uint32_t request = 0;
            const bool chained_here = run.chain && run.chain_group == group;
            if (state.in_cycle && !peek(device, group, request) &&
                !chained_here) {
                // The cycle is complete: the next one is due a period after
                // this one was, or now when it is already late.
                state.in_cycle = false;
                state.due_ns = std::max(
                    later(state.due_ns, channel_->poll_groups[group].period_ns),
                    now_ns);
            }
            if (state.in_cycle || state.due_ns > now_ns ||
                compiled.plan.by_group[group].empty()) {
                continue;
            }
            state.in_cycle = true;
            state.next = 0;
            state.next_split = 0;
            if (state.started) {
                state.last_period_ns = now_ns - state.cycle_start_ns;
            }
            state.started = true;
            state.cycle_start_ns = now_ns;
            run.limited = false;
            started = true;
        }
    }
    return started;
}

Choice Scheduler::take_order_step(std::uint32_t device,
                                  std::int64_t now_ns) noexcept {
    DeviceRun& run = devices_[device];
    const Choice choice{.kind = Choice::Kind::ORDER,
                        .device = device,
                        .order = run.order.id,
                        .step = run.order_step};
    record(device, true, now_ns);
    ++run.order_step;
    if (run.order_step >= run.order.steps) {
        run.order_active = false;
    }
    return choice;
}

Choice Scheduler::take_poll(std::uint32_t device, std::uint32_t group,
                            std::int64_t now_ns) noexcept {
    DeviceRun& run = devices_[device];
    std::uint32_t request = 0;
    (void)peek(device, group, request);
    advance(device, group, now_ns);
    record(device, false, now_ns);
    run.chain = channel_->devices[device].plan.requests[request].chained;
    run.chain_group = group;
    return Choice{
        .kind = Choice::Kind::POLL, .device = device, .request = request};
}

bool Scheduler::choose_order(std::int64_t now_ns, Choice& out,
                             std::int64_t& wake) noexcept {
    // Steps of orders already started whose sequence is not exclusive.
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        DeviceRun& run = devices_[device];
        run.order_blocked = run.chain || run.order_active;
        if (!run.order_active || run.order.exclusive) {
            continue;
        }
        const std::int64_t from = allowed_from(device, true);
        if (from <= now_ns) {
            out = take_order_step(device, now_ns);
            return true;
        }
        wake = std::min(wake, from);
    }
    // New orders in arrival order; per device, in its own arrival order.
    for (std::size_t index = 0; index < orders_.size(); ++index) {
        const OrderTicket order = orders_[index];
        DeviceRun& run = devices_[order.device];
        if (run.order_blocked) {
            continue;
        }
        const std::int64_t from = allowed_from(order.device, true);
        if (from > now_ns) {
            wake = std::min(wake, from);
            run.order_blocked = true;
            continue;
        }
        orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(index));
        run.order_active = true;
        run.order = order;
        run.order_step = 0;
        out = take_order_step(order.device, now_ns);
        return true;
    }
    return false;
}

bool Scheduler::choose_poll(std::int64_t now_ns, Choice& out,
                            std::int64_t& wake) noexcept {
    bool found = false;
    std::uint32_t best_device = 0;
    std::uint32_t best_group = 0;
    auto key = [&](std::uint32_t device, std::uint32_t group) {
        return std::tuple(channel_->poll_groups[group].priority,
                          devices_[device].groups[group].due_ns, device);
    };
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        DeviceRun& run = devices_[device];
        if (!channel_->devices[device].enabled || run.chain ||
            (run.order_active && run.order.exclusive)) {
            continue;
        }
        for (std::uint32_t group = 0; group < run.groups.size(); ++group) {
            std::uint32_t request = 0;
            if (!peek(device, group, request)) {
                if (!run.groups[group].in_cycle &&
                    !channel_->devices[device].plan.by_group[group].empty()) {
                    wake = std::min(wake, run.groups[group].due_ns);
                }
                continue;
            }
            const std::int64_t from = allowed_from(device, false);
            if (from > now_ns) {
                run.limited = true;
                wake = std::min(wake, from);
                continue;
            }
            if (!found || key(device, group) < key(best_device, best_group)) {
                found = true;
                best_device = device;
                best_group = group;
            }
        }
    }
    if (found) {
        out = take_poll(best_device, best_group, now_ns);
    }
    return found;
}

bool Scheduler::choose_keepalive(std::int64_t now_ns, Choice& out,
                                 std::int64_t& wake) noexcept {
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        const CompiledDevice& compiled = channel_->devices[device];
        const DeviceRun& run = devices_[device];
        if (!compiled.enabled || compiled.keepalive_ns <= 0 ||
            !compiled.plan.keepalive.has_value() || run.chain ||
            run.order_active) {
            continue;
        }
        const std::int64_t idle_since =
            run.has_request ? run.last_request_ns : start_ns_;
        const std::int64_t due = later(idle_since, compiled.keepalive_ns);
        const std::int64_t from = std::max(due, allowed_from(device, false));
        if (from > now_ns) {
            wake = std::min(wake, from);
            continue;
        }
        record(device, false, now_ns);
        out = Choice{.kind = Choice::Kind::KEEPALIVE,
                     .device = device,
                     .request = *compiled.plan.keepalive};
        return true;
    }
    return false;
}

Choice Scheduler::take(std::int64_t now_ns) noexcept {
    (void)start_due(now_ns);
    std::int64_t wake = NEVER;
    Choice out;
    // 1. The next step of an exclusive order in progress, or of a chain of
    //    companion requests: nothing else may reach that device between.
    for (std::uint32_t device = 0; device < devices_.size(); ++device) {
        DeviceRun& run = devices_[device];
        if (run.order_active && run.order.exclusive) {
            const std::int64_t from = allowed_from(device, true);
            if (from <= now_ns) {
                return take_order_step(device, now_ns);
            }
            wake = std::min(wake, from);
            continue;
        }
        if (run.chain) {
            std::uint32_t request = 0;
            if (!peek(device, run.chain_group, request)) {
                run.chain = false;
                continue;
            }
            const std::int64_t from = allowed_from(device, false);
            if (from <= now_ns) {
                return take_poll(device, run.chain_group, now_ns);
            }
            wake = std::min(wake, from);
        }
    }
    // 2. Orders; 3. due poll groups; 4. keep-alive (6.2).
    if (choose_order(now_ns, out, wake) || choose_poll(now_ns, out, wake) ||
        choose_keepalive(now_ns, out, wake)) {
        return out;
    }
    if (wake == NEVER) {
        return Choice{.kind = Choice::Kind::IDLE};
    }
    return Choice{.kind = Choice::Kind::WAIT, .wake_ns = wake};
}

bool Scheduler::add_order(const OrderTicket& order) noexcept {
    if (orders_.size() >= max_orders_ || order.device >= devices_.size() ||
        order.steps == 0) {
        return false;
    }
    orders_.push_back(order);  // capacity reserved: never allocates
    return true;
}

bool Scheduler::refused(std::uint32_t device, std::uint32_t request) noexcept {
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

void Scheduler::reconnect() noexcept {
    for (DeviceRun& run : devices_) {
        std::fill(run.split.begin(), run.split.end(), false);
        std::fill(run.absent.begin(), run.absent.end(), false);
        run.chain = false;
        for (GroupState& state : run.groups) {
            state.next_split = 0;
        }
    }
}

std::int64_t Scheduler::achieved_period_ns(std::uint32_t group) const noexcept {
    std::int64_t longest = 0;
    for (const DeviceRun& run : devices_) {
        if (group < run.groups.size()) {
            longest = std::max(longest, run.groups[group].last_period_ns);
        }
    }
    return longest;
}

bool Scheduler::pacing_limited(std::uint32_t device) const noexcept {
    return device < devices_.size() && devices_[device].limited;
}

}  // namespace scada::driver
