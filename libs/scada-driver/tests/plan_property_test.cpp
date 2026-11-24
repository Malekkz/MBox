// Properties of every plan (driver interface contract 6.3, 5.5; CS A.4),
// over random points and rules from a fixed seed, so every run checks the
// same plans. Each plan the planner accepts must keep every rule; the
// rules it refuses are covered by plan_test.cpp.

#include "scada/common/status.h"
#include "scada/driver/plan.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace scada::driver {
namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): the sizes of the random cases are the test data

using Kind = ReadRange::Kind;

constexpr int PLANS = 10'000;
constexpr std::uint32_t SPACES = 2;
constexpr std::uint32_t GROUPS = 2;
constexpr std::uint64_t SEED = 20261124;

struct Case {
    std::vector<PlanPoint> points;
    ReadRules rules;
};

std::uint64_t pick(std::mt19937_64& random, std::uint64_t low,
                   std::uint64_t high) {
    return std::uniform_int_distribution<std::uint64_t>(low, high)(random);
}

Case random_case(std::mt19937_64& random) {
    Case out;
    out.rules.max_units = pick(random, 1, 16);
    out.rules.max_gap = pick(random, 0, 4);
    out.rules.max_values = static_cast<std::uint32_t>(pick(random, 0, 6));
    if (pick(random, 0, 1) == 1) {
        out.rules.spaces.push_back({.space = 1,
                                    .max_units = pick(random, 1, 32),
                                    .max_gap = pick(random, 0, 8)});
    }
    const std::uint64_t ranges = pick(random, 0, 4);
    for (std::uint64_t index = 0; index < ranges; ++index) {
        const auto kind = static_cast<Kind>(pick(random, 1, 4));
        const std::uint64_t first = pick(random, 0, 60);
        out.rules.ranges.push_back(
            ReadRange{.kind = kind,
                      .space = static_cast<std::uint32_t>(pick(random, 0, 1)),
                      .unit_id = 1,
                      .first = first,
                      .last = first + pick(random, 0, 6)});
    }
    const std::uint64_t points = pick(random, 1, 24);
    for (std::uint32_t index = 0; index < points; ++index) {
        const auto cluster = static_cast<std::uint32_t>(
            pick(random, 0, 3) == 0 ? pick(random, 0, index) : index);
        out.points.push_back(PlanPoint{
            .point = index,
            .poll_group = static_cast<std::uint32_t>(pick(random, 0, 1)),
            .cluster = cluster,
            .space = static_cast<std::uint32_t>(pick(random, 0, SPACES - 1)),
            .unit_id = 1,
            .first = pick(random, 0, 70),
            .count = pick(random, 1, 3)});
    }
    return out;
}

std::uint64_t max_units_of(const ReadRules& rules, std::uint32_t space) {
    for (const SpaceLimits& own : rules.spaces) {
        if (own.space == space) {
            return own.max_units;
        }
    }
    return rules.max_units;
}

std::uint64_t max_gap_of(const ReadRules& rules, std::uint32_t space) {
    for (const SpaceLimits& own : rules.spaces) {
        if (own.space == space) {
            return own.max_gap;
        }
    }
    return rules.max_gap;
}

bool in(std::uint64_t first, std::uint64_t last, const Request& request) {
    return request.first <= first && last < request.first + request.count;
}

bool touches(const Request& request, const ReadRange& range) {
    return range.space == request.space && range.unit_id == request.unit_id &&
           request.first <= range.last &&
           range.first < request.first + request.count;
}

// The planned request (not a split form) of each point; NONE for a point
// no planned request reads, and TWICE for one that two read.
constexpr std::uint32_t NONE = UINT32_MAX;
constexpr std::uint32_t TWICE = UINT32_MAX - 1;

std::vector<std::uint32_t> request_of_points(const Case& tried,
                                             const ReadPlan& plan) {
    std::vector<std::uint32_t> out(tried.points.size(), NONE);
    for (const auto& group : plan.by_group) {
        for (const std::uint32_t index : group) {
            for (const std::uint32_t point : plan.requests[index].points) {
                out[point] = out[point] == NONE ? index : TWICE;
            }
        }
    }
    return out;
}

