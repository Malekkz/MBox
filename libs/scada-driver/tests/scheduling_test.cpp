// Scheduling with the test protocol over the loopback (driver interface
// contract 6.2 to 6.6; conformance tests 9 to 14, the parts that need no
// failure handling). Time is injected: every exchange takes 1 ms, and the
// loop jumps to the scheduler's wake time, so the request log is exact.
//
// Left to the slices that bring them: dead devices and the probe budget
// (test 12, 7.4), retries, identity reads and READ_NOW (test 14).

#include "channel_builder.h"
#include "loopback.h"
#include "scada/common/status.h"
#include "scada/driver/compiled.h"
#include "scada/driver/exchange.h"
#include "scada/driver/plan.h"
#include "scada/driver/protocol.h"
#include "scada/driver/scheduler.h"
#include "test_protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace scada::driver::test {
namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): the addresses, counts and times are the test data, each worked out by hand beside the rule it checks

constexpr std::int64_t EXCHANGE = MILLISECOND;
constexpr std::uint64_t LARGE = 64;
constexpr std::uint32_t FAST = 0;
constexpr std::uint32_t NORMAL = 1;
constexpr std::uint32_t SLOW = 2;

std::vector<LoggedRequest> of_unit(const SimBus& bus, std::uint8_t unit_id) {
    std::vector<LoggedRequest> out;
    for (const LoggedRequest& request : bus.log()) {
        if (request.unit_id == unit_id) {
            out.push_back(request);
        }
    }
    return out;
}

std::vector<std::uint32_t> firsts(const std::vector<LoggedRequest>& log) {
    std::vector<std::uint32_t> out;
    out.reserve(log.size());
    for (const LoggedRequest& request : log) {
        out.push_back(request.first);
    }
    return out;
}

std::vector<std::uint16_t> counts(const std::vector<LoggedRequest>& log) {
    std::vector<std::uint16_t> out;
    out.reserve(log.size());
    for (const LoggedRequest& request : log) {
        out.push_back(request.count);
    }
    return out;
}

// The least time between two consecutive requests of `log`.
std::int64_t min_spacing(const std::vector<LoggedRequest>& log) {
    std::int64_t least = std::numeric_limits<std::int64_t>::max();
    for (std::size_t index = 1; index < log.size(); ++index) {
        least = std::min(least, log[index].time_ns - log[index - 1].time_ns);
    }
    return least;
}

// For one request per `period` from 0: the earliest and the latest a
// request came against its due time.
std::pair<std::int64_t, std::int64_t> lateness(
    const std::vector<LoggedRequest>& log, std::int64_t period) {
    std::int64_t early = std::numeric_limits<std::int64_t>::max();
    std::int64_t late = std::numeric_limits<std::int64_t>::min();
    for (std::size_t index = 0; index < log.size(); ++index) {
        const std::int64_t off =
            log[index].time_ns - (static_cast<std::int64_t>(index) * period);
        early = std::min(early, off);
        late = std::max(late, off);
    }
    return {early, late};
}

// The times and requests of the keep-alives the loop sent.
std::pair<std::vector<std::int64_t>, std::vector<std::uint32_t>> keepalives_of(
    const Loop& loop) {
    std::vector<std::int64_t> times;
    std::vector<std::uint32_t> requests;
    for (const LoopEvent& event : loop.events()) {
        if (event.choice.kind == Choice::Kind::KEEPALIVE) {
            times.push_back(event.time_ns);
            requests.push_back(event.choice.request);
        }
    }
    return {times, requests};
}

std::size_t sample_count(const Loop& loop) {
    std::size_t total = 0;
    for (const LoopEvent& event : loop.events()) {
        total += event.samples.size();
    }
    return total;
}

// The first unit of the request each `take` at `times` reads (a poll or a
// keep-alive); NOT_A_READ for any other choice.
constexpr std::uint64_t NOT_A_READ = std::numeric_limits<std::uint64_t>::max();

std::vector<std::uint64_t> reads_at(Scheduler& scheduler, const ReadPlan& plan,
                                    const std::vector<std::int64_t>& times) {
    std::vector<std::uint64_t> out;
    out.reserve(times.size());
    for (const std::int64_t time : times) {
        const Choice choice = scheduler.take(time);
        const bool read = choice.kind == Choice::Kind::POLL ||
                          choice.kind == Choice::Kind::KEEPALIVE;
        out.push_back(read ? plan.requests[choice.request].first : NOT_A_READ);
    }
    return out;
}

// The start of each cycle of a one-request group, `count` of them, with the
// worker stalled once from `stall_from` to `stall_to`.
std::vector<std::int64_t> cycle_starts(Scheduler& scheduler, std::size_t count,
                                       std::int64_t stall_from,
                                       std::int64_t stall_to) {
    std::vector<std::int64_t> starts;
    std::int64_t now = 0;
    while (starts.size() < count) {
        const Choice choice = scheduler.take(now);
        if (choice.kind == Choice::Kind::POLL) {
            starts.push_back(now);
            now += MILLISECOND;
            continue;
        }
        if (choice.kind != Choice::Kind::WAIT) {
            break;
        }
        now = choice.wake_ns == stall_from ? stall_to : choice.wake_ns;
    }
    return starts;
}

// A bus with registers 0 to 199 mapped on units 1 to 4.
SimBus mapped_bus() {
    SimBus bus;
    for (std::uint8_t unit = 1; unit <= 4; ++unit) {
        bus.map(unit, 0, 0, 199);
    }
    return bus;
}

// --- Conformance test 9: orders first, then priority, then due time ------

TEST(Scheduling, OrdersComeBeforeDuePolls) {
    const CompiledChannel channel =
        make_channel({{.name = "d1", .points = adjacent(0, 1)}},
                     ReadRules{.max_units = LARGE});
    SimBus bus = mapped_bus();
    const TestProtocol protocol;
    Loop loop(channel, protocol, bus, 0, EXCHANGE);
    ASSERT_TRUE(loop.scheduler().add_order(
        OrderTicket{.id = 7, .device = 0, .steps = 1}));
    loop.run_until(2 * MILLISECOND);
    ASSERT_GE(loop.events().size(), 2U);
    EXPECT_EQ(loop.events()[0].choice.kind, Choice::Kind::ORDER);
    EXPECT_EQ(loop.events()[0].choice.order, 7U);
    EXPECT_EQ(loop.events()[1].choice.kind, Choice::Kind::POLL);
}

TEST(Scheduling, DuePollGroupsGoByPriority) {
    // One point in each default group, all due at the start.
    const CompiledChannel channel =
        make_channel({{.name = "d1",
                       .points = {{.first = 10, .group = SLOW},
                                  {.first = 0, .group = FAST},
                                  {.first = 5, .group = NORMAL}}}},
                     ReadRules{.max_units = 1});
    SimBus bus = mapped_bus();
    const TestProtocol protocol;
    Loop loop(channel, protocol, bus, 0, EXCHANGE);
    loop.run_until(3 * MILLISECOND);
    ASSERT_EQ(bus.log().size(), 3U);
    EXPECT_EQ(bus.log()[0].first, 0U);   // fast, priority 1
    EXPECT_EQ(bus.log()[1].first, 5U);   // normal, priority 2
    EXPECT_EQ(bus.log()[2].first, 10U);  // slow, priority 3
}

TEST(Scheduling, AtEqualPriorityTheEarlierDueGoesFirst) {
    // Two groups of priority 2: q (300 ms) on device 1, p (1 s) on device 2.
    // A channel spacing of 400 ms makes q's cycles late. At 400 ms, p's
    // first cycle (due 0) beats q's second (due 400 ms) although device 1
    // comes first.
    const std::vector<CompiledPollGroup> groups{
        {.name = "q", .period_ns = 300 * MILLISECOND, .priority = 2},
        {.name = "p", .period_ns = SECOND, .priority = 2}};
    const CompiledChannel channel = make_channel(
        {{.name = "d1", .unit_id = 1, .points = adjacent(0, 1, 0)},
         {.name = "d2", .unit_id = 2, .points = adjacent(0, 1, 1)}},
        ReadRules{.max_units = LARGE},
        Pacing{.min_request_interval_ns = 400 * MILLISECOND}, groups);
    SimBus bus = mapped_bus();
    const TestProtocol protocol;
    Loop loop(channel, protocol, bus, 0, EXCHANGE);
    loop.run_until(401 * MILLISECOND);
    ASSERT_EQ(bus.log().size(), 2U);
    EXPECT_EQ(bus.log()[0].unit_id, 1);  // tie at 0: device order
    EXPECT_EQ(bus.log()[1].unit_id, 2);  // due 0 before due 400 ms
    EXPECT_EQ(bus.log()[1].time_ns, 400 * MILLISECOND);
}

// --- Conformance test 10: block merging over the wire ---------------------

