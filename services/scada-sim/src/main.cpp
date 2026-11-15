// Thin executable: all logic is in libscada-sim.

#include "scada/sim/sim.h"

#include <cstddef>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

int main(int argc, char* argv[]) {
    // argv is the one C array the language hands us; wrap it with its length
    // at once so that nothing below indexes a raw pointer.
    const std::span<char*> raw_args(
        argv, argc > 0 ? static_cast<std::size_t>(argc) : 0);

    std::vector<std::string_view> args;
    if (raw_args.size() > 1) {
        // The first element is the program name.
        const std::span<char*> options = raw_args.subspan(1);
        args.assign(options.begin(), options.end());
    }
    return scada::sim::run(args, std::cout, std::cerr);
}