// `range` widened by every range of its kind and place that overlaps it,
// until none does: the range the planner reads as one.
ReadRange union_of(const ReadRange& range, const ReadRules& rules) {
    ReadRange out = range;
    bool grew = true;
    while (grew) {
        grew = false;
        for (const ReadRange& other : rules.ranges) {
            if (other.kind == out.kind && other.space == out.space &&
                other.unit_id == out.unit_id && other.first <= out.last &&
                out.first <= other.last &&
                (other.first < out.first || other.last > out.last)) {
                out.first = std::min(out.first, other.first);
                out.last = std::max(out.last, other.last);
                grew = true;
            }
        }
    }
    return out;
}

// Each check returns the rule a plan breaks, or nothing.
using Broken = std::string;

Broken check_range(const Request& request, const ReadRange& range) {
    if (!touches(request, range)) {
        return {};
    }
    const bool reads_whole =
        range.kind == Kind::SIDE_EFFECT || range.kind == Kind::EXCLUSIVE;
    const bool inside = range.first <= request.first &&
                        request.first + request.count - 1 <= range.last;
    if (range.kind == Kind::NO_READ) {
        return "reads a no_read range";
    }
    if (range.kind == Kind::ATOMIC && !in(range.first, range.last, request)) {
        return "splits an atomic range";
    }
    if (reads_whole && !request.side_effect) {
        return "a side-effect read not marked";
    }
    if (range.kind == Kind::EXCLUSIVE && (!request.exclusive || !inside)) {
        return "an exclusive request reads outside its range";
    }
    return {};
}

Broken check_request(const Case& tried, const Request& request) {
    if (request.count == 0 ||
        request.count > max_units_of(tried.rules, request.space)) {
        return "a request over its space's max_units";
    }
    if (tried.rules.max_values != 0 &&
        request.points.size() > tried.rules.max_values) {
        return "a request over max_values";
    }
    for (const ReadRange& given : tried.rules.ranges) {
        if (Broken broken = check_range(request, union_of(given, tried.rules));
            !broken.empty()) {
            return broken;
        }
    }
    for (const std::uint32_t point : request.points) {
        const PlanPoint& planned = tried.points[point];
        if (planned.space != request.space ||
            planned.poll_group != request.poll_group ||
            !in(planned.first, planned.first + planned.count - 1, request)) {
            return "a point outside its request";
        }
    }
    return {};
}

// All points of one side-effect range (as the union of the ranges of its
// kind that overlap) are in one planned request.
Broken check_side_effect(const Case& tried,
                         const std::vector<std::uint32_t>& of_point) {
    for (const ReadRange& given : tried.rules.ranges) {
        const ReadRange range = union_of(given, tried.rules);
        if (range.kind != Kind::SIDE_EFFECT && range.kind != Kind::EXCLUSIVE) {
            continue;
        }
        std::vector<std::uint32_t> requests;
        for (const PlanPoint& point : tried.points) {
            if (point.space == range.space && point.first <= range.last &&
                range.first <= point.first + point.count - 1) {
                requests.push_back(of_point[point.point]);
            }
        }
        std::sort(requests.begin(), requests.end());
        requests.erase(std::unique(requests.begin(), requests.end()),
                       requests.end());
        if (requests.size() > 1) {
            return "a side-effect range read by two requests";
        }
    }
    return {};
}

// The positions in `order` of the requests that read `cluster`.
std::vector<std::size_t> positions_of(
    const Case& tried, const std::vector<std::uint32_t>& order,
    const std::vector<std::uint32_t>& of_point, std::uint32_t cluster) {
    std::vector<std::size_t> out;
    for (const PlanPoint& point : tried.points) {
        const auto found =
            std::find(order.begin(), order.end(), of_point[point.point]);
        if (point.cluster == cluster && found != order.end()) {
            out.push_back(static_cast<std::size_t>(found - order.begin()));
        }
    }
    return out;
}

// A cluster in several requests of a group is in one chain: its requests
// are consecutive in the group and chained but the last.
Broken check_clusters(const Case& tried, const ReadPlan& plan,
                      const std::vector<std::uint32_t>& of_point) {
    for (const auto& order : plan.by_group) {
        for (const PlanPoint& point : tried.points) {
            const std::vector<std::size_t> positions =
                positions_of(tried, order, of_point, point.cluster);
            if (positions.size() < 2) {
                continue;
            }
            const auto [low, high] =
                std::minmax_element(positions.begin(), positions.end());
            for (std::size_t position = *low; position < *high; ++position) {
                if (!plan.requests[order[position]].chained) {
                    return "a cluster's requests are not one chain";
                }
            }
        }
    }
    return {};
}

