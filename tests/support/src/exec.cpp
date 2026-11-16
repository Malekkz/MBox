#include "exec.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace scada::test::detail {

namespace {

bool is_executable_file(const std::string& path) {
    struct stat status {};
    return ::stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
           ::access(path.c_str(), X_OK) == 0;
}

}  // namespace

std::optional<std::string> find_program(const std::string& name) {
    if (name.empty()) {
        return std::nullopt;
    }
    if (name.find('/') != std::string::npos) {
        return name;
    }
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing changes the environment while the tests run
    const char* const path = std::getenv("PATH");
    if (path == nullptr) {
        return std::nullopt;
    }
    std::string_view rest(path);
    while (true) {
        const std::size_t end = rest.find(':');
        const std::string_view directory = rest.substr(0, end);
        // An empty entry is the current directory, as for execvp().
        std::string candidate =
            directory.empty() ? std::string(".") : std::string(directory);
        candidate += '/';
        candidate += name;
        if (is_executable_file(candidate)) {
            return candidate;
        }
        if (end == std::string_view::npos) {
            return std::nullopt;
        }
        rest = rest.substr(end + 1);
    }
}

ArgumentVector::ArgumentVector(std::span<const std::string> command)
    : owned_(command.begin(), command.end()) {
    pointers_.reserve(owned_.size() + 1);
    for (std::string& argument : owned_) {
        pointers_.push_back(argument.data());
    }
    pointers_.push_back(nullptr);
}

}  // namespace scada::test::detail
