#ifndef SCADA_DRIVER_TIMING_H
#define SCADA_DRIVER_TIMING_H

// Deadlines of one exchange (driver interface contract 7.1, 7.2; CS 4.11).
// A request has timed out when no byte of its answer arrived by the end of
// the request on the wire plus `timeout`; once the first byte is in, the
// rest must arrive within the frame time of the longest answer the request
// allows plus a margin. Pure functions of the line's character format.

#include "scada/common/realtime.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace scada::driver {

inline constexpr std::int64_t NS_PER_SECOND = 1'000'000'000;

// The least margin, which covers the buffering of USB adapters (7.1).
inline constexpr std::int64_t MIN_MARGIN_NS = 20'000'000;

// A line's character format. A bit rate of 0 is a stream (TCP): no line
// time.
struct LineTiming {
    std::uint32_t bit_rate = 0;
    std::uint8_t bits_per_char = 10;  // start, data, parity, stop
};

// The bits of one character: 1 start bit, the data bits, a parity bit if
// any, the stop bits (7.1).
[[nodiscard]] constexpr std::uint8_t bits_per_char(std::uint8_t data_bits,
                                                   bool parity,
                                                   std::uint8_t stop_bits) noexcept {
    return static_cast<std::uint8_t>(1 + data_bits + (parity ? 1 : 0) +
                                     stop_bits);
}

// The time `bytes` take on the line, rounded up to the nanosecond; 0 on a
// stream. Saturates for any size.
SCADA_RT [[nodiscard]] constexpr std::int64_t frame_time_ns(
    const LineTiming& line, std::size_t bytes) noexcept {
    if (line.bit_rate == 0) {
        return 0;
    }
    const std::uint64_t bits = static_cast<std::uint64_t>(bytes) *
                               line.bits_per_char;
    constexpr auto MAX = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max());
    if (bits > MAX / NS_PER_SECOND) {
        return std::numeric_limits<std::int64_t>::max();
    }
    const std::uint64_t scaled = bits * NS_PER_SECOND;
    return static_cast<std::int64_t>((scaled + line.bit_rate - 1) /
                                     line.bit_rate);
}

// 3.5 characters at the line's rate, or 20 ms, whichever is larger (7.1);
// also the quiet time of a serial line after a timeout or a broken answer
// (7.2).
SCADA_RT [[nodiscard]] constexpr std::int64_t margin_ns(
    const LineTiming& line) noexcept {
    // 3.5 characters = 7 half characters.
    constexpr std::size_t HALF_CHARS = 7;
    const std::int64_t seven_halves =
        line.bit_rate == 0
            ? 0
            : frame_time_ns(LineTiming{.bit_rate = line.bit_rate * 2,
                                       .bits_per_char = line.bits_per_char},
                            HALF_CHARS);
    return std::max(seven_halves, MIN_MARGIN_NS);
}

namespace detail {
[[nodiscard]] constexpr std::int64_t saturating_add(std::int64_t left,
                                                    std::int64_t right) noexcept {
    constexpr std::int64_t MAX = std::numeric_limits<std::int64_t>::max();
    return left > MAX - right ? MAX : left + right;
}
}  // namespace detail

// The deadline of the first byte of the answer to a request of
// `request_bytes` sent at `sent_ns`: the end of the request on the wire
// plus `timeout_ns`.
SCADA_RT [[nodiscard]] constexpr std::int64_t first_byte_deadline_ns(
    const LineTiming& line, std::int64_t sent_ns, std::size_t request_bytes,
    std::int64_t timeout_ns) noexcept {
    return detail::saturating_add(
        detail::saturating_add(sent_ns, frame_time_ns(line, request_bytes)),
        timeout_ns);
}

// The deadline of the whole answer once its first byte arrived at
// `first_byte_ns`: the frame time of the longest answer the request allows
// (`EncodeResult::expected_max`) plus the margin.
SCADA_RT [[nodiscard]] constexpr std::int64_t answer_deadline_ns(
    const LineTiming& line, std::int64_t first_byte_ns,
    std::size_t expected_max) noexcept {
    return detail::saturating_add(
        detail::saturating_add(first_byte_ns,
                               frame_time_ns(line, expected_max)),
        margin_ns(line));
}

}  // namespace scada::driver

#endif  // SCADA_DRIVER_TIMING_H
