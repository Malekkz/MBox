#include "scada/sim/pattern.h"

#include "scada/tag/scada_tag.h"
#include "scada/tag/types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string_view>

namespace scada::sim {

namespace {

struct NamedPattern {
    std::string_view name;
    Pattern pattern;
};

constexpr std::array<NamedPattern, 5> PATTERNS{{
    {.name = "check", .pattern = Pattern::CHECK},
    {.name = "ramp", .pattern = Pattern::RAMP},
    {.name = "sine", .pattern = Pattern::SINE},
    {.name = "step", .pattern = Pattern::STEP},
    {.name = "walk", .pattern = Pattern::WALK},
}};

// SplitMix64 (Steele, Lea and Flood, 2014): a fixed, well-mixed function of
// its input, so that the steps of WALK need no generator state per tag. Not
// for security. The arithmetic wraps by design: unsigned.
constexpr std::uint64_t mix(std::uint64_t bits) noexcept {
    constexpr std::uint64_t GOLDEN = 0x9E37'79B9'7F4A'7C15U;
    constexpr std::uint64_t MUL1 = 0xBF58'476D'1CE4'E5B9U;
    constexpr std::uint64_t MUL2 = 0x94D0'49BB'1331'11EBU;
    constexpr unsigned SHIFT1 = 30;
    constexpr unsigned SHIFT2 = 27;
    constexpr unsigned SHIFT3 = 31;
    bits += GOLDEN;
    bits = (bits ^ (bits >> SHIFT1)) * MUL1;
    bits = (bits ^ (bits >> SHIFT2)) * MUL2;
    return bits ^ (bits >> SHIFT3);
}

// A number in [-1, 1) from the top 53 bits of `bits`.
double unit_interval(std::uint64_t bits) noexcept {
    constexpr unsigned MANTISSA_SHIFT = 11;
    constexpr double TWO_TO_MINUS_52 = 0x1p-52;
    // In range: a 53-bit number is exact in a double.
    return (static_cast<double>(bits >> MANTISSA_SHIFT) * TWO_TO_MINUS_52) -
           1.0;
}

constexpr double FULL_CIRCLE = 2.0 * std::numbers::pi;
// STEP is high from this fraction of the period on.
constexpr double STEP_HIGH_FROM = 0.5;
// The largest step of WALK, in units of amplitude / period.
constexpr double WALK_LARGEST_STEP = 2.0;

}  // namespace

std::optional<Pattern> pattern_from_name(std::string_view name) noexcept {
    for (const NamedPattern& entry : PATTERNS) {
        if (entry.name == name) {
            return entry.pattern;
        }
    }
    return std::nullopt;
}

std::string_view to_string(Pattern pattern) noexcept {
    for (const NamedPattern& entry : PATTERNS) {
        if (entry.pattern == pattern) {
            return entry.name;
        }
    }
    return "unknown";
}

scada::tag::TagType tag_type(Pattern pattern) noexcept {
    return pattern == Pattern::CHECK ? scada::tag::TagType::UINT64
                                     : scada::tag::TagType::FLOAT64;
}

scada_value check_value(std::uint64_t count) noexcept {
    scada_value value{};
    value.type = SCADA_TYPE_UINT64;
    value.value = count;
    // In range: below CHECK_QUALITIES and within CHECK_FLAGS.
    value.quality = static_cast<std::uint8_t>(count % CHECK_QUALITIES);
    value.quality_flags = static_cast<std::uint16_t>(count & CHECK_FLAGS);
    // In range: count is at most MAX_CHECK_COUNT.
    value.source_time_ns =
        CHECK_BASE_SOURCE_TIME + static_cast<std::int64_t>(count);
    return value;
}

bool is_check_value(const scada_value& value) noexcept {
    const std::uint64_t count = value.value;
    if (count == 0) {
        // Never written: as attach left the record.
        return value.type == SCADA_TYPE_UINT64 &&
               value.quality == SCADA_QUALITY_BAD &&
               value.quality_flags == SCADA_FLAG_NOT_INIT;
    }
    if (count > MAX_CHECK_COUNT) {
        return false;
    }
    const scada_value written = check_value(count);
    // The change count wraps at 32 bits, as the record's field does.
    return value.type == written.type && value.quality == written.quality &&
           value.quality_flags == written.quality_flags &&
           value.source_time_ns == written.source_time_ns &&
           value.change_count == static_cast<std::uint32_t>(count);
}

bool is_check_change(const scada_change& change) noexcept {
    const std::uint64_t count = change.value;
    if (count == 0 || count > MAX_CHECK_COUNT) {
        return false;
    }
    const scada_value written = check_value(count);
    return change.type == written.type && change.quality == written.quality &&
           change.quality_flags == written.quality_flags &&
           change.source_time_ns == written.source_time_ns;
}

double wave_value(Pattern pattern, const Waveform& waveform, std::uint64_t tag,
                  std::uint64_t count, double previous) noexcept {
    const double amplitude = waveform.amplitude;
    const std::uint64_t period = waveform.period;
    // The position in the cycle, shifted by the tag. In range: below
    // MAX_PERIOD, exact in a double.
    const auto phase =
        static_cast<double>(((count % period) + (tag % period)) % period);
    const auto cycle = static_cast<double>(period);
    switch (pattern) {
        case Pattern::RAMP:
            return amplitude * phase / cycle;
        case Pattern::SINE:
            return amplitude * std::sin(FULL_CIRCLE * phase / cycle);
        case Pattern::STEP:
            return phase < STEP_HIGH_FROM * cycle ? 0.0 : amplitude;
        case Pattern::WALK: {
            // A step of at most 2 * amplitude / period in either direction,
            // fixed by seed, tag and count.
            const double step =
                WALK_LARGEST_STEP * amplitude / cycle *
                unit_interval(mix(waveform.seed ^ mix(tag ^ mix(count))));
            return std::clamp(previous + step, -amplitude, amplitude);
        }
        case Pattern::CHECK:
            break;
    }
    return 0.0;
}

}  // namespace scada::sim
