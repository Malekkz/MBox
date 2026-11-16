// Thin executable: all logic is in libscada-tagd.

#include "scada/tagd/tagd.h"

#include <cstddef>
#include <cstdlib>
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
    scada::tagd::RunHooks hooks;
    // Coding standard, section 7: std::_Exit only here, after a bounded stop
    // failed (decision D-59). It skips the join of a retain writer that is
    // still inside a hung fsync.
    hooks.exit_now = [](int status) { std::_Exit(status); };
    return scada::tagd::run(args, std::cout, std::cerr, hooks);
}
