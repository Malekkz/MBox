// Block planning (driver interface contract 6.3; conformance tests 10 and
// 11, the planning half): merging, gaps, the four kinds of range, split
// forms, companions, keep-alive. The expected requests are worked out by
// hand from the rules, not from the planner.

#include "scada/driver/plan.h"
#include "scada/common/status.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>
#include <utility>
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

TEST(Plan, ASideEffectRangeIsReadOverItsPointsOnceAndMayBeMerged) {
    // The lead's ruling R4 (CS A.4): one point at 10 in a range 10..29
    // reads [10], not the whole range; all points of the range are in one
    // request over the span they decode; an adjacent neighbour merges; the
    // request is never split.
    ReadRules rules{.max_units = LARGE};
    rules.ranges = {range(Kind::SIDE_EFFECT, 10, 29)};
    EXPECT_EQ(spans(planned({at(0, 10)}, rules)), (std::vector<Span>{{10, 1}}));
    const ReadPlan plan = planned({at(0, 12), at(1, 20), at(2, 30)}, rules);
    // 12..20; 30 is outside, and the units between are the range's.
    EXPECT_EQ(spans(plan), (std::vector<Span>{{12, 9}, {30, 1}}));
    rules.ranges = {range(Kind::SIDE_EFFECT, 30, 31)};
    const ReadPlan merged = planned({at(0, 31), at(1, 32)}, rules);
    EXPECT_EQ(spans(merged), (std::vector<Span>{{31, 2}}));
    EXPECT_TRUE(merged.requests[merged.by_group[0][0]].side_effect);
    EXPECT_TRUE(merged.requests[merged.by_group[0][0]].split.empty());
    // No bridging out of it over its unread units: 30 and 32 with 31 unread.
    EXPECT_EQ(spans(planned({at(0, 30), at(1, 32)}, rules)),
              (std::vector<Span>{{30, 1}, {32, 1}}));
    // Its points that do not fit one request: refused.
    rules.ranges = {range(Kind::SIDE_EFFECT, 0, 199)};
    expect_refused({at(0, 0), at(1, 150)}, rules);
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
    // Point 0 at unit 0 with its companion at 100; 4 units per request. The
    // chain's first request still takes its neighbour at 1: a chain does
    // not cut a run.
    PlanPoint companion = at(1, 100);
    companion.cluster = 0;
    const ReadPlan plan =
        planned({at(0, 0), companion, at(2, 1)}, ReadRules{.max_units = 4});
    ASSERT_EQ(spans(plan), (std::vector<Span>{{0, 2}, {100, 1}}));
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

// --- Committee review of PR 135 -------------------------------------------

TEST(Plan, OverlappingRangesOfOneKindAreReadAsTheirUnion) {
    // K3: side-effect [47..50] and [50..52] are one range: points at 47 and
    // 52 in one request, read by one poll group only.
    ReadRules rules{.max_units = LARGE};
    rules.ranges = {range(Kind::SIDE_EFFECT, 47, 50),
                    range(Kind::SIDE_EFFECT, 50, 52)};
    EXPECT_EQ(spans(planned({at(0, 47), at(1, 52)}, rules)),
              (std::vector<Span>{{47, 6}}));
    PlanPoint other = at(1, 52);
    other.poll_group = 1;
    expect_refused({at(0, 47), other}, rules, 2);
    // Atomic [10..12] and [12..14]: one point at 10 reads 10..14.
    rules.ranges = {range(Kind::ATOMIC, 10, 12), range(Kind::ATOMIC, 12, 14)};
    EXPECT_EQ(spans(planned({at(0, 10)}, rules)), (std::vector<Span>{{10, 5}}));
    // Their union larger than one request: refused.
    rules.max_units = 4;
    expect_refused({at(0, 10)}, rules);
}

TEST(Plan, EachSpaceKeepsItsOwnLimits) {
    // F3: space 0 (registers) 2 units, space 1 (bits) 8, no gap in 0 and a
    // gap of 2 in 1.
    const ReadRules rules{
        .max_units = 1,
        .spaces = {{.space = 0, .max_units = 2},
                   {.space = 1, .max_units = 8, .max_gap = 2}}};
    std::vector<PlanPoint> points;
    for (std::uint32_t index = 0; index < 4; ++index) {
        points.push_back(at(index, std::uint64_t{index} * 2));  // 0, 2, 4, 6
        PlanPoint bit = at(10 + index, std::uint64_t{index} * 2);
        bit.space = 1;
        points.push_back(bit);
    }
    const ReadPlan plan = planned(points, rules);
    std::vector<std::pair<std::uint32_t, Span>> got;
    for (const std::uint32_t index : plan.by_group[0]) {
        const Request& request = plan.requests[index];
        got.emplace_back(request.space, Span{request.first, request.count});
    }
    EXPECT_EQ(
        got,
        (std::vector<std::pair<std::uint32_t, Span>>{
            {0, {0, 1}}, {0, {2, 1}}, {0, {4, 1}}, {0, {6, 1}}, {1, {0, 7}}}));
    const ReadRules twice{
        .max_units = 1,
        .spaces = {{.space = 0, .max_units = 2}, {.space = 0, .max_units = 3}}};
    expect_refused({at(0, 0)}, twice);
    const ReadRules zero{.max_units = 1,
                         .spaces = {{.space = 0, .max_units = 0}}};
    expect_refused({at(0, 0)}, zero);
}

TEST(Plan, MaxValuesHoldsForEveryRequest) {
    // F5: three points in an atomic range with max_values 2 cannot be read;
    // overlapping points count each.
    ReadRules rules{.max_units = LARGE, .max_values = 2};
    rules.ranges = {range(Kind::ATOMIC, 0, 5)};
    expect_refused({at(0, 0), at(1, 2), at(2, 4)}, rules);
    rules.ranges = {};
    expect_refused({at(0, 0, 3), at(1, 1), at(2, 2)}, rules);
    // A side-effect range whose points exceed it: refused too.
    rules.ranges = {range(Kind::SIDE_EFFECT, 0, 9)};
    expect_refused({at(0, 0), at(1, 3), at(2, 6)}, rules);
}

TEST(Plan, APointAcrossTheEdgeOfAnExclusiveRangeIsRefused) {
    // F6: exclusive [55..58]; a point of 53..56 would pull outside units
    // into the request that is read alone.
    ReadRules rules{.max_units = LARGE, .max_gap = 10};
    rules.ranges = {range(Kind::EXCLUSIVE, 55, 58)};
    expect_refused({at(0, 53, 4)}, rules);
    expect_refused({at(0, 58, 2)}, rules);
    EXPECT_EQ(spans(planned({at(0, 53), at(1, 55, 4), at(2, 59)}, rules)),
              (std::vector<Span>{{53, 1}, {55, 4}, {59, 1}}));
}

TEST(Plan, AFarCompanionThatFitsIsOneRequestWithThePointsBetween) {
    // F7: a value at 100, its companion at 150, 49 points between, 125
    // units per request: one request [100..150], not three.
    std::vector<PlanPoint> points;
    points.push_back(at(0, 100));
    PlanPoint companion = at(1, 150);
    companion.cluster = 0;
    points.push_back(companion);
    for (std::uint32_t index = 0; index < 49; ++index) {
        points.push_back(at(2 + index, 101 + index));
    }
    const ReadPlan plan = planned(points, ReadRules{.max_units = LARGE});
    EXPECT_EQ(spans(plan), (std::vector<Span>{{100, 51}}));
    // A cluster that does fit is never split across a request boundary
    // that the address order alone would choose: points 0..7 at 4 units a
    // request, with 3 and 4 a value and its companion.
    std::vector<PlanPoint> run;
    for (std::uint32_t index = 0; index < 8; ++index) {
        run.push_back(at(index, index));
    }
    run[4].cluster = 3;
    const ReadPlan bounded = planned(run, ReadRules{.max_units = 4});
    for (const std::uint32_t index : bounded.by_group[0]) {
        const Request& request = bounded.requests[index];
        const bool has3 =
            request.first <= 3 && 3 < request.first + request.count;
        const bool has4 =
            request.first <= 4 && 4 < request.first + request.count;
        EXPECT_EQ(has3, has4) << "request at " << request.first;
        EXPECT_FALSE(request.chained);
    }
}

TEST(Plan, TheKeepAliveIsNeverARequestOfAChain) {
    // K1: the only request outside a side-effect range is a chain's: no
    // keep-alive at all.
    PlanPoint companion = at(1, 100);
    companion.cluster = 0;
    const ReadPlan plan =
        planned({at(0, 0), companion}, ReadRules{.max_units = 4});
    ASSERT_EQ(spans(plan), (std::vector<Span>{{0, 1}, {100, 1}}));
    EXPECT_FALSE(plan.keepalive.has_value());
    EXPECT_FALSE(plan.probe.has_value());
}

TEST(Plan, ASplitFormKeepsAValueWithItsCompanion) {
    // Points at 0, 2 and 4, bridged into one request; 0 and 4 a value and
    // its companion: the split form is [0..4] whole, not three parts.
    std::vector<PlanPoint> points{at(0, 0), at(1, 2), at(2, 4)};
    points[2].cluster = 0;
    const ReadPlan plan =
        planned(points, ReadRules{.max_units = LARGE, .max_gap = 1});
    ASSERT_EQ(spans(plan), (std::vector<Span>{{0, 5}}));
    EXPECT_TRUE(plan.requests[plan.by_group[0][0]].split.empty());
}

// --- Delta check of PR 135: D3, E1 ----------------------------------------

TEST(Plan, ALatchOfAnotherClusterIsNotMergedIntoAChain) {
    // D3: a value at 0 with its companion at 100, 4 units per request, and a
    // latch at 1 in a side-effect range 1..1: the latch is its own request,
    // never a part of the chain that an order may drop and repeat.
    ReadRules rules{.max_units = 4};
    rules.ranges = {range(Kind::SIDE_EFFECT, 1, 1)};
    PlanPoint companion = at(1, 100);
    companion.cluster = 0;
    const ReadPlan plan = planned({at(0, 0), companion, at(2, 1)}, rules);
    EXPECT_EQ(spans(plan), (std::vector<Span>{{0, 1}, {100, 1}, {1, 1}}));
    EXPECT_TRUE(plan.requests[plan.by_group[0][0]].chained);
    EXPECT_FALSE(plan.requests[plan.by_group[0][2]].chained);
    EXPECT_TRUE(plan.requests[plan.by_group[0][2]].side_effect);
}

TEST(Plan, ASideEffectCompanionIsReadLastInItsChain) {
    // D3: a value at 100 whose companion, a status word at 0, is in a
    // side-effect range: the chain reads 100 first and the status word
    // last, so a chain dropped before its end never read the side effect.
    ReadRules rules{.max_units = 4};
    rules.ranges = {range(Kind::SIDE_EFFECT, 0, 0)};
    PlanPoint status = at(1, 0);
    status.cluster = 0;
    const ReadPlan plan = planned({at(0, 100), status}, rules);
    EXPECT_EQ(spans(plan), (std::vector<Span>{{100, 1}, {0, 1}}));
    EXPECT_TRUE(plan.requests[plan.by_group[0][0]].chained);
    EXPECT_TRUE(plan.requests[plan.by_group[0][1]].side_effect);
    // A chain that would read two side-effect ranges: refused.
    rules.ranges = {range(Kind::SIDE_EFFECT, 0, 0),
                    range(Kind::SIDE_EFFECT, 200, 200)};
    PlanPoint other = at(2, 200);
    other.cluster = 0;
    expect_refused({at(0, 100), status, other}, rules);
}

TEST(Plan, ADeviceWhoseRequestsAreAllChainedStillHasAKeepAlive) {
    // E1: 49 points at 101..149, one of them with a companion at 300, 125
    // units per request: [101..149] and [300] form a chain. The keep-alive
    // is a request of its own, reading one point whose cluster is whole.
    std::vector<PlanPoint> points;
    for (std::uint32_t index = 0; index < 49; ++index) {
        points.push_back(at(index, 101 + index));
    }
    PlanPoint companion = at(49, 300);
    companion.cluster = 0;
    points.push_back(companion);
    const ReadPlan plan = planned(points, ReadRules{.max_units = LARGE});
    ASSERT_EQ(spans(plan), (std::vector<Span>{{101, 49}, {300, 1}}));
    ASSERT_TRUE(plan.keepalive.has_value());
    const std::uint32_t keepalive = plan.keepalive.value_or(0);
    EXPECT_GE(keepalive, 2U);  // not a planned request
    EXPECT_EQ(plan.requests[keepalive].count, 1U);
    EXPECT_NE(plan.requests[keepalive].first, 101U);  // not the chained value
    EXPECT_EQ(plan.probe, plan.keepalive);
}

TEST(Plan, AClusterOfExactlyMaxUnitsIsOneRequest) {
    // A value at 0 and its companion at 3, 4 units per request: the
    // cluster spans exactly 4 units and is fused, not chained (the PR 135
    // final pass).
    PlanPoint companion = at(1, 3);
    companion.cluster = 0;
    const ReadPlan plan =
        planned({at(0, 0), companion}, ReadRules{.max_units = 4, .max_gap = 2});
    EXPECT_EQ(spans(plan), (std::vector<Span>{{0, 4}}));
    EXPECT_FALSE(plan.requests[plan.by_group[0][0]].chained);
    // One unit more is a chain.
    PlanPoint further = at(1, 4);
    further.cluster = 0;
    const ReadPlan chain =
        planned({at(0, 0), further}, ReadRules{.max_units = 4, .max_gap = 3});
    EXPECT_EQ(spans(chain), (std::vector<Span>{{0, 1}, {4, 1}}));
    EXPECT_TRUE(chain.requests[chain.by_group[0][0]].chained);
}

// --- The identity cycle (3.6, 7.6; D-147) --------------------------------

PlanPoint identity_at(std::uint32_t point, std::uint64_t first,
                      std::uint32_t group = 0) {
    PlanPoint out = at(point, first);
    out.poll_group = group;
    out.identity = true;
    return out;
}

TEST(Plan, TheIdentityCycleHoldsTheRequestsOfIdentityPoints) {
    // Two groups: the identity point 1 shares a request with 0 in group 0;
    // group 1's request reads none.
    PlanPoint other = at(2, 50);
    other.poll_group = 1;
    const ReadPlan plan = planned({at(0, 0), identity_at(1, 1), other},
                                  ReadRules{.max_units = 4}, 2);
    ASSERT_EQ(plan.identity.size(), 1U);
    EXPECT_EQ(plan.identity[0], plan.by_group[0][0]);
    // Without one, it is empty.
    EXPECT_TRUE(
        planned({at(0, 0)}, ReadRules{.max_units = 4}).identity.empty());
}

TEST(Plan, TheIdentityCycleTakesAChainWhole) {
    // The identity point 1 is the companion of 0, read back to back: both
    // requests of the chain are in the cycle, in order.
    PlanPoint companion = identity_at(1, 100);
    companion.cluster = 0;
    const ReadPlan plan =
        planned({at(0, 0), companion, at(2, 50)}, ReadRules{.max_units = 4});
    ASSERT_EQ(plan.identity.size(), 2U);  // not the request at 50
    EXPECT_TRUE(plan.requests[plan.identity[0]].chained);
    EXPECT_EQ(plan.requests[plan.identity[0]].first, 0U);
    EXPECT_EQ(plan.requests[plan.identity[1]].first, 100U);
}

TEST(Plan, APointInNoPollGroupIsReadOnlyByTheIdentityCycle) {
    // An identity check on a protocol address: no group, no keep-alive.
    const ReadPlan plan =
        planned({at(0, 0), identity_at(1, 300, NO_POLL_GROUP)},
                ReadRules{.max_units = 4});
    EXPECT_EQ(spans(plan), (std::vector<Span>{{0, 1}}));
    ASSERT_EQ(plan.identity.size(), 1U);
    const Request& read = plan.requests[plan.identity[0]];
    EXPECT_EQ(read.first, 300U);
    EXPECT_EQ(read.poll_group, NO_POLL_GROUP);
    if (!plan.keepalive.has_value()) {
        FAIL() << "no keep-alive request";
    }
    EXPECT_EQ(plan.requests[*plan.keepalive].first, 0U);
    EXPECT_EQ(requests_per_scan(plan), 1U);
    // A point in no group that is no identity point is refused.
    PlanPoint lost = at(1, 300);
    lost.poll_group = NO_POLL_GROUP;
    expect_refused({at(0, 0), lost}, ReadRules{.max_units = 4});
}

TEST(Plan, AnIdentityPointIsNeverReadWithASideEffect) {
    ReadRules rules{.max_units = 8, .max_gap = 4};
    rules.ranges = {range(Kind::SIDE_EFFECT, 0, 0)};
    // In the range: refused.
    expect_refused({identity_at(0, 0)}, rules);
    // Next to it, within the gap: two requests, the identity one without.
    const ReadPlan plan = planned({at(0, 0), identity_at(1, 2)}, rules);
    ASSERT_EQ(plan.identity.size(), 1U);
    EXPECT_FALSE(plan.requests[plan.identity[0]].side_effect);
    EXPECT_EQ(spans(plan).size(), 2U);
    // A companion of a side-effect point in another request: the chain
    // reads a side effect, refused.
    PlanPoint companion = identity_at(1, 100);
    companion.cluster = 0;
    ReadRules chained{.max_units = 4};
    chained.ranges = {range(Kind::SIDE_EFFECT, 0, 0)};
    expect_refused({at(0, 0), companion}, chained);
}

TEST(Plan, AnIdentityPointInASideEffectRangeIsRefusedAsSuch) {
    // Review P2: refused by the identity rule itself, before the later
    // check of the identity cycle, which would refuse the plan too.
    ReadRules rules{.max_units = 8};
    rules.ranges = {range(Kind::SIDE_EFFECT, 5, 6)};
    ReadPlan plan;
    const Status status = plan_blocks({identity_at(0, 5)}, rules, 1, plan);
    EXPECT_EQ(status.code(), StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(std::string_view(status.message()),
              "an identity point in a side-effect range");
}

TEST(Plan, AnIdentityPointInAnyRangeButAnAtomicOneIsRefused) {
    // PR 171 S7: each kind of range on its own, the identity point inside.
    for (const Kind kind :
         {Kind::NO_READ, Kind::EXCLUSIVE, Kind::SIDE_EFFECT}) {
        ReadRules rules{.max_units = 8};
        rules.ranges = {range(kind, 5, 6)};
        expect_refused({identity_at(0, 5)}, rules);
    }
    // An atomic range only keeps its points in one request.
    ReadRules atomic{.max_units = 8};
    atomic.ranges = {range(Kind::ATOMIC, 5, 6)};
    const ReadPlan plan = planned({identity_at(0, 5)}, atomic);
    EXPECT_EQ(plan.identity.size(), 1U);
}

TEST(Plan, ARequestInNoPollGroupIsNeverTheKeepAlive) {
    // PR 171 S7: the identity read in no group is the shortest request;
    // the keep-alive is still the group's longer one.
    const ReadPlan plan =
        planned({at(0, 0, 2), identity_at(1, 300, NO_POLL_GROUP)},
                ReadRules{.max_units = 4});
    ASSERT_TRUE(plan.keepalive.has_value());
    EXPECT_EQ(plan.requests[plan.keepalive.value_or(0)].first, 0U);
    EXPECT_EQ(plan.requests[plan.keepalive.value_or(0)].count, 2U);
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

}  // namespace
}  // namespace scada::driver
