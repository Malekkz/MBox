// Scheduling with the test protocol over the loopback (driver interface
// contract 6.2 to 6.6; conformance tests 9 to 14, the parts that need no
// failure handling). Time is injected: every exchange takes 1 ms, and the
// loop jumps to the scheduler's wake time, so the request log is exact.
//
// Left to the slices that bring them: dead devices and the probe budget
// (test 12, 7.4), retries, identity reads and READ_NOW (test 14).

#include "channel_builder.h"
#include "loopback.h"
#include "scada/driver/compiled.h"
#include "scada/driver/exchange.h"
#include "scada/driver/plan.h"
#include "scada/driver/scheduler.h"
#include "test_protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
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
    EXPECT_FALSE(loop.scheduler().pacing_limited(1));
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
    // takes 400 ms.
    const CompiledChannel channel = make_channel(
        {{.name = "d1",
          .points = adjacent(0, 4),
          .pacing = {.min_request_interval_ns = 100 * MILLISECOND}}},
        ReadRules{.max_units = 1});
    SimBus bus = mapped_bus();
    const TestProtocol protocol;
    Loop loop(channel, protocol, bus, 0, EXCHANGE);
    loop.run_until(2 * SECOND);
    EXPECT_EQ(loop.scheduler().achieved_period_ns(FAST), 400 * MILLISECOND);
    EXPECT_TRUE(loop.scheduler().pacing_limited(0));
}

// --- Conformance test 13: keep-alive --------------------------------------

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

TEST(Scheduling, AChainOfCompanionsHasNoOtherRequestToItsDeviceBetween) {
    // Point 0 at 0 with its companion at 100 (4 units per request: two
    // requests); an order arrives after the first part of the chain.
    const CompiledChannel channel = make_channel(
        {{.name = "d1",
          .points = {{.first = 0, .companions = {1}}, {.first = 100}}}},
        ReadRules{.max_units = 4});
    Scheduler scheduler(channel, 4, 0);
    const Choice first = scheduler.take(0);
    ASSERT_EQ(first.kind, Choice::Kind::POLL);
    EXPECT_EQ(channel.devices[0].plan.requests[first.request].first, 0U);
    ASSERT_TRUE(scheduler.add_order(OrderTicket{.id = 1, .device = 0}));
    const Choice second = scheduler.take(MILLISECOND);
    ASSERT_EQ(second.kind, Choice::Kind::POLL);
    EXPECT_EQ(channel.devices[0].plan.requests[second.request].first, 100U);
    EXPECT_EQ(scheduler.take(2 * MILLISECOND).kind, Choice::Kind::ORDER);
}

TEST(Scheduling, AnExclusiveOrderHoldsItsDevice) {
    // A two-step exclusive order (select, then operate) on device 1: no poll
    // of device 1 between its steps; device 2 is served meanwhile.
    const CompiledChannel channel =
        make_channel({{.name = "d1",
                       .unit_id = 1,
                       .points = adjacent(0, 1),
                       .pacing = {.min_write_interval_ns = 10 * MILLISECOND}},
                      {.name = "d2", .unit_id = 2, .points = adjacent(0, 1)}},
                     ReadRules{.max_units = LARGE});
    Scheduler scheduler(channel, 4, 0);
    ASSERT_TRUE(scheduler.add_order(
        OrderTicket{.id = 9, .device = 0, .steps = 2, .exclusive = true}));
    const Choice select = scheduler.take(0);
    ASSERT_EQ(select.kind, Choice::Kind::ORDER);
    EXPECT_EQ(select.step, 0U);
    const Choice other = scheduler.take(MILLISECOND);
    EXPECT_EQ(other.kind, Choice::Kind::POLL);
    EXPECT_EQ(other.device, 1U);  // device 2; device 1 is held
    const Choice wait = scheduler.take(2 * MILLISECOND);
    ASSERT_EQ(wait.kind, Choice::Kind::WAIT);
    EXPECT_EQ(wait.wake_ns, 10 * MILLISECOND);
    const Choice operate = scheduler.take(10 * MILLISECOND);
    ASSERT_EQ(operate.kind, Choice::Kind::ORDER);
    EXPECT_EQ(operate.step, 1U);
    EXPECT_EQ(scheduler.take(11 * MILLISECOND).device,
              0U);  // its poll, at last
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

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

}  // namespace
}  // namespace scada::driver::test
