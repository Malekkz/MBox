// Unit tests for libscada-hello.

#include "scada/hello/hello.h"

#include <gtest/gtest.h>

#include <array>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using scada::hello::EXIT_OK;
using scada::hello::EXIT_USAGE;

struct RunResult {
    int exit_code = 0;
    std::string out;
    std::string err;
};

RunResult run_with(std::span<const std::string_view> args) {
    std::ostringstream out;
    std::ostringstream err;
    const int exit_code = scada::hello::run(args, out, err);
    return {exit_code, out.str(), err.str()};
}

TEST(Hello, GreetingText) {
    EXPECT_EQ(scada::hello::greeting(), "Hello from libscada-hello");
}

TEST(Hello, NoArgumentsPrintsTheGreeting) {
    const RunResult result = run_with({});
    EXPECT_EQ(result.exit_code, EXIT_OK);
    EXPECT_EQ(result.out, "Hello from libscada-hello\n");
    EXPECT_TRUE(result.err.empty());
}

TEST(Hello, HelpPrintsUsage) {
    const std::array<std::string_view, 1> args{"--help"};
    const RunResult result = run_with(args);
    EXPECT_EQ(result.exit_code, EXIT_OK);
    EXPECT_TRUE(result.out.starts_with("Usage: scada-hello"));
    EXPECT_TRUE(result.err.empty());
}

TEST(Hello, UnknownOptionIsAUsageError) {
    const std::array<std::string_view, 1> args{"--bogus"};
    const RunResult result = run_with(args);
    EXPECT_EQ(result.exit_code, EXIT_USAGE);
    EXPECT_TRUE(result.out.empty());
    EXPECT_TRUE(result.err.starts_with("scada-hello: unexpected arguments"));
}

TEST(Hello, SurplusArgumentIsAUsageError) {
    const std::array<std::string_view, 2> args{"--help", "extra"};
    const RunResult result = run_with(args);
    EXPECT_EQ(result.exit_code, EXIT_USAGE);
}

}  // namespace

// TEMPORARY, removed before merge: proves that the pipeline rejects a
// failing test (issue #5, acceptance).
namespace {
TEST(PipelineProof, FailingTest) {
    EXPECT_EQ(scada::hello::greeting(), "not the greeting");
}
}  // namespace