TEST(Scheduling, AdjacentPointsSendThePlannedNumberOfRequests) {
    // 10 points, 4 units per request: 3 requests per 200 ms cycle.
    const CompiledChannel channel =
        make_channel({{.name = "d1", .points = adjacent(100, 10)}},
                     ReadRules{.max_units = 4});
    ASSERT_EQ(requests_per_scan(channel.devices[0].plan), 3U);
    SimBus bus = mapped_bus();
    const TestProtocol protocol;
    Loop loop(channel, protocol, bus, 0, EXCHANGE);
    loop.run_until(SECOND);
    // 5 cycles of 3, each starting on its 200 ms.
    std::vector<std::uint32_t> expected;
    std::vector<std::int64_t> starts;
    for (std::int64_t cycle = 0; cycle < 5; ++cycle) {
        expected.insert(expected.end(), {100, 104, 108});
        starts.push_back(cycle * 200 * MILLISECOND);
    }
    EXPECT_EQ(firsts(bus.log()), expected);
    std::vector<std::int64_t> started;
    for (std::size_t index = 0; index < bus.log().size(); index += 3) {
        started.push_back(bus.log()[index].time_ns);
    }
    EXPECT_EQ(started, starts);
    EXPECT_EQ(sample_count(loop), 50U);  // every sample, for its point
}

TEST(Scheduling, NoReadRegistersAreNeverOnTheWire) {
    ReadRules rules{.max_units = LARGE, .max_gap = 10};
    rules.ranges = {ReadRange{
        .kind = ReadRange::Kind::NO_READ, .unit_id = 1, .first = 3, .last = 3}};
    const CompiledChannel channel = make_channel(
        {{.name = "d1", .points = {{.first = 0}, {.first = 6}}}}, rules);
    SimBus bus = mapped_bus();
    const TestProtocol protocol;
    Loop loop(channel, protocol, bus, 0, EXCHANGE);
    loop.run_until(SECOND);
    for (const LoggedRequest& request : bus.log()) {
        EXPECT_FALSE(request.first <= 3 && 3 < request.first + request.count);
    }
}

// --- Conformance test 11: the split form, once ----------------------------

TEST(Scheduling, ARefusedMergedRequestUsesItsSplitFormOnce) {
    // Points at 0, 1 and 3, merged across the gap at 2, which the device
    // does not map: the merged read is refused once, then the split form
    // (one request per point here: each is its own cluster) is used for
    // good.
    const CompiledChannel channel = make_channel(
        {{.name = "d1", .points = {{.first = 0}, {.first = 1}, {.first = 3}}}},
        ReadRules{.max_units = LARGE, .max_gap = 1});
    SimBus bus;
    bus.map(1, 0, 0, 1);
    bus.map(1, 0, 3, 3);
    const TestProtocol protocol;
    Loop loop(channel, protocol, bus, 0, EXCHANGE);
    loop.run_until(SECOND);
    // The merged read once, its split form in the same cycle, then the
    // split form in each of 4 more cycles.
    std::vector<std::uint32_t> expected_firsts{0};
    std::vector<std::uint16_t> expected_counts{4};
    for (int cycle = 0; cycle < 5; ++cycle) {
        expected_firsts.insert(expected_firsts.end(), {0, 1, 3});
        expected_counts.insert(expected_counts.end(), {1, 1, 1});
    }
    EXPECT_EQ(firsts(bus.log()), expected_firsts);
    EXPECT_EQ(counts(bus.log()), expected_counts);
    ASSERT_GE(bus.log().size(), 4U);
    EXPECT_EQ(bus.log()[3].time_ns, 3 * MILLISECOND);
}

TEST(Scheduling, PointsRefusedAgainAreNotReadUntilReconnect) {
    // The device has 0 and 1 but not 3: after the split, the part at 3 is
    // refused again and no longer sent; a reconnect tries the plan again.
    const CompiledChannel channel = make_channel(
        {{.name = "d1", .points = {{.first = 0}, {.first = 1}, {.first = 3}}}},
        ReadRules{.max_units = LARGE, .max_gap = 1});
    SimBus bus;
    bus.map(1, 0, 0, 1);
    const TestProtocol protocol;
    Loop loop(channel, protocol, bus, 0, EXCHANGE);
    loop.run_until(SECOND);
    // The merged read, its 3 parts (3 refused again, then never sent), then
    // 2 parts in each of 4 more cycles.
    EXPECT_EQ(firsts(bus.log()),
              (std::vector<std::uint32_t>{0, 0, 1, 3, 0, 1, 0, 1, 0, 1, 0, 1}));
    loop.scheduler().reconnect();
    const std::size_t before = bus.log().size();
    loop.run_until(SECOND + (200 * MILLISECOND));
    ASSERT_GT(bus.log().size(), before);
    EXPECT_EQ(bus.log()[before].count, 4U);  // the merged request again
}

// --- Conformance test 12: pacing ------------------------------------------

TEST(Scheduling, NoTwoRequestsToADeviceCloserThanItsInterval) {
    // Device 1 is paced at 70 ms and has 3 requests per 200 ms cycle;
    // device 2 is not paced and keeps its 200 ms period.
    const CompiledChannel channel =
        make_channel({{.name = "d1",
                       .unit_id = 1,
                       .points = adjacent(0, 3),
                       .pacing = {.min_request_interval_ns = 70 * MILLISECOND}},
                      {.name = "d2", .unit_id = 2, .points = adjacent(0, 1)}},
                     ReadRules{.max_units = 1});
    SimBus bus = mapped_bus();
    const TestProtocol protocol;
    Loop loop(channel, protocol, bus, 0, EXCHANGE);
    loop.run_until(2 * SECOND);
    const std::vector<LoggedRequest> paced = of_unit(bus, 1);
    ASSERT_GT(paced.size(), 3U);
    EXPECT_GE(min_spacing(paced), 70 * MILLISECOND);
    // The neighbour is not held back: one read every 200 ms, at most one
    // exchange late when both devices are due at once (3.4).
    const std::vector<LoggedRequest> free = of_unit(bus, 2);
    ASSERT_EQ(free.size(), 10U);
    const auto [early, late] = lateness(free, 200 * MILLISECOND);
    EXPECT_GE(early, 0);
    EXPECT_LE(late, EXCHANGE);
    EXPECT_FALSE(loop.scheduler().pacing_limited(1, loop.now()));
}

TEST(Scheduling, WritesFirstKeepTheWriteInterval) {
    // Three orders queued at once on a device with a write interval of
    // 300 ms: written at 0, 300 and 600 ms, with polls in between.
    const CompiledChannel channel =
        make_channel({{.name = "d1",
                       .points = adjacent(0, 1),
                       .pacing = {.min_write_interval_ns = 300 * MILLISECOND}}},
                     ReadRules{.max_units = LARGE});
    SimBus bus = mapped_bus();
    const TestProtocol protocol;
    Loop loop(channel, protocol, bus, 0, EXCHANGE);
    for (std::uint64_t id = 1; id <= 3; ++id) {
        ASSERT_TRUE(loop.scheduler().add_order(
            OrderTicket{.id = id, .device = 0, .steps = 1}));
    }
    loop.run_until(SECOND);
    std::vector<std::int64_t> writes;
    std::size_t polls_before_last = 0;
    for (const LoopEvent& event : loop.events()) {
        if (event.choice.kind == Choice::Kind::ORDER) {
            writes.push_back(event.time_ns);
        } else if (writes.size() < 3) {
            ++polls_before_last;
        }
    }
    EXPECT_EQ(writes, (std::vector<std::int64_t>{0, 300 * MILLISECOND,
                                                 600 * MILLISECOND}));
    EXPECT_GT(polls_before_last, 0U);  // polls are served while writes wait
}

TEST(Scheduling, TheChannelSpacingAppliesToEveryDevice) {
    const CompiledChannel channel =
        make_channel({{.name = "d1", .unit_id = 1, .points = adjacent(0, 1)},
                      {.name = "d2", .unit_id = 2, .points = adjacent(0, 1)}},
                     ReadRules{.max_units = LARGE},
                     Pacing{.min_request_interval_ns = 50 * MILLISECOND});
    SimBus bus = mapped_bus();
    const TestProtocol protocol;
    Loop loop(channel, protocol, bus, 0, EXCHANGE);
    loop.run_until(SECOND);
    ASSERT_GT(bus.log().size(), 1U);
    EXPECT_GE(min_spacing(bus.log()), 50 * MILLISECOND);
}

TEST(Scheduling, AnUnservablePeriodIsReportedAsAchievedAndLimited) {
    // 4 requests per 200 ms cycle on a device paced at 100 ms: each cycle
    // takes 400 ms. After 10 s the last 10 periods are all 400 ms (the first
    // is shorter: it started at once).
    const CompiledChannel channel = make_channel(
        {{.name = "d1",
          .points = adjacent(0, 4),
          .pacing = {.min_request_interval_ns = 100 * MILLISECOND}}},
        ReadRules{.max_units = 1});
    SimBus bus = mapped_bus();
    const TestProtocol protocol;
    Loop loop(channel, protocol, bus, 0, EXCHANGE);
    loop.run_until(10 * SECOND);
    EXPECT_EQ(loop.scheduler().achieved_period_ns(FAST, loop.now()),
              400 * MILLISECOND);
    EXPECT_TRUE(loop.scheduler().pacing_limited(0, loop.now()));
}

// --- Conformance test 13: keep-alive --------------------------------------

