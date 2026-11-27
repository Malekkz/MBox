#ifndef SCADA_DRIVER_SCHEDULER_H
#define SCADA_DRIVER_SCHEDULER_H

// The choice of a channel worker's next request (driver interface contract
// 6.2 to 6.6): orders first, then due poll groups by priority and due time,
// then keep-alive, within the pacing of each physical unit and of the
// channel. Pure logic over an injected clock: the caller passes the
// monotonic time, so the same inputs always give the same choices.
//
// Failure handling (7.4) sets which devices are polled, asks for retries
// and for probes of demoted devices (6.2, step 5); the orders' content
// comes with the command slices: here an order is a ticket with a kind and
// a number of steps.
//
// Threads (2.4): only the channel worker calls a scheduler. The command
// thread hands orders to the worker through the channel's order queue; the
// worker drains it into `add_order` at each transaction boundary. Since the
// command thread admits at most `max_pending_writes` orders a channel has
// not answered, `add_order` refuses only when that rule was broken; the
// worker then posts the order's outcome REJECTED, BUSY (8.8).

#include "scada/common/realtime.h"
#include "scada/common/status.h"
#include "scada/driver/compiled.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

namespace scada::driver {

// What an order does on the wire (8.3), which decides its pacing (6.4).
enum class OrderKind : std::uint8_t {
    WRITE = 1,       // one write
    WRITE_LIST,      // a list of writes (8.5): exclusive, the write
                     // interval between its writes
    SELECT_OPERATE,  // select, then operate at once (8.6): exclusive
    CANCEL,          // the end of a selection (8.6): one write, paced by
                     // the request interval only (R5)
    READ_NOW,        // one read (8.14): no write interval
};

// An order the command thread handed to the worker. `steps` requests;
// `exclusive`: no other request to its physical unit between them (6.6). A
// list of writes and a select and operate are always exclusive; a select
// and operate has two steps; a write, a cancel and a READ_NOW have one.
struct OrderTicket {
    std::uint64_t id = 0;
    std::uint32_t device = 0;
    OrderKind kind = OrderKind::WRITE;
    std::uint32_t steps = 1;  // >= 1
    bool exclusive = false;
};

// What a device is read for (3.5, 3.6; D-147): nothing (DISABLED,
// CONNECTING, DEMOTED, REFUSING), every poll group (INITIALISING once its
// identity check passed, RUNNING), or only its identity cycle
// (`ReadPlan::identity`: INITIALISING while its identity check is pending,
// MISMATCH).
enum class DeviceMode : std::uint8_t {
    OFF = 1,
    ALL,
    IDENTITY,
};

// What the worker sends next.
struct Choice {
    enum class Kind : std::uint8_t {
        IDLE = 1,   // nothing to do until an order arrives
        WAIT,       // nothing allowed before `wake_ns`
        ORDER,      // step `step` of order `order` on `device`
        POLL,       // request `request` of `device`'s plan
        KEEPALIVE,  // the keep-alive request `request` of `device`
        PROBE,      // the probe request `request` of demoted `device` (7.4)
        IDENTITY,   // request `request` of `device`'s identity cycle (D-147)
    };
    Kind kind = Kind::IDLE;
    std::uint32_t device = 0;
    std::uint32_t request = 0;
    std::uint64_t order = 0;
    std::uint32_t step = 0;
    std::int64_t wake_ns = std::numeric_limits<std::int64_t>::max();
    // An ORDER that interrupted a chain of companions (5.5) on its physical
    // unit: the worker drops what it holds of the chain of `chain_device`,
    // which publishes nothing and is read again from its first request
    // (rulings R2, D2).
    bool chain_dropped = false;
    std::uint32_t chain_device = 0;
    // A PROBE that opens one whole normal poll cycle of its device, for a
    // device whose plan has no probe request (the lead's ruling R8): its
    // samples are published as a poll's; the cycle goes on only if the
    // device recovers.
    bool cycle = false;
    // A PROBE's time-out: its device's, at most 1 s, shortened to end when
    // the next poll falls due (review G7); 0 for every other choice.
    std::int64_t timeout_ns = 0;
    // The probe's time-out was shortened to a window: a time-out of it is no
    // failed probe (review J1).
    bool windowed = false;
    // A PROBE that is the first request of its device's identity cycle, for
    // a device whose plan has no probe request (D-147): its samples are the
    // identity check's, never published as a poll's.
    bool identity_probe = false;
    // A KEEPALIVE that is the first request of its device's identity cycle,
    // for a device in IDENTITY (D-147, PR 171 S8): its samples are the
    // identity check's.
    bool identity_keepalive = false;
};

class Scheduler {
public:
    // The periods `achieved_period_ns` averages (11.1).
    static constexpr std::size_t PERIODS = 10;

