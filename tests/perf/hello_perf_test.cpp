// Sample performance test. libscada-hello has no target in section 15 of the
// reference document; the budget below is a placeholder that shows the
// pattern: measure many calls, compare the median with a stated budget, fail
// on a regression. Real targets replace it from phase 1 (issue P1-12).

#include "scada/hello/hello.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <sstream>
#include <vector>

namespace {

constexpr std::size_t CALLS = 10'000;
// Median time of one scada::hello::run call with no arguments.
constexpr std::chrono::microseconds MEDIAN_BUDGET{50};

TEST(HelloPerf, RunStaysWithinItsMedianBudget) {
    using Clock = std::chrono::steady_clock;
    std::ostringstream out;
    std::ostringstream err;
    std::vector<Clock::duration> samples;
    samples.reserve(CALLS);

    for (std::size_t i = 0; i < CALLS; ++i) {
        out.str({});
        const Clock::time_point begin = Clock::now();
        const int exit_code = scada::hello::run({}, out, err);
        samples.push_back(Clock::now() - begin);
        ASSERT_EQ(exit_code, scada::hello::EXIT_OK);
    }

    // The median, not the mean: one preempted call must not fail the test.
    const auto middle = samples.begin() + static_cast<std::ptrdiff_t>(CALLS / 2);
    std::nth_element(samples.begin(), middle, samples.end());
    const auto median =
        std::chrono::duration_cast<std::chrono::nanoseconds>(*middle);
    RecordProperty("median_ns", std::to_string(median.count()));
    EXPECT_LE(median, MEDIAN_BUDGET)
        << "median " << median.count() << " ns over " << CALLS << " calls";
}

}  // namespace