TEST(Scheduling, NoRequestOfAChainRests) {
    // K1: a chain of companions (0 with its companion at 100): neither its
    // first nor its last request can rest.
    const CompiledChannel channel = make_channel(
        {{.name = "d1",
          .points = {{.first = 0, .companions = {1}}, {.first = 100}}}},
        ReadRules{.max_units = 4});
    Scheduler scheduler(channel, 4, 0);
    const ReadPlan& plan = channel.devices[0].plan;
    for (std::uint32_t request = 0; request < plan.requests.size(); ++request) {
        scheduler.rest(0, request, true);
        EXPECT_TRUE(scheduler.in_chain(0, request));
        EXPECT_FALSE(scheduler.resting(0, request));
    }
}

TEST(Scheduling, ARestingRequestIsNoKeepAlive) {
    // K5: the keep-alive request (the block at 50) rests as a dead block:
    // no keep-alive is sent while it rests; after its rest they resume.
    const CompiledChannel channel =
        make_channel({{.name = "d1",
                       .points = {{.first = 0, .count = 2, .group = SLOW},
                                  {.first = 50, .group = SLOW}},
                       .keepalive_ns = 2 * SECOND}},
                     ReadRules{.max_units = 4});
    SimBus bus = mapped_bus();
    const TestProtocol protocol;
    Loop loop(channel, protocol, bus, 0, EXCHANGE);
    const ReadPlan& plan = channel.devices[0].plan;
    ASSERT_TRUE(plan.keepalive.has_value());
    const std::uint32_t keepalive = plan.keepalive.value_or(0);
    ASSERT_EQ(plan.requests[keepalive].first, 50U);
    loop.scheduler().rest(0, keepalive, true);
    loop.run_until(9 * SECOND);
    EXPECT_TRUE(keepalives_of(loop).first.empty());
    loop.scheduler().rest(0, keepalive, false);
    loop.run_until(13 * SECOND);  // the poll at 10 s, a keep-alive 2 s on
    EXPECT_FALSE(keepalives_of(loop).first.empty());
}

TEST(Scheduling, AnIdleDeviceGetsAKeepAliveOutsideEverySideEffect) {
    // A slow group (10 s) and a keep-alive of 2 s: between polls the device
    // gets keep-alives, which never read the side-effect range at 0.
    ReadRules rules{.max_units = 1};
    rules.ranges = {ReadRange{.kind = ReadRange::Kind::SIDE_EFFECT,
                              .unit_id = 1,
                              .first = 0,
                              .last = 0}};
    const CompiledChannel channel = make_channel(
        {{.name = "d1",
          .points = {{.first = 0, .group = SLOW}, {.first = 50, .group = SLOW}},
          .keepalive_ns = 2 * SECOND}},
        rules);
    SimBus bus = mapped_bus();
    bus.side_effect(1, 0, 0, 0);
    const TestProtocol protocol;
    Loop loop(channel, protocol, bus, 0, EXCHANGE);
    loop.run_until(9 * SECOND);
    const ReadPlan& plan = channel.devices[0].plan;
    if (!plan.keepalive.has_value()) {
        ADD_FAILURE() << "no keep-alive request";
        return;
    }
    const std::uint32_t keepalive = *plan.keepalive;
    EXPECT_FALSE(plan.requests[keepalive].side_effect);
    EXPECT_EQ(plan.requests[keepalive].first, 50U);
    const auto [keepalives, kept] = keepalives_of(loop);
    EXPECT_EQ(kept, std::vector<std::uint32_t>(keepalives.size(), keepalive));
    // The poll ends at 1 ms; then every 2 s of silence.
    EXPECT_EQ(keepalives,
              (std::vector<std::int64_t>{
                  (2 * SECOND) + MILLISECOND, (4 * SECOND) + MILLISECOND,
                  (6 * SECOND) + MILLISECOND, (8 * SECOND) + MILLISECOND}));
    EXPECT_EQ(bus.side_effect_reads(), 1U);  // the one poll
}

// --- Conformance test 14: clear-on-read -----------------------------------

TEST(Scheduling, ASideEffectRangeIsReadOncePerPoll) {
    ReadRules rules{.max_units = LARGE};
    rules.ranges = {ReadRange{.kind = ReadRange::Kind::SIDE_EFFECT,
                              .unit_id = 1,
                              .first = 0,
                              .last = 1}};
    // Two points of the range and one beside it, in the fast group, with a
    // keep-alive that must not read it.
    const CompiledChannel channel =
        make_channel({{.name = "d1",
                       .points = {{.first = 0}, {.first = 1}, {.first = 20}},
                       .keepalive_ns = 50 * MILLISECOND}},
                     rules);
    SimBus bus = mapped_bus();
    bus.side_effect(1, 0, 0, 1);
    const TestProtocol protocol;
    Loop loop(channel, protocol, bus, 0, EXCHANGE);
    loop.run_until(SECOND);
    EXPECT_EQ(bus.side_effect_reads(), 5U);  // 0, 200, 400, 600, 800 ms
    for (const LoopEvent& event : loop.events()) {
        if (event.choice.kind == Choice::Kind::KEEPALIVE) {
            EXPECT_FALSE(channel.devices[0]
                             .plan.requests[event.choice.request]
                             .side_effect);
        }
    }
}

TEST(Scheduling, AFlagSetBetweenPollsIsReadOnceThenCleared) {
    ReadRules rules{.max_units = LARGE};
    rules.ranges = {ReadRange{.kind = ReadRange::Kind::SIDE_EFFECT,
                              .unit_id = 1,
                              .first = 0,
                              .last = 0}};
    const CompiledChannel channel =
        make_channel({{.name = "d1", .points = {{.first = 0}}}}, rules);
    SimBus bus = mapped_bus();
    bus.side_effect(1, 0, 0, 0);
    bus.set(1, 0, 0, 1);
    const TestProtocol protocol;
    Loop loop(channel, protocol, bus, 0, EXCHANGE);
    loop.run_until(300 * MILLISECOND);
    std::vector<std::uint64_t> values;
    for (const LoopEvent& event : loop.events()) {
        for (const DeliveredSample& sample : event.samples) {
            values.push_back(sample.sample.raw.number_u);
        }
    }
    EXPECT_EQ(values, (std::vector<std::uint64_t>{1, 0}));
}

// --- Companions back to back (5.5) ----------------------------------------

TEST(Scheduling, AChainOfCompanionsHasNoOtherPollToItsDeviceBetween) {
    // Point 0 at 0 with its companion at 100, and a point at 50 in the slow
    // group (4 units per request): nothing of the slow group between the
    // two parts of the chain, though it is due.
    const CompiledChannel channel =
        make_channel({{.name = "d1",
                       .points = {{.first = 0, .companions = {1}},
                                  {.first = 100},
                                  {.first = 50, .group = SLOW}}}},
                     ReadRules{.max_units = 4});
    Scheduler scheduler(channel, 4, 0);
    const ReadPlan& plan = channel.devices[0].plan;
    EXPECT_EQ(reads_at(scheduler, plan, {0, MILLISECOND, 2 * MILLISECOND}),
              (std::vector<std::uint64_t>{0, 100, 50}));
}

TEST(Scheduling, ANewOrderGoesBeforeTheRestOfAChainWhichStartsAgain) {
    // The lead's ruling R2: a device paced at 10 s, a chain of three
    // requests (a value at 0, companions at 100 and 200), an order queued
    // after the chain's first request: the order goes at the first free
    // slot (10 s), not after the chain (30 s); the chain is dropped and read
    // again from its first request.
    const CompiledChannel channel =
        make_channel({{.name = "d1",
                       .points = {{.first = 0, .companions = {1, 2}},
                                  {.first = 100},
                                  {.first = 200}},
                       .pacing = {.min_request_interval_ns = 10 * SECOND}}},
                     ReadRules{.max_units = 4});
    const ReadPlan& plan = channel.devices[0].plan;
    ASSERT_EQ(plan.by_group[FAST].size(), 3U);
    Scheduler scheduler(channel, 4, 0);
    const Choice first = scheduler.take(0);
    ASSERT_EQ(first.kind, Choice::Kind::POLL);
    EXPECT_EQ(plan.requests[first.request].first, 0U);
    ASSERT_TRUE(scheduler.add_order(OrderTicket{.id = 1, .device = 0}));
    const Choice wait = scheduler.take(MILLISECOND);
    ASSERT_EQ(wait.kind, Choice::Kind::WAIT);
    EXPECT_EQ(wait.wake_ns, 10 * SECOND);
    const Choice order = scheduler.take(10 * SECOND);
    ASSERT_EQ(order.kind, Choice::Kind::ORDER);
    EXPECT_TRUE(order.chain_dropped);
    EXPECT_EQ(
        reads_at(scheduler, plan, {20 * SECOND, 30 * SECOND, 40 * SECOND}),
        (std::vector<std::uint64_t>{0, 100, 200}));
}