    // Whether a scheduler can run `channel`: every enabled device's plan
    // has one list per poll group and only valid request indices, every
    // poll group a period above 0, every pacing interval and keep-alive at
    // least 0 (CS refuses the rest).
    [[nodiscard]] static Status check(const CompiledChannel& channel) noexcept;

    // Allocates, sized for `channel`, which must outlive the scheduler (the
    // compiled configuration; a reload builds a new scheduler state, slice
    // 8). `max_pending_orders` is the channel's `max_pending_writes`. Every
    // poll group of every enabled device is first due at `now_ns`. A channel
    // that fails `check` gives a scheduler whose `status` is that error and
    // which never chooses anything.
    Scheduler(const CompiledChannel& channel, std::size_t max_pending_orders,
              std::int64_t now_ns);

    [[nodiscard]] Status status() const noexcept { return status_; }

    // Chooses the next request at `now_ns` and records it as sent then
    // (6.2): the next step of an exclusive order in progress first, then
    // the steps of other orders and new orders in arrival order (any order
    // step interrupts a chain of companions open on its unit, which may be
    // a sibling device's: `Choice::chain_device`; R2), then the next
    // request of a due poll group, a chain in progress first, then by
    // priority, due time and device order, then a keep-alive (6.5). Nothing
    // is chosen that the pacing of its unit or of the channel does not
    // allow yet (6.4); a unit that waits does not hold up the others.
    // Devices that share a unit (`CompiledDevice::unit`) share its pacing,
    // and an exclusive order or a chain on one holds all of them (R3).
    // Orders may starve a chain on a busy unit; the achieved period shows it
    // (R7). `now_ns` is the monotonic clock: never negative, never going
    // back. Never allocates.
    SCADA_RT Choice take(std::int64_t now_ns) noexcept;

    // The last choice of `take` was not sent (an order step expired, an
    // encode failed): the pacing is as before it. A poll or keep-alive is
    // chosen again; an order ends, and its outcome is the worker's: returns
    // the order's id then. Only valid right after `take`. Never allocates.
    SCADA_RT std::optional<std::uint64_t> not_sent() noexcept;

    // Queues an order; false when it is malformed (steps for its kind, a
    // device out of range or disabled) or `max_pending_orders` are queued.
    // Never allocates.
    SCADA_RT bool add_order(const OrderTicket& order) noexcept;

    // Removes the queued order `id`; false when it is not queued (it has
    // started, or was never added). Never allocates.
    SCADA_RT bool remove_order(std::uint64_t order_id) noexcept;

    // Ends the order in progress on `device`, if any: its remaining steps
    // are not sent (a list whose write failed, an expired order). Never
    // allocates.
    SCADA_RT void abort_order(std::uint32_t device) noexcept;

    // The device refused request `request` (just taken) as a whole as an
    // illegal address (DecodeResult::SPLIT). A planned request with a split
    // form is replaced by its split form, kept until `reconnect` (6.3); a
    // poll sends it next in the same cycle, a keep-alive leaves the cycle as
    // it is. Any other request is no longer sent until `reconnect`, and the
    // caller writes its points ABSENT. Returns true when the split form was
    // taken.
    SCADA_RT bool refused(std::uint32_t device, std::uint32_t request) noexcept;

    // A new connection: every request is back to its planned form (6.3);
    // orders in progress end (a selection does not survive a reconnect,
    // 8.6 rule 3), queued orders stay; a chain in progress is read again
    // from its first request. Writes the ids of the orders it ended into
    // `ended` (at most one per device, so a span of the device count holds
    // them all) and returns how many it ended; ids beyond the span's size
    // are not written.
    std::size_t reconnect(std::span<std::uint64_t> ended = {}) noexcept;

    // The achieved period of poll group `group` (`sys.drv.<channel>.
    // period.<group>`, 11.1): per device, the average of its last
    // `PERIODS` periods (from one cycle's start to the next), or the age of
    // its running cycle at `now_ns` when that is already longer; the
    // channel's is the longest over its devices. 0 before any cycle started.
    [[nodiscard]] std::int64_t achieved_period_ns(
        std::uint32_t group, std::int64_t now_ns) const noexcept;

