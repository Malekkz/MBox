#include "sim_transport.h"

#include "loopback.h"
#include "scada/driver/channel_engine.h"
#include "scada/driver/sample_processing.h"
#include "scada/driver/transport.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace scada::driver::test {

IoStatus SimTransport::connect(std::int64_t deadline_ns) noexcept {
    ++connects_;
    connect_times_.push_back(clock_->monotonic_ns());
    if (refusals_ > 0) {
        --refusals_;
        clock_->advance_to(
            std::min(clock_->monotonic_ns() + refuse_ns_, deadline_ns));
        return IoStatus::FAILED;
    }
    open_ = true;
    peer_closed_ = closing_connects_ > 0;
    if (closing_connects_ > 0) {
        --closing_connects_;
    }
    pending_.clear();
    return IoStatus::OK;
}

void SimTransport::close(bool abortive) noexcept {
    open_ = false;
    pending_.clear();
    if (abortive) {
        ++abortive_closes_;
    }
}

SCADA_RT IoStatus
SimTransport::send(std::span<const std::byte> frame) noexcept {
    if (!open_ || peer_closed_) {
        return IoStatus::CLOSED;
    }
    const std::vector<std::byte> answer =
        bus_->answer(frame, clock_->monotonic_ns());
    pending_.insert(pending_.end(), answer.begin(), answer.end());
    arrival_ns_ = clock_->monotonic_ns() + latency_ns_;
    return IoStatus::OK;
}

SCADA_RT IoStatus SimTransport::receive(std::span<std::byte> into,
                                        std::int64_t deadline_ns,
                                        std::size_t& received) noexcept {
    received = 0;
    if (!open_ || peer_closed_) {
        return IoStatus::CLOSED;
    }
    if (pending_.empty() || arrival_ns_ > deadline_ns) {
        clock_->advance_to(deadline_ns);
        return IoStatus::TIMEOUT;
    }
    clock_->advance_to(arrival_ns_);
    received = std::min(into.size(), pending_.size());
    std::copy_n(pending_.begin(), received, into.begin());
    pending_.erase(pending_.begin(),
                   pending_.begin() + static_cast<std::ptrdiff_t>(received));
    return IoStatus::OK;
}

SCADA_RT void RecordingTags::write_point(
    std::uint32_t device, std::uint32_t point,
    const ProcessedSample& sample) noexcept {
    Written& written = points_[{device, point}];
    written.quality = sample.tag.quality;
    written.flags = sample.tag.flags;
    written.value = sample.tag.value.i;
    ++written.writes;
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