TEST(Scheduling, AnExclusiveOrderHoldsItsDevice) {
    // A select and operate on device 1 (request interval 5 ms, write
    // interval 10 ms): no poll of device 1 between its steps; device 2 is
    // served meanwhile. The lead's ruling R1: the operate follows within
    // the request interval only (at 5 ms, not 10 ms); the write interval
    // applies after it.
    const CompiledChannel channel =
        make_channel({{.name = "d1",
                       .unit_id = 1,
                       .points = adjacent(0, 1),
                       .pacing = {.min_request_interval_ns = 5 * MILLISECOND,
                                  .min_write_interval_ns = 10 * MILLISECOND}},
                      {.name = "d2", .unit_id = 2, .points = adjacent(0, 1)}},
                     ReadRules{.max_units = LARGE});
    Scheduler scheduler(channel, 4, 0);
    ASSERT_TRUE(scheduler.add_order(OrderTicket{
        .id = 9, .device = 0, .kind = OrderKind::SELECT_OPERATE, .steps = 2}));
    const Choice select = scheduler.take(0);
    ASSERT_EQ(select.kind, Choice::Kind::ORDER);
    EXPECT_EQ(select.step, 0U);
    const Choice other = scheduler.take(MILLISECOND);
    EXPECT_EQ(other.kind, Choice::Kind::POLL);
    EXPECT_EQ(other.device, 1U);  // device 2; device 1 is held
    const Choice wait = scheduler.take(2 * MILLISECOND);
    ASSERT_EQ(wait.kind, Choice::Kind::WAIT);
    EXPECT_EQ(wait.wake_ns, 5 * MILLISECOND);
    const Choice operate = scheduler.take(5 * MILLISECOND);
    ASSERT_EQ(operate.kind, Choice::Kind::ORDER);
    EXPECT_EQ(operate.step, 1U);
    // A write after the operate keeps the write interval from it: 15 ms.
    ASSERT_TRUE(scheduler.add_order(OrderTicket{.id = 10, .device = 0}));
    const Choice poll = scheduler.take(10 * MILLISECOND);
    EXPECT_EQ(poll.kind, Choice::Kind::POLL);  // its poll, at last
    EXPECT_EQ(poll.device, 0U);
    EXPECT_EQ(scheduler.take(11 * MILLISECOND).wake_ns, 15 * MILLISECOND);
    EXPECT_EQ(scheduler.take(15 * MILLISECOND).kind, Choice::Kind::ORDER);
}

TEST(Scheduling, AListOfWritesHoldsItsUnitAndKeepsTheWriteInterval) {
    // D1: a list of writes (8.5) is an exclusive sequence, whatever its
    // ticket says: no poll of its unit between its writes, which keep the
    // write interval (R1).
    const CompiledChannel channel =
        make_channel({{.name = "d1",
                       .points = adjacent(0, 1),
                       .pacing = {.min_write_interval_ns = 10 * MILLISECOND}}},
                     ReadRules{.max_units = LARGE});
    Scheduler scheduler(channel, 4, 0);
    ASSERT_TRUE(scheduler.add_order(OrderTicket{
        .id = 1, .device = 0, .kind = OrderKind::WRITE_LIST, .steps = 2}));
    EXPECT_EQ(scheduler.take(0).kind, Choice::Kind::ORDER);
    const Choice wait = scheduler.take(MILLISECOND);  // the poll is due
    EXPECT_EQ(wait.kind, Choice::Kind::WAIT);
    EXPECT_EQ(wait.wake_ns, 10 * MILLISECOND);
    EXPECT_EQ(scheduler.take(10 * MILLISECOND).step, 1U);
    EXPECT_EQ(scheduler.take(11 * MILLISECOND).kind, Choice::Kind::POLL);
}

TEST(Scheduling, NoOrderStepBetweenTheRequestsOfAChain) {
    // D2: write interval 10 ms, a chain of two requests in a 100 ms group,
    // a write at 95 ms and another queued at 100 ms. The second write is
    // chosen between the chain's requests: it drops the chain, which is
    // read again whole after it.
    const CompiledChannel channel = make_channel(
        {{.name = "d1",
          .points = {{.first = 0, .companions = {1}}, {.first = 100}},
          .pacing = {.min_write_interval_ns = 10 * MILLISECOND}}},
        ReadRules{.max_units = 4}, Pacing{},
        {{.name = "g", .period_ns = 100 * MILLISECOND, .priority = 1}});
    const ReadPlan& plan = channel.devices[0].plan;
    Scheduler scheduler(channel, 4, 0);
    EXPECT_EQ(reads_at(scheduler, plan, {0, MILLISECOND}),
              (std::vector<std::uint64_t>{0, 100}));
    ASSERT_TRUE(scheduler.add_order(OrderTicket{.id = 1, .device = 0}));
    EXPECT_EQ(scheduler.take(95 * MILLISECOND).kind, Choice::Kind::ORDER);
    EXPECT_EQ(reads_at(scheduler, plan, {100 * MILLISECOND}),
              (std::vector<std::uint64_t>{0}));
    ASSERT_TRUE(scheduler.add_order(OrderTicket{.id = 2, .device = 0}));
    const Choice order = scheduler.take(105 * MILLISECOND);
    ASSERT_EQ(order.kind, Choice::Kind::ORDER);
    EXPECT_TRUE(order.chain_dropped);
    EXPECT_EQ(order.chain_device, 0U);
    EXPECT_EQ(reads_at(scheduler, plan, {106 * MILLISECOND, 107 * MILLISECOND}),
              (std::vector<std::uint64_t>{0, 100}));
}

TEST(Scheduling, ACancelIsNotHeldByTheWriteInterval) {
    // R5: write interval 10 s; a select at 0, aborted; a cancel queued at
    // 1 ms goes at once.
    const CompiledChannel channel =
        make_channel({{.name = "d1",
                       .points = adjacent(0, 1, SLOW),
                       .pacing = {.min_write_interval_ns = 10 * SECOND}}},
                     ReadRules{.max_units = LARGE});
    Scheduler scheduler(channel, 4, 0);
    ASSERT_TRUE(scheduler.add_order(OrderTicket{
        .id = 1, .device = 0, .kind = OrderKind::SELECT_OPERATE, .steps = 2}));
    EXPECT_EQ(scheduler.take(0).kind, Choice::Kind::ORDER);
    scheduler.abort_order(0);
    ASSERT_TRUE(scheduler.add_order(
        OrderTicket{.id = 2, .device = 0, .kind = OrderKind::CANCEL}));
    const Choice cancel = scheduler.take(MILLISECOND);
    EXPECT_EQ(cancel.kind, Choice::Kind::ORDER);
    EXPECT_EQ(cancel.order, 2U);
}

TEST(Scheduling, TheChannelsWriteIntervalDoesNotHoldTheOperate) {
    // R6: a gateway's write interval of 10 s on the channel: the operate
    // still follows its select at the next free slot.
    const CompiledChannel channel =
        make_channel({{.name = "d1", .points = adjacent(0, 1, SLOW)}},
                     ReadRules{.max_units = LARGE},
                     Pacing{.min_write_interval_ns = 10 * SECOND});
    Scheduler scheduler(channel, 4, 0);
    ASSERT_TRUE(scheduler.add_order(OrderTicket{
        .id = 1, .device = 0, .kind = OrderKind::SELECT_OPERATE, .steps = 2}));
    EXPECT_EQ(scheduler.take(0).step, 0U);
    const Choice operate = scheduler.take(MILLISECOND);
    EXPECT_EQ(operate.kind, Choice::Kind::ORDER);
    EXPECT_EQ(operate.step, 1U);
}

TEST(Scheduling, AnOrderToASiblingNamesTheChainItDrops) {
    // E2: devices 0 and 1 on one unit; device 0's chain is open when an
    // order to device 1 goes: the choice names device 0 as the chain's.
    const CompiledChannel channel = make_channel(
        {{.name = "a",
          .unit_id = 7,
          .points = {{.first = 0, .companions = {1}}, {.first = 100}}},
         {.name = "b", .unit_id = 7, .points = adjacent(50, 1, SLOW)}},
        ReadRules{.max_units = 4});
    Scheduler scheduler(channel, 4, 0);
    const Choice first = scheduler.take(0);
    ASSERT_EQ(first.device, 0U);
    ASSERT_TRUE(scheduler.add_order(OrderTicket{.id = 1, .device = 1}));
    const Choice order = scheduler.take(MILLISECOND);
    ASSERT_EQ(order.kind, Choice::Kind::ORDER);
    EXPECT_EQ(order.device, 1U);
    EXPECT_TRUE(order.chain_dropped);
    EXPECT_EQ(order.chain_device, 0U);
}

TEST(Scheduling, DevicesWithoutAUnitKeyAreUnitsOfTheirOwn) {
    // E3: two devices built without a unit key do not share pacing.
    CompiledChannel channel =
        make_channel({{.name = "a",
                       .points = adjacent(0, 1),
                       .pacing = {.min_request_interval_ns = 10 * SECOND}},
                      {.name = "b", .points = adjacent(0, 1)}},
                     ReadRules{.max_units = LARGE});
    for (CompiledDevice& device : channel.devices) {
        device.unit = CompiledDevice::OWN_UNIT;
    }
    Scheduler scheduler(channel, 4, 0);
    EXPECT_EQ(scheduler.take(0).device, 0U);
    EXPECT_EQ(scheduler.take(MILLISECOND).device, 1U);
    EXPECT_EQ(scheduler.take(FAST_PERIOD).device, 1U);  // not every 10 s
}

