// The values the engine gives the tracker and the scheduler for a device's
// identity check (decision D-147; review of PR 171): the deadline, the full
// period and the scheduler mode of each tracker state.

#include "scada/driver/identity_setup.h"

#include "scada/driver/compiled.h"
#include "scada/driver/failure.h"
#include "scada/driver/plan.h"
#include "scada/driver/scheduler.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace scada::driver {
namespace {

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers): the periods and times are the test data

constexpr std::int64_t SECOND = 1'000'000'000;

std::vector<CompiledPollGroup> groups() {
    return {{.name = "fast", .period_ns = SECOND / 5},
            {.name = "normal", .period_ns = SECOND},
            {.name = "slow", .period_ns = 10 * SECOND}};
}

// Requests in `in_groups` (one each), the identity list `identity`.
ReadPlan plan_of(const std::vector<std::uint32_t>& in_groups,
                 const std::vector<std::uint32_t>& identity) {
    ReadPlan plan;
    plan.by_group.resize(3);
    for (const std::uint32_t group : in_groups) {
        Request request;
        request.poll_group = group;
        if (group < plan.by_group.size()) {
            plan.by_group[group].push_back(
                static_cast<std::uint32_t>(plan.requests.size()));
        }
        plan.requests.push_back(request);
    }
    plan.identity = identity;
    return plan;
}

TEST(IdentitySetup, TheDeadlineIsTheGraceOrThreePeriodsOfTheIdentityGroup) {
    const std::vector<CompiledPollGroup> all = groups();
    struct Row {
        std::int64_t grace_ns;
        std::vector<std::uint32_t> in_groups;
        std::vector<std::uint32_t> identity;
        std::int64_t expected_ns;
    };
    const std::vector<Row> rows{
        // An identity request in normal: 3 s, or the grace when longer.
        {0, {0, 1}, {1}, 3 * SECOND},
        {5 * SECOND, {0, 1}, {1}, 5 * SECOND},
        // In fast and slow: the slowest, 30 s.
        {0, {0, 2}, {0, 1}, 30 * SECOND},
        {0, {2, 0}, {0, 1}, 30 * SECOND},
        // In no group: the device's slowest group, normal here.
        {0, {0, 1, NO_POLL_GROUP}, {2}, 3 * SECOND},
        // No group at all: the grace alone.
        {2 * SECOND, {NO_POLL_GROUP}, {0}, 2 * SECOND},
    };
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const Row& row = rows[index];
        EXPECT_EQ(identity_wait_ns(row.grace_ns,
                                   plan_of(row.in_groups, row.identity), all),
                  row.expected_ns)
            << index;
    }
    // Saturates.
    const std::vector<CompiledPollGroup> huge{
        {.name = "g", .period_ns = NEVER_NS / 2}};
    ReadPlan plan = plan_of({0}, {0});
    plan.by_group.resize(1);
    EXPECT_EQ(identity_wait_ns(0, plan, huge), NEVER_NS);
}

TEST(IdentitySetup, TheFullPeriodIsTheLongestGroup) {
    EXPECT_EQ(full_period_ns(groups()), 10 * SECOND);
    const std::vector<CompiledPollGroup> one{{.name = "g", .period_ns = 7}};
    EXPECT_EQ(full_period_ns(one), 7);
    EXPECT_EQ(full_period_ns({}), NEVER_NS);
}

TEST(IdentitySetup, EachStateHasItsSchedulerMode) {
    struct Row {
        DeviceStatus state;
        bool pending;
        DeviceMode expected;
    };
    const std::vector<Row> rows{
        {DeviceStatus::DISABLED, false, DeviceMode::OFF},
        {DeviceStatus::CONNECTING, true, DeviceMode::OFF},
        {DeviceStatus::DEMOTED, true, DeviceMode::OFF},
        {DeviceStatus::REFUSING, false, DeviceMode::OFF},
        {DeviceStatus::INITIALISING, true, DeviceMode::IDENTITY},
        {DeviceStatus::INITIALISING, false, DeviceMode::ALL},
        {DeviceStatus::RUNNING, false, DeviceMode::ALL},
        {DeviceStatus::RUNNING, true, DeviceMode::IDENTITY},
        {DeviceStatus::MISMATCH, false, DeviceMode::IDENTITY},
        {DeviceStatus::MISMATCH, true, DeviceMode::IDENTITY},
    };
    for (const Row& row : rows) {
        EXPECT_EQ(scheduler_mode(row.state, row.pending), row.expected)
            << static_cast<int>(row.state) << " " << row.pending;
    }
}

TEST(IdentitySetup, OnlyARunningDeviceTakesWrites) {
    // The interim strict default (D-147): no write, fail-safe ones
    // included, before the identity check passed or in MISMATCH.
    struct Row {
        DeviceStatus state;
        bool expected;
    };
    const std::vector<Row> rows{
        {DeviceStatus::DISABLED, false},     {DeviceStatus::CONNECTING, false},
        {DeviceStatus::INITIALISING, false}, {DeviceStatus::RUNNING, true},
        {DeviceStatus::DEMOTED, false},      {DeviceStatus::MISMATCH, false},
        {DeviceStatus::REFUSING, false},
    };
    for (const Row& row : rows) {
        EXPECT_EQ(accepts_writes(row.state), row.expected)
            << static_cast<int>(row.state);
    }
}

TEST(IdentitySetup, IdentityAndRestartPointsAreReadByTheCycle) {
    CompiledDevice device;
    IdentityCheck check;
    check.point = 1;
    device.identify = {check};
    device.restart_indication = {RestartRule{.point = 3}};
    EXPECT_FALSE(reads_identity(device, 0));
    EXPECT_TRUE(reads_identity(device, 1));
    EXPECT_FALSE(reads_identity(device, 2));
    EXPECT_TRUE(reads_identity(device, 3));
}

TEST(IdentitySetup, UnsetBoundsAreSafe) {
    // A tracker device left with the defaults: the check never becomes a
    // mismatch by the clock, and a restart re-arms the grace once only.
    const FailureTracker::Device device;
    EXPECT_EQ(device.identity_wait_ns, NEVER_NS);
    EXPECT_EQ(device.full_period_ns, NEVER_NS);
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

}  // namespace
}  // namespace scada::driver
