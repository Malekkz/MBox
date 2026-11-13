// Integration tests for scada-hello: they start the real executable, so they
// also cover linking, start-up and the exit code seen by the caller.

#include "scada/hello/hello.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

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

TEST(HelloProcess, PrintsTheGreetingAndExitsWithSuccess) {
    const std::vector<std::string> command = hello_command();
    ASSERT_FALSE(command.empty()) << "SCADA_HELLO_COMMAND is not set";
    std::optional<ChildProcess> child = ChildProcess::start(command);
    ASSERT_TRUE(child.has_value()) << "could not start " << command.back();

    const bool greeted = child->wait_for_output(
        [](std::string_view out) {
            return out.find("Hello from libscada-hello\n") !=
                   std::string_view::npos;
        },
        WAIT_LIMIT);
    EXPECT_TRUE(greeted) << "output so far: " << child->out();

    const std::optional<int> exit_code = child->wait_for_exit(WAIT_LIMIT);
    ASSERT_TRUE(exit_code.has_value()) << "did not exit normally in time";
    EXPECT_EQ(*exit_code, scada::hello::EXIT_OK);
    EXPECT_EQ(child->out(), "Hello from libscada-hello\n");
    EXPECT_TRUE(child->err().empty()) << child->err();
}

TEST(HelloProcess, RejectsAnUnknownOption) {
    const std::vector<std::string> command = hello_command({"--bogus"});
    ASSERT_GT(command.size(), 1U) << "SCADA_HELLO_COMMAND is not set";
    std::optional<ChildProcess> child = ChildProcess::start(command);
    ASSERT_TRUE(child.has_value());

    const std::optional<int> exit_code = child->wait_for_exit(WAIT_LIMIT);
    ASSERT_TRUE(exit_code.has_value()) << "did not exit normally in time";
    EXPECT_EQ(*exit_code, scada::hello::EXIT_USAGE);
    EXPECT_TRUE(child->out().empty()) << child->out();
    EXPECT_TRUE(child->err().starts_with("scada-hello: unexpected arguments"))
        << child->err();
}

TEST(ChildProcess, ReportsAProgramThatCannotBeStarted) {
    EXPECT_FALSE(ChildProcess::start({}).has_value());

    const std::vector<std::string> command{"/nonexistent/scada-no-such-program"};
    std::optional<ChildProcess> child = ChildProcess::start(command);
    if (child.has_value()) {
        // Under a user-mode emulator the failure is only visible as the exit
        // code that the C library gives a failed exec.
        constexpr int EXEC_FAILED = 127;
        EXPECT_EQ(child->wait_for_exit(WAIT_LIMIT), EXEC_FAILED);
    }
}

}  // namespace