TEST(Scheduling, PacingLimitedCountsFromTheLastRequestHandedOut) {
    // E4: interval 10 ms, two requests in a 100 ms group, the last answer
    // taking 140 ms: the cycle's requests went at 0 and 10 ms, within the
    // period, so the device is not pacing-limited.
    const CompiledChannel channel = make_channel(
        {{.name = "d1",
          .points = adjacent(0, 2, NORMAL),
          .pacing = {.min_request_interval_ns = 10 * MILLISECOND}}},
        ReadRules{.max_units = 1}, Pacing{},
        {{.name = "g", .period_ns = 100 * MILLISECOND, .priority = 1},
         {.name = "n", .period_ns = SECOND, .priority = 2},
         {.name = "s", .period_ns = SLOW_PERIOD, .priority = 3}});
    Scheduler scheduler(channel, 4, 0);
    EXPECT_EQ(scheduler.take(0).kind, Choice::Kind::POLL);
    EXPECT_EQ(scheduler.take(MILLISECOND).wake_ns, 10 * MILLISECOND);
    EXPECT_EQ(scheduler.take(10 * MILLISECOND).kind, Choice::Kind::POLL);
    // Between the last send and the `take` after the slow answer, too (the
    // PR 135 final pass): false at 100, 101 and 149 ms.
    EXPECT_FALSE(scheduler.pacing_limited(0, 100 * MILLISECOND));
    EXPECT_FALSE(scheduler.pacing_limited(0, 101 * MILLISECOND));
    EXPECT_FALSE(scheduler.pacing_limited(0, 149 * MILLISECOND));
    (void)scheduler.take(150 * MILLISECOND);  // after the slow answer
    EXPECT_FALSE(scheduler.pacing_limited(0, 150 * MILLISECOND));
}

TEST(Scheduling, ReconnectAndNotSentNameTheOrdersTheyEnd) {
    // E10: the worker answers each command whose order ended.
    const CompiledChannel channel = make_channel(
        {{.name = "a", .unit_id = 1, .points = adjacent(0, 1, SLOW)},
         {.name = "b", .unit_id = 2, .points = adjacent(0, 1, SLOW)}},
        ReadRules{.max_units = LARGE});
    Scheduler scheduler(channel, 4, 0);
    ASSERT_TRUE(scheduler.add_order(OrderTicket{
        .id = 5, .device = 0, .kind = OrderKind::SELECT_OPERATE, .steps = 2}));
    EXPECT_EQ(scheduler.take(SLOW_PERIOD).order, 5U);
    std::array<std::uint64_t, 2> ended{};
    EXPECT_EQ(scheduler.reconnect(ended), 1U);
    EXPECT_EQ(ended[0], 5U);
    ASSERT_TRUE(scheduler.add_order(OrderTicket{.id = 6, .device = 1}));
    EXPECT_EQ(scheduler.take(SLOW_PERIOD + MILLISECOND).order, 6U);
    EXPECT_EQ(scheduler.not_sent(), std::optional<std::uint64_t>{6});
    EXPECT_EQ(scheduler.not_sent(), std::nullopt);  // only right after take
}

TEST(Scheduling, ANegativeOrHugeClockDoesNotOverflow) {
    // E9: the published period saturates rather than overflow.
    const CompiledChannel channel =
        make_channel({{.name = "d1", .points = adjacent(0, 1)}},
                     ReadRules{.max_units = LARGE});
    constexpr std::int64_t EARLY = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t LATE = std::numeric_limits<std::int64_t>::max();
    Scheduler scheduler(channel, 4, EARLY);
    EXPECT_EQ(scheduler.take(EARLY).kind, Choice::Kind::POLL);
    EXPECT_EQ(scheduler.achieved_period_ns(FAST, LATE), LATE);
    EXPECT_EQ(scheduler.achieved_period_ns(FAST, EARLY), 0);
}

TEST(Scheduling, AReadNowNeitherWaitsForNorTakesTheWriteInterval) {
    // F2: after a write, a READ_NOW goes at once, and a write after it
    // still counts its interval from the first write.
    const CompiledChannel channel =
        make_channel({{.name = "d1",
                       .points = adjacent(0, 1, SLOW),
                       .pacing = {.min_write_interval_ns = 10 * MILLISECOND}}},
                     ReadRules{.max_units = LARGE});
    Scheduler scheduler(channel, 4, 0);
    EXPECT_EQ(scheduler.take(0).kind, Choice::Kind::POLL);
    ASSERT_TRUE(scheduler.add_order(OrderTicket{.id = 1, .device = 0}));
    ASSERT_TRUE(scheduler.add_order(
        OrderTicket{.id = 2, .device = 0, .kind = OrderKind::READ_NOW}));
    ASSERT_TRUE(scheduler.add_order(OrderTicket{.id = 3, .device = 0}));
    EXPECT_EQ(scheduler.take(MILLISECOND).order, 1U);
    EXPECT_EQ(scheduler.take(2 * MILLISECOND).order, 2U);
    EXPECT_EQ(scheduler.take(3 * MILLISECOND).wake_ns, 11 * MILLISECOND);
    EXPECT_EQ(scheduler.take(11 * MILLISECOND).order, 3U);
}

TEST(Scheduling, DevicesOfOneUnitShareItsPacingAndExclusivity) {
    // R3: two devices on unit 7 (request interval 10 ms on one of them) and
    // one on unit 8. The interval holds across the two of unit 7, and an
    // exclusive order on one holds the other.
    const CompiledChannel channel =
        make_channel({{.name = "a",
                       .unit_id = 7,
                       .points = adjacent(0, 1),
                       .pacing = {.min_request_interval_ns = 10 * MILLISECOND}},
                      {.name = "b", .unit_id = 7, .points = adjacent(10, 1)},
                      {.name = "c", .unit_id = 8, .points = adjacent(0, 1)}},
                     ReadRules{.max_units = LARGE});
    Scheduler scheduler(channel, 4, 0);
    EXPECT_EQ(scheduler.take(0).device, 0U);
    EXPECT_EQ(scheduler.take(MILLISECOND).device, 2U);  // b waits
    EXPECT_EQ(scheduler.take(2 * MILLISECOND).wake_ns, 10 * MILLISECOND);
    EXPECT_EQ(scheduler.take(10 * MILLISECOND).device, 1U);
    ASSERT_TRUE(scheduler.add_order(OrderTicket{
        .id = 1, .device = 1, .kind = OrderKind::SELECT_OPERATE, .steps = 2}));
    EXPECT_EQ(scheduler.take(FAST_PERIOD).kind, Choice::Kind::ORDER);
    // Device a is due too, but b's selection holds the unit: c goes.
    EXPECT_EQ(scheduler.take(FAST_PERIOD + MILLISECOND).device, 2U);
    const Choice operate = scheduler.take(FAST_PERIOD + 10 * MILLISECOND);
    EXPECT_EQ(operate.kind, Choice::Kind::ORDER);
    EXPECT_EQ(operate.device, 1U);
    EXPECT_EQ(scheduler.take(FAST_PERIOD + 20 * MILLISECOND).device, 0U);
}

TEST(Scheduling, TheOrderQueueIsBoundedAndANewOrderIsRefusedWhenFull) {
    const CompiledChannel channel =
        make_channel({{.name = "d1", .points = adjacent(0, 1)}},
                     ReadRules{.max_units = LARGE});
    Scheduler scheduler(channel, 2, 0);
    EXPECT_TRUE(scheduler.add_order(OrderTicket{.id = 1}));
    EXPECT_TRUE(scheduler.add_order(OrderTicket{.id = 2}));
    EXPECT_FALSE(scheduler.add_order(OrderTicket{.id = 3}));
    EXPECT_FALSE(scheduler.add_order(OrderTicket{.id = 4, .device = 5}));
    EXPECT_FALSE(scheduler.add_order(OrderTicket{.id = 5, .steps = 0}));
    EXPECT_EQ(scheduler.pending_orders(), 2U);
}

TEST(Scheduling, ADisabledDeviceIsNeverPolledAndAChannelWithoutWorkIsIdle) {
    const CompiledChannel channel = make_channel(
        {{.name = "d1", .points = adjacent(0, 1), .enabled = false}},
        ReadRules{.max_units = LARGE});
    Scheduler scheduler(channel, 4, 0);
    EXPECT_EQ(scheduler.take(0).kind, Choice::Kind::IDLE);
}

// --- Committee review of PR 135: K1, K2, K4, F1, F8 -----------------------

