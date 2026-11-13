#include "scada/hello/hello.h"

#include <ostream>
#include <span>
#include <string_view>

namespace scada::hello {

namespace {

constexpr std::string_view USAGE =
    "Usage: scada-hello [--help]\n"
    "\n"
    "Prints a greeting. Proves the build and the library-plus-thin-executable\n"
    "pattern; it has no other function.\n"
    "\n"
    "Options:\n"
    "  --help  Show this text and exit.\n";

}  // namespace

std::string_view greeting() noexcept {
    return "Hello from libscada-hello";
}

int run(std::span<const std::string_view> args, std::ostream& out,
        std::ostream& err) {
    if (args.empty()) {
        out << greeting() << '\n';
        return EXIT_OK;
    }
    if (args.size() == 1 && args.front() == "--help") {
        out << USAGE;
        return EXIT_OK;
    }
    err << "scada-hello: unexpected arguments\n" << USAGE;
    return EXIT_USAGE;
}

}  // namespace scada::hello
