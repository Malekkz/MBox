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

// Counts failed expectations and reports each one.
class Checker {
public:
    void expect(bool condition, std::string_view what) {
        if (!condition) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures_;
        }
    }

    [[nodiscard]] bool passed() const { return failures_ == 0; }

private:
    int failures_ = 0;
};

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

}  // namespace

int main() {
    using scada::hello::EXIT_OK;
    using scada::hello::EXIT_USAGE;

    Checker checker;
    const auto expect = [&checker](bool condition, std::string_view what) {
        checker.expect(condition, what);
    };

    expect(scada::hello::greeting() == "Hello from libscada-hello",
           "greeting text");

    // Running with no arguments starts the service loop, which needs a real
    // process and signals; services/scada-hello/tests covers it.
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

    return checker.passed() ? 0 : 1;
}
