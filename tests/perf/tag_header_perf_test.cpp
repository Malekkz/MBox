// Performance test for the header builder (issue P1-03): a 50,000-tag header
// builds in under 1 second. 50,000 tags across 10 owners is the reference
// load of the tag database contract (section 15, test 2).

#include "scada/tag/header_builder.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace scada::tag;
using Clock = std::chrono::steady_clock;

constexpr std::size_t REFERENCE_TAGS = 50'000;
constexpr std::size_t REFERENCE_OWNERS = 10;
constexpr std::chrono::milliseconds BUILD_BUDGET{1000};
constexpr int RUNS = 5;

// A prime that divides neither tag count used here, so that stepping by it
// visits every tag number once, in an order that is far from sorted.
constexpr std::size_t SCATTER = 7919;
constexpr std::uint32_t UID = 1000;

// `tags` tags spread evenly over `owners` owner threads of one process, in
// a scattered order so that the builder has to sort.
TagSet make_tag_set(std::size_t tags, std::size_t owners,
                    std::uint32_t ring_capacity) {
    TagSet tag_set;
    tag_set.processes = {{.name = "io", .uid = UID}};
    for (std::size_t owner = 0; owner < owners; ++owner) {
        tag_set.owners.push_back({.name = "io.channel" + std::to_string(owner),
                                  .process = "io",
                                  .change_ring_capacity = ring_capacity});
    }
    tag_set.tags.reserve(tags);
    for (std::size_t i = 0; i < tags; ++i) {
        const std::size_t number = (i * SCATTER) % tags;
        const std::size_t owner = number % owners;
        tag_set.tags.push_back(
            {.name = "site.area" + std::to_string(owner) + ".device" +
                     std::to_string(number / owners) + ".measurement.value",
             .owner = "io.channel" + std::to_string(owner),
             .type = TagType::FLOAT64});
    }
    return tag_set;
}

// Builds the header of `tag_set` `RUNS` times and returns the median time.
std::chrono::microseconds median_build_time(const TagSet& tag_set,
                                            std::size_t expected_tags) {
    std::vector<Clock::duration> samples;
    for (int run = 0; run < RUNS; ++run) {
        std::string detail;
        const Clock::time_point begin = Clock::now();
        const auto image = build_header(tag_set, {}, detail);
        samples.push_back(Clock::now() - begin);
        EXPECT_TRUE(image.ok()) << detail;
        if (image.ok()) {
            EXPECT_GT(image.value().bytes.size(), expected_tags);
        }
    }
    // The median, not the mean: one preempted run must not fail the test.
    std::ranges::sort(samples);
    return std::chrono::duration_cast<std::chrono::microseconds>(
        samples.at(samples.size() / 2));
}

TEST(TagHeaderPerf, FiftyThousandTagHeaderBuildsWithinOneSecond) {
    const TagSet tag_set = make_tag_set(REFERENCE_TAGS, REFERENCE_OWNERS,
                                        DEFAULT_CHANGE_RING_CAPACITY);
    const auto median = median_build_time(tag_set, REFERENCE_TAGS);
    RecordProperty("median_us", std::to_string(median.count()));
    EXPECT_LE(median, BUILD_BUDGET)
        << "median " << median.count() << " us over " << RUNS << " builds";
}

// Not a target, but reported: the largest tag set of layout version 1
// (contract, section 14) builds, and how long it takes.
TEST(TagHeaderPerf, LargestTagSetBuilds) {
    constexpr std::size_t OWNERS = 2;
    const TagSet tag_set =
        make_tag_set(MAX_TAGS, OWNERS, MAX_CHANGE_RING_CAPACITY);
    std::string detail;
    const Clock::time_point begin = Clock::now();
    const auto image = build_header(tag_set, {}, detail);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - begin);
    ASSERT_TRUE(image.ok()) << detail;
    RecordProperty("elapsed_ms", std::to_string(elapsed.count()));
    RecordProperty("image_bytes", std::to_string(image.value().bytes.size()));
}

}  // namespace
