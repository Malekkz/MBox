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
#include <random>
#include <string>
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
    // Enough cases pass the planner to mean something.
    EXPECT_GT(accepted, PLANS / 4);
}

TEST(PlanProperty, AThousandPointsArePlannedQuickly) {
    // The fusing of clusters repeats over the segments: a device of 1,000
    // points with many clusters must still plan in well under a second (the
    // test's own time is the bound ctest reports).
    std::vector<PlanPoint> points;
    for (std::uint32_t index = 0; index < 1000; ++index) {
        points.push_back(PlanPoint{.point = index,
                                   .cluster = index / 2,
                                   .first = std::uint64_t{index} * 3,
                                   .count = 2});
    }
    ReadPlan plan;
    EXPECT_TRUE(
        plan_blocks(points, ReadRules{.max_units = 125, .max_gap = 1}, 1, plan)
            .ok());
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

}  // namespace
}  // namespace scada::driver
