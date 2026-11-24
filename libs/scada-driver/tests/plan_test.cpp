// Block planning (driver interface contract 6.3; conformance tests 10 and
// 11, the planning half): merging, gaps, the four kinds of range, split
// forms, companions, keep-alive. The expected requests are worked out by
// hand from the rules, not from the planner.

#include "scada/driver/plan.h"
#include "scada/common/status.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace scada::driver {
namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): the addresses, counts and times are the test data, each worked out by hand beside the rule it checks

using Kind = ReadRange::Kind;

constexpr std::uint64_t LARGE = 125;

struct Span {
    std::uint64_t first = 0;
    std::uint64_t count = 0;
    friend bool operator==(const Span&, const Span&) = default;
};

// One point of one unit, in group 0, space 0, unit id 1, its own cluster.
PlanPoint at(std::uint32_t point, std::uint64_t first,
             std::uint64_t count = 1) {
    return PlanPoint{.point = point,
                     .poll_group = 0,
                     .cluster = point,
                     .space = 0,
                     .unit_id = 1,
                     .first = first,
                     .count = count};
}

ReadPlan planned(const std::vector<PlanPoint>& points, const ReadRules& rules,
                 std::uint32_t groups = 1) {
    ReadPlan plan;
    const Status status = plan_blocks(points, rules, groups, plan);
    EXPECT_TRUE(status.ok()) << status.message();
    return plan;
}

// The spans of group `group`'s planned requests, in order.
std::vector<Span> spans(const ReadPlan& plan, std::uint32_t group = 0) {
    std::vector<Span> out;
    if (group >= plan.by_group.size()) {
        return out;
    }
    for (const std::uint32_t index : plan.by_group[group]) {
        out.push_back({plan.requests[index].first, plan.requests[index].count});
    }
    return out;
}

ReadRange range(Kind kind, std::uint64_t first, std::uint64_t last) {
    return ReadRange{
        .kind = kind, .space = 0, .unit_id = 1, .first = first, .last = last};
}

void expect_refused(const std::vector<PlanPoint>& points,
                    const ReadRules& rules, std::uint32_t groups = 1) {
    ReadPlan plan;
    EXPECT_EQ(plan_blocks(points, rules, groups, plan).code(),
              StatusCode::INVALID_ARGUMENT);
}

TEST(Plan, AdjacentPointsAreOneRequestUpToTheLargest) {
    std::vector<PlanPoint> points;
    for (std::uint32_t index = 0; index < 10; ++index) {
        points.push_back(at(index, 100 + index));
    }
    EXPECT_EQ(spans(planned(points, ReadRules{.max_units = LARGE})),
              (std::vector<Span>{{100, 10}}));
    // At most 4 units per request: 4, 4, 2.
    const ReadPlan four = planned(points, ReadRules{.max_units = 4});
    EXPECT_EQ(spans(four), (std::vector<Span>{{100, 4}, {104, 4}, {108, 2}}));
    EXPECT_EQ(requests_per_scan(four), 3U);
    // At most 3 values (points) per request: 3, 3, 3, 1.
    EXPECT_EQ(
        spans(planned(points, ReadRules{.max_units = LARGE, .max_values = 3})),
        (std::vector<Span>{{100, 3}, {103, 3}, {106, 3}, {109, 1}}));
}

TEST(Plan, AGapIsBridgedOnlyWithinMaxGap) {
    const std::vector<PlanPoint> points{at(0, 0), at(1, 1), at(2, 5), at(3, 6)};
    // Units 2, 3 and 4 between: a gap of 3.
    EXPECT_EQ(
        spans(planned(points, ReadRules{.max_units = LARGE, .max_gap = 3})),
        (std::vector<Span>{{0, 7}}));
    EXPECT_EQ(
        spans(planned(points, ReadRules{.max_units = LARGE, .max_gap = 2})),
        (std::vector<Span>{{0, 2}, {5, 2}}));
}

TEST(Plan, ANoReadRangeIsNeverBridgedNorRead) {
    const std::vector<PlanPoint> points{at(0, 0), at(1, 1), at(2, 5), at(3, 6)};
    ReadRules rules{.max_units = LARGE, .max_gap = 10};
    rules.ranges = {range(Kind::NO_READ, 3, 3)};
    EXPECT_EQ(spans(planned(points, rules)),
              (std::vector<Span>{{0, 2}, {5, 2}}));
    // A point inside it is refused.
    expect_refused({at(0, 3)}, rules);
}

