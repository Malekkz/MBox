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

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::driver {

// A raw sample: one outcome of one point in one answer (contract 5.1).
struct RawSample {
    Outcome outcome = Outcome::COMM_FAIL;
    RawValue raw{};  // meaningful for VALUE
    // For NO_VALUE: the marker's flags; for DEVICE_INVALID: the quality
    // (BAD or UNCERTAIN) and flags of the protocol's mapping (contract 5.2).
    QualityMark mark{.quality = tag::Quality::BAD};
};

// The outcome and raw value of a companion in the same poll (contract 5.5).
struct CompanionInput {
    Outcome outcome = Outcome::COMM_FAIL;
    RawValue raw{};
    // True when the companion's own tag is older than its `max_age`; a
    // health point that is stale counts as not good (CS 4.9).
    bool stale = false;
};

// The run-time state of one point that its samples depend on (contract
// 3.1: "STALE timer, a counter's previous value"). It lives as long as the
// point's run-time object, across a reload (contract 3.2).
//
// For a STRING point, `text` holds the last value written, so that a sample
// that is not VALUE writes it again; its storage is the state's own.
struct PointState {
    // The value the tag was last written with, and its quality and flags.
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
    // its time (monotonic), for `keep_last`.
    bool has_valid = false;
    TagValue valid_value{};
    std::int64_t valid_mono_ns = 0;
    // A `keep_last` marker is holding the last value (CS 4.7).
    bool keeping_last = false;
    // The point is ABSENT (contract 5.2), for `points_absent`.
    bool absent = false;
    // Counters (contract 5.4): the previous raw value, and whether `hold`
    // is keeping it.
    bool counter_has_previous = false;
    std::uint64_t counter_previous = 0;
    bool counter_holding = false;
    // The bits' last values, by bit rule.
    std::array<bool, MAX_POINT_BITS> bit_values{};
    // STRING points.
    std::array<char, TAG_STRING_MAX_BYTES> text{};
    std::size_t text_length = 0;
};

// Resets `state` for a new attach (contract 9.1): no value, NOT_INIT, the
// staleness timers start at `now_mono_ns`. `type` is the tag's type.
SCADA_RT void reset_point_state(PointState& state, tag::TagType type,
                                std::int64_t now_mono_ns) noexcept;

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

// The result of processing one sample of a point.
struct ProcessedSample {
    // The point's own tag; `write_tag` is false for `tag: false`.
    bool write_tag = false;
    TagWrite tag{};
    // One write per bit rule, in the order of PointRules::bits.
    std::size_t bit_count = 0;
    std::array<TagWrite, MAX_POINT_BITS> bits{};
    SampleCounts counts{};
};

// The times of one answer.
struct SampleTimes {
    std::int64_t receive_time_ns = 0;  // CLOCK_REALTIME at the answer
    std::int64_t receive_mono_ns = 0;  // CLOCK_MONOTONIC at the answer
};

// Processes `sample` of the point with `rules` and `state`, in the order of
// contract 5.1, steps 1 to 7, and updates `state`. `companions` holds one
// input per slot of `rules.companions`, in order; a missing slot counts as
// COMM_FAIL. The returned STRING value (if any) points into `state` or into
// `sample.raw.bytes`, and is valid until the next call with `state` or the
// end of the delivery, whichever comes first.
SCADA_RT [[nodiscard]] ProcessedSample process_sample(
    const PointRules& rules, PointState& state, const RawSample& sample,
    std::span<const CompanionInput> companions,
    const SampleTimes& times) noexcept;

// The staleness check of contract 7.5 at a transaction boundary. Returns
// true and fills `out` when the point's tag must be written again: STALE
// added once to its last value and quality when no read confirmed it within
// `max_age`, or a `keep_last` value that passed `max_age` going BAD (CS
// 4.7). `out` covers the point's tag and its bits, as process_sample.
SCADA_RT [[nodiscard]] bool check_stale(const PointRules& rules,
                                        PointState& state,
                                        std::int64_t now_mono_ns,
                                        ProcessedSample& out) noexcept;

}  // namespace scada::driver

#endif  // SCADA_DRIVER_SAMPLE_PROCESSING_H
