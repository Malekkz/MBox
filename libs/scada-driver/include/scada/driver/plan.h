#ifndef SCADA_DRIVER_PLAN_H
#define SCADA_DRIVER_PLAN_H

// The read plan of a device (driver interface contract 6.3): the requests
// each poll group sends in one period, each with its split form, and the
// device's keep-alive and probe requests. A protocol's `plan` builds it;
// `plan_blocks` is the framework's block planner, which a protocol whose
// points are spans of numbered units (registers, coils) can use as its
// whole `plan`.
//
// Units are the protocol's: a register, a bit. An address space is the
// protocol's too (a Modbus table); the framework only compares them.

#include "scada/common/status.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace scada::driver {

// A range of the read rules (CS `read:`, annex A.4 for Modbus).
struct ReadRange {
    enum class Kind : std::uint8_t {
        NO_READ = 1,  // never read, also not inside a block
        ATOMIC,       // always in one request, never split
        EXCLUSIVE,    // read alone, by one poll group; implies SIDE_EFFECT
        SIDE_EFFECT,  // a read changes the device: once per poll, no retry
    };
    Kind kind = Kind::NO_READ;
    std::uint32_t space = 0;
    std::uint32_t unit_id = 0;
    std::uint64_t first = 0;  // first unit, inclusive
    std::uint64_t last = 0;   // last unit, inclusive
};

// The request limits of one address space (CS A.4: `max_registers` for
// registers, `max_bits` for bits).
struct SpaceLimits {
    std::uint32_t space = 0;
    std::uint64_t max_units = 1;  // largest request, in units (>= 1)
    std::uint64_t max_gap = 0;    // unrequested units a request may bridge
};

// The rules a plan keeps (6.3). Ranges of different kinds must not overlap
// (CS CF-610); `plan_blocks` refuses rules where they do. Ranges of one
// kind may overlap: they are read as their union.
struct ReadRules {
    std::uint64_t max_units = 1;   // for spaces not in `spaces` (>= 1)
    std::uint64_t max_gap = 0;     // for spaces not in `spaces`
    std::uint32_t max_values = 0;  // points per request; 0: no limit
    // NOLINTBEGIN(readability-redundant-member-init): designated initializers may omit it (-Wmissing-field-initializers)
    std::vector<SpaceLimits> spaces{};
    std::vector<ReadRange> ranges{};
    // NOLINTEND(readability-redundant-member-init)
};

// The poll group of a point read only by the identity check: a protocol
// address an identity check reads, which has no tag (D-147).
inline constexpr std::uint32_t NO_POLL_GROUP = 0xFFFF'FFFFU;

// The protocol units a point reads (one span), in one poll group. A point
// and its companions (5.5) name the same `cluster`: the plan reads them in
// one poll, in one request when they fit, otherwise back to back.
struct PlanPoint {
    std::uint32_t point = 0;  // index of the point in its device
    // NO_POLL_GROUP only for an identity point (D-147).
    std::uint32_t poll_group = 0;
    std::uint32_t cluster = 0;
    std::uint32_t space = 0;
    std::uint32_t unit_id = 0;
    std::uint64_t first = 0;
    std::uint64_t count = 1;  // >= 1
    // Read by an identity check or a restart rule (3.6, 7.6; D-147): never
    // in a request that reads a side effect.
    bool identity = false;
};

