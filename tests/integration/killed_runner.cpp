// scada-test-killed-runner: a test binary that the cleanup test kills
// (issue #64, decision D-48). Its one test starts scada-tagd and two
// simulator processes in the temporary directory of the fixture, prints
// their process ids, the directory and its cleaner, and then waits to be
// killed. Not a CTest test itself: on its own it would only wait.

#include "tag_processes_fixture.h"

#include "scada/sim/layout.h"
#include "scada/sim/pattern.h"
#include "scada/test/child_process.h"
#include "tagd_fixture.h"

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <optional>

namespace scada::test::tagd {
namespace {

// Longer than any test limit: the cleanup test kills the binary long
// before.
constexpr std::chrono::minutes UNTIL_KILLED{10};

class KilledRunner : public TagProcesses {};

TEST_F(KilledRunner, StartsProcessesAndWaitsToBeKilled) {
    const scada::sim::Layout layout{.prefix = "sim",
                                    .processes = 2,
                                    .owners = 2,
                                    .tags = 8,
                                    .patterns = {scada::sim::Pattern::CHECK}};
    write_sim_tag_set(layout);
    std::optional<RunningTagd> tagd = ready_tagd();
    // Without --rounds or --seconds: each runs until it is stopped.
    std::optional<ChildProcess> first = start_sim(layout, {}, 0);
    std::optional<ChildProcess> second = start_sim(layout, {}, 1);
    // Each has added a failure if it did not start.
    if (!tagd.has_value() || !first.has_value() || !second.has_value()) {
        return;
    }

    std::cout << "directory " << root().string() << "\n"
              << "cleaner " << cleaner_pid() << "\n"
              << "process " << tagd->child.pid() << "\n"
              << "process " << first->pid() << "\n"
              << "process " << second->pid() << "\n"
              << "ready\n"
              << std::flush;
    EXPECT_FALSE(tagd->child.wait_for_exit(UNTIL_KILLED).has_value());
}

}  // namespace
}  // namespace scada::test::tagd
