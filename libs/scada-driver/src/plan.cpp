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

// The request limits of one space.
struct Limits {
    std::uint64_t max_units = 1;
    std::uint64_t max_gap = 0;
};

Limits limits_of(const ReadRules& rules, std::uint32_t space) noexcept {
    for (const SpaceLimits& own : rules.spaces) {
        if (own.space == space) {
            return {.max_units = own.max_units, .max_gap = own.max_gap};
        }
    }
    return {.max_units = rules.max_units, .max_gap = rules.max_gap};
}

bool overlaps(std::uint64_t first, std::uint64_t last, const ReadRange& range,
              std::uint32_t space, std::uint32_t unit_id) noexcept {
    return range.space == space && range.unit_id == unit_id &&
           first <= range.last && range.first <= last;
}

std::uint64_t last_of(const PlanPoint& point) noexcept {
    return point.first + point.count - 1;
}

bool same_place(const ReadRange& left, const ReadRange& right) noexcept {
    return left.kind == right.kind && left.space == right.space &&
           left.unit_id == right.unit_id;
}

// The ranges with every overlapping pair of one kind and place joined into
// one, until none overlaps: CS allows such ranges, and a range read whole
// must be read whole with the ranges it overlaps (K3).
std::vector<ReadRange> merged_ranges(const ReadRules& rules) {
    std::vector<ReadRange> out = rules.ranges;
    bool joined = true;
    while (joined) {
        joined = false;
        for (std::size_t one = 0; one < out.size() && !joined; ++one) {
            for (std::size_t other = one + 1; other < out.size(); ++other) {
                if (!same_place(out[one], out[other]) ||
                    !overlaps(out[other].first, out[other].last, out[one],
                              out[other].space, out[other].unit_id)) {
                    continue;
                }
                out[one].first = std::min(out[one].first, out[other].first);
                out[one].last = std::max(out[one].last, out[other].last);
                out.erase(out.begin() + static_cast<std::ptrdiff_t>(other));
                joined = true;
                break;
            }
        }
    }
    return out;
}

Status validate_limits(const ReadRules& rules) noexcept {
    if (rules.max_units == 0) {
        return refused("max_units must be at least 1");
    }
    for (std::size_t one = 0; one < rules.spaces.size(); ++one) {
        if (rules.spaces[one].max_units == 0) {
            return refused("max_units of a space must be at least 1");
        }
        for (std::size_t other = one + 1; other < rules.spaces.size();
             ++other) {
            if (rules.spaces[other].space == rules.spaces[one].space) {
                return refused("limits given twice for one space");
            }
        }
    }
    return {};
}

