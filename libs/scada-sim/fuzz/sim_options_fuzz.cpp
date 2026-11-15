// Fuzz target for the command line of scada-sim, which is input from outside
// the process. The input is split at NUL bytes into arguments.

#include "scada/sim/sim.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

// More arguments than any valid command line has.
constexpr std::size_t MAX_ARGUMENTS = 256;

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::span<const std::uint8_t> input(data, size);
    std::string text;
    text.reserve(input.size());
    for (const std::uint8_t byte : input) {
        text.push_back(static_cast<char>(byte));
    }
    std::vector<std::string_view> args;
    std::string_view rest(text);
    while (!rest.empty() && args.size() < MAX_ARGUMENTS) {
        const std::size_t end = rest.find('\0');
        args.push_back(rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view()
                                             : rest.substr(end + 1);
    }
    std::string detail;
    // Rejecting input is correct behaviour; the result is not needed.
    static_cast<void>(scada::sim::parse_options(args, detail));
    return 0;
}
