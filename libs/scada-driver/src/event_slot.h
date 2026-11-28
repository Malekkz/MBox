#ifndef SCADA_DRIVER_SRC_EVENT_SLOT_H
#define SCADA_DRIVER_SRC_EVENT_SLOT_H

// One device's identity and restart events, from its worker to the main
// thread for the log (decision D-147; the lead's option (b) for 5b-2b): per
// kind, the count since the main thread last took it and the last event's
// details. An interim slot until 5b-3's logging path, which replaces it;
// between two takes only the last event's text survives.
//
// One writer (the worker) and one reader (the main thread). The counts are
// relaxed atomics, as the refused-write counts are; the details are atomic
// words under a sequence number, so that a take never reads a torn text and
// neither side waits for the other.

#include "scada/driver/channel_engine.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace scada::driver::detail {

class EventSlot {
public:
    // Adds `record.count` events of `kind`, `record`'s details the last
    // one's. Worker thread only. Never blocks.
    void add(DeviceEvent kind, const EventRecord& record) noexcept;

    // The events of `kind` since the last take into `out`: false when there
    // were none. The details are the last event's, or empty when the worker
    // was writing them through every one of a few reads (`out.length` 0,
    // `out.time_ns` 0). Main thread only. Never blocks.
    bool take(DeviceEvent kind, EventRecord& out) noexcept;

private:
    // The text in words, after one word of time and one of the reason,
    // check and length.
    static constexpr std::size_t TEXT_WORDS =
        (TAG_STRING_MAX_BYTES + sizeof(std::uint64_t) - 1) /
        sizeof(std::uint64_t);
    static constexpr std::size_t WORDS = 2 + TEXT_WORDS;

    struct Kind {
        std::atomic<std::uint64_t> count{0};
        // Odd while the worker writes the details.
        std::atomic<std::uint64_t> sequence{0};
        std::array<std::atomic<std::uint64_t>, WORDS> words{};
    };

    std::array<Kind, DEVICE_EVENTS> kinds_{};
};

}  // namespace scada::driver::detail

#endif  // SCADA_DRIVER_SRC_EVENT_SLOT_H
