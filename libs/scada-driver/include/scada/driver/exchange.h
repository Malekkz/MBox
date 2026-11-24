#ifndef SCADA_DRIVER_EXCHANGE_H
#define SCADA_DRIVER_EXCHANGE_H

// The run-time objects a protocol is handed during one exchange (driver
// interface contract 2.2, 3.1): the connection's state, the attempt record,
// the device's mutable protocol state, the transmit buffer and the sample
// sink. All are owned by the framework; a protocol keeps no pointer or
// reference to one beyond the call that passed it.

#include "scada/common/realtime.h"
#include "scada/driver/sample_processing.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::driver {

// Per connection: the transaction counter, reset at each connect (3.1).
// Written by the channel's worker only.
class ChannelState {
public:
    // Returns the next transaction id of this connection. Ids wrap at 2^32;
    // a protocol keeps as many bits as its frame has.
    SCADA_RT std::uint32_t take_transaction_id() noexcept {
        return next_transaction_id_++;
    }

    // Called by the framework at each connect.
    void reset() noexcept { next_transaction_id_ = 0; }

private:
    std::uint32_t next_transaction_id_ = 0;
};

// The worker's record of one attempt of a request: every attempt, a retry
// included, has its own transaction id; the planned request stays
// immutable (2.2).
struct Attempt {
    std::uint32_t transaction_id = 0;
};

// A device's mutable protocol state, owned by the framework's Device and
// written only by the channel's worker (3.1). Phase 2 needs none beyond a
// session flag; later protocols add room here.
struct DeviceState {
    bool session_open = false;
};

// The buffer encode writes a request into, sized by the framework at
// configure from the device's `max_frame_bytes` (2.8).
struct TxBuffer {
    std::span<std::byte> bytes;
};

// One raw sample decode delivers for a point of the request (5.1).
struct DeliveredSample {
    std::uint32_t point = 0;  // index of the point in its device
    RawSample sample{};
};

// Where decode delivers its samples: a buffer of the framework sized at
// configure for the request with the most points, so that delivery never
// allocates. A sample beyond the capacity is counted, not stored (a
// protocol that delivers more samples than its request has points is at
// fault).
class SampleSink {
public:
    explicit SampleSink(std::span<DeliveredSample> storage) noexcept
        : storage_(storage) {}

    // Adds one sample; false when the sink is full (counted in `dropped`).
    SCADA_RT bool add(std::uint32_t point, const RawSample& sample) noexcept {
        if (size_ == storage_.size()) {
            ++dropped_;
            return false;
        }
        storage_[size_] = DeliveredSample{.point = point, .sample = sample};
        ++size_;
        return true;
    }

    // The device dropped events, or the protocol's buffer overflowed (5.7).
    SCADA_RT void events_lost() noexcept { events_lost_ = true; }

    // Empties the sink for the next answer.
    SCADA_RT void clear() noexcept {
        size_ = 0;
        dropped_ = 0;
        events_lost_ = false;
    }

    // The samples delivered since the last clear, in order.
    [[nodiscard]] std::span<const DeliveredSample> samples() const noexcept {
        return storage_.first(size_);
    }

    [[nodiscard]] std::size_t dropped() const noexcept { return dropped_; }
    [[nodiscard]] bool lost_events() const noexcept { return events_lost_; }

private:
    std::span<DeliveredSample> storage_;
    std::size_t size_ = 0;
    std::size_t dropped_ = 0;
    bool events_lost_ = false;
};

}  // namespace scada::driver

#endif  // SCADA_DRIVER_EXCHANGE_H
