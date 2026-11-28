#include "sim_transport.h"

#include "loopback.h"
#include "scada/driver/channel_engine.h"
#include "scada/driver/sample_processing.h"
#include "scada/driver/transport.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace scada::driver::test {

IoStatus SimTransport::connect(std::int64_t deadline_ns) noexcept {
    if (abandon_) {
        open_ = false;
        return IoStatus::ABANDONED;
    }
    ++connects_;
    connect_times_.push_back(clock_->monotonic_ns());
    if (refusals_ > 0) {
        --refusals_;
        clock_->advance_to(
            std::min(clock_->monotonic_ns() + refuse_ns_, deadline_ns));
        return IoStatus::REFUSED;
    }
    if (clock_->monotonic_ns() + connect_delay_ns_ > deadline_ns) {
        clock_->advance_to(deadline_ns);
        return IoStatus::TIMEOUT;
    }
    clock_->advance_to(clock_->monotonic_ns() + connect_delay_ns_);
    open_ = true;
    peer_closed_ = closing_connects_ > 0;
    if (closing_connects_ > 0) {
        --closing_connects_;
    }
    return IoStatus::OK;
}

void SimTransport::close(bool abortive) noexcept {
    open_ = false;
    deliveries_.clear();
    if (abortive) {
        ++abortive_closes_;
    }
}

IoStatus SimTransport::send(std::span<const std::byte> frame) noexcept {
    if (!open_ || peer_closed_) {
        return IoStatus::CLOSED;
    }
    if (partial_sends_ > 0) {
        --partial_sends_;
        return IoStatus::PARTIAL;
    }
    if (failed_sends_ > 0) {
        --failed_sends_;
        return IoStatus::FAILED;
    }
    const std::int64_t now = clock_->monotonic_ns();
    if (collisions_ && std::any_of(deliveries_.begin(), deliveries_.end(),
                                   [&](const Delivery& delivery) {
                                       return delivery.arrival_ns > now;
                                   })) {
        ++collided_;
        return IoStatus::OK;  // lost on the line
    }
    const auto target = static_cast<std::uint8_t>(frame[2]);
    if (burst_unit_ != 0 && target == burst_unit_) {
        (void)bus_->answer(frame, now);  // logged, not answered
        burst_unit_ = 0;
        noise(now + burst_after_ns_, now + burst_after_ns_ + burst_length_ns_);
        return IoStatus::OK;
    }
    std::vector<std::byte> answer = bus_->answer(frame, now);
    if (answer.empty()) {
        return IoStatus::OK;
    }
    for (const std::byte byte : trailing_) {
        answer.push_back(byte);
    }
    trailing_.clear();
    const auto unit = static_cast<std::uint8_t>(frame[2]);
    const auto found = latencies_.find(unit);
    const std::int64_t latency =
        found == latencies_.end() ? latency_ns_ : found->second;
    Delivery delivery{.arrival_ns = clock_->monotonic_ns() + latency,
                      .bytes = answer};
    const auto place = std::upper_bound(
        deliveries_.begin(), deliveries_.end(), delivery.arrival_ns,
        [](std::int64_t time, const Delivery& other) {
            return time < other.arrival_ns;
        });
    deliveries_.insert(place, std::move(delivery));
    return IoStatus::OK;
}

IoStatus SimTransport::receive(std::span<std::byte> into,
                               std::int64_t deadline_ns,
                               std::size_t& received) noexcept {
    received = 0;
    if (!open_ || peer_closed_) {
        return IoStatus::CLOSED;
    }
    if (abandon_) {
        return IoStatus::ABANDONED;
    }
    if (empty_read_) {
        empty_read_ = false;
        return IoStatus::OK;  // a read of 0 bytes
    }
    if (stall_ns_ > 0) {
        clock_->advance_to(clock_->monotonic_ns() + stall_ns_);
        stall_ns_ = 0;
    }
    std::stable_sort(deliveries_.begin(), deliveries_.end(),
                     [](const Delivery& left, const Delivery& right) {
                         return left.arrival_ns < right.arrival_ns;
                     });
    // What is readable when the wait ends: bytes already there, or the
    // first that arrive by the deadline.
    const std::int64_t now = clock_->monotonic_ns();
    if (deliveries_.empty() ||
        deliveries_.front().arrival_ns > std::max(now, deadline_ns)) {
        clock_->advance_to(deadline_ns);
        return IoStatus::TIMEOUT;
    }
    clock_->advance_to(deliveries_.front().arrival_ns);
    // One read takes everything that has arrived by then, in order.
    while (!deliveries_.empty() && received < into.size() &&
           deliveries_.front().arrival_ns <= clock_->monotonic_ns()) {
        std::vector<std::byte>& bytes = deliveries_.front().bytes;
        const std::size_t take = std::min(into.size() - received, bytes.size());
        std::copy_n(bytes.begin(), take,
                    into.begin() + static_cast<std::ptrdiff_t>(received));
        received += take;
        bytes.erase(bytes.begin(),
                    bytes.begin() + static_cast<std::ptrdiff_t>(take));
        if (bytes.empty()) {
            deliveries_.erase(deliveries_.begin());
        }
    }
    return IoStatus::OK;
}

bool SimTransport::discard_input() noexcept {
    const std::int64_t now = clock_->monotonic_ns();
    return std::erase_if(deliveries_, [&](const Delivery& delivery) {
               return delivery.arrival_ns <= now;
           }) > 0;
}

void SimTransport::noise(std::int64_t from_ns, std::int64_t to_ns) {
    constexpr std::int64_t EVERY_NS = 1'000'000;
    constexpr std::byte NOISE{0xAA};
    for (std::int64_t time = from_ns; time < to_ns; time += EVERY_NS) {
        inject({NOISE}, time);
    }
}

SCADA_RT void RecordingTags::write_point(
    std::uint32_t device, std::uint32_t point,
    const ProcessedSample& sample) noexcept {
    order_.emplace_back(device, point);
    Written& written = points_[{device, point}];
    written.quality = sample.tag.quality;
    written.flags = sample.tag.flags;
    written.value = sample.tag.value.i;
    written.time_ns = sample.tag.source_time_ns;
    ++written.writes;
    history_.push_back(
        Logged{.device = device, .point = point, .written = written});
}

std::size_t run_engine(ChannelEngine& engine, SimClock& clock,
                       std::int64_t end_ns) {
    // A bound on the steps, so that an engine that never waits fails the
    // test instead of hanging it.
    constexpr std::size_t MAX_STEPS = 1'000'000;
    std::size_t steps = 0;
    while (clock.monotonic_ns() <= end_ns && steps < MAX_STEPS) {
        const std::int64_t next = engine.step();
        ++steps;
        if (next > end_ns) {
            clock.advance_to(end_ns + 1);
            break;
        }
        clock.advance_to(next);
    }
    return steps;
}

}  // namespace scada::driver::test