TEST(Scheduling, ARefusedKeepAliveUsesItsSplitFormAndLeavesTheCycle) {
    // K1: a 10 s group; r0 = [0..1] (two clusters: it has a split form),
    // r1 = [50] side-effect; the keep-alive (2 s) is r0. Its refusal takes
    // the split form without rewinding the cycle: the side-effect range is
    // not read again early, the next cycle is still due at 10 s, and the
    // next keep-alive is the split form's first part.
    ReadRules rules{.max_units = LARGE};
    rules.ranges = {ReadRange{.kind = ReadRange::Kind::SIDE_EFFECT,
                              .unit_id = 1,
                              .first = 50,
                              .last = 50}};
    const CompiledChannel channel =
        make_channel({{.name = "d1",
                       .points = {{.first = 0, .group = SLOW},
                                  {.first = 1, .group = SLOW},
                                  {.first = 50, .group = SLOW}},
                       .keepalive_ns = 2 * SECOND}},
                     rules);
    const ReadPlan& plan = channel.devices[0].plan;
    ASSERT_TRUE(plan.keepalive.has_value());
    const std::uint32_t merged = plan.keepalive.value_or(0);
    ASSERT_EQ(plan.requests[merged].split.size(), 2U);
    EXPECT_EQ(plan.requests[merged].first, 0U);
    Scheduler scheduler(channel, 4, 0);
    // The cycle (0, 50), then the keep-alive of [0..1] at 2.001 s.
    EXPECT_EQ(
        reads_at(scheduler, plan, {0, MILLISECOND, (2 * SECOND) + MILLISECOND}),
        (std::vector<std::uint64_t>{0, 50, 0}));
    EXPECT_TRUE(scheduler.refused(0, merged));
    const Choice wait = scheduler.take((2 * SECOND) + (2 * MILLISECOND));
    EXPECT_EQ(wait.kind, Choice::Kind::WAIT);
    EXPECT_EQ(wait.wake_ns, (4 * SECOND) + MILLISECOND);
    // The next keep-alive is the split form's first part [0]; the next
    // cycle, at 10 s, reads [0], [1], then the side-effect range once.
    EXPECT_EQ(reads_at(scheduler, plan,
                       {(4 * SECOND) + MILLISECOND, 10 * SECOND,
                        (10 * SECOND) + MILLISECOND,
                        (10 * SECOND) + (2 * MILLISECOND)}),
              (std::vector<std::uint64_t>{0, 0, 1, 50}));
    const Choice part = scheduler.take(10 * SECOND + 3 * MILLISECOND);
    EXPECT_EQ(part.kind, Choice::Kind::WAIT);
}

TEST(Scheduling, AChainWhoseLastRequestIsRefusedEndsWithItsCycle) {
    // K2: a 1 s group, a value at 0 with its companion at 100 (a chain of
    // two); the companion's request is refused for good. The channel is not
    // idle for ever: the group stays due every second.
    const CompiledChannel channel = make_channel(
        {{.name = "d1",
          .points = {{.first = 0, .group = NORMAL, .companions = {1}},
                     {.first = 100, .group = NORMAL}}}},
        ReadRules{.max_units = 4});
    Scheduler scheduler(channel, 4, 0);
    EXPECT_EQ(scheduler.take(0).kind, Choice::Kind::POLL);
    const Choice companion = scheduler.take(MILLISECOND);
    EXPECT_FALSE(scheduler.refused(0, companion.request));
    EXPECT_EQ(scheduler.take(SECOND).kind, Choice::Kind::POLL);
    const Choice wait = scheduler.take(SECOND + MILLISECOND);
    ASSERT_EQ(wait.kind, Choice::Kind::WAIT);
    EXPECT_EQ(wait.wake_ns, 2 * SECOND);
    EXPECT_EQ(scheduler.take(2 * SECOND).kind, Choice::Kind::POLL);
}

TEST(Scheduling, AStarvedGroupPublishesTheAgeOfItsRunningCycle) {
    // K4 (b): the fast group of 4 requests at 100 ms pacing never leaves the
    // device idle, so the slow group of the same device is never read. Its
    // achieved period is the age of its running cycle, not 0: strict
    // priority starves it, and the published period shows it.
    const CompiledChannel channel = make_channel(
        {{.name = "d1",
          .points = {{.first = 0},
                     {.first = 1},
                     {.first = 2},
                     {.first = 3},
                     {.first = 50, .group = SLOW}},
          .pacing = {.min_request_interval_ns = 100 * MILLISECOND}}},
        ReadRules{.max_units = 1});
    SimBus bus = mapped_bus();
    const TestProtocol protocol;
    Loop loop(channel, protocol, bus, 0, EXCHANGE);
    loop.run_until(60 * SECOND);
    EXPECT_GE(loop.scheduler().achieved_period_ns(SLOW, loop.now()),
              59 * SECOND);
    EXPECT_TRUE(loop.scheduler().pacing_limited(0, loop.now()));
}

TEST(Scheduling, AMetPeriodIsNotPacingLimited) {
    // K4 (c): two requests 50 ms apart in a 1 s group meet the period: they
    // wait for the interval, but the device is not pacing-limited, and the
    // group's period is 1 s.
    const CompiledChannel channel = make_channel(
        {{.name = "d1",
          .points = adjacent(0, 2, NORMAL),
          .pacing = {.min_request_interval_ns = 50 * MILLISECOND}}},
        ReadRules{.max_units = 1});
    SimBus bus = mapped_bus();
    const TestProtocol protocol;
    Loop loop(channel, protocol, bus, 0, EXCHANGE);
    loop.run_until(12 * SECOND);
    EXPECT_FALSE(loop.scheduler().pacing_limited(0, loop.now()));
    EXPECT_EQ(loop.scheduler().achieved_period_ns(NORMAL, loop.now()), SECOND);
}

TEST(Scheduling, OnePeriodLateAmongTenRaisesTheAverage) {
    // K4 (a): a 1 s group whose worker stalls once for 2 s: the published
    // period is the average of the last ten periods (start to start), which
    // the late one raises, not the last period alone.
    const CompiledChannel channel =
        make_channel({{.name = "d1", .points = adjacent(0, 1, NORMAL)}},
                     ReadRules{.max_units = LARGE});
    Scheduler scheduler(channel, 4, 0);
    const std::vector<std::int64_t> starts =
        cycle_starts(scheduler, 13, 4 * SECOND, 6 * SECOND);
    ASSERT_EQ(starts.size(), 13U);
    const std::int64_t now = starts.back() + MILLISECOND;
    (void)scheduler.take(now);  // the last cycle completes
    // The last ten periods end at the last start.
    const std::int64_t expected =
        (starts.back() - starts[starts.size() - 11]) / 10;
    EXPECT_GT(expected, SECOND);
    const std::int64_t last_period = starts.back() - starts[starts.size() - 2];
    EXPECT_LE(last_period, SECOND);
    EXPECT_EQ(scheduler.achieved_period_ns(NORMAL, now), expected);
}

TEST(Scheduling, AnOrderCanBeEndedRemovedOrNotSent) {
    // F1: a list of three writes aborted after its first; a queued order
    // removed; a select whose connection is lost is not followed by its
    // operate; a step not sent leaves the pacing as it was.
    const CompiledChannel channel =
        make_channel({{.name = "d1",
                       .points = adjacent(0, 1, SLOW),
                       .pacing = {.min_write_interval_ns = 10 * MILLISECOND}}},
                     ReadRules{.max_units = LARGE});
    Scheduler scheduler(channel, 4, 0);
    EXPECT_EQ(scheduler.take(0).kind, Choice::Kind::POLL);
    ASSERT_TRUE(scheduler.add_order(OrderTicket{
        .id = 1, .device = 0, .kind = OrderKind::WRITE_LIST, .steps = 3}));
    EXPECT_EQ(scheduler.take(MILLISECOND).order, 1U);
    scheduler.abort_order(0);
    EXPECT_EQ(scheduler.take(20 * MILLISECOND).kind, Choice::Kind::WAIT);
    ASSERT_TRUE(scheduler.add_order(OrderTicket{.id = 2, .device = 0}));
    EXPECT_TRUE(scheduler.remove_order(2));
    EXPECT_FALSE(scheduler.remove_order(2));
    EXPECT_EQ(scheduler.pending_orders(), 0U);
    ASSERT_TRUE(scheduler.add_order(OrderTicket{
        .id = 3, .device = 0, .kind = OrderKind::SELECT_OPERATE, .steps = 2}));
    EXPECT_EQ(scheduler.take(30 * MILLISECOND).step, 0U);
    scheduler.reconnect();
    EXPECT_EQ(scheduler.take(41 * MILLISECOND).kind, Choice::Kind::WAIT);
    // A write chosen at 50 ms and not sent: the next write may go at once.
    ASSERT_TRUE(scheduler.add_order(OrderTicket{.id = 4, .device = 0}));
    ASSERT_TRUE(scheduler.add_order(OrderTicket{.id = 5, .device = 0}));
    EXPECT_EQ(scheduler.take(50 * MILLISECOND).order, 4U);
    scheduler.not_sent();
    EXPECT_EQ(scheduler.take(51 * MILLISECOND).order, 5U);
    // A poll not sent is chosen again.
    const Choice poll = scheduler.take(SLOW_PERIOD);
    ASSERT_EQ(poll.kind, Choice::Kind::POLL);
    scheduler.not_sent();
    EXPECT_EQ(scheduler.take(SLOW_PERIOD).request, poll.request);
}