Status validate_ranges(const ReadRules& rules,
                       const std::vector<ReadRange>& merged) noexcept {
    if (const Status status = validate_limits(rules); !status.ok()) {
        return status;
    }
    for (std::size_t one = 0; one < rules.ranges.size(); ++one) {
        const ReadRange& range = rules.ranges[one];
        if (range.last < range.first) {
            return refused("read range ends before it starts");
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
    for (const ReadRange& range : merged) {
        if (range.kind == Kind::ATOMIC &&
            range.last - range.first >=
                limits_of(rules, range.space).max_units) {
            return refused("atomic range larger than one request");
        }
    }
    return {};
}

// A point against one range it overlaps; `reader` is the poll group that
// reads the range so far.
Status check_point_in(const PlanPoint& point, const ReadRange& range,
                      std::optional<std::uint32_t>& reader) noexcept {
    if (range.kind == Kind::NO_READ) {
        return refused("point in a no_read range");
    }
    if (range.kind == Kind::EXCLUSIVE &&
        (point.first < range.first || last_of(point) > range.last)) {
        // Read alone means its request holds nothing of outside.
        return refused("point across the edge of an exclusive range");
    }
    if (range.kind == Kind::SIDE_EFFECT || range.kind == Kind::EXCLUSIVE) {
        if (reader.has_value() && *reader != point.poll_group) {
            return refused("side-effect range read by two poll groups");
        }
        reader = point.poll_group;
    }
    return {};
}

Status validate_points(const std::vector<PlanPoint>& points,
                       const ReadRules& rules,
                       const std::vector<ReadRange>& merged,
                       std::uint32_t poll_groups) {
    // The poll group that reads each side-effect or exclusive range.
    std::vector<std::optional<std::uint32_t>> reader(merged.size());
    for (const PlanPoint& point : points) {
        if (point.count == 0 ||
            point.count > limits_of(rules, point.space).max_units ||
            point.first > UINT64_MAX - (point.count - 1)) {
            return refused("point span empty or larger than one request");
        }
        if (point.poll_group >= poll_groups) {
            return refused("poll group out of range");
        }
        for (std::size_t index = 0; index < merged.size(); ++index) {
            if (!overlaps(point.first, last_of(point), merged[index],
                          point.space, point.unit_id)) {
                continue;
            }
            if (const Status status =
                    check_point_in(point, merged[index], reader[index]);
                !status.ok()) {
                return status;
            }
        }
    }
    return {};
}

void add_distinct(std::vector<std::uint32_t>& into,
                  const std::vector<std::uint32_t>& from) {
    for (const std::uint32_t value : from) {
        if (std::find(into.begin(), into.end(), value) == into.end()) {
            into.push_back(value);
        }
    }
}

bool share(const std::vector<std::uint32_t>& left,
           const std::vector<std::uint32_t>& right) noexcept {
    return std::any_of(left.begin(), left.end(), [&](std::uint32_t value) {
        return std::find(right.begin(), right.end(), value) != right.end();
    });
}

// Points that must be in one request: those that overlap, share an atomic
// range (widened to the whole range) or share a side-effect or exclusive
// range (all its points in one read, over the span they decode, CS A.4).
struct Segment {
    Place place;
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    std::vector<std::uint32_t> members;   // indices into the points
    std::vector<std::uint32_t> clusters;  // distinct
    std::vector<std::uint32_t> tags;      // side-effect ranges it reads
    bool side_effect = false;
    bool exclusive = false;
};

Segment segment_of(std::uint32_t index, const std::vector<PlanPoint>& points,
                   const std::vector<ReadRange>& merged) {
    const PlanPoint& point = points[index];
    Segment segment{.place = {.group = point.poll_group,
                              .space = point.space,
                              .unit_id = point.unit_id},
                    .first = point.first,
                    .last = last_of(point),
                    .members = {index},
                    .clusters = {point.cluster},
                    .tags = {},
                    .side_effect = false,
                    .exclusive = false};
    for (std::uint32_t tag = 0; tag < merged.size(); ++tag) {
        const ReadRange& range = merged[tag];
        if (!overlaps(point.first, last_of(point), range, point.space,
                      point.unit_id)) {
            continue;
        }
        if (range.kind == Kind::ATOMIC) {
            // Merged atomic ranges overlap nothing else of any kind, so one
            // widening is final.
            segment.first = std::min(segment.first, range.first);
            segment.last = std::max(segment.last, range.last);
        } else if (range.kind != Kind::NO_READ) {
            segment.tags.push_back(tag);
            segment.side_effect = true;
            segment.exclusive =
                segment.exclusive || range.kind == Kind::EXCLUSIVE;
        }
    }
    return segment;
}

void absorb(Segment& into, const Segment& from) {
    into.first = std::min(into.first, from.first);
    into.last = std::max(into.last, from.last);
    into.members.insert(into.members.end(), from.members.begin(),
                        from.members.end());
    add_distinct(into.clusters, from.clusters);
    add_distinct(into.tags, from.tags);
    into.side_effect = into.side_effect || from.side_effect;
    into.exclusive = into.exclusive || from.exclusive;
}

void sort_segments(std::vector<Segment>& segments) {
    std::sort(segments.begin(), segments.end(),
              [](const Segment& left, const Segment& right) {
                  return std::tie(left.place, left.first) <
                         std::tie(right.place, right.first);
              });
}

// Builds the segments of the points. Between two segments of one
// side-effect range there is only that range, so after sorting they are
// neighbours or joined through the segments between.
std::vector<Segment> segments_of(const std::vector<PlanPoint>& points,
                                 const std::vector<ReadRange>& merged) {
    std::vector<Segment> segments;
    segments.reserve(points.size());
    for (std::uint32_t index = 0; index < points.size(); ++index) {
        segments.push_back(segment_of(index, points, merged));
    }
    bool joined = true;
    while (joined) {
        joined = false;
        sort_segments(segments);
        std::vector<Segment> out;
        for (Segment& segment : segments) {
            const bool joins = !out.empty() &&
                               out.back().place == segment.place &&
                               (segment.first <= out.back().last ||
                                share(out.back().tags, segment.tags));
            if (!joins) {
                out.push_back(std::move(segment));
                continue;
            }
            absorb(out.back(), segment);
            joined = true;
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
    if (gap > limits_of(rules, place.space).max_gap) {
        return false;
    }
    return std::none_of(rules.ranges.begin(), rules.ranges.end(),
                        [&](const ReadRange& range) {
                            return overlaps(after + 1, before - 1, range,
                                            place.space, place.unit_id);
                        });
}

bool fits(const Place& place, std::uint64_t first, std::uint64_t last,
          std::size_t values, const ReadRules& rules) noexcept {
    return last - first < limits_of(rules, place.space).max_units &&
           (rules.max_values == 0 || values <= rules.max_values);
}

// True when the live segments [from, last_index] of one place can be one
// request.
bool one_request(const std::vector<Segment>& segments,
                 const std::vector<bool>& live, std::size_t from,
                 std::size_t last_index, const ReadRules& rules) {
    const Segment& start = segments[from];
    std::uint64_t last = start.last;
    std::size_t values = start.members.size();
    for (std::size_t index = from + 1; index <= last_index; ++index) {
        if (!live[index]) {
            continue;
        }
        const Segment& next = segments[index];
        if (next.place != start.place || next.exclusive || start.exclusive ||
            !bridgeable(next.place, last, next.first, rules)) {
            return false;
        }
        last = std::max(last, next.last);
        values += next.members.size();
    }
    return fits(start.place, start.first, last, values, rules);
}

// Joins the segments of each cluster that fit in one request, with the
// segments between them, into one segment (5.5: one request when they fit
// in one). One pass in address order: a join only adds clusters to the
// segment it joins into, whose new clusters are tried in turn.
void fuse_clusters(std::vector<Segment>& segments, const ReadRules& rules) {
    sort_segments(segments);
    // The last segment, in address order, that holds each cluster.
    std::map<std::uint32_t, std::size_t> last_of_cluster;
    for (std::size_t index = 0; index < segments.size(); ++index) {
        for (const std::uint32_t cluster : segments[index].clusters) {
            last_of_cluster[cluster] = index;
        }
    }
    std::vector<bool> live(segments.size(), true);
    for (std::size_t from = 0; from < segments.size(); ++from) {
        if (!live[from]) {
            continue;
        }
        // Clusters are appended as segments join, so iterate by position.
        for (std::size_t at = 0; at < segments[from].clusters.size(); ++at) {
            const std::size_t last_index =
                last_of_cluster[segments[from].clusters[at]];
            if (last_index <= from ||
                !one_request(segments, live, from, last_index, rules)) {
                continue;
            }
            for (std::size_t index = from + 1; index <= last_index; ++index) {
                if (live[index]) {
                    absorb(segments[from], segments[index]);
                    live[index] = false;
                }
            }
        }
    }
    std::vector<Segment> out;
    out.reserve(segments.size());
    for (std::size_t index = 0; index < segments.size(); ++index) {
        if (live[index]) {
            out.push_back(std::move(segments[index]));
        }
    }
    segments = std::move(out);
}

// A run of segments read in one request; `parts` are its segments.
struct Block {
    Segment whole;
    std::vector<Segment> parts;
};

// Joins the segments of one poll group in address order, each request as
// large as its limits allow.
std::vector<Block> blocks_of(const std::vector<Segment>& segments,
                             const ReadRules& rules) {
    std::vector<Block> blocks;
    for (const Segment& segment : segments) {
        if (!blocks.empty()) {
            Block& open = blocks.back();
            const Segment& whole = open.whole;
            if (whole.place == segment.place && !whole.exclusive &&
                !segment.exclusive &&
                bridgeable(whole.place, whole.last, segment.first, rules) &&
                fits(whole.place, whole.first,
                     std::max(whole.last, segment.last),
                     whole.members.size() + segment.members.size(), rules)) {
                absorb(open.whole, segment);
                open.parts.push_back(segment);
                continue;
            }
        }
        blocks.push_back(Block{.whole = segment, .parts = {segment}});
    }
    return blocks;
}

Request request_of(const Segment& segment,
                   const std::vector<PlanPoint>& points) {
    Request request{.poll_group = segment.place.group,
                    .space = segment.place.space,
                    .unit_id = segment.place.unit_id,
                    .first = segment.first,
                    .count = segment.last - segment.first + 1,
                    .points = {},
                    .side_effect = segment.side_effect,
                    .exclusive = segment.exclusive,
                    .chained = false,
                    .split = {}};
    std::vector<std::uint32_t> members = segment.members;
    std::sort(members.begin(), members.end(),
              [&](std::uint32_t left, std::uint32_t right) {
                  return std::tie(points[left].first, points[left].point) <
                         std::tie(points[right].first, points[right].point);
              });
    request.points.reserve(members.size());
    for (const std::uint32_t member : members) {
        request.points.push_back(points[member].point);
    }
    return request;
}

// The parts of a block's split form: its segments, with the parts from the
// first to the last that share a cluster joined, so that a value and its
// companion stay in one request.
std::vector<Segment> split_parts(const Block& block) {
    std::vector<Segment> parts = block.parts;
    bool joined = true;
    while (joined) {
        joined = false;
        for (std::size_t from = 0; from < parts.size() && !joined; ++from) {
            for (std::size_t last_part = parts.size() - 1; last_part > from;
                 --last_part) {
                if (!share(parts[from].clusters, parts[last_part].clusters)) {
                    continue;
                }
                for (std::size_t index = from + 1; index <= last_part;
                     ++index) {
                    absorb(parts[from], parts[index]);
                }
                parts.erase(
                    parts.begin() + static_cast<std::ptrdiff_t>(from + 1),
                    parts.begin() + static_cast<std::ptrdiff_t>(last_part + 1));
                joined = true;
                break;
            }
        }
    }
    return parts;
}

// Blocks that share a cluster form a chain, sent back to back in address
// order (5.5); every other block stands alone. Returns the chains in the
// order of their first block.
std::vector<std::vector<std::size_t>> chains_of(
    const std::vector<Block>& blocks) {
    std::vector<std::size_t> root(blocks.size());
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        root[index] = index;
    }
    auto find = [&](std::size_t index) {
        while (root[index] != index) {
            index = root[index];
        }
        return index;
    };
    std::map<std::uint32_t, std::size_t> first_block;
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        for (const std::uint32_t cluster : blocks[index].whole.clusters) {
            const auto [found, added] = first_block.try_emplace(cluster, index);
            if (!added) {
                const std::size_t left = find(found->second);
                const std::size_t right = find(index);
                root[std::max(left, right)] = std::min(left, right);
            }
        }
    }
    std::vector<std::vector<std::size_t>> chains;
    std::vector<std::size_t> chain_of(blocks.size(), blocks.size());
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const std::size_t top = find(index);
        if (chain_of[top] == blocks.size()) {
            chain_of[top] = chains.size();
            chains.emplace_back();
        }
        chains[chain_of[top]].push_back(index);
    }
    return chains;
}

// Plans one poll group and appends its requests to `out`; `parts` gets the
// split parts of each planned request.
void plan_group(std::vector<Segment> segments,
                const std::vector<PlanPoint>& points, const ReadRules& rules,
                ReadPlan& out, std::vector<std::uint32_t>& order,
                std::vector<std::vector<Segment>>& parts,
                std::vector<bool>& in_chain) {
    fuse_clusters(segments, rules);
    sort_segments(segments);
    const std::vector<Block> blocks = blocks_of(segments, rules);
    for (const std::vector<std::size_t>& chain : chains_of(blocks)) {
        for (std::size_t position = 0; position < chain.size(); ++position) {
            const Block& block = blocks[chain[position]];
            order.push_back(static_cast<std::uint32_t>(out.requests.size()));
            Request request = request_of(block.whole, points);
            request.chained = position + 1 < chain.size();
            out.requests.push_back(std::move(request));
            // A request of a chain has no split form: its parts are bound to
            // the other requests of the chain.
            parts.push_back(chain.size() > 1 ? std::vector<Segment>{}
                                             : split_parts(block));
            in_chain.push_back(chain.size() > 1);
        }
    }
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
    const std::vector<ReadRange> merged = merged_ranges(rules);
    if (const Status status = validate_ranges(rules, merged); !status.ok()) {
        return status;
    }
    if (const Status status =
            validate_points(points, rules, merged, poll_groups);
        !status.ok()) {
        return status;
    }
    const std::vector<Segment> segments = segments_of(points, merged);
    for (const Segment& segment : segments) {
        if (!fits(segment.place, segment.first, segment.last,
                  segment.members.size(), rules)) {
            return refused(
                "points that must be read together exceed one request");
        }
    }
    out = ReadPlan{};
    out.by_group.resize(poll_groups);
    std::vector<std::vector<Segment>> parts;
    std::vector<bool> in_chain;
    for (std::uint32_t group = 0; group < poll_groups; ++group) {
        std::vector<Segment> of_group;
        for (const Segment& segment : segments) {
            if (segment.place.group == group) {
                of_group.push_back(segment);
            }
        }
        plan_group(std::move(of_group), points, rules, out, out.by_group[group],
                   parts, in_chain);
    }
    // Split forms: one request per part, for a request of several parts
    // that touches no side-effect range (CS A.4: such a range is never
    // split).
    const auto planned = static_cast<std::uint32_t>(out.requests.size());
    for (std::uint32_t index = 0; index < planned; ++index) {
        if (parts[index].size() < 2 || out.requests[index].side_effect) {
            continue;
        }
        for (const Segment& part : parts[index]) {
            out.requests[index].split.push_back(
                static_cast<std::uint32_t>(out.requests.size()));
            out.requests.push_back(request_of(part, points));
        }
    }
    // Keep-alive and probe: the shortest planned request outside every
    // side-effect and exclusive range and outside every chain, so that it
    // never reads a value without its companion (bridging never touches a
    // range).
    for (std::uint32_t index = 0; index < planned; ++index) {
        const Request& request = out.requests[index];
        if (request.side_effect || request.exclusive || in_chain[index]) {
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
