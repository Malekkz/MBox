#include "scada/driver/plan.h"

#include "scada/common/static_string.h"
#include "scada/common/status.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace scada::driver {

namespace {

using Kind = ReadRange::Kind;

Status refused(StaticString what) noexcept {
    return {StatusCode::INVALID_ARGUMENT, what};
}

// Where a span of units is: poll group, space and unit id.
struct Place {
    std::uint32_t group = 0;
    std::uint32_t space = 0;
    std::uint32_t unit_id = 0;
    friend auto operator<=>(const Place&, const Place&) = default;
};

bool overlaps(std::uint64_t first, std::uint64_t last, const ReadRange& range,
              std::uint32_t space, std::uint32_t unit_id) noexcept {
    return range.space == space && range.unit_id == unit_id &&
           first <= range.last && range.first <= last;
}

std::uint64_t last_of(const PlanPoint& point) noexcept {
    return point.first + point.count - 1;
}

Status validate_ranges(const ReadRules& rules) noexcept {
    if (rules.max_units == 0) {
        return refused("max_units must be at least 1");
    }
    for (std::size_t one = 0; one < rules.ranges.size(); ++one) {
        const ReadRange& range = rules.ranges[one];
        if (range.last < range.first) {
            return refused("read range ends before it starts");
        }
        if (range.kind == Kind::ATOMIC &&
            range.last - range.first >= rules.max_units) {
            return refused("atomic range larger than one request");
        }
        for (std::size_t other = one + 1; other < rules.ranges.size();
             ++other) {
            const ReadRange& next = rules.ranges[other];
            if (next.kind != range.kind &&
                overlaps(next.first, next.last, range, next.space,
                         next.unit_id)) {
                return refused("read ranges of different kinds overlap");
            }
        }
    }
    return {};
}

Status validate_points(const std::vector<PlanPoint>& points,
                       const ReadRules& rules,
                       std::uint32_t poll_groups) noexcept {
    // The poll group that reads each side-effect or exclusive range.
    std::vector<std::optional<std::uint32_t>> reader(rules.ranges.size());
    for (const PlanPoint& point : points) {
        if (point.count == 0 || point.count > rules.max_units ||
            point.first > UINT64_MAX - (point.count - 1)) {
            return refused("point span empty or larger than one request");
        }
        if (point.poll_group >= poll_groups) {
            return refused("poll group out of range");
        }
        for (std::size_t index = 0; index < rules.ranges.size(); ++index) {
            const ReadRange& range = rules.ranges[index];
            if (!overlaps(point.first, last_of(point), range, point.space,
                          point.unit_id)) {
                continue;
            }
            if (range.kind == Kind::NO_READ) {
                return refused("point in a no_read range");
            }
            if (range.kind == Kind::SIDE_EFFECT ||
                range.kind == Kind::EXCLUSIVE) {
                if (reader[index].has_value() &&
                    *reader[index] != point.poll_group) {
                    return refused("side-effect range read by two poll groups");
                }
                reader[index] = point.poll_group;
            }
        }
    }
    return {};
}

// Points that must be in one request: those that share a range which is
// read whole (atomic, side-effect, exclusive), widened to the whole range.
struct Segment {
    Place place;
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    std::vector<std::uint32_t> members;  // indices into the points
    bool side_effect = false;
    bool exclusive = false;
};

// Widens `segment` by every whole-read range it touches; true when it grew.
bool widen(Segment& segment, const ReadRules& rules) noexcept {
    bool grew = false;
    for (const ReadRange& range : rules.ranges) {
        if (range.kind == Kind::NO_READ ||
            !overlaps(segment.first, segment.last, range, segment.place.space,
                      segment.place.unit_id)) {
            continue;
        }
        segment.side_effect = segment.side_effect ||
                              range.kind == Kind::SIDE_EFFECT ||
                              range.kind == Kind::EXCLUSIVE;
        segment.exclusive = segment.exclusive || range.kind == Kind::EXCLUSIVE;
        if (range.first < segment.first || range.last > segment.last) {
            segment.first = std::min(segment.first, range.first);
            segment.last = std::max(segment.last, range.last);
            grew = true;
        }
    }
    return grew;
}

// Builds the segments of the points, merged where they share a whole-read
// range or overlap one another.
std::vector<Segment> segments_of(const std::vector<PlanPoint>& points,
                                 const ReadRules& rules) {
    std::vector<Segment> segments;
    for (std::uint32_t index = 0; index < points.size(); ++index) {
        const PlanPoint& point = points[index];
        Segment segment{.place = {.group = point.poll_group,
                                  .space = point.space,
                                  .unit_id = point.unit_id},
                        .first = point.first,
                        .last = last_of(point),
                        .members = {index}};
        (void)widen(segment, rules);
        segments.push_back(std::move(segment));
    }
    // Merge segments of one place whose spans overlap, until none does; a
    // merge can widen into another range, so repeat.
    bool merged = true;
    while (merged) {
        merged = false;
        std::sort(segments.begin(), segments.end(),
                  [](const Segment& left, const Segment& right) {
                      return std::tie(left.place, left.first) <
                             std::tie(right.place, right.first);
                  });
        std::vector<Segment> out;
        for (Segment& segment : segments) {
            Segment* previous = out.empty() ? nullptr : &out.back();
            const bool joins = previous != nullptr &&
                               previous->place == segment.place &&
                               segment.first <= previous->last;
            if (!joins) {
                out.push_back(std::move(segment));
                continue;
            }
            previous->last = std::max(previous->last, segment.last);
            previous->side_effect =
                previous->side_effect || segment.side_effect;
            previous->exclusive = previous->exclusive || segment.exclusive;
            previous->members.insert(previous->members.end(),
                                     segment.members.begin(),
                                     segment.members.end());
            (void)widen(*previous, rules);
            merged = true;
        }
        segments = std::move(out);
    }
    return segments;
}

// True when the units strictly between `after` and `before` can be read and
// thrown away: within max_gap, touching no range at all.
bool bridgeable(const Place& place, std::uint64_t after, std::uint64_t before,
                const ReadRules& rules) noexcept {
    if (before <= after + 1) {
        return true;  // adjacent or overlapping: no gap
    }
    const std::uint64_t gap = before - after - 1;
    if (gap > rules.max_gap) {
        return false;
    }
    return std::none_of(rules.ranges.begin(), rules.ranges.end(),
                        [&](const ReadRange& range) {
                            return overlaps(after + 1, before - 1, range,
                                            place.space, place.unit_id);
                        });
}

// A run of segments read in one request.
struct Block {
    Place place;
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    std::vector<std::uint32_t> members;
    std::vector<std::uint32_t> clusters;  // distinct, in order
    bool side_effect = false;
    bool exclusive = false;
};

Block block_of(const Segment& segment, const std::vector<PlanPoint>& points) {
    Block block{.place = segment.place,
                .first = segment.first,
                .last = segment.last,
                .members = segment.members,
                .clusters = {},
                .side_effect = segment.side_effect,
                .exclusive = segment.exclusive};
    for (const std::uint32_t member : segment.members) {
        const std::uint32_t cluster = points[member].cluster;
        if (std::find(block.clusters.begin(), block.clusters.end(), cluster) ==
            block.clusters.end()) {
            block.clusters.push_back(cluster);
        }
    }
    return block;
}

// True when `next` can join `block` in one request.
bool joinable(const Block& block, const Block& next, const ReadRules& rules) {
    if (block.place != next.place || block.exclusive || next.exclusive) {
        return false;
    }
    const std::uint64_t last = std::max(block.last, next.last);
    if (last - block.first >= rules.max_units) {
        return false;
    }
    if (rules.max_values != 0 &&
        block.members.size() + next.members.size() > rules.max_values) {
        return false;
    }
    return bridgeable(block.place, block.last, next.first, rules);
}

void join(Block& block, const Block& next) {
    block.last = std::max(block.last, next.last);
    block.members.insert(block.members.end(), next.members.begin(),
                         next.members.end());
    for (const std::uint32_t cluster : next.clusters) {
        if (std::find(block.clusters.begin(), block.clusters.end(), cluster) ==
            block.clusters.end()) {
            block.clusters.push_back(cluster);
        }
    }
    block.side_effect = block.side_effect || next.side_effect;
}

Request request_of(const Block& block, const std::vector<PlanPoint>& points) {
    Request request{.poll_group = block.place.group,
                    .space = block.place.space,
                    .unit_id = block.place.unit_id,
                    .first = block.first,
                    .count = block.last - block.first + 1,
                    .points = {},
                    .side_effect = block.side_effect,
                    .exclusive = block.exclusive,
                    .chained = false,
                    .split = {}};
    std::vector<std::uint32_t> members = block.members;
    std::sort(members.begin(), members.end(),
              [&](std::uint32_t left, std::uint32_t right) {
                  return std::tie(points[left].first, points[left].point) <
                         std::tie(points[right].first, points[right].point);
              });
    for (const std::uint32_t member : members) {
        request.points.push_back(points[member].point);
    }
    return request;
}

// The work of one poll group: a cluster that fits one request, or the
// chained requests of one that does not.
struct Unit {
    std::vector<Block> blocks;  // one: mergeable; several: a chain
};

// Groups the segments of one poll group by cluster. A cluster whose
// segments fit one request is one block; otherwise its segments (each
// merged with its cluster's neighbours where they fit) form a chain.
std::vector<Unit> units_of(const std::vector<Segment>& segments,
                           const std::vector<PlanPoint>& points,
                           const ReadRules& rules) {
    // Segments of a cluster; a segment holding several clusters ties them
    // together into one (union by the lowest cluster seen).
    std::map<std::uint32_t, std::uint32_t> parent;
    auto find = [&](std::uint32_t cluster) {
        while (parent.contains(cluster) && parent[cluster] != cluster) {
            cluster = parent[cluster];
        }
        return cluster;
    };
    for (const Segment& segment : segments) {
        std::uint32_t root = find(points[segment.members.front()].cluster);
        parent.try_emplace(root, root);
        for (const std::uint32_t member : segment.members) {
            const std::uint32_t other = find(points[member].cluster);
            parent.try_emplace(other, other);
            if (other != root) {
                parent[std::max(other, root)] = std::min(other, root);
                root = std::min(other, root);
            }
        }
    }
    std::map<std::uint32_t, std::vector<Block>> by_cluster;
    for (const Segment& segment : segments) {
        by_cluster[find(points[segment.members.front()].cluster)].push_back(
            block_of(segment, points));
    }
    std::vector<Unit> units;
    for (auto& [cluster, blocks] : by_cluster) {
        std::sort(blocks.begin(), blocks.end(),
                  [](const Block& left, const Block& right) {
                      return std::tie(left.place, left.first) <
                             std::tie(right.place, right.first);
                  });
        std::vector<Block> merged;
        for (const Block& block : blocks) {
            if (!merged.empty() && joinable(merged.back(), block, rules)) {
                join(merged.back(), block);
            } else {
                merged.push_back(block);
            }
        }
        units.push_back(Unit{.blocks = std::move(merged)});
    }
    std::sort(units.begin(), units.end(),
              [](const Unit& left, const Unit& right) {
                  return std::tie(left.blocks.front().place,
                                  left.blocks.front().first) <
                         std::tie(right.blocks.front().place,
                                  right.blocks.front().first);
              });
    return units;
}

// Merges single-block units of one poll group in address order and appends
// the requests of the group to `out`.
void plan_group(std::vector<Unit> units, const std::vector<PlanPoint>& points,
                const ReadRules& rules, ReadPlan& out,
                std::vector<std::uint32_t>& order,
                std::vector<std::vector<Block>>& parts) {
    std::optional<Block> open;
    std::vector<Block> open_parts;
    auto flush = [&] {
        if (!open.has_value()) {
            return;
        }
        order.push_back(static_cast<std::uint32_t>(out.requests.size()));
        out.requests.push_back(request_of(*open, points));
        parts.push_back(open_parts);
        open.reset();
        open_parts.clear();
    };
    for (Unit& unit : units) {
        if (unit.blocks.size() > 1) {
            flush();
            for (std::size_t index = 0; index < unit.blocks.size(); ++index) {
                order.push_back(
                    static_cast<std::uint32_t>(out.requests.size()));
                Request request = request_of(unit.blocks[index], points);
                request.chained = index + 1 < unit.blocks.size();
                out.requests.push_back(std::move(request));
                parts.emplace_back();
            }
            continue;
        }
        const Block& block = unit.blocks.front();
        if (open.has_value() && joinable(*open, block, rules)) {
            join(*open, block);
            open_parts.push_back(block);
            continue;
        }
        flush();
        open = block;
        open_parts = {block};
    }
    flush();
}

}  // namespace

