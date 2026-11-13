#ifndef SCADA_COMMON_CLOCK_H
#define SCADA_COMMON_CLOCK_H

#include <cstdint>

namespace scada {

inline constexpr std::int64_t NANOSECONDS_PER_SECOND = 1'000'000'000;

// Returns the wall-clock time (CLOCK_REALTIME) in nanoseconds since
// 1970-01-01T00:00:00Z. Use it for timestamps that people or other systems
// read. Time synchronisation can step it in either direction, so never
// subtract two readings to measure an interval.
//
// Does not allocate, lock or block. If the operating system refuses the clock
// the process ends through `panic`, because no caller can continue without
// time.
[[nodiscard]] std::int64_t realtime_ns() noexcept;

// Returns the monotonic time (CLOCK_MONOTONIC) in nanoseconds since an
// unspecified start point, usually boot. It never goes backwards and is not
// stepped, so it is the clock for ages, intervals and timeouts (reference
// 6.2: age calculations use `update_mono_ns`). The value means nothing in
// another boot or on another machine.
//
// Same guarantees as `realtime_ns`.
[[nodiscard]] std::int64_t monotonic_ns() noexcept;

}  // namespace scada

#endif  // SCADA_COMMON_CLOCK_H