bool ends_a_chain(const ReadPlan& plan, std::uint32_t request) {
    for (const auto& group : plan.by_group) {
        for (std::size_t position = 1; position < group.size(); ++position) {
            if (group[position] == request &&
                plan.requests[group[position - 1]].chained) {
                return true;
            }
        }
    }
    return false;
}

Broken check_extras(const ReadPlan& plan) {
    if (plan.keepalive.has_value()) {
        const Request& keepalive = plan.requests[*plan.keepalive];
        if (keepalive.side_effect || keepalive.chained ||
            ends_a_chain(plan, *plan.keepalive)) {
            return "the keep-alive reads a side effect or a chain";
        }
    }
    for (const Request& request : plan.requests) {
        if (!request.split.empty() &&
            (request.side_effect || request.chained)) {
            return "a split form of a side-effect or chained request";
        }
    }
    return {};
}

// The units of `request` that something accounts for: its points, and the
// parts of ranges read as one (atomic, side-effect, exclusive) inside it.
// Between them, a request may bridge at most `max_gap` units (E6).
using Interval = std::pair<std::uint64_t, std::uint64_t>;

Broken check_gaps(const Case& tried, const Request& request) {
    std::vector<Interval> covered;
    for (const std::uint32_t point : request.points) {
        const PlanPoint& planned = tried.points[point];
        covered.emplace_back(planned.first, planned.first + planned.count - 1);
    }
    const std::uint64_t request_last = request.first + request.count - 1;
    for (const ReadRange& given : tried.rules.ranges) {
        const ReadRange range = union_of(given, tried.rules);
        if (range.kind != Kind::NO_READ && touches(request, range)) {
            covered.emplace_back(std::max(range.first, request.first),
                                 std::min(range.last, request_last));
        }
    }
    std::sort(covered.begin(), covered.end());
    if (covered.empty() || covered.front().first != request.first) {
        return covered.empty() ? Broken{} : "a request starts on unread units";
    }
    std::uint64_t reach = covered.front().second;
    for (const Interval& next : covered) {
        if (next.first > reach + 1 &&
            next.first - reach - 1 > max_gap_of(tried.rules, request.space)) {
            return "a gap over max_gap bridged";
        }
        reach = std::max(reach, next.second);
    }
    return reach == request_last ? Broken{} : "a request ends on unread units";
}

// Every split form names parts no other names, and its parts read exactly
// the points of the request they split (E6).
Broken check_splits(const ReadPlan& plan) {
    std::vector<std::uint32_t> named;
    for (const Request& request : plan.requests) {
        std::vector<std::uint32_t> parts_points;
        for (const std::uint32_t part : request.split) {
            named.push_back(part);
            const auto& points = plan.requests[part].points;
            parts_points.insert(parts_points.end(), points.begin(),
                                points.end());
        }
        if (request.split.empty()) {
            continue;
        }
        std::vector<std::uint32_t> whole = request.points;
        std::sort(whole.begin(), whole.end());
        std::sort(parts_points.begin(), parts_points.end());
        if (whole != parts_points) {
            return "a split form that does not read its request's points";
        }
    }
    std::sort(named.begin(), named.end());
    if (std::adjacent_find(named.begin(), named.end()) != named.end()) {
        return "a split part named twice";
    }
    return {};
}

// The points of the cluster `cluster` and those between them, in address
// order; false when the cluster could not be one request for a reason this
// test does not model (several places, ranges, other clusters of several
// points).
bool cluster_run(const Case& tried, std::uint32_t cluster,
                 std::vector<const PlanPoint*>& run) {
    std::vector<const PlanPoint*> members;
    for (const PlanPoint& point : tried.points) {
        if (point.cluster == cluster) {
            members.push_back(&point);
        }
    }
    const PlanPoint& first = *members.front();
    std::uint64_t low = UINT64_MAX;
    std::uint64_t high = 0;
    for (const PlanPoint* point : members) {
        if (point->space != first.space ||
            point->poll_group != first.poll_group) {
            return false;
        }
        low = std::min(low, point->first);
        high = std::max(high, point->first + point->count - 1);
    }
    for (const PlanPoint& point : tried.points) {
        if (point.space == first.space &&
            point.poll_group == first.poll_group && point.first <= high &&
            low <= point.first + point.count - 1) {
            run.push_back(&point);
        }
    }
    for (const PlanPoint* point : run) {
        const auto same =
            std::count_if(tried.points.begin(), tried.points.end(),
                          [&](const PlanPoint& other) {
                              return other.cluster == point->cluster;
                          });
        if (point->cluster != cluster && same > 1) {
            return false;
        }
    }
    return true;
}

