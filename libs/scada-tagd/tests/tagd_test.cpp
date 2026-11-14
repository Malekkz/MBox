// Unit tests for libscada-tagd: the command line and the errors found
// before the service starts. The running service is covered by the
// integration test tests/integration/tagd_integration_test.cpp.

#include "scada/tagd/tagd.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using scada::tagd::EXIT_ERROR;
using scada::tagd::EXIT_OK;
using scada::tagd::EXIT_USAGE;

struct RunResult {
    int exit_code = 0;
    std::string out;
    std::string err;
};

RunResult run_with(std::initializer_list<std::string_view> args) {
    const std::vector<std::string_view> list(args);
    std::ostringstream out;
    std::ostringstream err;
    const int exit_code = scada::tagd::run(list, out, err);
    return {exit_code, out.str(), err.str()};
}

// A directory of its own for one test, removed afterwards.
class TagdCommand : public ::testing::Test {
protected:
    void SetUp() override {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "scada-tagd-test-XXXXXX")
                .string();
        ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
        directory_ = pattern;
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    // Writes `text` to a file in the directory and returns its path.
    [[nodiscard]] std::string write_file(std::string_view text) const {
        const std::filesystem::path path = directory_ / "tag-set.json";
        std::ofstream(path) << text;
        return path.string();
    }

    [[nodiscard]] const std::filesystem::path& directory() const {
        return directory_;
    }

private:
    std::filesystem::path directory_;
};

TEST(TagdUsage, HelpDocumentsEveryOptionAndExitCode) {
    const RunResult result = run_with({"--help"});
    EXPECT_EQ(result.exit_code, EXIT_OK);
    EXPECT_TRUE(result.err.empty());
    for (const std::string_view word :
         {"--tag-set", "--base-dir", "--help", "SIGTERM", "NOTIFY_SOCKET",
          "WATCHDOG_USEC", "/etc/scada/tag-set.json", "/dev/shm/scada",
          "Exit codes"}) {
        EXPECT_NE(result.out.find(word), std::string::npos) << word;
    }
}

TEST(TagdUsage, RejectsUnknownRepeatedAndIncompleteArguments) {
    for (const std::initializer_list<std::string_view> args :
         {std::initializer_list<std::string_view>{"--bogus"},
          {"--help", "--help"},
          {"--help", "--tag-set", "x"},
          {"--tag-set"},
          {"--tag-set", ""},
          {"--base-dir"},
          {"--tag-set", "a", "--tag-set", "b"},
          {"--base-dir", "a", "--base-dir", "b"},
          {"file.json"}}) {
        const RunResult result = run_with(args);
        EXPECT_EQ(result.exit_code, EXIT_USAGE);
        EXPECT_TRUE(result.out.empty());
        EXPECT_TRUE(
            result.err.starts_with("scada-tagd: unexpected arguments\n"))
            << result.err;
    }
}

TEST_F(TagdCommand, MissingTagSetFileIsAnError) {
    const std::string path = (directory() / "none.json").string();
    const RunResult result = run_with({"--tag-set", path});
    EXPECT_EQ(result.exit_code, EXIT_ERROR);
    EXPECT_NE(result.err.find("cannot read the tag set"), std::string::npos)
        << result.err;
    EXPECT_NE(result.err.find(path), std::string::npos) << result.err;
}

TEST_F(TagdCommand, TagSetThatIsADirectoryIsAnError) {
    const RunResult result = run_with({"--tag-set", directory().string()});
    EXPECT_EQ(result.exit_code, EXIT_ERROR);
    EXPECT_NE(result.err.find("not a regular file"), std::string::npos)
        << result.err;
}

TEST_F(TagdCommand, MalformedTagSetIsReportedWithItsPosition) {
    const std::string path = write_file("{\"format\": 1,\n  \"bogus\": 2}");
    const RunResult result = run_with({"--tag-set", path});
    EXPECT_EQ(result.exit_code, EXIT_ERROR);
    EXPECT_NE(result.err.find("the tag-set file is not valid"),
              std::string::npos)
        << result.err;
    EXPECT_NE(result.err.find("line 2"), std::string::npos) << result.err;
}

TEST_F(TagdCommand, TagSetWithoutTheProcessTagdIsAnError) {
    const std::string path = write_file(
        R"({"format": 1, "processes": [{"name": "io", "uid": 1}],
            "owners": [], "queues": [], "tags": []})");
    const std::string base = (directory() / "base").string();
    const RunResult result = run_with({"--tag-set", path, "--base-dir", base});
    EXPECT_EQ(result.exit_code, EXIT_ERROR);
    EXPECT_NE(result.err.find("no process named tagd"), std::string::npos)
        << result.err;
    // Nothing was created.
    EXPECT_FALSE(std::filesystem::exists(base));
}

TEST_F(TagdCommand, MissingBaseDirectoryIsAnError) {
    const std::string path =
        write_file(R"({"format": 1, "processes": [{"name": "tagd", "uid": )" +
                   std::to_string(::geteuid()) +
                   R"(}], "owners": [], "queues": [], "tags": []})");
    const std::string base = (directory() / "base").string();
    const RunResult result = run_with({"--tag-set", path, "--base-dir", base});
    EXPECT_EQ(result.exit_code, EXIT_ERROR);
    EXPECT_NE(result.err.find("the base directory does not exist"),
              std::string::npos)
        << result.err;
    EXPECT_FALSE(std::filesystem::exists(base));
}

}  // namespace
