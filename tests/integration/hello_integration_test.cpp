// Integration tests for scada-hello: they start the real executable, so they
// also cover linking, start-up and the exit code seen by the caller.

#include "scada/hello/hello.h"
#include "scada/service/notify_listener.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal numbers are declared here, not in <csignal>
#include <sys/resource.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scada::service::NotifyListener;
using scada::test::ChildProcess;

// Upper bound for one wait. It only matters when something is broken; a
// passing test returns as soon as the condition holds.
constexpr std::chrono::seconds WAIT_LIMIT{30};

// The command that starts scada-hello, from the environment variable
// SCADA_HELLO_COMMAND set by CTest: elements separated by `|`. `extra` is
// appended as further arguments.
std::vector<std::string> hello_command(
    std::initializer_list<std::string_view> extra = {}) {
    std::vector<std::string> command;
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing changes the environment while the tests run
    const char* const value = std::getenv("SCADA_HELLO_COMMAND");
    if (value == nullptr) {
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

// Watchdog interval given to the service. It feeds at half of it.
constexpr std::chrono::milliseconds WATCHDOG_INTERVAL{200};
// How long the watchdog must stay silent to count as not fed: ten feed
// periods.
constexpr std::chrono::milliseconds SILENCE{1000};

// The command that starts scada-hello as a service, with NOTIFY_SOCKET set to
// `notify_socket`, the test's watchdog interval, and no WATCHDOG_PID. The
// `env` utility sets the variables, so the environment of the test itself is
// not changed. Empty if SCADA_HELLO_COMMAND is not set.
std::vector<std::string> service_command(const std::string& notify_socket) {
    const std::vector<std::string> hello = hello_command({"--service"});
    if (hello.size() < 2) {
        return {};
    }
    const std::string usec =
        std::to_string(std::chrono::microseconds(WATCHDOG_INTERVAL).count());
    std::vector<std::string> command{"env", "-u", "WATCHDOG_PID",
                                     "NOTIFY_SOCKET=" + notify_socket,
                                     "WATCHDOG_USEC=" + usec};
    command.insert(command.end(), hello.begin(), hello.end());
    return command;
}

// Receives messages until `wanted` arrives. Watchdog messages before it are
// skipped; any other message, or none within WAIT_LIMIT, fails.
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
        if (*message != "WATCHDOG=1") {
            return ::testing::AssertionFailure()
                   << "got '" << *message << "' while waiting for '" << wanted
                   << "'";
        }
    }
}

// The service as a running process, with the listener that stands in for
// systemd.
struct RunningService {
    NotifyListener listener;
    ChildProcess child;
};

// Starts scada-hello as a service. Returns nothing if the listener or the
// process could not be started, or SCADA_HELLO_COMMAND is not set.
std::optional<RunningService> start_service() {
    std::optional<NotifyListener> listener = NotifyListener::create();
    if (!listener.has_value()) {
        return std::nullopt;
    }
    const std::vector<std::string> command =
        service_command(listener->socket_name());
    if (command.empty()) {
        return std::nullopt;
    }
    std::optional<ChildProcess> child = ChildProcess::start(command);
    if (!child.has_value()) {
        return std::nullopt;
    }
    return RunningService{std::move(*listener), std::move(*child)};
}

// Hangs the main loop of `service` and waits until it has reported the hang,
// which is the last message it sends. The service must have reported
// readiness: before that it has not yet taken over its signals, and SIGUSR1
// would end it.
::testing::AssertionResult hang(RunningService& service) {
    if (!service.child.send_signal(SIGUSR1)) {
        return ::testing::AssertionFailure() << "could not send SIGUSR1";
    }
    return receives(service.listener,
                    "STATUS=" + std::string(scada::hello::HANG_STATUS));
}

// The service tests end a process with SIGABRT, as the watchdog does. That
// must not write a core file. The limit is inherited by the child processes.
class HelloService : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        const rlimit no_core{0, 0};
        ASSERT_EQ(::setrlimit(RLIMIT_CORE, &no_core), 0);
    }
};

bool contains_greeting(std::string_view out) {
    return out.find("Hello from libscada-hello\n") != std::string_view::npos;
}

TEST(HelloProcess, PrintsTheGreetingAndExitsWithSuccess) {
    const std::vector<std::string> command = hello_command();
    ASSERT_FALSE(command.empty()) << "SCADA_HELLO_COMMAND is not set";
    std::optional<ChildProcess> child = ChildProcess::start(command);
    if (!child.has_value()) {
        FAIL() << "could not start " << command.back();
    }

    EXPECT_TRUE(child->wait_for_output(contains_greeting, WAIT_LIMIT));
    // An exit code that is missing means: not exited in time, or killed.
    EXPECT_EQ(child->wait_for_exit(WAIT_LIMIT), scada::hello::EXIT_OK);
    EXPECT_EQ(child->out(), "Hello from libscada-hello\n");
    EXPECT_EQ(child->err(), "");
}