TEST(Plan, AnAtomicRangeIsReadWholeAndNeverSplit) {
    ReadRules rules{.max_units = 4};
    rules.ranges = {range(Kind::ATOMIC, 10, 13)};
    // One point in it: the request covers the whole range.
    EXPECT_EQ(spans(planned({at(0, 11)}, rules)), (std::vector<Span>{{10, 4}}));
    // A point before it does not fit with it (9 to 13 is 5 units).
    EXPECT_EQ(spans(planned({at(0, 9), at(1, 11)}, rules)),
              (std::vector<Span>{{9, 1}, {10, 4}}));
    // A gap is never bridged into part of an atomic range.
    ReadRules bridging{.max_units = LARGE, .max_gap = 10};
    bridging.ranges = {range(Kind::ATOMIC, 4, 6)};
    EXPECT_EQ(spans(planned({at(0, 0), at(1, 10)}, bridging)),
              (std::vector<Span>{{0, 1}, {10, 1}}));
    // An atomic range larger than one request is refused.
    ReadRules too_large{.max_units = 3};
    too_large.ranges = {range(Kind::ATOMIC, 10, 13)};
    expect_refused({at(0, 11)}, too_large);
}

TEST(Plan, AnExclusiveRangeIsReadAloneAndNeverBridged) {
    ReadRules rules{.max_units = LARGE, .max_gap = 10};
    rules.ranges = {range(Kind::EXCLUSIVE, 20, 21)};
    const ReadPlan plan =
        planned({at(0, 19), at(1, 20), at(2, 21), at(3, 22)}, rules);
    EXPECT_EQ(spans(plan), (std::vector<Span>{{19, 1}, {20, 2}, {22, 1}}));
    const Request& exclusive = plan.requests[plan.by_group[0][1]];
    EXPECT_TRUE(exclusive.exclusive);
    EXPECT_TRUE(exclusive.side_effect);
    EXPECT_TRUE(exclusive.split.empty());
}

TEST(Plan, ASideEffectRangeIsReadOnceAndMayBeMerged) {
    ReadRules rules{.max_units = LARGE};
    rules.ranges = {range(Kind::SIDE_EFFECT, 30, 31)};
    // A point in it widens the request to the whole range; a neighbour
    // merges; the request is never split (CS A.4).
    const ReadPlan plan = planned({at(0, 30), at(1, 32)}, rules);
    EXPECT_EQ(spans(plan), (std::vector<Span>{{30, 3}}));
    EXPECT_TRUE(plan.requests[plan.by_group[0][0]].side_effect);
    EXPECT_TRUE(plan.requests[plan.by_group[0][0]].split.empty());
    // A gap is never bridged into it.
    ReadRules bridging{.max_units = LARGE, .max_gap = 10};
    bridging.ranges = {range(Kind::SIDE_EFFECT, 4, 6)};
    EXPECT_EQ(spans(planned({at(0, 0), at(1, 10)}, bridging)),
              (std::vector<Span>{{0, 1}, {10, 1}}));
    // Read by two poll groups: refused (CS CF-610).
    PlanPoint other = at(1, 31);
    other.poll_group = 1;
    expect_refused({at(0, 30), other}, rules, 2);
}

TEST(Plan, NeverAcrossSpacesUnitIdsOrPollGroups) {
    PlanPoint space = at(1, 1);
    space.space = 1;
    PlanPoint unit = at(2, 2);
    unit.unit_id = 2;
    PlanPoint group = at(3, 3);
    group.poll_group = 1;
    const ReadPlan plan = planned({at(0, 0), space, unit, group},
                                  ReadRules{.max_units = LARGE}, 2);
    EXPECT_EQ(plan.by_group[0].size(), 3U);
    EXPECT_EQ(spans(plan, 1), (std::vector<Span>{{3, 1}}));
}

TEST(Plan, AMultiUnitValueIsNeverSplit) {
    // Two 2-unit values with max_units 3: never 3 + 1.
    EXPECT_EQ(
        spans(planned({at(0, 0, 2), at(1, 2, 2)}, ReadRules{.max_units = 3})),
        (std::vector<Span>{{0, 2}, {2, 2}}));
    // A value larger than one request is refused.
    expect_refused({at(0, 0, 4)}, ReadRules{.max_units = 3});
}

