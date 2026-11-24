#ifndef SCADA_DRIVER_TESTS_LOOPBACK_H
#define SCADA_DRIVER_TESTS_LOOPBACK_H

// The in-process loopback of the conformance tests: simulated devices that
// answer the test protocol's frames (test_protocol.h) and report every
// request they receive, in order, with its time (contract section 14, item
// 11), and a loop that drives a channel's Scheduler and the protocol over
// them on an injected clock. Nothing sleeps: time moves only when the loop
// waits for the scheduler's wake time or an exchange takes its duration.

#include "scada/driver/compiled.h"
#include "scada/driver/exchange.h"
#include "scada/driver/protocol.h"
#include "scada/driver/scheduler.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace scada::driver::test {

// One request a simulated device received.
struct LoggedRequest {
    std::int64_t time_ns = 0;
    std::uint8_t unit_id = 0;
    std::uint8_t space = 0;
    std::uint32_t first = 0;
    std::uint16_t count = 0;
};

// The devices on one simulated line, by unit id. A register that is not
// mapped answers as an illegal address; a side-effect range clears on read
// and counts its reads.
class SimBus {
public:
    // Maps registers `first` to `last` of `space` on `unit_id`, all 0.
    void map(std::uint8_t unit_id, std::uint8_t space, std::uint32_t first,
             std::uint32_t last);
    void set(std::uint8_t unit_id, std::uint8_t space, std::uint32_t address,
             std::uint16_t value);
    // Registers whose read clears them (6.6).
    void side_effect(std::uint8_t unit_id, std::uint8_t space,
                     std::uint32_t first, std::uint32_t last);

    // How a unit answers, for the fault tests (7.1 to 7.4).
    enum class Fault : std::uint8_t {
        NONE = 0,     // as mapped
        SILENT,       // no answer at all: a dead unit
        BUSY,         // a busy answer
        UNREACHABLE,  // a gateway's answer that it cannot reach the unit
        GARBAGE,      // a frame of another unit: broken
    };
    void fault(std::uint8_t unit_id, Fault fault) { faults_[unit_id] = fault; }

    // The answer to one request frame at `now_ns`; logs it.
    std::vector<std::byte> answer(std::span<const std::byte> request,
                                  std::int64_t now_ns);

    [[nodiscard]] const std::vector<LoggedRequest>& log() const noexcept {
        return log_;
    }
    // Requests that touched a side-effect register.
    [[nodiscard]] std::size_t side_effect_reads() const noexcept {
        return side_effect_reads_;
    }

private:
    using Key = std::pair<std::uint32_t,
                          std::uint32_t>;  // (unit << 8 | space, address)
    std::map<Key, std::uint16_t> registers_;
    std::map<Key, bool> clears_;
    std::map<std::uint8_t, Fault> faults_;
    std::vector<LoggedRequest> log_;
    std::size_t side_effect_reads_ = 0;
};

// CS 4.6 default of `max_pending_writes`.
inline constexpr std::size_t DEFAULT_PENDING_ORDERS = 16;

// What the loop did at one choice.
struct LoopEvent {
    std::int64_t time_ns = 0;
    Choice choice;
    DecodeResult::Kind result = DecodeResult::Kind::BROKEN;
    std::vector<DeliveredSample> samples;
};

// Drives one channel: takes the scheduler's choices, encodes, lets the bus
// answer, frames and decodes, and tells the scheduler of a refused request.
class Loop {
public:
    Loop(const CompiledChannel& channel, const FrameProtocol& protocol,
         SimBus& bus, std::int64_t start_ns, std::int64_t exchange_ns,
         std::size_t max_pending_orders = DEFAULT_PENDING_ORDERS);

    // Runs until `end_ns`, or until the scheduler is idle.
    void run_until(std::int64_t end_ns);

    [[nodiscard]] Scheduler& scheduler() noexcept { return scheduler_; }
    [[nodiscard]] std::int64_t now() const noexcept { return now_ns_; }
    [[nodiscard]] const std::vector<LoopEvent>& events() const noexcept {
        return events_;
    }

private:
    void exchange(const Choice& choice);

    const CompiledChannel* channel_;
    const FrameProtocol* protocol_;
    SimBus* bus_;
    Scheduler scheduler_;
    ChannelState channel_state_;
    std::vector<DeviceState> device_states_;
    std::vector<std::byte> tx_;
    std::vector<DeliveredSample> sink_storage_;
    std::int64_t now_ns_;
    std::int64_t exchange_ns_;
    std::vector<LoopEvent> events_;
};

}  // namespace scada::driver::test

#endif  // SCADA_DRIVER_TESTS_LOOPBACK_H
