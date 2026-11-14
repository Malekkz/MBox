// Integration tests for scada-tagd: they start the real executable against
// a tag database in a temporary directory, with a listener standing in for
// systemd and a helper process standing in for another service.
//
// Every wait is on a condition: a message from the service, a line in its
// log, or its exit.

#include "scada/common/result.h"
#include "scada/service/notify_listener.h"
#include "scada/tagd/tagd.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal numbers are declared here, not in <csignal>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using scada::service::NotifyListener;
using scada::test::ChildProcess;

// Upper bound for one wait. It only matters when something is broken; a
// passing test returns as soon as the condition holds.
constexpr std::chrono::seconds WAIT_LIMIT{30};

// Times of the tag set the tests use, in ms. The heartbeat timeout of the
// process `io` is long against its heartbeat period, so that a busy machine
// does not make it look dead.
constexpr int CHECK_MS = 50;
constexpr int IO_TIMEOUT_MS = 1000;
constexpr int IO_PERIOD_MS = 50;
// Watchdog intervals given to the service; it feeds four times in one. The
// short one is for tests that read the feeds: every 50 ms. The long one is
// for all others: the stand-in for systemd holds only about 10 unread
// messages, and a service that fed often while the test waits for something
// else would fill it and lose its stop message.
constexpr std::chrono::milliseconds FAST_WATCHDOG{200};
constexpr std::chrono::minutes QUIET_WATCHDOG{10};
// Watchdog messages with the short interval that take longer together than
// the heartbeat timeout of `io`: 30 messages, at least 50 ms apart.
constexpr int FEEDS_BEYOND_IO_TIMEOUT = 30;

// Modes of the tag database contract, section 3.1.
constexpr mode_t BASE_DIR_MODE = 02750;
constexpr mode_t GENERATION_DIR_MODE = 03770;
constexpr mode_t REGION_FILE_MODE = 0640;
// The permission bits, the set-id bits and the sticky bit of a mode.
constexpr mode_t MODE_BITS = 07777;
// The mode of the base directory without its set-group-id bit.
constexpr mode_t NOT_SETGID_MODE = 0750;

