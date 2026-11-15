#ifndef SCADA_TESTS_INTEGRATION_TAGD_FIXTURE_H
#define SCADA_TESTS_INTEGRATION_TAGD_FIXTURE_H

// What the integration tests of the tag database share: scada-tagd as a
// running process with a listener standing in for systemd, and waits on
// what a process prints or logs. Every wait is on a condition with a
// limit.

#include "scada/common/result.h"
#include "scada/service/notify_listener.h"
#include "scada/tagd/tagd.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal numbers are declared here, not in <csignal>

#include <sys/stat.h>

#include <chrono>
#include <filesystem>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::test::tagd {

using scada::service::NotifyListener;
using scada::test::ChildProcess;

// Upper bound for one wait. It only matters when something is broken; a
// passing test returns as soon as the condition holds.
inline constexpr std::chrono::seconds WAIT_LIMIT{30};

// The watchdog interval for tests that do not read the feeds: the stand-in
// for systemd holds only about 10 unread messages, and a service that fed
// often while the test waits for something else would fill it and lose its
// stop message.
inline constexpr std::chrono::minutes QUIET_WATCHDOG{10};

// The command in the environment variable `variable`, set by CTest:
// elements separated by `|`, followed by `extra` as further arguments.
// Empty if the variable is not set.
inline std::vector<std::string> command_from(
    const char* variable, const std::vector<std::string>& extra) {
    std::vector<std::string> command;
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing changes the environment while the tests run
    const char* const value = std::getenv(variable);
    if (value == nullptr) {
        ADD_FAILURE() << variable << " is not set";
        return command;
    }
    std::string_view rest(value);
    while (!rest.empty()) {
        const std::size_t end = rest.find('|');
        command.emplace_back(rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view{}
                                             : rest.substr(end + 1);
    }
    command.insert(command.end(), extra.begin(), extra.end());
    return command;
}

// Receives messages until `wanted` arrives. Watchdog and status messages
// before it are skipped; any other message, or none within WAIT_LIMIT,
// fails.
inline ::testing::AssertionResult receives(NotifyListener& listener,
                                           std::string_view wanted) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point deadline = Clock::now() + WAIT_LIMIT;
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now());
        const std::optional<std::string> message = listener.receive(left);
        if (!message.has_value()) {
            return ::testing::AssertionFailure()
                   << "no '" << wanted << "' within the time limit";
        }
        if (*message == wanted) {
            return ::testing::AssertionSuccess();
        }
        if (*message != "WATCHDOG=1" && !message->starts_with("STATUS=")) {
            return ::testing::AssertionFailure()
                   << "got '" << *message << "' while waiting for '" << wanted
                   << "'";
        }
    }
}

// scada-tagd as a running process, with the listener that stands in for
// systemd.
struct RunningTagd {
    NotifyListener listener;
    ChildProcess child;
};

// Waits until the output of `process` holds `text`.
inline ::testing::AssertionResult prints(ChildProcess& process,
                                         std::string_view text) {
    const bool found = process.wait_for_output(
        [&](std::string_view out) {
            return out.find(text) != std::string_view::npos;
        },
        WAIT_LIMIT);
    return found ? ::testing::AssertionSuccess()
                 : ::testing::AssertionFailure()
                       << "no '" << text << "' in: " << process.out()
                       << process.err();
}

// Waits until the log of `tagd` holds `line` at or after position `from`.
inline ::testing::AssertionResult logs(RunningTagd& tagd, std::string_view line,
                                       std::size_t from = 0) {
    const bool found = tagd.child.wait_for_error(
        [&](std::string_view err) {
            return err.find(line, from) != std::string_view::npos;
        },
        WAIT_LIMIT);
    if (found) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "no '" << line << "' in the log:\n"
                                         << tagd.child.err();
}

// Receives `count` watchdog messages. The loop of `tagd` has then run for
// at least `count` feed periods.
inline ::testing::AssertionResult feeds_watchdog(RunningTagd& tagd, int count) {
    for (int feed = 0; feed < count; ++feed) {
        const ::testing::AssertionResult fed =
            receives(tagd.listener, "WATCHDOG=1");
        if (!fed) {
            return fed;
        }
    }
    return ::testing::AssertionSuccess();
}

