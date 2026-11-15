#ifndef SCADA_SIM_PATTERN_H
#define SCADA_SIM_PATTERN_H

// The value patterns of the simulator writer (issue P1-10). Every value is a
// function of the tag and of how often the tag was written, not of the
// clock, so that a test can compute what a reader must see.

#include "scada/tag/scada_tag.h"
#include "scada/tag/types.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace scada::sim {

// What a tag of the simulator holds.
enum class Pattern : std::uint8_t {
    // Self-checking: every field is derived from the write count, so that a
    // reader can tell a torn read. UINT64.
    CHECK,
    RAMP,  // rises from 0 towards the amplitude, then starts again. FLOAT64
    SINE,  // between minus and plus the amplitude. FLOAT64
    STEP,  // 0 for half a period, then the amplitude. FLOAT64
    WALK,  // random steps within minus and plus the amplitude. FLOAT64
};

// The pattern named `name` (`check`, `ramp`, `sine`, `step`, `walk`), or
// nothing.
[[nodiscard]] std::optional<Pattern> pattern_from_name(
    std::string_view name) noexcept;

// The name of `pattern`. The view refers to static storage.
[[nodiscard]] std::string_view to_string(Pattern pattern) noexcept;

// The tag type a tag with `pattern` has.
[[nodiscard]] scada::tag::TagType tag_type(Pattern pattern) noexcept;

inline constexpr double DEFAULT_AMPLITUDE = 100.0;
inline constexpr std::uint64_t DEFAULT_PERIOD = 100;

// Shape of the waveforms (all patterns but CHECK).
struct Waveform {
    double amplitude = DEFAULT_AMPLITUDE;  // finite and greater than 0
    std::uint64_t period =
        DEFAULT_PERIOD;      // writes of one tag per cycle; at least 2
    std::uint64_t seed = 1;  // of WALK
};

inline constexpr double MAX_AMPLITUDE = 1e12;
inline constexpr std::uint64_t MIN_PERIOD = 2;
inline constexpr std::uint64_t MAX_PERIOD = 1'000'000'000;

// The fields CHECK derives from the write count `count` (from 1) of a tag:
// value `count`, quality `count % 3`, quality flags `count & CHECK_FLAGS`,
// source time CHECK_BASE_SOURCE_TIME + count. Every write changes the value,
// so a reader sees change count `count` as well.
inline constexpr std::uint16_t CHECK_FLAGS = 0x0FDF;  // all but OWNER_DOWN
inline constexpr std::uint64_t CHECK_QUALITIES = 3;
inline constexpr std::int64_t CHECK_BASE_SOURCE_TIME =
    1'700'000'000'000'000'000;
// The largest write count CHECK takes, so that the source time stays in
// range.
inline constexpr std::uint64_t MAX_CHECK_COUNT = 1'000'000'000'000'000;

// The value of write number `count` of a CHECK tag, as it is written. The
// read-only fields are 0. Precondition: 1 <= count <= MAX_CHECK_COUNT.
[[nodiscard]] scada_value check_value(std::uint64_t count) noexcept;

// True if `value`, read from a CHECK tag of a running owner, is whole: its
// fields belong to one write, or it was never written (quality BAD, flag
// NOT_INIT, value 0). False for anything else, including OWNER_DOWN.
[[nodiscard]] bool is_check_value(const scada_value& value) noexcept;

// The same for an entry of the change ring of a CHECK tag. An entry is
// written only by a write, so "never written" is not whole here.
[[nodiscard]] bool is_check_change(const scada_change& change) noexcept;

// The value of write number `count` (from 1) of the tag `tag` with the
// waveform `pattern`. `tag` shifts the phase, so that the tags of an owner
// differ. `previous` is the value of write `count - 1`, used by WALK only
// (0 before the first write). The result lies within plus and minus the
// amplitude. Precondition: `pattern` is not CHECK, and `waveform` holds the
// limits above.
[[nodiscard]] double wave_value(Pattern pattern, const Waveform& waveform,
                                std::uint64_t tag, std::uint64_t count,
                                double previous) noexcept;

}  // namespace scada::sim

#endif  // SCADA_SIM_PATTERN_H