    // True when a poll group of the device misses its period because of
    // pacing (11.1, `sys.dev.<device>.pacing_limited`): its last cycle, or
    // its running one at `now_ns`, waited for a pacing interval and ended
    // (or is still running) after its next due time.
    [[nodiscard]] bool pacing_limited(std::uint32_t device,
                                      std::int64_t now_ns) const noexcept;

    // What `device` is read for (D-147). A change drops its chain and
    // retry. Leaving OFF makes every group of it due at once; between ALL
    // and IDENTITY each group keeps its due time and its place in its
    // cycle, and the time the device spent in IDENTITY is not an achieved
    // period. In IDENTITY the device sends only its identity cycle, the
    // requests of `plan.identity` in order within its unit's pacing: no
    // group cycle starts; the keep-alive is the cycle's first request; a
    // request of a chain holds the unit until the chain ends; a retry of an
    // identity request is an IDENTITY choice. The cycle takes its turn
    // among the polls of the channel by its due time and the priority of
    // its request's group (255 for a request in no group), and is no
    // achieved period and no pacing limit. Entering IDENTITY makes a cycle
    // due, at once or one identity period after the last cycle started
    // (`identity_period_ns`); `identity_again` makes the next. Never
    // allocates.
    SCADA_RT void set_mode(std::uint32_t device, DeviceMode mode) noexcept;
    [[nodiscard]] DeviceMode mode(std::uint32_t device) const noexcept {
        return device < devices_.size() ? devices_[device].mode
                                        : DeviceMode::OFF;
    }
    // `set_mode` with ALL or OFF.
    void set_polled(std::uint32_t device, bool polled) noexcept {
        set_mode(device, polled ? DeviceMode::ALL : DeviceMode::OFF);
    }
    // Another identity cycle of `device`, due at `due_ns` or one identity
    // period after the last cycle started, whichever is later: a re-check
    // of a device in MISMATCH at its back-off, or a check without a verdict
    // yet (D-147). The floor keeps a device that never gives a verdict (a
    // busy answer for ever) to one cycle per period (PR 171 M3). Ignored
    // unless the device is in IDENTITY with no cycle in progress. Never
    // allocates.
    SCADA_RT void identity_again(std::uint32_t device,
                                 std::int64_t due_ns) noexcept;
    // The identity period of `device`: the shortest period of the poll
    // groups of its identity requests, or of all its groups when none of
    // them is in a group; 0 when it has no group at all.
    [[nodiscard]] std::int64_t identity_period_ns(
        std::uint32_t device) const noexcept {
        return device < devices_.size() ? devices_[device].identity_period_ns
                                        : 0;
    }
    // Whether `device` has an identity cycle in progress.
    [[nodiscard]] bool identity_running(std::uint32_t device) const noexcept {
        return device < devices_.size() && devices_[device].identity_cycle;
    }

    // A probe waits for a poll that falls due within its time-out at most
    // this many time-outs past its own due time (D-138, Q4).
    static constexpr std::int64_t PROBE_FLOOR = 10;
    // The shortest window before the next due poll that a probe is sent in,
    // its time-out shortened to the window (G7); a probe whose time-out is
    // shorter needs its whole time-out.
    static constexpr std::int64_t PROBE_MIN_WINDOW_NS = 100'000'000;

    // A demoted `device` is due for a probe at `due_ns` (7.4); a device
    // whose plan has no probe request is probed by one whole poll cycle
    // (Choice::cycle, R8). The probe waits at most `timeout_ns` for its
    // answer. A probe is chosen only when no order is queued or in progress
    // and no polled device has a retry, a chain or a request of a started
    // cycle left (6.2 step 5), and when no other probe of the channel is in
    // flight; `probe_done` ends the flight. When the next poll falls due
    // within `timeout_ns`, the probe goes only if that leaves at least
    // PROBE_MIN_WINDOW_NS (or `timeout_ns`, if shorter), with its time-out
    // ending when that poll falls due (`Choice::timeout_ns`). A probe
    // PROBE_FLOOR time-outs late goes ahead of due polls with its whole
    // time-out, the oldest first, at most one such probe per channel in
    // PROBE_FLOOR + 1 probe time-outs (D-138, G7, the lead's ruling N1). A
    // cycle probe has no window: its first request, the probe, waits its
    // whole probe time-out.
    void set_probe(std::uint32_t device, std::int64_t due_ns,
                   std::int64_t timeout_ns) noexcept;
    void clear_probe(std::uint32_t device) noexcept;