TEST(Scheduling, AMalformedOrderIsRefused) {
    const CompiledChannel channel =
        make_channel({{.name = "d1", .points = adjacent(0, 1)}},
                     ReadRules{.max_units = LARGE});
    Scheduler scheduler(channel, 4, 0);
    EXPECT_FALSE(scheduler.add_order(OrderTicket{.id = 1, .steps = 2}));
    EXPECT_FALSE(scheduler.add_order(
        OrderTicket{.id = 2, .kind = OrderKind::READ_NOW, .steps = 2}));
    EXPECT_FALSE(scheduler.add_order(
        OrderTicket{.id = 4, .kind = OrderKind::SELECT_OPERATE, .steps = 1}));
    EXPECT_FALSE(scheduler.add_order(
        OrderTicket{.id = 5, .kind = OrderKind::SELECT_OPERATE, .steps = 3}));
    EXPECT_TRUE(scheduler.add_order(
        OrderTicket{.id = 6, .kind = OrderKind::WRITE_LIST, .steps = 2}));
    // E8: an order to a disabled device.
    const CompiledChannel disabled = make_channel(
        {{.name = "d1", .points = adjacent(0, 1), .enabled = false}},
        ReadRules{.max_units = LARGE});
    Scheduler idle(disabled, 4, 0);
    EXPECT_FALSE(idle.add_order(OrderTicket{.id = 7}));
}

