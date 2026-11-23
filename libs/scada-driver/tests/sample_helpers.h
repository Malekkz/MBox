#ifndef SCADA_DRIVER_TESTS_SAMPLE_HELPERS_H
#define SCADA_DRIVER_TESTS_SAMPLE_HELPERS_H

// Test helpers around process_sample, which fills a caller's result.

#include "scada/driver/point_rules.h"
#include "scada/driver/sample_processing.h"

#include <span>

namespace scada::driver::test {

// One call of process_sample, returning its result by value.
inline ProcessedSample processed(const PointRules& rules, PointState& state,
                                 const RawSample& sample,
                                 std::span<const CompanionInput> companions,
                                 const SampleTimes& times) {
    ProcessedSample out;
    process_sample(rules, state, sample, companions, times, out);
    return out;
}

// A point state prepared and reset as the framework does at attach.
inline PointState fresh_state(const PointRules& rules) {
    PointState state;
    init_point_state(state, rules.tag_type);
    reset_point_state(state, rules.tag_type, 0);
    return state;
}

}  // namespace scada::driver::test

#endif  // SCADA_DRIVER_TESTS_SAMPLE_HELPERS_H