// The command in the environment variable `variable`, set by CTest:
// elements separated by `|`, followed by `extra` as further arguments.
// Empty if the variable is not set.
std::vector<std::string> command_from(const char* variable,
                                      const std::vector<std::string>& extra) {
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
::testing::AssertionResult receives(NotifyListener& listener,
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

// Waits until the log of `tagd` holds `line` at or after position `from`.
::testing::AssertionResult logs(RunningTagd& tagd, std::string_view line,
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
::testing::AssertionResult feeds_watchdog(RunningTagd& tagd, int count) {
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
void stop(RunningTagd& tagd) {
    EXPECT_TRUE(tagd.child.send_signal(SIGTERM));
    EXPECT_TRUE(receives(tagd.listener, "STOPPING=1"));
    EXPECT_EQ(tagd.child.wait_for_exit(WAIT_LIMIT), scada::tagd::EXIT_OK)
        << tagd.child.err();
}

// Kills `process` with SIGKILL and waits until it is gone.
void kill(ChildProcess& process) {
    EXPECT_TRUE(process.send_signal(SIGKILL));
    EXPECT_EQ(process.wait_for_signal_exit(WAIT_LIMIT), SIGKILL);
}

// Asks `process` to stop and expects it to exit with code 0.
::testing::AssertionResult stops_cleanly(ChildProcess& process) {
    if (!process.send_signal(SIGTERM)) {
        return ::testing::AssertionFailure() << "the process is gone";
    }
    if (process.wait_for_exit(WAIT_LIMIT) != 0) {
        return ::testing::AssertionFailure()
               << "no clean exit: " << process.err();
    }
    return ::testing::AssertionSuccess();
}

bool attached(std::string_view out) {
    return out.find("attached\n") != std::string_view::npos;
}

mode_t mode_of(const std::filesystem::path& path) {
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0 ? info.st_mode & MODE_BITS : 0;
}

ino_t inode_of(const std::filesystem::path& path) {
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0 ? info.st_ino : 0;
}

// The log lines of scada-tagd about the process `io`.
constexpr std::string_view IO_UP = R"(process up PROCESS="io")";
constexpr std::string_view IO_NOT_ATTACHED =
    R"(process down PROCESS="io" REASON="region missing")";
constexpr std::string_view IO_HEARTBEAT_STALE =
    R"(process down PROCESS="io" REASON="heartbeat stale")";
constexpr std::string_view IO_NOT_RUNNING =
    R"(process down PROCESS="io" REASON="not running")";

class TagdService : public ::testing::Test {
protected:
    void SetUp() override {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "scada-tagd-it-XXXXXX")
                .string();
        ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
        root_ = pattern;
        base_ = root_ / "base";
        std::filesystem::create_directory(base_);
        // The owner and mode the platform gives /dev/shm/scada.
        ASSERT_EQ(::chmod(base_.c_str(), BASE_DIR_MODE), 0);
        write_tag_set(false);
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    // scada-tagd and a process `io` with one tag, both as the user of the
    // test. With `more_tags` it is another tag set. In name order `io` is
    // process 0.
    void write_tag_set(bool more_tags) const {
        const std::string uid = std::to_string(::geteuid());
        std::ofstream file(tag_set_path());
        file << R"({"format": 1, "heartbeat_check_ms": )" << CHECK_MS
             << R"(, "processes": [{"name": "tagd", "uid": )" << uid
             << R"(}, {"name": "io", "uid": )" << uid
             << R"(, "heartbeat_timeout_ms": )" << IO_TIMEOUT_MS
             << R"(}], "owners": [{"name": "io", "process": "io", )"
             << R"("change_ring_capacity": 1024}], "queues": [], )"
             << R"("tags": [{"name": "plant.level", "owner": "io", )"
             << R"("type": "float64"})";
        if (more_tags) {
            file << R"(, {"name": "plant.flow", "owner": "io", )"
                 << R"("type": "float64"})";
        }
        file << "]}\n";
    }

    [[nodiscard]] std::string tag_set_path() const {
        return (root_ / "tag-set.json").string();
    }

    [[nodiscard]] const std::filesystem::path& base() const { return base_; }

    [[nodiscard]] std::filesystem::path generation_one() const {
        return base_ / "gen-1";
    }

    [[nodiscard]] std::string current_target() const {
        std::error_code error;
        return std::filesystem::read_symlink(base_ / "current", error).string();
    }

    // Starts scada-tagd on the test's tag set and base directory, with the
    // watchdog interval `watchdog`. Nothing, with a failed expectation, if
    // it could not be started.
    [[nodiscard]] std::optional<RunningTagd> start_tagd(
        std::chrono::milliseconds watchdog = QUIET_WATCHDOG) const {
        scada::Result<NotifyListener> listener = NotifyListener::create();
        const std::vector<std::string> tagd = command_from(
            "SCADA_TAGD_COMMAND",
            {"--tag-set", tag_set_path(), "--base-dir", base_.string()});
        if (!listener.ok() || tagd.empty()) {
            ADD_FAILURE() << "no listener or no command for scada-tagd";
            return std::nullopt;
        }
        const std::string usec =
            std::to_string(std::chrono::microseconds(watchdog).count());
        // The `env` utility sets the variables, so the environment of the
        // test itself is not changed. Without JOURNAL_STREAM the log goes
        // to the standard error stream.
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
    [[nodiscard]] std::optional<RunningTagd> ready_tagd(
        std::chrono::milliseconds watchdog = QUIET_WATCHDOG) const {
        std::optional<RunningTagd> tagd = start_tagd(watchdog);
        if (tagd.has_value()) {
            EXPECT_TRUE(receives(tagd->listener, "READY=1"))
                << tagd->child.err();
        }
        return tagd;
    }

    // Starts the helper that attaches as the process `io` of generation 1
    // and writes its heartbeat, and waits until it has attached. Nothing,
    // with a failed expectation, if it could not be started.
    [[nodiscard]] std::optional<ChildProcess> attached_io() const {
        const std::vector<std::string> command = command_from(
            "SCADA_TAG_PEER_COMMAND", {generation_one().string(), "io", "0",
                                       "1", "0", std::to_string(IO_PERIOD_MS)});
        if (command.empty()) {
            return std::nullopt;
        }
        std::optional<ChildProcess> helper = ChildProcess::start(command);
        if (!helper.has_value()) {
            ADD_FAILURE() << "could not start the helper process";
            return std::nullopt;
        }
        EXPECT_TRUE(helper->wait_for_output(attached, WAIT_LIMIT))
            << helper->err();
        return helper;
    }

private:
    std::filesystem::path root_;
    std::filesystem::path base_;
};

TEST(TagdProcess, HelpPrintsTheUsageAndExitsWithSuccess) {
    const std::vector<std::string> command =
        command_from("SCADA_TAGD_COMMAND", {"--help"});
    ASSERT_FALSE(command.empty());
    std::optional<ChildProcess> child = ChildProcess::start(command);
    if (!child.has_value()) {
        FAIL() << "could not start " << command.front();
    }
    EXPECT_EQ(child->wait_for_exit(WAIT_LIMIT), scada::tagd::EXIT_OK);
    EXPECT_TRUE(child->out().starts_with("Usage: scada-tagd")) << child->out();
    EXPECT_EQ(child->err(), "");
}

TEST_F(TagdService, ReportsReadyOnceTheGenerationExists) {
    std::optional<RunningTagd> tagd = start_tagd();
    if (!tagd.has_value()) {
        return;
    }
    // Ready is the first message, and it comes after the generation is
    // there: other services start on it.
    EXPECT_EQ(tagd->listener.receive(WAIT_LIMIT), "READY=1")
        << tagd->child.err();
    EXPECT_EQ(current_target(), "gen-1");
    EXPECT_EQ(mode_of(generation_one()), GENERATION_DIR_MODE);
    EXPECT_EQ(mode_of(generation_one() / "header"), REGION_FILE_MODE);
    EXPECT_EQ(mode_of(generation_one() / "proc.tagd"), REGION_FILE_MODE);
    EXPECT_EQ(tagd->listener.receive(WAIT_LIMIT), "STATUS=Generation 1");
    stop(*tagd);
}

TEST_F(TagdService, FeedsTheWatchdogAndStopsOnSigterm) {
    std::optional<RunningTagd> tagd = ready_tagd(FAST_WATCHDOG);
    if (!tagd.has_value()) {
        return;
    }
    // The watchdog is fed from the loop that checks liveness.
    EXPECT_TRUE(feeds_watchdog(*tagd, 3));
    stop(*tagd);

    EXPECT_EQ(tagd->child.out(), "");
    EXPECT_NE(tagd->child.err().find(R"(generation created GENERATION="1")"),
              std::string::npos)
        << tagd->child.err();
    EXPECT_NE(tagd->child.err().find("stopped on request"), std::string::npos);
    // The generation stays for the other services.
    EXPECT_EQ(current_target(), "gen-1");
    EXPECT_TRUE(std::filesystem::exists(generation_one() / "header"));
}

// Conformance test 5, the part of scada-tagd, with real processes: an owner
// killed with SIGKILL is reported down once its heartbeat is older than its
// timeout.
TEST_F(TagdService, Conformance05KilledProcessIsMarkedDown) {
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    // Nothing has attached yet.
    EXPECT_TRUE(logs(*tagd, IO_NOT_ATTACHED));
    std::optional<ChildProcess> helper = attached_io();
    if (!helper.has_value()) {
        return;
    }
    EXPECT_TRUE(logs(*tagd, IO_UP));

    const std::size_t mark = tagd->child.err().size();
    kill(*helper);
    EXPECT_TRUE(logs(*tagd, IO_HEARTBEAT_STALE, mark));
    stop(*tagd);
}

// Conformance test 5, continued: the process is restarted, reuses its
// region and is reported up again.
TEST_F(TagdService, Conformance05RestartedProcessIsMarkedUpAgain) {
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> helper = attached_io();
    if (!tagd.has_value() || !helper.has_value()) {
        return;
    }
    kill(*helper);
    EXPECT_TRUE(logs(*tagd, IO_HEARTBEAT_STALE));

    const std::size_t mark = tagd->child.err().size();
    const std::optional<ChildProcess> again = attached_io();
    EXPECT_TRUE(logs(*tagd, IO_UP, mark));
    stop(*tagd);
}

// Conformance test 6, the part of scada-tagd: a process that stops cleanly
// is reported down as not running, without waiting for its timeout.
TEST_F(TagdService, Conformance06CleanlyStoppedProcessIsMarkedDown) {
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> helper = attached_io();
    if (!tagd.has_value() || !helper.has_value()) {
        return;
    }
    EXPECT_TRUE(logs(*tagd, IO_UP));

    EXPECT_TRUE(stops_cleanly(*helper));
    EXPECT_TRUE(logs(*tagd, IO_NOT_RUNNING));
    stop(*tagd);
}

// Conformance test 20: restarting scada-tagd with an unchanged tag set
// reuses the generation and replaces no file in it.
TEST_F(TagdService, Conformance20RestartWithTheSameTagSetReusesTheGeneration) {
    std::optional<RunningTagd> first = ready_tagd();
    const std::optional<ChildProcess> helper = attached_io();
    if (!first.has_value() || !helper.has_value()) {
        return;
    }
    const ino_t header = inode_of(generation_one() / "header");
    const ino_t region = inode_of(generation_one() / "proc.io");
    stop(*first);

    std::optional<RunningTagd> second = ready_tagd();
    if (!second.has_value()) {
        return;
    }
    EXPECT_TRUE(logs(*second, R"(generation reused GENERATION="1")"));
    EXPECT_EQ(current_target(), "gen-1");
    EXPECT_EQ(inode_of(generation_one() / "header"), header);
    EXPECT_EQ(inode_of(generation_one() / "proc.io"), region);
    stop(*second);
}

// Conformance test 20, continued: the other processes keep running without
// interruption.
TEST_F(TagdService, Conformance20RestartLeavesTheOtherProcessRunning) {
    std::optional<RunningTagd> first = ready_tagd();
    std::optional<ChildProcess> helper = attached_io();
    if (!first.has_value() || !helper.has_value()) {
        return;
    }
    EXPECT_TRUE(logs(*first, IO_UP));
    stop(*first);

    std::optional<RunningTagd> second = ready_tagd(FAST_WATCHDOG);
    if (!second.has_value()) {
        return;
    }
    // The liveness check runs for longer than the heartbeat timeout of
    // `io`: had the restart disturbed the process or its region, it would
    // be reported down in that time.
    EXPECT_TRUE(feeds_watchdog(*second, FEEDS_BEYOND_IO_TIMEOUT));
    stop(*second);
    EXPECT_EQ(second->child.err().find("process down"), std::string::npos)
        << second->child.err();
    // The helper never stopped: it still answers a stop request.
    EXPECT_TRUE(stops_cleanly(*helper));
}

TEST_F(TagdService, ChangedTagSetCreatesTheNextGeneration) {
    std::optional<RunningTagd> first = ready_tagd();
    if (!first.has_value()) {
        return;
    }
    stop(*first);

    write_tag_set(true);
    std::optional<RunningTagd> second = ready_tagd();
    if (!second.has_value()) {
        return;
    }
    EXPECT_EQ(current_target(), "gen-2");
    EXPECT_TRUE(logs(*second, R"(generation created GENERATION="2")"));
    EXPECT_TRUE(logs(*second, "gen-1 is set to RETIRING"));
    stop(*second);
}

TEST_F(TagdService, SecondInstanceIsRefused) {
    std::optional<RunningTagd> first = ready_tagd();
    std::optional<RunningTagd> second = start_tagd();
    if (!first.has_value() || !second.has_value()) {
        return;
    }
    EXPECT_EQ(second->child.wait_for_exit(WAIT_LIMIT), scada::tagd::EXIT_ERROR);
    EXPECT_NE(
        second->child.err().find("another scada-tagd uses the base directory"),
        std::string::npos)
        << second->child.err();
    // It never reported ready.
    EXPECT_EQ(second->listener.receive(std::chrono::milliseconds(0)),
              std::nullopt);
    stop(*first);
}

TEST_F(TagdService, BaseDirectoryWithTheWrongModeIsRefused) {
    ASSERT_EQ(::chmod(base().c_str(), NOT_SETGID_MODE), 0);
    std::optional<RunningTagd> tagd = start_tagd();
    if (!tagd.has_value()) {
        return;
    }
    EXPECT_EQ(tagd->child.wait_for_exit(WAIT_LIMIT), scada::tagd::EXIT_ERROR);
    EXPECT_NE(tagd->child.err().find("the base directory has the wrong mode"),
              std::string::npos)
        << tagd->child.err();
    EXPECT_TRUE(std::filesystem::is_empty(base()));
}

}  // namespace