// One request: a planned read, or one write of an order (2.2). A planned
// read is immutable once planned; every attempt of it has its own record
// (exchange.h, Attempt). A write is built by the framework for each step of
// an order (8.3): its point, its span, and the raw value to write.
struct Request {
    enum class Kind : std::uint8_t {
        READ = 1,
        WRITE,
    };
    Kind kind = Kind::READ;
    std::uint32_t poll_group = 0;
    std::uint32_t space = 0;
    std::uint32_t unit_id = 0;
    std::uint64_t first = 0;
    std::uint64_t count = 0;
    // NOLINTBEGIN(readability-redundant-member-init): designated initializers may omit it (-Wmissing-field-initializers)
    std::vector<std::uint32_t> points{};  // in address order
    // NOLINTEND(readability-redundant-member-init)
    // Reads a SIDE_EFFECT or EXCLUSIVE range: read once per poll, never
    // retried, never sent as keep-alive, probe, identity or READ_NOW (6.6).
    bool side_effect = false;
    // Holds an EXCLUSIVE range: never merged with anything else.
    bool exclusive = false;
    // The next request of this poll group must follow it on this device
    // with no other request in between: a point and a companion that did
    // not fit in one request (5.5).
    bool chained = false;
    // The requests it becomes if the device refuses it as a whole as an
    // illegal address (indices into ReadPlan::requests). Empty: refused,
    // its points are ABSENT (6.3).
    // NOLINTBEGIN(readability-redundant-member-init): designated initializers may omit it (-Wmissing-field-initializers)
    std::vector<std::uint32_t> split{};
    // A write: the raw value in the protocol's units, already converted
    // exactly from the command's value (8.4), `count` units long. It lives
    // in the order's slot of the framework and is valid during the call
    // that is passed the request. Empty for a read.
    std::span<const std::byte> payload{};
    // NOLINTEND(readability-redundant-member-init)
};

// The plan of one device.
struct ReadPlan {
    // Every request: the planned ones first, then the split forms.
    std::vector<Request> requests;
    // For each poll group of the channel, the planned requests of one period
    // in the order they are sent.
    std::vector<std::vector<std::uint32_t>> by_group;
    // The keep-alive and probe request (6.5, 7.4): the shortest planned
    // request outside every side-effect, exclusive and no-read range and
    // outside every chain; else a request of its own (not in `by_group`)
    // that reads the shortest part of a chain whose clusters are whole and
    // that reads no side effect; else none, which the compile step reports
    // when a keep-alive is configured. A demoted device with no probe
    // request is probed by one whole poll cycle at each back-off instant
    // (D-138, R8); the compile step reports such a device too.
    std::optional<std::uint32_t> keepalive;
    std::optional<std::uint32_t> probe;
    // The identity cycle (3.6, 7.6; D-147): the planned requests that read
    // an identity point, each with its whole chain, in the order of their
    // groups, then those of points in no poll group (which are in no group
    // of `by_group` and are never the keep-alive). None reads a side effect
    // or an exclusive range.
    // NOLINTNEXTLINE(readability-redundant-member-init): designated initializers may omit it (-Wmissing-field-initializers)
    std::vector<std::uint32_t> identity{};
};

// `sys.dev.<device>.requests_per_scan`: the requests of one period of every
// poll group of the device (6.3).
[[nodiscard]] std::uint32_t requests_per_scan(const ReadPlan& plan) noexcept;

// Plans the points of one device for `poll_groups` poll groups within
// `rules` (6.3). An atomic range is read whole. All points of a side-effect
// or exclusive range are read by one request, over the span from the first
// to the last unit they decode (CS A.4); an exclusive range's request holds
// nothing else. A point's cluster (5.5) is one request when it fits in one,
// with the points between; otherwise its requests are chained back to back,
// a side-effect request last (a chain reading two side-effect ranges is
// refused), and no side-effect point of another cluster joins a request of
// the chain.
// Never splits a point's span, never merges across spaces, unit ids or poll
// groups, never bridges a gap that touches a range. A bridged gap returns
// units that are never decoded. Every request keeps its space's `max_units`
// and `max_values`. A request of several parts outside every chain gets a
// split form of one request per part (a part keeps its clusters whole); a
// request that reads a side-effect range has none (CS A.4: never split).
// Returns INVALID_ARGUMENT for rules or points the compile step should have
// refused: overlapping ranges of different kinds, a point in a no-read
// range, a point across the edge of an exclusive range, a point, an atomic
// range or the points of one side-effect range that do not fit one request,
// a side-effect range read by more than one poll group, a poll group out of
// range, limits given twice for a space, an identity point in a side-effect
// or exclusive range or read with one, a point in no poll group that is not
// an identity point. Allocates.
[[nodiscard]] Status plan_blocks(const std::vector<PlanPoint>& points,
                                 const ReadRules& rules,
                                 std::uint32_t poll_groups, ReadPlan& out);

}  // namespace scada::driver

#endif  // SCADA_DRIVER_PLAN_H