// Stops scada-tagd and expects a clean exit.
inline void stop(RunningTagd& tagd) {
    EXPECT_TRUE(tagd.child.send_signal(SIGTERM));
    EXPECT_TRUE(receives(tagd.listener, "STOPPING=1"));
    EXPECT_EQ(tagd.child.wait_for_exit(WAIT_LIMIT), scada::tagd::EXIT_OK)
        << tagd.child.err();
}

// Kills `process` with SIGKILL and waits until it is gone.
inline void kill(ChildProcess& process) {
    EXPECT_TRUE(process.send_signal(SIGKILL));
    EXPECT_EQ(process.wait_for_signal_exit(WAIT_LIMIT), SIGKILL);
}

// Asks `process` to stop and expects it to exit with code 0.
inline ::testing::AssertionResult stops_cleanly(ChildProcess& process) {
    if (!process.send_signal(SIGTERM)) {
        return ::testing::AssertionFailure() << "the process is gone";
    }
    if (process.wait_for_exit(WAIT_LIMIT) != 0) {
        return ::testing::AssertionFailure()
               << "no clean exit: " << process.err();
    }
    return ::testing::AssertionSuccess();
}

inline bool attached(std::string_view out) {
    return out.find("attached\n") != std::string_view::npos;
}

// The retain directory of a test whose base directory is `base_dir`:
// `retain` beside it, created with the owner and mode scada-tagd requires
// (mode 2750) if it is not there.
[[nodiscard]] inline std::string retain_dir_for(const std::string& base_dir) {
    const std::filesystem::path retain =
        std::filesystem::path(base_dir).parent_path() / "retain";
    constexpr mode_t RETAIN_MODE = 02750;
    constexpr mode_t PRIVATE_MODE = 0700;
    if (::mkdir(retain.c_str(), PRIVATE_MODE) == 0) {
        EXPECT_EQ(::chmod(retain.c_str(), RETAIN_MODE), 0) << retain;
    }
    return retain.string();
}

// Starts scada-tagd on the tag set file `tag_set_path` and the base
// directory `base_dir`, with the retain directory of `retain_dir_for`, the
// watchdog interval `watchdog` and the arguments `extra` added. Nothing,
// with a failed expectation, if it could not be started.
[[nodiscard]] inline std::optional<RunningTagd> start_tagd(
    const std::string& tag_set_path, const std::string& base_dir,
    std::chrono::milliseconds watchdog = QUIET_WATCHDOG,
    const std::vector<std::string>& extra = {}) {
    scada::Result<NotifyListener> listener = NotifyListener::create();
    std::vector<std::string> arguments{"--tag-set", tag_set_path, "--base-dir",
                                       base_dir, "--retain-dir",
                                       retain_dir_for(base_dir)};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    const std::vector<std::string> tagd =
        command_from("SCADA_TAGD_COMMAND", arguments);
    if (!listener.ok() || tagd.empty()) {
        ADD_FAILURE() << "no listener or no command for scada-tagd";
        return std::nullopt;
    }
    const std::string usec =
        std::to_string(std::chrono::microseconds(watchdog).count());
    // The `env` utility sets the variables, so the environment of the test
    // itself is not changed. Without JOURNAL_STREAM the log goes to the
    // standard error stream.
    std::vector<std::string> command{
        "env",
        "-u",
        "WATCHDOG_PID",
        "-u",
        "JOURNAL_STREAM",
        "NOTIFY_SOCKET=" + listener.value().socket_name(),
        "WATCHDOG_USEC=" + usec};
    command.insert(command.end(), tagd.begin(), tagd.end());
    std::optional<ChildProcess> child = ChildProcess::start(command);
    if (!child.has_value()) {
        ADD_FAILURE() << "could not start scada-tagd";
        return std::nullopt;
    }
    return RunningTagd{std::move(listener).value(), std::move(*child)};
}

// Starts scada-tagd and waits until it has reported readiness.
[[nodiscard]] inline std::optional<RunningTagd> ready_tagd(
    const std::string& tag_set_path, const std::string& base_dir,
    std::chrono::milliseconds watchdog = QUIET_WATCHDOG,
    const std::vector<std::string>& extra = {}) {
    std::optional<RunningTagd> tagd =
        start_tagd(tag_set_path, base_dir, watchdog, extra);
    if (tagd.has_value()) {
        EXPECT_TRUE(receives(tagd->listener, "READY=1")) << tagd->child.err();
    }
    return tagd;
}

}  // namespace scada::test::tagd

#endif  // SCADA_TESTS_INTEGRATION_TAGD_FIXTURE_H
