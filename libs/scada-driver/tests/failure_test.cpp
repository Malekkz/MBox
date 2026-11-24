// Failure accounting (driver interface contract 7.4; conformance test 15,
// the accounting half): demotion after `demote_after` failed requests,
// grace, a shared count per unit id, probes at `backoff` doubling to
// `backoff_max`, recovery.

#include "scada/driver/failure.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace scada::driver {
namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): the counts, unit ids and times are the test data, each worked out by hand beside the rule it checks

constexpr std::int64_t SECOND = 1'000'000'000;
constexpr std::uint32_t FIRST = 0;
constexpr std::uint32_t SECOND_DEVICE = 1;
constexpr std::uint32_t OTHER_UNIT = 2;

FailureTracker tracker(std::vector<FailureTracker::Device> devices) {
    FailureTracker out(std::move(devices));
    out.connected(0);
    return out;
}

FailureRules rules(std::uint8_t demote_after = 3, std::int64_t grace_ns = 0) {
    return FailureRules{.demote_after = demote_after,
                        .backoff_ns = 10 * SECOND,
                        .backoff_max_ns = 35 * SECOND,
                        .startup_grace_ns = grace_ns};
}

void fail(FailureTracker& tracked, std::uint32_t device, int times,
          std::int64_t at_ns) {
    for (int index = 0; index < times; ++index) {
        (void)tracked.request_ended(device, RequestEnd::FAILED, at_ns);
    }
}

TEST(Failure, ADeviceIsDemotedAfterDemoteAfterFailedRequests) {
    FailureTracker tracked = tracker({{.unit_id = 1, .rules = rules()}});
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::INITIALISING);
    fail(tracked, FIRST, 2, SECOND);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::INITIALISING);
    EXPECT_EQ(tracked.failures(FIRST), 2U);
    EXPECT_TRUE(tracked.request_ended(FIRST, RequestEnd::FAILED, SECOND));
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::DEMOTED);
    EXPECT_EQ(tracked.probe_due_ns(FIRST), 11 * SECOND);
    EXPECT_FALSE(tracked.polled(FIRST));
}

TEST(Failure, AnAnswerEmptiesTheCountAndBusyCountsNeitherWay) {
    FailureTracker tracked = tracker({{.unit_id = 1, .rules = rules()}});
    fail(tracked, FIRST, 2, SECOND);
    (void)tracked.request_ended(FIRST, RequestEnd::NOT_COUNTED, SECOND);
    EXPECT_EQ(tracked.failures(FIRST), 2U);
    (void)tracked.request_ended(FIRST, RequestEnd::ANSWERED, SECOND);
    EXPECT_EQ(tracked.failures(FIRST), 0U);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::RUNNING);
    fail(tracked, FIRST, 2, SECOND);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::RUNNING);
}

TEST(Failure, FailuresWithinStartupGraceDoNotCount) {
    FailureTracker tracked =
        tracker({{.unit_id = 1, .rules = rules(3, 5 * SECOND)}});
    fail(tracked, FIRST, 10, 4 * SECOND);
    EXPECT_EQ(tracked.failures(FIRST), 0U);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::INITIALISING);
    fail(tracked, FIRST, 3, 5 * SECOND);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::DEMOTED);
}

TEST(Failure, DevicesSharingAUnitIdShareOneCount) {
    // Two slices of one gateway unit, and a device on another unit: the
    // dead unit costs demote_after requests once, for both slices.
    FailureTracker tracked = tracker({{.unit_id = 7, .rules = rules()},
                                      {.unit_id = 7, .rules = rules()},
                                      {.unit_id = 8, .rules = rules()}});
    (void)tracked.request_ended(FIRST, RequestEnd::FAILED, SECOND);
    (void)tracked.request_ended(SECOND_DEVICE, RequestEnd::FAILED, SECOND);
    EXPECT_EQ(tracked.failures(SECOND_DEVICE), 2U);
    (void)tracked.request_ended(FIRST, RequestEnd::FAILED, SECOND);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::DEMOTED);
    EXPECT_EQ(tracked.state(SECOND_DEVICE), DeviceStatus::DEMOTED);
    EXPECT_EQ(tracked.state(OTHER_UNIT), DeviceStatus::INITIALISING);
    EXPECT_EQ(tracked.failures(OTHER_UNIT), 0U);
}