bool touches_a_range(const Case& tried, const PlanPoint& point) {
    return std::any_of(tried.rules.ranges.begin(), tried.rules.ranges.end(),
                       [&](const ReadRange& range) {
                           return range.space == point.space &&
                                  range.first <=
                                      point.first + point.count - 1 &&
                                  point.first <= range.last;
                       });
}

// True when the run could be one request: within the limits, every gap
// within max_gap, no range touched.
bool fits_one(const Case& tried, std::vector<const PlanPoint*> run) {
    std::sort(run.begin(), run.end(),
              [](const PlanPoint* left, const PlanPoint* right) {
                  return left->first < right->first;
              });
    const std::uint32_t space = run.front()->space;
    std::uint64_t reach = run.front()->first + run.front()->count - 1;
    for (const PlanPoint* point : run) {
        if (touches_a_range(tried, *point) ||
            (point->first > reach + 1 &&
             point->first - reach - 1 > max_gap_of(tried.rules, space))) {
            return false;
        }
        reach = std::max(reach, point->first + point->count - 1);
    }
    return reach - run.front()->first < max_units_of(tried.rules, space) &&
           (tried.rules.max_values == 0 ||
            run.size() <= tried.rules.max_values);
}

// A cluster that fits one request is one request (5.5, E6).
Broken check_fusion(const Case& tried,
                    const std::vector<std::uint32_t>& of_point) {
    for (const PlanPoint& point : tried.points) {
        std::vector<const PlanPoint*> run;
        if (!cluster_run(tried, point.cluster, run) || !fits_one(tried, run)) {
            continue;
        }
        for (const PlanPoint& other : tried.points) {
            if (other.cluster == point.cluster &&
                of_point[other.point] != of_point[point.point]) {
                return "a cluster that fits one request is read by two";
            }
        }
    }
    return {};
}

// The side-effect or exclusive range (as read, the union) `point` is in.
std::optional<ReadRange> side_effect_of(const Case& tried,
                                        const PlanPoint& point) {
    for (const ReadRange& given : tried.rules.ranges) {
        const ReadRange range = union_of(given, tried.rules);
        if ((range.kind == Kind::SIDE_EFFECT ||
             range.kind == Kind::EXCLUSIVE) &&
            range.space == point.space &&
            range.first <= point.first + point.count - 1 &&
            point.first <= range.last) {
            return range;
        }
    }
    return std::nullopt;
}

// True when a cluster of a point of `request` inside `range` is read by
// another request of the chain [from, to] too: the range is the chain's own.
bool range_of_the_chain(const Case& tried, const ReadPlan& plan,
                        const std::vector<std::uint32_t>& order,
                        std::size_t from, std::size_t last,
                        std::size_t position, const ReadRange& range) {
    for (const std::uint32_t point : plan.requests[order[position]].points) {
        const PlanPoint& mine = tried.points[point];
        if (mine.first > range.last ||
            mine.first + mine.count - 1 < range.first) {
            continue;
        }
        for (std::size_t other = from; other <= last; ++other) {
            const auto& points = plan.requests[order[other]].points;
            if (other != position &&
                std::any_of(
                    points.begin(), points.end(), [&](std::uint32_t member) {
                        return tried.points[member].cluster == mine.cluster;
                    })) {
                return true;
            }
        }
    }
    return false;
}

// D3: in a chain, only its last request reads a side effect, and that
// side effect is the chain's own: a cluster of the chain has a point in
// the range (all points of a range are read together, R4).
Broken check_chain(const Case& tried, const ReadPlan& plan,
                   const std::vector<std::uint32_t>& order, std::size_t from,
                   std::size_t last) {
    for (std::size_t position = from; position <= last; ++position) {
        const Request& request = plan.requests[order[position]];
        if (request.side_effect && position != last) {
            return "a side effect read before the end of its chain";
        }
        for (const std::uint32_t point : request.points) {
            const std::optional<ReadRange> range =
                side_effect_of(tried, tried.points[point]);
            if (range.has_value() &&
                !range_of_the_chain(tried, plan, order, from, last, position,
                                    *range)) {
                return "a side effect of another cluster inside a chain";
            }
        }
    }
    return {};
}

