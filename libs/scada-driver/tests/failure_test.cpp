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
    // Demoted again before any poll answered: the back-off is kept (Q3).
    fail(tracked, FIRST, 1, 150 * SECOND);
    EXPECT_EQ(tracked.probe_due_ns(FIRST), 185 * SECOND);
    tracked.probe_ended(FIRST, true, 185 * SECOND);
    // A poll answers: the back-off is `backoff` again.
    (void)tracked.request_ended(FIRST, RequestEnd::ANSWERED, 186 * SECOND);
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
    // A reconnect empties the run but keeps the count (PR 137 review, P1).
    tracked.connected(2 * SECOND);
    EXPECT_EQ(tracked.timeout_run(), 0U);
    EXPECT_EQ(tracked.failures(FIRST), 3U);
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

// --- PR 137 review: a reconnect keeps demotion; one probe per unit --------

TEST(Failure, AReconnectKeepsDemotionAndCounts) {
    // Q2: a demoted device stays demoted across a lost connection, with its
    // probe schedule; another device's count is kept.
    FailureTracker tracked(
        {{.unit_id = 1, .rules = rules(1)}, {.unit_id = 2, .rules = rules()}});
    tracked.connected(0);
    fail(tracked, FIRST, 1, SECOND);
    fail(tracked, SECOND_DEVICE, 2, SECOND);
    ASSERT_EQ(tracked.state(FIRST), DeviceStatus::DEMOTED);
    tracked.lost();
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::CONNECTING);
    tracked.connected(2 * SECOND);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::DEMOTED);
    EXPECT_EQ(tracked.probe_due_ns(FIRST), 11 * SECOND);
    EXPECT_EQ(tracked.state(SECOND_DEVICE), DeviceStatus::INITIALISING);
    EXPECT_EQ(tracked.failures(SECOND_DEVICE), 2U);
}

TEST(Failure, OneProbeServesEveryDeviceOfAUnit) {
    // Q5: two slices of one unit are demoted together; the first probes for
    // both, and its answer recovers both.
    FailureTracker tracked = tracker(
        {{.unit_id = 7, .rules = rules(1)}, {.unit_id = 7, .rules = rules(1)}});
    fail(tracked, FIRST, 1, SECOND);
    ASSERT_EQ(tracked.state(SECOND_DEVICE), DeviceStatus::DEMOTED);
    EXPECT_TRUE(tracked.probes_for_unit(FIRST));
    EXPECT_FALSE(tracked.probes_for_unit(SECOND_DEVICE));
    tracked.probe_ended(FIRST, false, 11 * SECOND);
    EXPECT_EQ(tracked.probe_due_ns(SECOND_DEVICE), 31 * SECOND);
    tracked.probe_ended(FIRST, true, 31 * SECOND);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::INITIALISING);
    EXPECT_EQ(tracked.state(SECOND_DEVICE), DeviceStatus::INITIALISING);
}

TEST(FailureTracker, AUnitThatAnswersOnlyProbesBacksOffFurtherEachTime) {
    // G6: demote_after 1, back-off 1 s to 8 s. The probe answers, the next
    // poll fails: demoted again with the back-off doubled, 2 s, then 4 s,
    // 8 s, 8 s. A poll that answers resets it to 1 s.
    FailureRules rules;
    rules.demote_after = 1;
    rules.backoff_ns = SECOND;
    rules.backoff_max_ns = 8 * SECOND;
    FailureTracker tracker({{.unit_id = 1, .rules = rules}});
    tracker.connected(0);
    std::int64_t now = 0;
    (void)tracker.request_ended(0, RequestEnd::FAILED, now);
    EXPECT_EQ(tracker.probe_due_ns(0), now + SECOND);
    // A probe answers, the next poll fails: the wait until the next probe.
    const auto flap = [&] {
        now = tracker.probe_due_ns(0);
        tracker.probe_ended(0, true, now);
        (void)tracker.request_ended(0, RequestEnd::FAILED, now);
        return tracker.probe_due_ns(0) - now;
    };
    const std::vector<std::int64_t> waits{flap(), flap(), flap(), flap()};
    EXPECT_EQ(waits, (std::vector<std::int64_t>{2 * SECOND, 4 * SECOND,
                                                8 * SECOND, 8 * SECOND}));
    EXPECT_EQ(tracker.state(0), DeviceStatus::DEMOTED);
    now = tracker.probe_due_ns(0);
    tracker.probe_ended(0, true, now);
    (void)tracker.request_ended(0, RequestEnd::ANSWERED, now);
    (void)tracker.request_ended(0, RequestEnd::FAILED, now);
    EXPECT_EQ(tracker.probe_due_ns(0), now + SECOND);
}