    // The planned request `request` of `device` rests (`resting`) or is
    // polled again: a resting request is left out of its group's cycles
    // (review G8). A request of a chain of companions, first, middle or
    // last, never rests (K1). `reconnect` ends every rest.
    SCADA_RT void rest(std::uint32_t device, std::uint32_t request,
                       bool resting) noexcept;
    // Whether `request` of `device` is part of a chain of companions: it is
    // `chained`, or it ends a chain.
    [[nodiscard]] bool in_chain(std::uint32_t device,
                                std::uint32_t request) const noexcept;
    // Whether `request` of `device` rests.
    [[nodiscard]] bool resting(std::uint32_t device,
                               std::uint32_t request) const noexcept {
        return device < devices_.size() &&
               request < devices_[device].resting.size() &&
               devices_[device].resting[request];
    }
    void probe_done() noexcept { probe_in_flight_ = false; }

    // Sends the poll request `request` of `device` again as its next
    // request, before its other polls and within the pacing (7.1: a read is
    // retried within its poll group's turn). It takes its turn among the
    // other devices' polls by its group's priority, due at `now_ns`, the
    // time of the failure (3.4: one device's failure delays the others by
    // one request at most). Ignored for a side-effect request (6.6), an
    // unknown one, a device that is not polled, or, for a device in
    // IDENTITY, a request that is neither in `plan.identity` nor a part of
    // the split form of one (PR 171 S2). An order step that drops
    // the device's chain drops the retry too: the chain is read again
    // whole. Never allocates.
    SCADA_RT void retry(std::uint32_t device, std::uint32_t request,
                        std::int64_t now_ns) noexcept;

    // Orders queued and not yet started.
    [[nodiscard]] std::size_t pending_orders() const noexcept {
        return orders_.size();
    }

private:
    static constexpr std::uint32_t NOBODY =
        std::numeric_limits<std::uint32_t>::max();

    // A poll group's cycle on one device.
    struct GroupState {
        std::int64_t due_ns = 0;
        bool in_cycle = false;
        std::size_t next = 0;        // position in the plan's by_group list
        std::size_t next_split = 0;  // position in a split form being sent
        std::int64_t cycle_start_ns = 0;
        bool started = false;  // a cycle has started before
        // The last PERIODS periods, a ring.
        std::array<std::int64_t, PERIODS> periods{};
        std::size_t period_count = 0;
        std::size_t period_head = 0;
        std::int64_t last_sent_ns = 0;  // this cycle's last request handed out
        bool waited = false;   // this cycle waited for a pacing interval
        bool limited = false;  // the last cycle waited and ended late
    };

    // Members in the order that packs them best.
    struct DeviceRun {
        // Where in the group's cycle a chain in progress started.
        std::size_t chain_next = 0;
        std::size_t chain_next_split = 0;
        // The identity cycle's position in `plan.identity` (D-147).
        std::size_t identity_next = 0;
        std::size_t identity_next_split = 0;
        // Failure handling (7.4).
        std::int64_t probe_due_ns = 0;
        std::int64_t retry_due_ns = 0;
        std::int64_t probe_timeout_ns = 0;
        std::int64_t identity_due_ns = 0;  // when the next identity cycle is
        // The due time of the identity cycle in progress, its key among the
        // polls; when the last cycle started; the floor between two.
        std::int64_t identity_cycle_due_ns = 0;
        std::int64_t identity_start_ns = 0;
        std::int64_t identity_period_ns = 0;
        std::vector<GroupState> groups;
        // An order whose steps have started.
        OrderTicket order{};
        std::vector<bool> split;    // per request: replaced by its split form
        std::vector<bool> absent;   // per request: refused, not sent
        std::vector<bool> resting;  // per request: in its back-off (G8)
        // All false: the identity cycle reads every request, a resting one
        // too, since a failure of it is no verdict (D-147).
        std::vector<bool> never_rests;
        std::uint32_t unit = 0;         // index into units_
        std::uint32_t chain_group = 0;  // the group of a chain in progress
        std::uint32_t order_step = 0;
        std::uint32_t retry_request = 0;
        bool chain = false;  // a chain of companion requests (5.5) in progress
        bool order_active = false;
        // Within one `take`: an earlier order of this device waits, so its
        // later orders wait too (arrival order per device).
        bool order_blocked = false;
        DeviceMode mode = DeviceMode::ALL;
        // An identity cycle in progress, or one due (D-147).
        bool identity_cycle = false;
        bool identity_due = false;
        bool probe = false;
        bool retry = false;
        // A windowed probe was sent for this due time: the next one waits for
        // the floor, with its whole time-out (J1).
        bool windowed_tried = false;
    };

