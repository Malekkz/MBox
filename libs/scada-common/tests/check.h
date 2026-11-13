#ifndef SCADA_COMMON_TESTS_CHECK_H
#define SCADA_COMMON_TESTS_CHECK_H

// Minimal test support until the test framework of issue P0-03 exists.

#include <iostream>
#include <string_view>

#include <sys/wait.h>
#include <unistd.h>

namespace scada::test {

// Counts failed expectations and reports each one.
class Checker {
public:
    void expect(bool condition, std::string_view what) {
        if (!condition) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures_;
        }
    }

    // The value `main` returns.
    [[nodiscard]] int exit_code() const { return failures_ == 0 ? 0 : 1; }

private:
    int failures_ = 0;
};

// Runs `action` in a child process and returns true if the child was ended
// by SIGABRT, which is what `scada::panic` does.
template <typename Action>
[[nodiscard]] bool aborts(Action action) {
    const pid_t child = ::fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        action();
        ::_exit(0);
    }
    int state = 0;
    if (::waitpid(child, &state, 0) != child) {
        return false;
    }
    return WIFSIGNALED(state) && WTERMSIG(state) == SIGABRT;
}

}  // namespace scada::test

#endif  // SCADA_COMMON_TESTS_CHECK_H
