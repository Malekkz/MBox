// Thin executable: all logic is in libscada-hello.

#include "scada/hello/hello.h"

#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char* argv[]) {
    const std::vector<std::string_view> args(argv + (argc > 0 ? 1 : 0),
                                             argv + argc);
    return scada::hello::run(args, std::cout, std::cerr);
}
