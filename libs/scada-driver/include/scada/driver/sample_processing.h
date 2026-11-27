#ifndef SCADA_DRIVER_SAMPLE_PROCESSING_H
#define SCADA_DRIVER_SAMPLE_PROCESSING_H

// The processing after decode (driver interface contract 5.1): from one raw
// sample of a point to the values, qualities and flags its tags are
// written with, and the staleness check of contract 7.5.
//
// Pure functions of the point's rules, its run-time state and the sample:
// no allocation, no lock, no system call, no clock read (the caller passes
// the times). Floating-point contraction is off for this code (decision
// D-98), so both architectures give the same bits.

#include "scada/common/realtime.h"
#include "scada/driver/point_rules.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/tag_value.h"
#include "scada/tag/types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace scada::driver {

// A raw sample: one outcome of one point in one answer (contract 5.1).
struct RawSample {
    Outcome outcome = Outcome::COMM_FAIL;
    RawValue raw{};  // meaningful for VALUE
    // For NO_VALUE: the marker's flags; for DEVICE_INVALID: the quality
    // (BAD or UNCERTAIN) and flags of the protocol's mapping (contract 5.2).
    // The flags are masked to DEVICE_FLAGS_MASK.
    QualityMark mark{.quality = tag::Quality::BAD};
};

// A companion in the same poll (contract 5.5): its outcome and raw value,
// and the quality and flags its own processing gave it (its invalid
// markers, its non-finite check, its own companions). A companion whose
// own value is BAD fails the points that depend on it, like a failed
// outcome; an UNCERTAIN one makes them at most UNCERTAIN (decision D-131).
struct CompanionInput {
    Outcome outcome = Outcome::COMM_FAIL;
    RawValue raw{};
    QualityMark mark{};
    // True when the companion's own tag is older than its `max_age`; a
    // health point that is stale counts as not good (CS 4.9).
    bool stale = false;
};

// The run-time state of one point that its samples depend on (contract
// 3.1: "STALE timer, a counter's previous value"). It lives as long as the
// point's run-time object, across a reload (contract 3.2).
struct PointState {
    // The value the tag was last written with, and its quality and flags.
    // For a STRING point the text is in `text`, not in `value`.
    TagValue value{};
    tag::Quality quality = tag::Quality::BAD;
    std::uint16_t flags = 0;
    std::int64_t source_time_ns = 0;
    // A value read from the device was written since the attach.
    bool has_value = false;
    // The time (monotonic) of the last read that confirmed the value
    // (contract 7.5): an answer with VALUE that matched no marker.
    std::int64_t confirmed_mono_ns = 0;
    // The last valid value (GOOD or UNCERTAIN, read from the device) and
    // its time (monotonic), for `keep_last` and a held DEVICE_INVALID.
    bool has_valid = false;
    TagValue valid_value{};
    std::int64_t valid_mono_ns = 0;
    // The last valid value is being held, UNCERTAIN with STALE, by a
    // `keep_last` marker or a DEVICE_INVALID answer; BAD after `max_age`
    // (CS 4.7; decision D-131).
    bool holding = false;
    // The point is ABSENT (contract 5.2), for `points_absent`.
    bool absent = false;
    // Counters (contract 5.4): the previous raw value, and the value `hold`
    // keeps until the counter passes it.
    bool counter_has_previous = false;
    std::uint64_t counter_previous = 0;
    bool counter_holding = false;
    TagValue counter_held{};
    // The bits' last values, bit n of `bits` for bit rule n.
    std::uint64_t bit_values = 0;
    // STRING points: the last text written; its capacity is reserved by
    // init_point_state and never grows.
    std::vector<char> text;
};

// Prepares `state` for a point of `type`, outside every cycle: reserves the
// text of a STRING point. Allocates.
void init_point_state(PointState& state, tag::TagType type);

// Resets `state` for a new attach (contract 9.1): no value, NOT_INIT, the
// staleness timers start at `now_mono_ns`. Keeps the reserved text.
SCADA_RT void reset_point_state(PointState& state, tag::TagType type,
                                std::int64_t now_mono_ns) noexcept;

// Takes `stored`, the value the point's tag holds after the writes of 9.1,
// as the state's last value, so that a sample before the first read (a
// COMM_FAIL) writes the value as stored and not 0 (contract 9.1 "value as
// stored", 5.2 "keeps its last value"; review M4). Quality, flags and
// `has_value` stay those of the reset. A value of another type than the
// state's is ignored; a STRING's text is copied within the reserved
// capacity.
SCADA_RT void seed_point_state(PointState& state,
                               const TagValue& stored) noexcept;

// One tag write that processing asks for.
struct TagWrite {
    TagValue value{};
    tag::Quality quality = tag::Quality::BAD;
    std::uint16_t flags = 0;
    std::int64_t source_time_ns = 0;
};

// What the diagnostics of contract 11.1 count from one sample.
struct SampleCounts {
    bool sentinel = false;       // `sentinels`: a NO_VALUE sample or marker
    bool invalid = false;        // `invalid`: DEVICE_INVALID, or non-finite
    bool counter_reset = false;  // `on_decrease: reset` applied
    bool became_absent = false;  // the point went ABSENT
    bool left_absent = false;    // the point was ABSENT and is no longer
};

// The result of processing one sample of a point. The caller keeps one and
// passes it to every call: nothing is built or copied per sample.
struct ProcessedSample {
    // The point's own tag; `write_tag` is false for `tag: false`.
    bool write_tag = false;
    TagWrite tag{};
    // One BOOL write per bit rule, in the order of PointRules::bits: bit n
    // of `bit_values`, with the quality, flags and time of `tag`.
    std::size_t bit_count = 0;
    std::uint64_t bit_values = 0;
    SampleCounts counts{};
};

// The value of bit rule `index` in `sample`.
[[nodiscard]] constexpr bool sample_bit(const ProcessedSample& sample,
                                        std::size_t index) noexcept {
    return index < sample.bit_count && ((sample.bit_values >> index) & 1U) != 0;
}

// The times of one answer.
struct SampleTimes {
    std::int64_t receive_time_ns = 0;  // CLOCK_REALTIME at the answer
    std::int64_t receive_mono_ns = 0;  // CLOCK_MONOTONIC at the answer
};

// Processes `sample` of the point with `rules` and `state`, in the order of
// contract 5.1, steps 1 to 7, updates `state` and fills `out`. `companions`
// holds one input per slot of `rules.companions`, in order; a missing slot
// counts as COMM_FAIL. The returned STRING value (if any) points into
// `state`, and is valid until the next call with `state`. Rules that
// validate_point_rules refuses give BAD, never undefined behaviour.
SCADA_RT void process_sample(const PointRules& rules, PointState& state,
                             const RawSample& sample,
                             std::span<const CompanionInput> companions,
                             const SampleTimes& times,
                             ProcessedSample& out) noexcept;

// The staleness check of contract 7.5 at a transaction boundary. Returns
// true and fills `out` when the point's tag must be written again: STALE
// added once to its last value and quality when no read confirmed it within
// `max_age`, or a held value that passed `max_age` going BAD (CS 4.7;
// decision D-131).
SCADA_RT [[nodiscard]] bool check_stale(const PointRules& rules,
                                        PointState& state,
                                        std::int64_t now_mono_ns,
                                        ProcessedSample& out) noexcept;

}  // namespace scada::driver

#endif  // SCADA_DRIVER_SAMPLE_PROCESSING_H