TEST(Scheduling, AChannelTheSchedulerCannotRunIsRefusedOnce) {
    // F8: checked when the scheduler is built, never in `take`.
    CompiledChannel unsized = make_channel(
        {{.name = "d1", .points = adjacent(0, 1)}}, ReadRules{.max_units = 4});
    unsized.devices[0].plan.by_group.pop_back();
    Scheduler first(unsized, 4, 0);
    EXPECT_EQ(first.status().code(), StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(first.take(0).kind, Choice::Kind::IDLE);
    EXPECT_FALSE(first.add_order(OrderTicket{.id = 1}));
    EXPECT_FALSE(first.refused(0, 0));
    CompiledChannel negative = make_channel(
        {{.name = "d1", .points = adjacent(0, 1)}}, ReadRules{.max_units = 4});
    negative.poll_groups[0].period_ns = -1;
    EXPECT_FALSE(Scheduler::check(negative).ok());
    CompiledChannel stray = make_channel(
        {{.name = "d1", .points = adjacent(0, 1)}}, ReadRules{.max_units = 4});
    stray.devices[0].plan.by_group[0].push_back(99);
    EXPECT_FALSE(Scheduler::check(stray).ok());
    // E5: a request of a poll group the channel lacks, a split part with a
    // split form of its own, and a disabled device are all checked.
    CompiledChannel group = make_channel(
        {{.name = "d1", .points = adjacent(0, 2), .enabled = false}},
        ReadRules{.max_units = 4});
    group.devices[0].plan.requests.push_back(Request{.poll_group = 9});
    EXPECT_FALSE(Scheduler::check(group).ok());
    CompiledChannel nested = make_channel(
        {{.name = "d1", .points = adjacent(0, 2)}}, ReadRules{.max_units = 4});
    ReadPlan& plan = nested.devices[0].plan;
    plan.requests.push_back(Request{});
    plan.requests.push_back(Request{});
    plan.requests[0].split = {1, 2};
    plan.requests[1].split = {2};
    EXPECT_FALSE(Scheduler::check(nested).ok());
    // A protocol's own plan whose keep-alive is a side-effect read or a
    // request of a chain (the PR 135 final pass).
    ReadRules side{.max_units = 4};
    side.ranges = {ReadRange{.kind = ReadRange::Kind::SIDE_EFFECT,
                             .unit_id = 1,
                             .first = 0,
                             .last = 0}};
    CompiledChannel latch =
        make_channel({{.name = "d1", .points = adjacent(0, 1)}}, side);
    latch.devices[0].plan.keepalive = latch.devices[0].plan.by_group[0][0];
    EXPECT_FALSE(Scheduler::check(latch).ok());
    CompiledChannel chained = make_channel(
        {{.name = "d1",
          .points = {{.first = 0, .companions = {1}}, {.first = 100}}}},
        ReadRules{.max_units = 4});
    ReadPlan& chain_plan = chained.devices[0].plan;
    chain_plan.keepalive = chain_plan.by_group[0][0];
    EXPECT_FALSE(Scheduler::check(chained).ok());
    chain_plan.keepalive = chain_plan.by_group[0][1];  // the chain's end
    EXPECT_FALSE(Scheduler::check(chained).ok());
    const CompiledChannel good = make_channel(
        {{.name = "d1", .points = adjacent(0, 1)}}, ReadRules{.max_units = 4});
    Scheduler scheduler(good, 4, 0);
    EXPECT_TRUE(scheduler.status().ok());
    EXPECT_FALSE(scheduler.refused(5, 0));  // no such device
}

TEST(Scheduling, AWriteReachesEncodeAsARequestOfKindWrite) {
    // F4: a write is a Request of kind WRITE with its raw payload; the
    // attempt carries the framing and a protocol area. The test protocol
    // reads only, so it refuses the write and leaves the attempt as given.
    const CompiledChannel channel = make_channel(
        {{.name = "d1", .points = adjacent(0, 1)}}, ReadRules{.max_units = 4});
    const TestProtocol protocol;
    ChannelState state;
    const std::array<std::byte, 2> value{std::byte{0x12}, std::byte{0x34}};
    Request write{.kind = Request::Kind::WRITE, .first = 0, .count = 1};
    write.points = {0};
    write.payload = value;
    std::array<std::byte, 64> bytes{};
    TxBuffer out{.bytes = bytes};
    Attempt attempt{.framing = Framing::SERIAL};
    const EncodeResult result =
        protocol.encode(channel.devices[0], state, write, attempt, out);
    EXPECT_EQ(result.kind, EncodeResult::Kind::REFUSED);
    EXPECT_EQ(attempt.framing, Framing::SERIAL);
    EXPECT_EQ(attempt.protocol.size(), ATTEMPT_PROTOCOL_BYTES);
    // E9: what encode records for the answer is copied in and out as bytes.
    const std::uint32_t echo = 0x0610'0002;  // invented function and address
    const auto bytes_in =
        std::bit_cast<std::array<std::byte, sizeof echo>>(echo);
    std::copy(bytes_in.begin(), bytes_in.end(), attempt.protocol.begin());
    const Attempt copy = attempt;
    std::array<std::byte, sizeof echo> out_bytes{};
    std::copy_n(copy.protocol.begin(), out_bytes.size(), out_bytes.begin());
    EXPECT_EQ(std::bit_cast<std::uint32_t>(out_bytes), echo);
}

// --- Failure handling: not polled, retries, probes (6.2, 7.1, 7.4) -------

TEST(Scheduling, ADeviceThatIsNotPolledGetsNoPollAndNoKeepAlive) {
    const CompiledChannel channel = make_channel(
        {{.name = "d1", .points = adjacent(0, 1), .keepalive_ns = SECOND}},
        ReadRules{.max_units = LARGE});
    Scheduler scheduler(channel, 4, 0);
    scheduler.set_polled(0, false);
    EXPECT_EQ(scheduler.take(0).kind, Choice::Kind::IDLE);
    EXPECT_EQ(scheduler.take(5 * SECOND).kind, Choice::Kind::IDLE);
    // Polled again: its groups are due at once.
    scheduler.set_polled(0, true);
    const Choice choice = scheduler.take(6 * SECOND);
    EXPECT_EQ(choice.kind, Choice::Kind::POLL);
    EXPECT_EQ(choice.request, channel.devices[0].plan.by_group[FAST].front());
    // The outage is not an achieved period.
    EXPECT_LT(scheduler.achieved_period_ns(FAST, 6 * SECOND + MILLISECOND),
              FAST_PERIOD);
}

TEST(Scheduling, ARetryIsTheDevicesNextRequestWithinItsSpacing) {
    // Two devices, 10 ms request spacing on d1: d1's failed request is sent
    // again before anything else of d1, 10 ms after the first attempt, and
    // d2 is served in between.
    const CompiledChannel channel = make_channel(
        {{.name = "d1",
          .unit_id = 1,
          .points = adjacent(0, 1),
          .pacing = Pacing{.min_request_interval_ns = 10 * MILLISECOND}},
         {.name = "d2", .unit_id = 2, .points = adjacent(0, 1)}},
        ReadRules{.max_units = LARGE});
    Scheduler scheduler(channel, 4, 0);
    const Choice first = scheduler.take(0);
    ASSERT_EQ(first.kind, Choice::Kind::POLL);
    ASSERT_EQ(first.device, 0U);
    scheduler.retry(0, first.request, 0);
    const Choice other = scheduler.take(MILLISECOND);
    EXPECT_EQ(other.kind, Choice::Kind::POLL);
    EXPECT_EQ(other.device, 1U);
    const Choice wait = scheduler.take(2 * MILLISECOND);
    EXPECT_EQ(wait.kind, Choice::Kind::WAIT);
    EXPECT_EQ(wait.wake_ns, 10 * MILLISECOND);
    const Choice again = scheduler.take(10 * MILLISECOND);
    EXPECT_EQ(again.kind, Choice::Kind::POLL);
    EXPECT_EQ(again.device, 0U);
    EXPECT_EQ(again.request, first.request);
    // Once only: the cycle is complete, the next poll is a period later.
    EXPECT_EQ(scheduler.take(11 * MILLISECOND).wake_ns, FAST_PERIOD);
}

TEST(Scheduling, ASideEffectReadIsNeverRetried) {
    ReadRules rules{.max_units = LARGE};
    rules.ranges = {ReadRange{.kind = ReadRange::Kind::SIDE_EFFECT,
                              .unit_id = 1,
                              .first = 0,
                              .last = 0}};
    const CompiledChannel channel =
        make_channel({{.name = "d1", .points = adjacent(0, 1)}}, rules);
    Scheduler scheduler(channel, 4, 0);
    const Choice first = scheduler.take(0);
    ASSERT_EQ(first.kind, Choice::Kind::POLL);
    ASSERT_TRUE(channel.devices[0].plan.requests[first.request].side_effect);
    scheduler.retry(0, first.request, 0);
    EXPECT_EQ(scheduler.take(MILLISECOND).kind, Choice::Kind::WAIT);
}

TEST(Scheduling, AnOrderThatDropsAChainDropsItsRetryToo) {
    // A chain's first request failed and is to be retried; an order goes
    // first (R2, D2) and drops the chain: it is read again whole, once.
    const CompiledChannel channel = make_channel(
        {{.name = "d1",
          .points = {{.first = 0, .companions = {1}}, {.first = 100}}}},
        ReadRules{.max_units = 4});
    const ReadPlan& plan = channel.devices[0].plan;
    Scheduler scheduler(channel, 4, 0);
    const Choice first = scheduler.take(0);
    scheduler.retry(0, first.request, 0);
    ASSERT_TRUE(scheduler.add_order(OrderTicket{.id = 1, .device = 0}));
    EXPECT_TRUE(scheduler.take(MILLISECOND).chain_dropped);
    EXPECT_EQ(reads_at(scheduler, plan,
                       {2 * MILLISECOND, 3 * MILLISECOND, 4 * MILLISECOND}),
              (std::vector<std::uint64_t>{0, 100, NOT_A_READ}));
}

TEST(Scheduling, FloorProbesGoOneInElevenProbeTimeOuts) {
    // J3, the lead's ruling N1: 20 demoted devices whose probes are past
    // their floor (time-out 100 ms), and a live device whose 200 ms polls
    // fill the line, so a request is always due and no window opens. The
    // oldest probe past its floor goes ahead of the due polls, at most one
    // in PROBE_FLOOR + 1 probe time-outs: about 60 s / 1.1 s = 54 probes,
    // and the live device waits at most its period and one probe time-out.
    std::vector<DeviceSpec> specs{
        {.name = "live", .unit_id = 1, .points = adjacent(0, 1, FAST)},
        {.name = "failing", .unit_id = 2, .points = adjacent(0, 1, SLOW)}};
    for (std::uint8_t unit = 3; unit <= 22; ++unit) {
        specs.push_back({.name = "p" + std::to_string(unit),
                         .unit_id = unit,
                         .points = adjacent(0, 1, SLOW)});
    }
    const CompiledChannel channel =
        make_channel(specs, ReadRules{.max_units = LARGE});
    Scheduler scheduler(channel, 4, 0);
    for (std::uint32_t device = 2; device < 22; ++device) {
        scheduler.set_polled(device, false);
        scheduler.set_probe(device, 0, 100 * MILLISECOND);
    }
    std::int64_t last_live = 0;
    std::int64_t worst = 0;
    std::size_t probes = 0;
    std::int64_t now = 0;
    for (std::size_t step = 0; step < 20'000 && now < 60 * SECOND; ++step) {
        const Choice choice = scheduler.take(now);
        if (choice.kind == Choice::Kind::PROBE) {
            ++probes;
            now += 100 * MILLISECOND;  // unanswered; due again at once
            scheduler.probe_done();
            scheduler.set_probe(choice.device, now, 100 * MILLISECOND);
        } else if (choice.kind == Choice::Kind::POLL) {
            if (choice.device == 0) {
                worst = std::max(worst, now - last_live);
                last_live = now;
            }
            now += choice.device == 0 ? FAST_PERIOD : 100 * MILLISECOND;
        } else {
            now = std::max(now + MILLISECOND, choice.wake_ns);
        }
    }
    EXPECT_LE(worst, FAST_PERIOD + 100 * MILLISECOND);
    EXPECT_GE(probes, 50U);
    EXPECT_LE(probes, 55U);
}

TEST(Scheduling, AProbeWaitsForItsDueTimeAndForTheLiveDevices) {
    // d1 is demoted, its probe due at 5 ms; d2 is live in the fast group.
    const CompiledChannel channel =
        make_channel({{.name = "d1", .unit_id = 1, .points = adjacent(0, 1)},
                      {.name = "d2", .unit_id = 2, .points = adjacent(0, 1)}},
                     ReadRules{.max_units = LARGE});
    Scheduler scheduler(channel, 4, 0);
    scheduler.set_polled(0, false);
    scheduler.set_probe(0, 5 * MILLISECOND, 100 * MILLISECOND);
    EXPECT_EQ(scheduler.take(0).device, 1U);  // d2's poll
    const Choice wait = scheduler.take(MILLISECOND);
    EXPECT_EQ(wait.kind, Choice::Kind::WAIT);
    EXPECT_EQ(wait.wake_ns, 5 * MILLISECOND);
    // A due poll of d2 at the same time goes first.
    EXPECT_EQ(scheduler.take(FAST_PERIOD).kind, Choice::Kind::POLL);
    scheduler.set_probe(0, FAST_PERIOD, 100 * MILLISECOND);
    const Choice probe = scheduler.take(FAST_PERIOD + MILLISECOND);
    EXPECT_EQ(probe.kind, Choice::Kind::PROBE);
    EXPECT_EQ(probe.device, 0U);
    EXPECT_EQ(probe.request, channel.devices[0].plan.probe.value_or(~0U));
}

TEST(Scheduling, AtMostOneProbeIsInFlightPerChannel) {
    const CompiledChannel channel =
        make_channel({{.name = "d1", .unit_id = 1, .points = adjacent(0, 1)},
                      {.name = "d2", .unit_id = 2, .points = adjacent(0, 1)}},
                     ReadRules{.max_units = LARGE});
    Scheduler scheduler(channel, 4, 0);
    for (std::uint32_t device = 0; device < 2; ++device) {
        scheduler.set_polled(device, false);
        scheduler.set_probe(device, 0, 0);
    }
    const Choice first = scheduler.take(0);
    ASSERT_EQ(first.kind, Choice::Kind::PROBE);
    EXPECT_EQ(first.device, 0U);
    scheduler.clear_probe(0);
    EXPECT_EQ(scheduler.take(MILLISECOND).kind, Choice::Kind::IDLE);
    scheduler.probe_done();
    const Choice second = scheduler.take(2 * MILLISECOND);
    EXPECT_EQ(second.kind, Choice::Kind::PROBE);
    EXPECT_EQ(second.device, 1U);
}

TEST(Scheduling, AProbeNotSentLeavesNoProbeInFlight) {
    const CompiledChannel channel = make_channel(
        {{.name = "d1", .points = adjacent(0, 1)}}, ReadRules{.max_units = 4});
    Scheduler scheduler(channel, 4, 0);
    scheduler.set_polled(0, false);
    scheduler.set_probe(0, 0, 0);
    EXPECT_EQ(scheduler.take(0).kind, Choice::Kind::PROBE);
    EXPECT_EQ(scheduler.not_sent(), std::nullopt);
    EXPECT_EQ(scheduler.take(0).kind, Choice::Kind::PROBE);
}

TEST(Scheduling, NoProbeWhileAnOrderWaits) {
    const CompiledChannel channel =
        make_channel({{.name = "d1", .unit_id = 1, .points = adjacent(0, 1)},
                      {.name = "d2",
                       .unit_id = 2,
                       .points = adjacent(0, 1, SLOW),
                       .pacing = Pacing{.min_write_interval_ns = SECOND}}},
                     ReadRules{.max_units = LARGE});
    Scheduler scheduler(channel, 4, 0);
    scheduler.set_polled(0, false);
    scheduler.set_probe(0, 0, 0);
    ASSERT_TRUE(scheduler.add_order(OrderTicket{.id = 1, .device = 1}));
    EXPECT_EQ(scheduler.take(0).kind, Choice::Kind::ORDER);
    ASSERT_TRUE(scheduler.add_order(OrderTicket{.id = 2, .device = 1}));
    // d2's poll, then the second order waits for d2's write spacing: no
    // probe in that time.
    EXPECT_EQ(scheduler.take(MILLISECOND).kind, Choice::Kind::POLL);
    const Choice wait = scheduler.take(2 * MILLISECOND);
    EXPECT_EQ(wait.kind, Choice::Kind::WAIT);
    EXPECT_EQ(wait.wake_ns, SECOND);
    EXPECT_EQ(scheduler.take(SECOND).kind, Choice::Kind::ORDER);
    EXPECT_EQ(scheduler.take(SECOND + MILLISECOND).kind, Choice::Kind::PROBE);
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

}  // namespace
}  // namespace scada::driver::test
