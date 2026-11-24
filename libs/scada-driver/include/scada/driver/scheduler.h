#ifndef SCADA_DRIVER_SCHEDULER_H
#define SCADA_DRIVER_SCHEDULER_H

// The choice of a channel worker's next request (driver interface contract
// 6.2 to 6.6): orders first, then due poll groups by priority and due time,
// then keep-alive, within the pacing of each device and of the channel.
// Pure logic over an injected clock: the caller passes the monotonic time,
// so the same inputs always give the same choices.
//
// Probes of demoted devices (6.2, step 5) join with failure handling
// (7.4), and the orders' content with the command slices; here an order is
// a ticket with a number of steps.

#include "scada/common/realtime.h"
#include "scada/driver/compiled.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace scada::driver {

// An order the command thread handed to the worker: a write, a list of
// writes, a select and operate (8.3). `steps` requests; `exclusive`: no
// other request to its device between them (6.6).
struct OrderTicket {
    std::uint64_t id = 0;
    std::uint32_t device = 0;
    std::uint32_t steps = 1;  // >= 1
    bool exclusive = false;
};

// What the worker sends next.
struct Choice {
    enum class Kind : std::uint8_t {
        IDLE = 1,   // nothing to do until an order arrives
        WAIT,       // nothing allowed before `wake_ns`
        ORDER,      // step `step` of order `order` on `device`
        POLL,       // request `request` of `device`'s plan
        KEEPALIVE,  // the keep-alive request `request` of `device`
    };
    Kind kind = Kind::IDLE;
    std::uint32_t device = 0;
    std::uint32_t request = 0;
    std::uint64_t order = 0;
    std::uint32_t step = 0;
    std::int64_t wake_ns = std::numeric_limits<std::int64_t>::max();
};

class Scheduler {
public:
    // Allocates, sized for `channel`, which must outlive the scheduler (the
    // compiled configuration; a reload builds a new scheduler state, slice
    // 8). `max_pending_orders` is the channel's `max_pending_writes`.
    // Every poll group of every enabled device is first due at `now_ns`.
    Scheduler(const CompiledChannel& channel, std::size_t max_pending_orders,
              std::int64_t now_ns);

    // Chooses the next request at `now_ns` and records it as sent then
    // (6.2): an exclusive order in progress or a chain of companions (5.5)
    // on its device first, then new orders in arrival order, then the next
    // request of a due poll group by priority, then due time, then device
    // order, then a keep-alive (6.5). Nothing is chosen that the pacing of
    // its device or of the channel does not allow yet (6.4); a device that
    // waits does not hold up the others. Never allocates.
    SCADA_RT Choice take(std::int64_t now_ns) noexcept;

    // Queues an order; false when `max_pending_orders` are queued (the
    // command thread answers BUSY, 8.8). Never allocates.
    SCADA_RT bool add_order(const OrderTicket& order) noexcept;

    // The device refused request `request` (just taken) as a whole as an
    // illegal address (DecodeResult::SPLIT). A planned request with a split
    // form is replaced by its split form, which is sent next and kept until
    // `reconnect` (6.3); any other request is no longer sent until
    // `reconnect`, and the caller writes its points ABSENT. Returns true
    // when the split form was taken.
    SCADA_RT bool refused(std::uint32_t device, std::uint32_t request) noexcept;

    // A new connection: every request is back to its planned form (6.3).
    void reconnect() noexcept;

    // The longest period achieved by poll group `group` over the devices of
    // the channel in their last completed cycle (6.4,
    // `sys.drv.<channel>.period.<group>`); 0 before any cycle completed.
    [[nodiscard]] std::int64_t achieved_period_ns(
        std::uint32_t group) const noexcept;

    // True when pacing held back a due request of the device in its current
    // or last cycle (6.4, `sys.dev.<device>.pacing_limited`).
    [[nodiscard]] bool pacing_limited(std::uint32_t device) const noexcept;

    // Orders queued and not yet started.
    [[nodiscard]] std::size_t pending_orders() const noexcept {
        return orders_.size();
    }

private:
    // A poll group's cycle on one device.
    struct GroupState {
        std::int64_t due_ns = 0;
        bool in_cycle = false;
        std::size_t next = 0;        // position in the plan's by_group list
        std::size_t next_split = 0;  // position in a split form being sent
        std::int64_t cycle_start_ns = 0;
        std::int64_t last_period_ns = 0;
        bool started = false;  // a cycle has started before
    };

    struct DeviceRun {
        std::vector<GroupState> groups;
        std::vector<bool> split;   // per request: replaced by its split form
        std::vector<bool> absent;  // per request: refused, not sent
        bool has_request = false;
        std::int64_t last_request_ns = 0;
        bool has_write = false;
        std::int64_t last_write_ns = 0;
        // A chain of companion requests (5.5) in progress: its group.
        bool chain = false;
        std::uint32_t chain_group = 0;
        // An order whose steps have started.
        bool order_active = false;
        OrderTicket order{};
        std::uint32_t order_step = 0;
        bool limited = false;
        // Within one `take`: an earlier order of this device waits, so its
        // later orders wait too (arrival order per device).
        bool order_blocked = false;
    };

    // The time from which a request (or a write) to `device` is allowed.
    [[nodiscard]] std::int64_t allowed_from(std::uint32_t device,
                                            bool write) const noexcept;
    void record(std::uint32_t device, bool write, std::int64_t now_ns) noexcept;

    // The next request of `group` on `device` in its current cycle, if any
    // is left; advances nothing.
    [[nodiscard]] bool peek(std::uint32_t device, std::uint32_t group,
                            std::uint32_t& request) const noexcept;
    void advance(std::uint32_t device, std::uint32_t group,
                 std::int64_t now_ns) noexcept;

    Choice take_order_step(std::uint32_t device, std::int64_t now_ns) noexcept;
    Choice take_poll(std::uint32_t device, std::uint32_t group,
                     std::int64_t now_ns) noexcept;
    bool start_due(std::int64_t now_ns) noexcept;
    bool choose_poll(std::int64_t now_ns, Choice& out,
                     std::int64_t& wake) noexcept;
    bool choose_order(std::int64_t now_ns, Choice& out,
                      std::int64_t& wake) noexcept;
    bool choose_keepalive(std::int64_t now_ns, Choice& out,
                          std::int64_t& wake) noexcept;

    const CompiledChannel* channel_;
    std::vector<DeviceRun> devices_;
    std::vector<OrderTicket> orders_;  // capacity reserved, never grown
    std::size_t max_orders_;
    bool has_request_ = false;
    std::int64_t last_request_ns_ = 0;
    bool has_write_ = false;
    std::int64_t last_write_ns_ = 0;
    std::int64_t start_ns_ = 0;
};

}  // namespace scada::driver

#endif  // SCADA_DRIVER_SCHEDULER_H
