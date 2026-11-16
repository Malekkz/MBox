#ifndef SCADA_TESTS_SUPPORT_EXEC_H
#define SCADA_TESTS_SUPPORT_EXEC_H

// What the helpers that start processes share: everything a child needs
// between fork and exec is prepared before the fork, because the test
// process may have other threads and only async-signal-safe calls are
// allowed in the child until exec.

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace scada::test::detail {

// The path execve() needs for the program `name`: `name` itself if it holds
// a slash, otherwise the first regular, executable file of that name in a
// directory of PATH, as execvp() would find it. Nothing if there is none or
// PATH is not set.
[[nodiscard]] std::optional<std::string> find_program(const std::string& name);

// The writable, null-terminated argument vector execve() wants, built from
// `command`. Neither copied nor moved: the pointers point into its own
// strings.
class ArgumentVector {
public:
    explicit ArgumentVector(std::span<const std::string> command);
    ArgumentVector(const ArgumentVector&) = delete;
    ArgumentVector& operator=(const ArgumentVector&) = delete;
    ArgumentVector(ArgumentVector&&) = delete;
    ArgumentVector& operator=(ArgumentVector&&) = delete;
    ~ArgumentVector() = default;

    [[nodiscard]] char* const* get() const noexcept { return pointers_.data(); }

private:
    std::vector<std::string> owned_;
    std::vector<char*> pointers_;
};

}  // namespace scada::test::detail

#endif  // SCADA_TESTS_SUPPORT_EXEC_H
