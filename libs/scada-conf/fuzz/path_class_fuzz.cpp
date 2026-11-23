// Fuzz target: the path classifier of stage 1 (configuration schemas
// contract, section 1). The input is one path; a path the classifier accepts
// must keep to the path grammar of section 1.

#include "scada/conf/tree.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

bool in_grammar(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.back() == '/' ||
        path.find("//") != std::string_view::npos) {
        return false;
    }
    const bool letters_ok = std::ranges::all_of(path, [](char letter) {
        return (letter >= 'a' && letter <= 'z') ||
               (letter >= '0' && letter <= '9') || letter == '_' ||
               letter == '/' || letter == '.';
    });
    if (!letters_ok) {
        return false;
    }
    // No component that is only dots, and none that starts with one.
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string_view part = path.substr(
            start, slash == std::string_view::npos ? std::string_view::npos
                                                   : slash - start);
        if (part.empty() || part.front() == '.') {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    const std::string path(bytes.begin(), bytes.end());
    for (const bool is_directory : {false, true}) {
        const std::optional<scada::conf::PathClass> found =
            scada::conf::classify_path(path, is_directory);
        if (found && !in_grammar(path)) {
            std::abort();
        }
    }
    return 0;
}