TEST(Failure, ProbesBackOffDoublingToTheMaximumThenRecover) {
    FailureTracker tracked = tracker({{.unit_id = 1, .rules = rules(1)}});
    fail(tracked, FIRST, 1, 0);
    ASSERT_EQ(tracked.state(FIRST), DeviceStatus::DEMOTED);
    EXPECT_EQ(tracked.probe_due_ns(FIRST), 10 * SECOND);
    tracked.probe_ended(FIRST, false, 10 * SECOND);
    EXPECT_EQ(tracked.probe_due_ns(FIRST), 30 * SECOND);  // 20 s back-off
    tracked.probe_ended(FIRST, false, 30 * SECOND);
    EXPECT_EQ(tracked.probe_due_ns(FIRST), 65 * SECOND);  // 35 s: the maximum
    tracked.probe_ended(FIRST, false, 65 * SECOND);
    EXPECT_EQ(tracked.probe_due_ns(FIRST), 100 * SECOND);
    tracked.probe_ended(FIRST, true, 100 * SECOND);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::INITIALISING);
    EXPECT_EQ(tracked.failures(FIRST), 0U);
    // Demoted again: the back-off starts from `backoff` once more.
    fail(tracked, FIRST, 1, 200 * SECOND);
    EXPECT_EQ(tracked.probe_due_ns(FIRST), 210 * SECOND);
}

TEST(Failure, ALostConnectionAndADisabledDevice) {
    FailureTracker tracked =
        tracker({{.unit_id = 1, .rules = rules()},
                 {.unit_id = 2, .rules = rules(), .enabled = false}});
    EXPECT_EQ(tracked.state(SECOND_DEVICE), DeviceStatus::DISABLED);
    tracked.lost();
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::CONNECTING);
    EXPECT_EQ(tracked.state(SECOND_DEVICE), DeviceStatus::DISABLED);
    // A request of a device that is not polled changes nothing.
    EXPECT_FALSE(tracked.request_ended(FIRST, RequestEnd::FAILED, SECOND));
    EXPECT_EQ(tracked.failures(FIRST), 0U);
}

// --- The run of consecutive read timeouts (7.3, R-15) ---------------------

TEST(Failure, ARunOfTimeoutsOverEveryLiveUnitLosesTheConnection) {
    // Three units, lost_after_timeouts 4: timeouts of units 1 and 2 alone
    // never lose it, however many; one of unit 3 completes the set.
    FailureTracker tracked({{.unit_id = 1, .rules = rules()},
                            {.unit_id = 2, .rules = rules()},
                            {.unit_id = 3, .rules = rules()}},
                           4);
    tracked.connected(0);
    for (int index = 0; index < 3; ++index) {
        EXPECT_FALSE(tracked.read_timed_out(FIRST, SECOND).lost);
        EXPECT_FALSE(tracked.read_timed_out(SECOND_DEVICE, SECOND).lost);
    }
    EXPECT_EQ(tracked.timeout_run(), 6U);
    EXPECT_TRUE(tracked.read_timed_out(OTHER_UNIT, SECOND).lost);
}

TEST(Failure, AnyValidAnswerEmptiesTheRunAndTheRunNeedsItsLength) {
    FailureTracker tracked(
        {{.unit_id = 1, .rules = rules()}, {.unit_id = 2, .rules = rules()}},
        3);
    tracked.connected(0);
    EXPECT_FALSE(tracked.read_timed_out(FIRST, SECOND).lost);
    EXPECT_FALSE(tracked.read_timed_out(SECOND_DEVICE, SECOND)
                     .lost);  // both units, 2 < 3
    tracked.answer_seen();
    EXPECT_EQ(tracked.timeout_run(), 0U);
    EXPECT_FALSE(tracked.read_timed_out(FIRST, SECOND).lost);
    EXPECT_FALSE(tracked.read_timed_out(FIRST, SECOND).lost);
    EXPECT_TRUE(tracked.read_timed_out(SECOND_DEVICE, SECOND).lost);
}

TEST(Failure, OnlyEligibleUnitsAreNeededForTheLoss) {
    // Unit 2 is demoted and unit 3 has no poll group: neither is eligible,
    // so unit 1's timeouts alone lose the connection.
    FailureTracker tracked(
        {{.unit_id = 1, .rules = rules()},
         {.unit_id = 2, .rules = rules(1)},
         {.unit_id = 3, .rules = rules(), .has_poll_group = false}},
        2);
    tracked.connected(0);
    fail(tracked, SECOND_DEVICE, 1, SECOND);
    ASSERT_EQ(tracked.state(SECOND_DEVICE), DeviceStatus::DEMOTED);
    EXPECT_FALSE(tracked.read_timed_out(FIRST, SECOND).lost);
    EXPECT_TRUE(tracked.read_timed_out(FIRST, SECOND).lost);
}

