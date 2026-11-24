#ifndef SCADA_DRIVER_TESTS_SIM_TRANSPORT_H
#define SCADA_DRIVER_TESTS_SIM_TRANSPORT_H

// A transport and a clock for the channel engine's tests: the transport
// hands each frame to a SimBus and delivers its answer after a fixed
// latency, on a clock that moves only when the engine waits. Connects can
// be refused and the peer can close, on cue. A tag sink records what the
// engine writes.

#include "loopback.h"
#include "scada/common/realtime.h"
#include "scada/driver/channel_engine.h"
#include "scada/driver/failure.h"
#include "scada/driver/sample_processing.h"
#include "scada/driver/transport.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace scada::driver::test {

class SimClock final : public Clock {
public:
    // CLOCK_REALTIME runs this far ahead of CLOCK_MONOTONIC.
    static constexpr std::int64_t REALTIME_OFFSET_NS = 1'000'000'000'000'000;

    SCADA_RT [[nodiscard]] std::int64_t monotonic_ns() const noexcept override {
        return now_ns_;
    }
    SCADA_RT [[nodiscard]] std::int64_t realtime_ns() const noexcept override {
        return now_ns_ + REALTIME_OFFSET_NS;
    }
    // Moves the clock forward to `time_ns`; never back.
    void advance_to(std::int64_t time_ns) noexcept {
        now_ns_ = std::max(now_ns_, time_ns);
    }

private:
    std::int64_t now_ns_ = 0;
};

class SimTransport final : public Transport {
public:
    SimTransport(SimBus& bus, SimClock& clock, std::int64_t latency_ns)
        : bus_(&bus), clock_(&clock), latency_ns_(latency_ns) {}

    // The next `count` connects are refused, each after `refuse_ns`.
    void refuse_connects(int count, std::int64_t refuse_ns) noexcept {
        refusals_ = count;
        refuse_ns_ = refuse_ns;
    }
    // Each connect takes `delay_ns`: past its deadline it is a TIMEOUT.
    void connect_delay(std::int64_t delay_ns) noexcept {
        connect_delay_ns_ = delay_ns;
    }
    // The peer closes the connection now.
    void close_by_peer() noexcept { peer_closed_ = true; }
    // The next `count` connects are accepted and closed by the peer at once.
    void close_on_connect(int count) noexcept { closing_connects_ = count; }
    // Unit `unit_id` answers after `latency_ns` instead of the default.
    void latency(std::uint8_t unit_id, std::int64_t latency_ns) {
        latencies_[unit_id] = latency_ns;
    }
    // Bytes that arrive at `arrival_ns` on their own: a stale frame, a late
    // answer, noise during a quiet time.
    void inject(std::vector<std::byte> bytes, std::int64_t arrival_ns) {
        deliveries_.push_back(
            {.arrival_ns = arrival_ns, .bytes = std::move(bytes)});
    }
    // The next `count` sends are taken only in part.
    void partial_sends(int count) noexcept { partial_sends_ = count; }

    IoStatus connect(std::int64_t deadline_ns) noexcept override;
    void close(bool abortive) noexcept override;
    [[nodiscard]] bool closed_by_peer() noexcept override {
        return open_ && peer_closed_;
    }
    IoStatus send(std::span<const std::byte> frame) noexcept override;
    IoStatus receive(std::span<std::byte> into, std::int64_t deadline_ns,
                     std::size_t& received) noexcept override;
    // Drops what has arrived by now; later bytes still arrive.
    void discard_input() noexcept override;

    [[nodiscard]] int connects() const noexcept { return connects_; }
    [[nodiscard]] int abortive_closes() const noexcept {
        return abortive_closes_;
    }
    [[nodiscard]] const std::vector<std::int64_t>& connect_times()
        const noexcept {
        return connect_times_;
    }

private:
    struct Delivery {
        std::int64_t arrival_ns = 0;
        std::vector<std::byte> bytes;
    };

    SimBus* bus_;
    SimClock* clock_;
    std::int64_t latency_ns_;
    std::map<std::uint8_t, std::int64_t> latencies_;
    bool open_ = false;
    bool peer_closed_ = false;
    int refusals_ = 0;
    std::int64_t refuse_ns_ = 0;
    std::int64_t connect_delay_ns_ = 0;
    int closing_connects_ = 0;
    int partial_sends_ = 0;
    int connects_ = 0;
    int abortive_closes_ = 0;
    std::vector<std::int64_t> connect_times_;
    std::vector<Delivery> deliveries_;  // in order of arrival
};

// What the engine wrote: the last write of each point, and every device
// state in order.
class RecordingTags final : public TagSink {
public:
    struct Written {
        tag::Quality quality = tag::Quality::BAD;
        std::uint16_t flags = 0;
        std::int64_t value = 0;
        int writes = 0;
    };

    SCADA_RT void write_point(std::uint32_t device, std::uint32_t point,
                              const ProcessedSample& sample) noexcept override;
    SCADA_RT void device_state(std::uint32_t device,
                               DeviceStatus state) noexcept override {
        states_.emplace_back(device, state);
    }

    [[nodiscard]] Written point(std::uint32_t device,
                                std::uint32_t point) const {
        const auto found = points_.find({device, point});
        return found == points_.end() ? Written{} : found->second;
    }
    [[nodiscard]] bool has_flag(std::uint32_t device, std::uint32_t index,
                                tag::QualityFlag flag) const {
        return (point(device, index).flags &
                static_cast<std::uint16_t>(flag)) != 0;
    }
    [[nodiscard]] const std::vector<std::pair<std::uint32_t, DeviceStatus>>&
    states() const noexcept {
        return states_;
    }

private:
    std::map<std::pair<std::uint32_t, std::uint32_t>, Written> points_;
    std::vector<std::pair<std::uint32_t, DeviceStatus>> states_;
};

// Steps `engine` until the clock passes `end_ns`, moving the clock to each
// time the engine asks to wait for. Returns the steps taken.
std::size_t run_engine(ChannelEngine& engine, SimClock& clock,
                       std::int64_t end_ns);

}  // namespace scada::driver::test

#endif  // SCADA_DRIVER_TESTS_SIM_TRANSPORT_H