TEST(Plan, AMergedRequestHasASplitFormOfOneRequestPerCluster) {
    // 0 and 1 are one cluster (a point and its companion), 3 another; the
    // gap at 2 is bridged.
    PlanPoint companion = at(1, 1);
    companion.cluster = 0;
    const ReadPlan plan = planned({at(0, 0), companion, at(2, 3)},
                                  ReadRules{.max_units = LARGE, .max_gap = 1});
    ASSERT_EQ(spans(plan), (std::vector<Span>{{0, 4}}));
    const Request& merged = plan.requests[plan.by_group[0][0]];
    ASSERT_EQ(merged.split.size(), 2U);
    EXPECT_EQ(plan.requests[merged.split[0]].first, 0U);
    EXPECT_EQ(plan.requests[merged.split[0]].count, 2U);
    EXPECT_EQ(plan.requests[merged.split[1]].first, 3U);
    EXPECT_EQ(plan.requests[merged.split[1]].count, 1U);
    // A split request is not part of the scan.
    EXPECT_EQ(requests_per_scan(plan), 1U);
    // A request of one cluster has no split form: refused, its points are
    // ABSENT.
    const ReadPlan single = planned({at(0, 0)}, ReadRules{.max_units = LARGE});
    EXPECT_TRUE(single.requests[single.by_group[0][0]].split.empty());
}

TEST(Plan, ACompanionThatDoesNotFitIsChainedBackToBack) {
    // Point 0 at unit 0 with its companion at 100; 4 units per request.
    PlanPoint companion = at(1, 100);
    companion.cluster = 0;
    const ReadPlan plan =
        planned({at(0, 0), companion, at(2, 1)}, ReadRules{.max_units = 4});
    ASSERT_EQ(spans(plan), (std::vector<Span>{{0, 1}, {100, 1}, {1, 1}}));
    EXPECT_TRUE(plan.requests[plan.by_group[0][0]].chained);
    EXPECT_FALSE(plan.requests[plan.by_group[0][1]].chained);
    // When they fit, they are one request.
    const ReadPlan fits =
        planned({at(0, 0), at(1, 3)}, ReadRules{.max_units = 4, .max_gap = 2});
    EXPECT_EQ(spans(fits), (std::vector<Span>{{0, 4}}));
}

TEST(Plan, KeepAliveIsTheShortestRequestOutsideEverySideEffect) {
    ReadRules rules{.max_units = 4};
    rules.ranges = {range(Kind::SIDE_EFFECT, 0, 0)};
    const ReadPlan plan =
        planned({at(0, 0), at(1, 10), at(2, 11), at(3, 20)}, rules);
    if (!plan.keepalive.has_value()) {
        ADD_FAILURE() << "no keep-alive request";
        return;
    }
    const Request& keepalive = plan.requests[*plan.keepalive];
    EXPECT_FALSE(keepalive.side_effect);
    EXPECT_EQ(keepalive.first, 20U);
    EXPECT_EQ(plan.probe, plan.keepalive);
    // Every request touches a side effect: no keep-alive.
    ReadRules all{.max_units = 4};
    all.ranges = {range(Kind::SIDE_EFFECT, 0, 1)};
    EXPECT_FALSE(planned({at(0, 0)}, all).keepalive.has_value());
}

TEST(Plan, RulesTheCompileStepRefuses) {
    expect_refused({at(0, 0)}, ReadRules{.max_units = 0});
    ReadRules overlapping{.max_units = LARGE};
    overlapping.ranges = {range(Kind::ATOMIC, 0, 5),
                          range(Kind::NO_READ, 5, 6)};
    expect_refused({at(0, 0)}, overlapping);
    ReadRules backwards{.max_units = LARGE};
    backwards.ranges = {range(Kind::ATOMIC, 5, 4)};
    expect_refused({at(0, 0)}, backwards);
    PlanPoint outside = at(0, 0);
    outside.poll_group = 3;
    expect_refused({outside}, ReadRules{.max_units = LARGE}, 2);
    expect_refused({at(0, 0, 0)}, ReadRules{.max_units = LARGE});
    expect_refused({at(0, UINT64_MAX, 2)}, ReadRules{.max_units = LARGE});
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

}  // namespace
}  // namespace scada::driver