Broken check_chains(const Case& tried, const ReadPlan& plan) {
    for (const auto& order : plan.by_group) {
        std::size_t from = 0;
        for (std::size_t position = 0; position < order.size(); ++position) {
            if (plan.requests[order[position]].chained) {
                continue;
            }
            if (position > from) {
                if (Broken broken =
                        check_chain(tried, plan, order, from, position);
                    !broken.empty()) {
                    return broken;
                }
            }
            from = position + 1;
        }
    }
    return {};
}

Broken check_plan(const Case& tried, const ReadPlan& plan) {
    const std::vector<std::uint32_t> of_point = request_of_points(tried, plan);
    if (std::any_of(of_point.begin(), of_point.end(),
                    [](std::uint32_t request) {
                        return request == NONE || request == TWICE;
                    })) {
        return "a point read by no planned request, or by two";
    }
    for (const Request& request : plan.requests) {
        if (Broken broken = check_request(tried, request); !broken.empty()) {
            return broken;
        }
        if (Broken broken = check_gaps(tried, request); !broken.empty()) {
            return broken;
        }
    }
    if (Broken broken = check_splits(plan); !broken.empty()) {
        return broken;
    }
    if (Broken broken = check_fusion(tried, of_point); !broken.empty()) {
        return broken;
    }
    if (Broken broken = check_chains(tried, plan); !broken.empty()) {
        return broken;
    }
    if (Broken broken = check_side_effect(tried, of_point); !broken.empty()) {
        return broken;
    }
    if (Broken broken = check_clusters(tried, plan, of_point);
        !broken.empty()) {
        return broken;
    }
    return check_extras(plan);
}

std::string dump(const Case& tried) {
    std::string out = "max_units " + std::to_string(tried.rules.max_units) +
                      " max_gap " + std::to_string(tried.rules.max_gap) +
                      " max_values " + std::to_string(tried.rules.max_values) +
                      "\n";
    for (const ReadRange& range : tried.rules.ranges) {
        out += "range kind " + std::to_string(static_cast<int>(range.kind)) +
               " space " + std::to_string(range.space) + " " +
               std::to_string(range.first) + ".." + std::to_string(range.last) +
               "\n";
    }
    for (const PlanPoint& point : tried.points) {
        out += "point " + std::to_string(point.point) + " group " +
               std::to_string(point.poll_group) + " cluster " +
               std::to_string(point.cluster) + " space " +
               std::to_string(point.space) + " " + std::to_string(point.first) +
               "+" + std::to_string(point.count) + "\n";
    }
    return out;
}

TEST(PlanProperty, EveryAcceptedPlanKeepsEveryRule) {
    // A fixed seed: the same cases on every run (deterministic tests).
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp): a fixed seed is the point
    std::mt19937_64 random(SEED);
    int accepted = 0;
    for (int index = 0; index < PLANS; ++index) {
        const Case tried = random_case(random);
        ReadPlan plan;
        plan.by_group.resize(GROUPS);
        if (!plan_blocks(tried.points, tried.rules, GROUPS, plan).ok()) {
            continue;
        }
        ++accepted;
        const Broken broken = check_plan(tried, plan);
        ASSERT_TRUE(broken.empty())
            << "case " << index << ": " << broken << "\n"
            << dump(tried);
    }
    // Enough cases pass the planner last mean something.
    EXPECT_GT(accepted, PLANS / 4);
}

TEST(PlanProperty, ManyPointsArePlannedQuickly) {
    // The fusing of clusters walks the segments: 1,000 points in pairs that
    // fit, and 10,000 points whose clusters cannot fit one request (each
    // with a companion 1,000 units away, E7), must plan in well under a
    // second each (the test's own time is the bound ctest reports).
    std::vector<PlanPoint> pairs;
    for (std::uint32_t index = 0; index < 1000; ++index) {
        pairs.push_back(PlanPoint{.point = index,
                                  .cluster = index / 2,
                                  .first = std::uint64_t{index} * 3,
                                  .count = 2});
    }
    ReadPlan plan;
    EXPECT_TRUE(
        plan_blocks(pairs, ReadRules{.max_units = 125, .max_gap = 1}, 1, plan)
            .ok());
    std::vector<PlanPoint> far;
    for (std::uint32_t index = 0; index < 10'000; ++index) {
        const std::uint64_t first =
            index % 2 == 0 ? index : std::uint64_t{index} + 1000;
        far.push_back(PlanPoint{
            .point = index, .cluster = index / 2, .first = first, .count = 1});
    }
    EXPECT_TRUE(
        plan_blocks(far, ReadRules{.max_units = 125, .max_gap = 1}, 1, plan)
            .ok());
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

}  // namespace
}  // namespace scada::driver
