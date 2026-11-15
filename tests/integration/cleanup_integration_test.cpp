// A killed test binary leaves no process and no directory behind (issue
// #64, decision D-48): the children of ChildProcess die with the test
// process, and the cleaner of TempDirectory removes its directory.

#include "tagd_fixture.h"

#include "scada/test/child_process.h"
#include "scada/test/temp_directory.h"

#include <gtest/gtest.h>

#include <sys/poll.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace scada::test::tagd {
namespace {

using scada::test::TempDirectory;
using scada::test::UniqueFd;

// scada-tagd and the two simulator processes.
constexpr std::size_t RUNNER_PROCESSES = 3;

// A descriptor that becomes readable when the process `pid` ends, whether
// or not it is a child of the test. Invalid if the process is already gone.
UniqueFd watch_process(int pid) {
    const auto pidfd =
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): syscall has no other form
        ::syscall(SYS_pidfd_open, pid, 0U);
    return UniqueFd(static_cast<int>(pidfd));
}

// Waits until the process watched by `pidfd` has ended.
::testing::AssertionResult ends(const UniqueFd& pidfd) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point deadline = Clock::now() + WAIT_LIMIT;
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now());
        if (left.count() < 0) {
            return ::testing::AssertionFailure() << "still running";
        }
        pollfd wait{.fd = pidfd.get(), .events = POLLIN, .revents = 0};
        const int ready = ::poll(&wait, 1, static_cast<int>(left.count()) + 1);
        if (ready > 0) {
            return ::testing::AssertionSuccess();
        }
        if (ready < 0 && errno != EINTR) {
            return ::testing::AssertionFailure() << "poll failed";
        }
    }
}

std::optional<int> parse_pid(std::string_view text) {
    int pid = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), pid);
    if (error != std::errc{} || end != text.data() + text.size() || pid <= 0) {
        return std::nullopt;
    }
    return pid;
}

// What the killed runner printed.
struct RunnerReport {
    std::filesystem::path directory;
    std::optional<int> cleaner;
    std::vector<int> processes;
    bool complete = false;
};

// The rest of `line` after `prefix`, or nothing if it does not start so.
std::optional<std::string_view> after(std::string_view line,
                                      std::string_view prefix) {
    if (!line.starts_with(prefix)) {
        return std::nullopt;
    }
    return line.substr(prefix.size());
}

RunnerReport parse_report(std::string_view out) {
    RunnerReport report;
    while (!out.empty()) {
        const std::size_t end = out.find('\n');
        const std::string_view line = out.substr(0, end);
        out = end == std::string_view::npos ? std::string_view{}
                                            : out.substr(end + 1);
        if (const auto directory = after(line, "directory ")) {
            report.directory = std::string(*directory);
        } else if (const auto cleaner = after(line, "cleaner ")) {
            report.cleaner = parse_pid(*cleaner);
        } else if (const auto process = after(line, "process ")) {
            if (const std::optional<int> pid = parse_pid(*process)) {
                report.processes.push_back(*pid);
            }
        } else if (line == "ready") {
            report.complete = true;
        }
    }
    return report;
}

// Waits until `runner` has started its processes, and reads what it
// printed into `report`.
::testing::AssertionResult reports(ChildProcess& runner, RunnerReport& report) {
    const bool ready = runner.wait_for_output(
        [](std::string_view out) {
            return out.find("\nready\n") != std::string_view::npos;
        },
        WAIT_LIMIT);
    if (!ready) {
        return ::testing::AssertionFailure()
               << "no report: " << runner.out() << runner.err();
    }
    report = parse_report(runner.out());
    if (!report.complete || !report.cleaner.has_value() ||
        report.processes.size() != RUNNER_PROCESSES ||
        !std::filesystem::is_directory(report.directory)) {
        return ::testing::AssertionFailure()
               << "an incomplete report: " << runner.out();
    }
    return ::testing::AssertionSuccess();
}

// Watches every process of `report` and its cleaner, appending to
// `watched`. Fails if one of them is not running.
::testing::AssertionResult watch_all(const RunnerReport& report,
                                     std::vector<UniqueFd>& watched) {
    std::vector<int> pids = report.processes;
    pids.push_back(report.cleaner.value_or(-1));
    for (const int pid : pids) {
        watched.push_back(watch_process(pid));
        if (!watched.back().valid()) {
            return ::testing::AssertionFailure()
                   << "process " << pid << " is not running";
        }
    }
    return ::testing::AssertionSuccess();
}

TEST(KilledTestBinary, LeavesNoProcessAndNoDirectory) {
    const std::vector<std::string> command =
        command_from("SCADA_KILLED_RUNNER_COMMAND", {});
    if (command.empty()) {
        return;
    }
    std::optional<ChildProcess> runner = ChildProcess::start(command);
    if (!runner.has_value()) {
        FAIL() << "could not start " << command.front();
    }
    RunnerReport report;
    ASSERT_TRUE(reports(*runner, report));
    // Watched while the runner still waits, so each is known to be alive.
    // The cleaner is the last.
    std::vector<UniqueFd> watched;
    ASSERT_TRUE(watch_all(report, watched));

    kill(*runner);

    // The cleaner ends once it has removed the directory.
    for (const UniqueFd& process : watched) {
        EXPECT_TRUE(ends(process));
    }
    std::error_code error;
    EXPECT_FALSE(std::filesystem::exists(report.directory, error))
        << report.directory;
    EXPECT_FALSE(error) << error.message();
}

TEST(TempDirectory, IsRemovedAndItsCleanerEndsWhenDestroyed) {
    std::filesystem::path path;
    UniqueFd cleaner;
    {
        const std::optional<TempDirectory> temp =
            TempDirectory::create("scada-cleanup-it-");
        if (!temp.has_value()) {
            FAIL() << "no temporary directory";
        }
        path = temp->path();
        EXPECT_TRUE(path.is_absolute());
        EXPECT_TRUE(path.filename().string().starts_with("scada-cleanup-it-"));
        EXPECT_TRUE(std::filesystem::is_directory(path));
        cleaner = watch_process(temp->cleaner_pid());
        ASSERT_TRUE(cleaner.valid());
        std::filesystem::create_directory(path / "sub");
    }
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_TRUE(ends(cleaner));
}

TEST(TempDirectory, NamesAreUnique) {
    const std::optional<TempDirectory> first =
        TempDirectory::create("scada-cleanup-it-");
    const std::optional<TempDirectory> second =
        TempDirectory::create("scada-cleanup-it-");
    if (!first.has_value() || !second.has_value()) {
        FAIL() << "no temporary directory";
    }
    EXPECT_NE(first->path(), second->path());
    EXPECT_NE(first->cleaner_pid(), second->cleaner_pid());
}

}  // namespace
}  // namespace scada::test::tagd