// --- The identity check (D-147; DI 3.6, 7.3) ------------------------------

FailureTracker::Device identified(std::uint32_t unit_id,
                                  std::int64_t grace_ns = 0) {
    return {.unit_id = unit_id, .rules = rules(3, grace_ns), .identify = true};
}

TEST(Failure, AConnectOrARecoveryStartsTheCheckAndResumingDoesNot) {
    FailureTracker tracked =
        tracker({identified(1), {.unit_id = 2, .rules = rules()}});
    EXPECT_TRUE(tracked.identity_pending(FIRST));
    EXPECT_FALSE(tracked.identity_pending(SECOND_DEVICE));  // no checks
    tracked.identity_passed(FIRST);
    EXPECT_FALSE(tracked.identity_pending(FIRST));
    // The driver's own close and reconnect within connect_timeout: no check.
    tracked.resumed();
    EXPECT_FALSE(tracked.identity_pending(FIRST));
    // A lost connection and a connect: checked again.
    tracked.lost();
    tracked.connected(SECOND);
    EXPECT_TRUE(tracked.identity_pending(FIRST));
    tracked.identity_passed(FIRST);
    // Demoted, then recovered by a probe: checked again; a demoted device
    // is not checked at a connect, only when a probe recovers it.
    fail(tracked, FIRST, 3, 2 * SECOND);
    ASSERT_EQ(tracked.state(FIRST), DeviceStatus::DEMOTED);
    tracked.lost();
    tracked.connected(3 * SECOND);
    EXPECT_FALSE(tracked.identity_pending(FIRST));
    tracked.probe_ended(FIRST, true, 13 * SECOND);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::INITIALISING);
    EXPECT_TRUE(tracked.identity_pending(FIRST));
}

TEST(Failure, AnIdentityReadCountsTowardDemotionButNotTheRun) {
    FailureTracker tracked(
        {identified(1, 5 * SECOND), {.unit_id = 2, .rules = rules()}}, 2);
    tracked.connected(0);
    // Within the grace: not counted.
    EXPECT_FALSE(tracked.identity_ended(FIRST, RequestEnd::FAILED, SECOND));
    EXPECT_EQ(tracked.failures(FIRST), 0U);
    // Outside it: counted, and demoted as a polled read would be, never
    // MISMATCH; the run of time-outs is untouched.
    (void)tracked.read_timed_out(SECOND_DEVICE, 5 * SECOND);
    ASSERT_EQ(tracked.timeout_run(), 1U);
    EXPECT_FALSE(tracked.identity_ended(FIRST, RequestEnd::FAILED, 6 * SECOND));
    EXPECT_FALSE(tracked.identity_ended(FIRST, RequestEnd::FAILED, 6 * SECOND));
    EXPECT_TRUE(tracked.identity_ended(FIRST, RequestEnd::FAILED, 6 * SECOND));
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::DEMOTED);
    EXPECT_EQ(tracked.timeout_run(), 1U);
}

TEST(Failure, AnAnsweredIdentityReadIsNoRecovery) {
    FailureTracker tracked = tracker({identified(1)});
    fail(tracked, FIRST, 2, SECOND);
    // It empties the count, but makes the device no RUNNING and leaves the
    // back-off as it grew.
    EXPECT_FALSE(tracked.identity_ended(FIRST, RequestEnd::ANSWERED, SECOND));
    EXPECT_EQ(tracked.failures(FIRST), 0U);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::INITIALISING);
    // Busy: neither way.
    fail(tracked, FIRST, 1, SECOND);
    (void)tracked.identity_ended(FIRST, RequestEnd::NOT_COUNTED, SECOND);
    EXPECT_EQ(tracked.failures(FIRST), 1U);
}

TEST(Failure, AUnitThatAnswersOnlyItsIdentityBacksOffToTheMaximum) {
    // Probe answered, identity answered and matched, every poll failed:
    // the back-off grows as for a unit that answers only probes (G6).
    FailureTracker tracked = tracker({identified(1)});
    fail(tracked, FIRST, 3, 0);
    std::int64_t now = 0;
    std::vector<std::int64_t> waits;
    for (int round = 0; round < 4; ++round) {
        ASSERT_EQ(tracked.state(FIRST), DeviceStatus::DEMOTED);
        waits.push_back(tracked.probe_due_ns(FIRST) - now);
        now = tracked.probe_due_ns(FIRST);
        tracked.probe_ended(FIRST, true, now);
        ASSERT_TRUE(tracked.identity_pending(FIRST));
        (void)tracked.identity_ended(FIRST, RequestEnd::ANSWERED, now);
        tracked.identity_passed(FIRST);
        fail(tracked, FIRST, 3, now);
    }
    EXPECT_EQ(waits, (std::vector<std::int64_t>{10 * SECOND, 20 * SECOND,
                                                35 * SECOND, 35 * SECOND}));
}