    // One physical unit: the devices that share its key (R3).
    struct UnitRun {
        Pacing pacing;  // the largest intervals of its devices
        bool has_request = false;
        std::int64_t last_request_ns = 0;
        bool has_write = false;
        std::int64_t last_write_ns = 0;
        // The device whose exclusive order or chain holds the unit.
        std::uint32_t holder = NOBODY;
    };

    // What `not_sent` restores.
    struct Undo {
        // Ordered by size, which the padding check asks for.
        std::int64_t last_request_ns = 0;
        std::int64_t last_write_ns = 0;
        std::size_t chain_next = 0;
        std::size_t chain_next_split = 0;
        std::size_t identity_next = 0;
        std::size_t identity_next_split = 0;
        std::int64_t identity_start_ns = 0;
        std::int64_t identity_cycle_due_ns = 0;
        Choice choice;
        UnitRun unit;
        GroupState group_state;
        std::uint32_t group = 0;
        std::uint32_t chain_group = 0;
        std::uint32_t retry_request = 0;
        bool valid = false;
        bool has_request = false;
        bool has_write = false;
        bool chain = false;
        bool retry = false;
        bool probe_in_flight = false;
        bool identity_cycle = false;
        bool identity_due = false;
        DeviceMode mode = DeviceMode::ALL;
    };

    // The time from which a request (or a write) to `device` is allowed.
    [[nodiscard]] std::int64_t allowed_from(std::uint32_t device,
                                            bool write) const noexcept;
    // The time from which step `step` of `order` is allowed.
    [[nodiscard]] std::int64_t step_allowed_from(
        const OrderTicket& order, std::uint32_t step) const noexcept;
    void record(std::uint32_t device, bool write, std::int64_t now_ns) noexcept;
    void save_undo(const Choice& choice, std::uint32_t group) noexcept;

    // The next request of `group` on `device` in its current cycle, if any
    // is left; advances nothing.
    [[nodiscard]] bool peek(std::uint32_t device, std::uint32_t group,
                            std::uint32_t& request) const noexcept;
    void advance(std::uint32_t device, std::uint32_t group) noexcept;
    // The form of request `request` to send now: itself, or the first part
    // of its split form not refused; false when every form is refused.
    [[nodiscard]] bool current_form(std::uint32_t device, std::uint32_t request,
                                    std::uint32_t& form) const noexcept;

    [[nodiscard]] bool unit_held_by_other(std::uint32_t device) const noexcept;
    [[nodiscard]] bool unit_exclusive(std::uint32_t device) const noexcept;
    void drop_chain(std::uint32_t device) noexcept;
    void end_order(std::uint32_t device) noexcept;
    static void complete_cycle(GroupState& state, std::int64_t period_ns,
                               std::int64_t now_ns) noexcept;
    // Drops a chain open on `device`'s unit before an order step; true when
    // there was one, whose device is `chain_device`.
    bool drop_unit_chain(std::uint32_t device,
                         std::uint32_t& chain_device) noexcept;
    [[nodiscard]] bool polls_allowed(std::uint32_t device) const noexcept;