std::uint32_t requests_per_scan(const ReadPlan& plan) noexcept {
    std::size_t total = 0;
    for (const std::vector<std::uint32_t>& group : plan.by_group) {
        total += group.size();
    }
    return static_cast<std::uint32_t>(total);
}

Status plan_blocks(const std::vector<PlanPoint>& points, const ReadRules& rules,
                   std::uint32_t poll_groups, ReadPlan& out) {
    if (const Status status = validate_ranges(rules); !status.ok()) {
        return status;
    }
    if (const Status status = validate_points(points, rules, poll_groups);
        !status.ok()) {
        return status;
    }
    const std::vector<Segment> segments = segments_of(points, rules);
    for (const Segment& segment : segments) {
        if (segment.last - segment.first >= rules.max_units) {
            return refused(
                "points that must be read together exceed one request");
        }
    }
    out = ReadPlan{};
    out.by_group.resize(poll_groups);
    // The parts of each planned request that its split form reads.
    std::vector<std::vector<Block>> parts;
    for (std::uint32_t group = 0; group < poll_groups; ++group) {
        std::vector<Segment> of_group;
        for (const Segment& segment : segments) {
            if (segment.place.group == group) {
                of_group.push_back(segment);
            }
        }
        plan_group(units_of(of_group, points, rules), points, rules, out,
                   out.by_group[group], parts);
    }
    // Split forms: one request per part, for a request of several parts
    // that touches no side-effect range (CS A.4: such a range is never
    // split).
    const auto planned = static_cast<std::uint32_t>(out.requests.size());
    for (std::uint32_t index = 0; index < planned; ++index) {
        if (parts[index].size() < 2 || out.requests[index].side_effect) {
            continue;
        }
        for (const Block& part : parts[index]) {
            out.requests[index].split.push_back(
                static_cast<std::uint32_t>(out.requests.size()));
            out.requests.push_back(request_of(part, points));
        }
    }
    // Keep-alive and probe: the shortest planned request outside every
    // side-effect and exclusive range (bridging never touches a range).
    for (std::uint32_t index = 0; index < planned; ++index) {
        const Request& request = out.requests[index];
        if (request.side_effect || request.exclusive) {
            continue;
        }
        if (!out.keepalive.has_value() ||
            request.count < out.requests[*out.keepalive].count) {
            out.keepalive = index;
        }
    }
    out.probe = out.keepalive;
    return {};
}

}  // namespace scada::driver