TEST(Failure, AMismatchIsLeftThroughInitialisingAndRecheckedAtConstantBackoff) {
    FailureTracker tracked = tracker({identified(1)});
    tracked.identity_mismatched(FIRST, SECOND);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::MISMATCH);
    EXPECT_FALSE(tracked.polled(FIRST));
    EXPECT_FALSE(tracked.identity_pending(FIRST));
    EXPECT_EQ(tracked.probe_due_ns(FIRST), 11 * SECOND);
    // A failed re-check counts; a mismatch again keeps the back-off as it is.
    (void)tracked.identity_ended(FIRST, RequestEnd::FAILED, 11 * SECOND);
    EXPECT_EQ(tracked.failures(FIRST), 1U);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::MISMATCH);
    // A poll's end is not counted for it: it is not polled.
    fail(tracked, FIRST, 1, 11 * SECOND);
    EXPECT_EQ(tracked.failures(FIRST), 1U);
    tracked.identity_passed(FIRST);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::INITIALISING);
    tracked.identity_mismatched(FIRST, 12 * SECOND);
    EXPECT_EQ(tracked.probe_due_ns(FIRST), 22 * SECOND);
    // A match makes it INITIALISING; only a poll's answer makes it RUNNING.
    tracked.identity_passed(FIRST);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::INITIALISING);
    (void)tracked.request_ended(FIRST, RequestEnd::ANSWERED, 23 * SECOND);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::RUNNING);
    // A mismatch only from INITIALISING.
    tracked.identity_mismatched(FIRST, 24 * SECOND);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::RUNNING);
}

TEST(Failure, ADeadMismatchedDeviceIsDemoted) {
    FailureTracker tracked = tracker({identified(1)});
    tracked.identity_mismatched(FIRST, 0);
    (void)tracked.identity_ended(FIRST, RequestEnd::FAILED, SECOND);
    (void)tracked.identity_ended(FIRST, RequestEnd::FAILED, SECOND);
    EXPECT_TRUE(tracked.identity_ended(FIRST, RequestEnd::FAILED, SECOND));
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::DEMOTED);
}

// Three devices, the first two on one unit, each matched and RUNNING.
FailureTracker running_three() {
    FailureTracker tracked =
        tracker({identified(1, 5 * SECOND), identified(1, 5 * SECOND),
                 identified(2, 5 * SECOND)});
    for (std::uint32_t device = 0; device < 3; ++device) {
        tracked.identity_passed(device);
        (void)tracked.request_ended(device, RequestEnd::ANSWERED, 6 * SECOND);
    }
    return tracked;
}

TEST(Failure, ARestartChecksEveryRunningDeviceOfTheUnitAgain) {
    FailureTracker tracked = running_three();
    tracked.restarted(FIRST, 10 * SECOND, true);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::INITIALISING);
    EXPECT_TRUE(tracked.identity_pending(FIRST));
    EXPECT_EQ(tracked.state(SECOND_DEVICE), DeviceStatus::INITIALISING);
    EXPECT_TRUE(tracked.identity_pending(SECOND_DEVICE));
    EXPECT_EQ(tracked.state(OTHER_UNIT), DeviceStatus::RUNNING);
    // The grace started again: no failure counted within it.
    fail(tracked, FIRST, 3, 14 * SECOND);
    EXPECT_EQ(tracked.failures(FIRST), 0U);
}

TEST(Failure, ARestartWhileInitialisingChangesNothing) {
    FailureTracker tracked = running_three();
    tracked.restarted(FIRST, 10 * SECOND, true);
    tracked.identity_passed(FIRST);
    tracked.restarted(FIRST, 15 * SECOND, true);
    EXPECT_FALSE(tracked.identity_pending(FIRST));
    // Without `rearm_grace` the grace is not started again.
    (void)tracked.request_ended(FIRST, RequestEnd::ANSWERED, 16 * SECOND);
    tracked.restarted(FIRST, 16 * SECOND, false);
    EXPECT_EQ(tracked.state(FIRST), DeviceStatus::INITIALISING);
    fail(tracked, FIRST, 1, 16 * SECOND);
    EXPECT_EQ(tracked.failures(FIRST), 1U);
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

}  // namespace
}  // namespace scada::driver