    Choice take_order_step(std::uint32_t device, std::int64_t now_ns) noexcept;
    Choice take_poll(std::uint32_t device, std::uint32_t group,
                     std::int64_t now_ns) noexcept;
    void start_due(std::int64_t now_ns) noexcept;
    bool choose_exclusive_step(std::int64_t now_ns, Choice& out,
                               std::int64_t& wake) noexcept;
    bool choose_order(std::int64_t now_ns, Choice& out,
                      std::int64_t& wake) noexcept;
    // A candidate poll: the next request of a group, or a retry.
    struct PollPick {
        std::uint32_t device = 0;
        std::uint32_t group = 0;
        bool retry = false;
        // A request of the identity cycle, or its retry (D-147): its due
        // time and priority are its own.
        bool identity = false;
        std::int64_t due_ns = 0;
        std::uint8_t priority = 0;
    };
    // A chain in progress first, then priority, due time, device (DI 6.2
    // step 3).
    using PollKey = std::tuple<bool, std::uint8_t, std::int64_t, std::uint32_t>;
    void consider(const PollPick& pick, PollKey& best_key, PollPick& best,
                  bool& found) const noexcept;
    void consider_groups(std::uint32_t device, std::int64_t now_ns,
                         std::int64_t& wake, PollKey& best_key, PollPick& best,
                         bool& found) noexcept;
    // The oldest probe past its floor, ahead of due polls, at most one in
    // PROBE_FLOOR + 1 probe time-outs per channel (the lead's ruling N1).
    bool choose_floor_probe(std::int64_t now_ns, Choice& out,
                            std::int64_t& wake) noexcept;
    // `device`'s identity cycle or its retry as a candidate poll (D-147).
    void consider_identity(std::uint32_t device, std::int64_t now_ns,
                           std::int64_t& wake, PollKey& best_key,
                           PollPick& best, bool& found) noexcept;
    // Takes the identity request or retry `consider_identity` chose.
    Choice take_identity(std::uint32_t device, std::int64_t now_ns) noexcept;
    // The priority of an identity request: its group's, 255 in no group.
    [[nodiscard]] std::uint8_t identity_priority(
        std::uint32_t device, std::uint32_t request) const noexcept;
    // Where the identity cycle of `device` goes on, or starts; false when
    // it has nothing left to read.
    [[nodiscard]] bool identity_position(std::uint32_t device,
                                         std::size_t& index, std::size_t& part,
                                         bool& in_split,
                                         std::uint32_t& request) const noexcept;
    // An identity cycle holds a unit for a chain of `device`.
    [[nodiscard]] bool identity_chain_open(std::uint32_t device) const noexcept;
    // Moves `device`'s identity cycle past the request `sent`, at `index`
    // (and `part` of its split form, `in_split`) in `plan.identity`: the
    // cycle ends with its last request; a chain holds the unit to its end.
    void advance_identity(std::uint32_t device, std::size_t index,
                          std::size_t part, bool in_split,
                          std::uint32_t sent) noexcept;
    // Restarts an identity cycle on `device`'s unit that a chain of it holds,
    // for an order step (D2): true when one was, its device in
    // `chain_device`.
    bool drop_unit_identity(std::uint32_t device,
                            std::uint32_t& chain_device) noexcept;
    bool choose_poll(std::int64_t now_ns, Choice& out,
                     std::int64_t& wake) noexcept;
    bool choose_keepalive(std::int64_t now_ns, Choice& out,
                          std::int64_t& wake) noexcept;
    [[nodiscard]] bool due_work_pending() const noexcept;
    [[nodiscard]] std::int64_t next_poll_due_ns() const noexcept;
    // The pick continues a chain of companions: one is in progress, or it
    // is the retry of a part of one (its last part included).
    [[nodiscard]] bool chain_open(const PollPick& pick) const noexcept;
    // Takes the probe of `device` with time-out `wait_ns`.
    bool take_probe(std::uint32_t device, std::int64_t now_ns,
                    std::int64_t wait_ns, bool windowed, Choice& out) noexcept;
    bool choose_probe(std::int64_t now_ns, Choice& out,
                      std::int64_t& wake) noexcept;
    bool start_probe_cycle(std::uint32_t device, std::int64_t now_ns,
                           Choice& out) noexcept;
    // The device's group of the highest priority that has requests.
    [[nodiscard]] std::optional<std::uint32_t> first_group(
        std::uint32_t device) const noexcept;
    Choice take_retry(std::uint32_t device, std::int64_t now_ns) noexcept;

    const CompiledChannel* channel_;
    Status status_;
    std::vector<DeviceRun> devices_;
    std::vector<UnitRun> units_;
    std::vector<OrderTicket> orders_;  // capacity reserved, never grown
    std::size_t max_orders_;
    bool has_request_ = false;
    std::int64_t last_request_ns_ = 0;
    bool has_write_ = false;
    std::int64_t last_write_ns_ = 0;
    std::int64_t start_ns_ = 0;
    Undo undo_;
    bool probe_in_flight_ = false;
    // Until when no further probe past its floor goes (N1).
    std::int64_t floor_release_ns_ = 0;
};

}  // namespace scada::driver

#endif  // SCADA_DRIVER_SCHEDULER_H
