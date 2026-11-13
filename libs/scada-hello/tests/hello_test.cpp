// Unit test for libscada-hello. It uses no test framework: the framework is
// chosen in issue P0-03, and this file moves to it then.

#include "scada/hello/hello.h"

#include <array>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view what) {
    if (!condition) {
        std::cerr << "FAILED: " << what << '\n';
        ++failures;
    }
}

struct RunResult {
    int exit_code;
    std::string out;
    std::string err;
};

RunResult run_with(std::span<const std::string_view> args) {
    std::ostringstream out;
    std::ostringstream err;
    const int exit_code = scada::hello::run(args, out, err);
    return {exit_code, out.str(), err.str()};
}

}  // namespace

int main() {
    using scada::hello::EXIT_OK;
    using scada::hello::EXIT_USAGE;

    expect(scada::hello::greeting() == "Hello from libscada-hello",
           "greeting text");

    {
        const RunResult result = run_with({});
        expect(result.exit_code == EXIT_OK, "no arguments: exit code");
        expect(result.out == "Hello from libscada-hello\n",
               "no arguments: prints the greeting");
        expect(result.err.empty(), "no arguments: nothing on the error stream");
    }
    {
        const std::array<std::string_view, 1> args{"--help"};
        const RunResult result = run_with(args);
        expect(result.exit_code == EXIT_OK, "--help: exit code");
        expect(result.out.starts_with("Usage: scada-hello"),
               "--help: prints usage");
        expect(result.err.empty(), "--help: nothing on the error stream");
    }
    {
        const std::array<std::string_view, 1> args{"--bogus"};
        const RunResult result = run_with(args);
        expect(result.exit_code == EXIT_USAGE, "unknown option: exit code");
        expect(result.out.empty(), "unknown option: nothing on the output");
        expect(result.err.starts_with("scada-hello: unexpected arguments"),
               "unknown option: diagnostic");
    }
    {
        const std::array<std::string_view, 2> args{"--help", "extra"};
        const RunResult result = run_with(args);
        expect(result.exit_code == EXIT_USAGE, "surplus argument: exit code");
    }

    return failures == 0 ? 0 : 1;
}