TEST(HelloProcess, RejectsAnUnknownOption) {
    const std::vector<std::string> command = hello_command({"--bogus"});
    ASSERT_GT(command.size(), 1U) << "SCADA_HELLO_COMMAND is not set";
    std::optional<ChildProcess> child = ChildProcess::start(command);
    if (!child.has_value()) {
        FAIL() << "could not start " << command.front();
    }

    EXPECT_EQ(child->wait_for_exit(WAIT_LIMIT), scada::hello::EXIT_USAGE);
    EXPECT_EQ(child->out(), "");
    EXPECT_TRUE(child->err().starts_with("scada-hello: unexpected arguments"))
        << child->err();
}

TEST_F(HelloService, ReportsReadyFirstAndThenFeedsTheWatchdog) {
    std::optional<RunningService> service = start_service();
    if (!service.has_value()) {
        FAIL() << "could not start the service";
    }
    EXPECT_EQ(service->listener.receive(WAIT_LIMIT), "READY=1");
    EXPECT_EQ(service->listener.receive(WAIT_LIMIT), "WATCHDOG=1");
    EXPECT_EQ(service->listener.receive(WAIT_LIMIT), "WATCHDOG=1");
}

TEST_F(HelloService, StopsOnSigterm) {
    std::optional<RunningService> service = start_service();
    if (!service.has_value()) {
        FAIL() << "could not start the service";
    }
    EXPECT_TRUE(receives(service->listener, "READY=1"));

    EXPECT_TRUE(service->child.send_signal(SIGTERM));
    EXPECT_TRUE(receives(service->listener, "STOPPING=1"));
    EXPECT_EQ(service->child.wait_for_exit(WAIT_LIMIT), scada::hello::EXIT_OK);
    EXPECT_EQ(service->child.out(), "Hello from libscada-hello\n");
    EXPECT_EQ(service->child.err(), "");
}

TEST_F(HelloService, StopsFeedingTheWatchdogWhenHung) {
    std::optional<RunningService> service = start_service();
    if (!service.has_value()) {
        FAIL() << "could not start the service";
    }
    EXPECT_EQ(service->listener.receive(WAIT_LIMIT), "READY=1");
    EXPECT_EQ(service->listener.receive(WAIT_LIMIT), "WATCHDOG=1");

    EXPECT_TRUE(hang(*service));
    // The status is the last thing the service sends, so from here on any
    // message would be a watchdog feed, which must not happen.
    EXPECT_EQ(service->listener.receive(SILENCE), std::nullopt);
}

TEST_F(HelloService, WhenHungIgnoresSigtermAndIsEndedByTheWatchdogSignal) {
    std::optional<RunningService> service = start_service();
    if (!service.has_value()) {
        FAIL() << "could not start the service";
    }
    EXPECT_TRUE(receives(service->listener, "READY=1"));
    EXPECT_TRUE(hang(*service));

    // A hung process does not react to SIGTERM.
    EXPECT_TRUE(service->child.send_signal(SIGTERM));
    EXPECT_EQ(service->child.wait_for_signal_exit(SILENCE), std::nullopt);

    // systemd ends a service that missed its watchdog with SIGABRT.
    EXPECT_TRUE(service->child.send_signal(SIGABRT));
    EXPECT_EQ(service->child.wait_for_signal_exit(WAIT_LIMIT), SIGABRT);
}

TEST_F(HelloService, RefusesToStartOnAnInvalidNotifySocket) {
    const std::vector<std::string> command =
        service_command("not-a-socket-name");
    ASSERT_FALSE(command.empty()) << "SCADA_HELLO_COMMAND is not set";
    std::optional<ChildProcess> child = ChildProcess::start(command);
    if (!child.has_value()) {
        FAIL() << "could not start " << command.back();
    }
    EXPECT_EQ(child->wait_for_exit(WAIT_LIMIT), scada::hello::EXIT_ERROR);
    EXPECT_EQ(child->out(), "");
    EXPECT_TRUE(child->err().starts_with("scada-hello: invalid"))
        << child->err();
}

TEST(ChildProcess, ReportsAProgramThatCannotBeStarted) {
    EXPECT_FALSE(ChildProcess::start({}).has_value());

    const std::vector<std::string> command{
        "/nonexistent/scada-no-such-program"};
    std::optional<ChildProcess> child = ChildProcess::start(command);
    if (child.has_value()) {
        // Under a user-mode emulator the failure is only visible as the exit
        // code that the C library gives a failed exec.
        constexpr int EXEC_FAILED = 127;
        EXPECT_EQ(child->wait_for_exit(WAIT_LIMIT), EXEC_FAILED);
    }
}

}  // namespace