TEST(Failure, AReconnectEmptiesTheRunAndAResumeKeepsStatesAndCounts) {
    // demote_after 5: one broken answer and one time-out count 2.
    FailureTracker tracked({{.unit_id = 1, .rules = rules(5)}}, 2);
    tracked.connected(0);
    fail(tracked, FIRST, 1, SECOND);
    EXPECT_FALSE(tracked.read_timed_out(FIRST, SECOND).lost);
    tracked.resumed();
    EXPECT_EQ(tracked.timeout_run(), 0U);
    EXPECT_EQ(tracked.failures(FIRST), 2U);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::INITIALISING);
    EXPECT_FALSE(tracked.read_timed_out(FIRST, SECOND).lost);
    tracked.connected(2 * SECOND);
    EXPECT_EQ(tracked.timeout_run(), 0U);
    EXPECT_EQ(tracked.failures(FIRST), 0U);
}

TEST(Failure, WithoutTheRuleATimeoutRunNeverLosesTheConnection) {
    FailureTracker tracked({{.unit_id = 1, .rules = rules()}});
    tracked.connected(0);
    for (int index = 0; index < 10; ++index) {
        EXPECT_FALSE(tracked.read_timed_out(FIRST, SECOND).lost);
    }
}

// --- The lead's ruling on 7.3 (D-139) -------------------------------------

TEST(Failure, TheTimeOutThatLosesTheConnectionIsNotCountedTowardDemotion) {
    // One unit, lost_after_timeouts 3, demote_after 3. Three time-outs: the
    // third loses the connection, and the device keeps 2 failures. After
    // the reconnect, which keeps states and counts (a close the driver made
    // itself, resumed), the run is empty: the next time-out makes 3 and
    // demotes it, with the run at 1.
    FailureTracker tracked({{.unit_id = 1, .rules = rules()}}, 3);
    tracked.connected(0);
    EXPECT_FALSE(tracked.read_timed_out(FIRST, SECOND).lost);
    EXPECT_FALSE(tracked.read_timed_out(FIRST, SECOND).lost);
    const FailureTracker::TimeoutEnd third =
        tracked.read_timed_out(FIRST, SECOND);
    EXPECT_TRUE(third.lost);
    EXPECT_FALSE(third.demoted);
    EXPECT_EQ(tracked.failures(FIRST), 2U);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::INITIALISING);
    tracked.resumed();
    const FailureTracker::TimeoutEnd fourth =
        tracked.read_timed_out(FIRST, 2 * SECOND);
    EXPECT_FALSE(fourth.lost);
    EXPECT_TRUE(fourth.demoted);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::DEMOTED);
    EXPECT_EQ(tracked.timeout_run(), 1U);
}

TEST(Failure, ADeadUnitAmongLiveOnesIsDemotedWithoutLosingTheConnection) {
    // Two units, lost_after_timeouts 3, demote_after 3: unit 1 is dead,
    // unit 2 answers between. The run never spans both units, so the
    // connection stays, and unit 1 is demoted at its third time-out.
    FailureTracker tracked(
        {{.unit_id = 1, .rules = rules()}, {.unit_id = 2, .rules = rules()}},
        3);
    tracked.connected(0);
    for (int index = 0; index < 2; ++index) {
        EXPECT_FALSE(tracked.read_timed_out(FIRST, SECOND).lost);
        (void)tracked.request_ended(SECOND_DEVICE, RequestEnd::ANSWERED,
                                    SECOND);
        tracked.answer_seen();
    }
    const FailureTracker::TimeoutEnd third =
        tracked.read_timed_out(FIRST, SECOND);
    EXPECT_FALSE(third.lost);
    EXPECT_TRUE(third.demoted);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::DEMOTED);
    EXPECT_EQ(tracked.state(SECOND_DEVICE), DeviceStatus::RUNNING);
}

TEST(Failure, WithNoEligibleUnitTheRunsLengthAloneDecides) {
    // D-139: both devices without a poll group (keep-alive only): neither is
    // eligible, and two time-outs lose the connection.
    FailureTracker tracked(
        {{.unit_id = 1, .rules = rules(), .has_poll_group = false},
         {.unit_id = 2, .rules = rules(), .has_poll_group = false}},
        2);
    tracked.connected(0);
    EXPECT_FALSE(tracked.read_timed_out(FIRST, SECOND).lost);
    EXPECT_TRUE(tracked.read_timed_out(FIRST, SECOND).lost);
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

}  // namespace
}  // namespace scada::driver
