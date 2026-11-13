// Unit test for libscada-common. It uses no test framework: the framework is
// chosen in issue P0-03, and this file moves to it then.
//
// SCADA_RT has no effect at run time. This test proves that a marked
// declaration, definition and member function compile with every supported
// compiler and still behave as written.

#include "scada/common/realtime.h"

#include <iostream>

namespace {

SCADA_RT int add(int left, int right) noexcept;

int add(int left, int right) noexcept {
    return left + right;
}

class Counter {
public:
    SCADA_RT void count() noexcept { ++count_; }
    [[nodiscard]] int value() const noexcept { return count_; }

private:
    int count_ = 0;
};

}  // namespace

int main() {
    Counter counter;
    counter.count();
    if (add(1, 2) != 3 || counter.value() != 1) {
        std::cerr << "FAILED: SCADA_RT changed the behaviour of a function\n";
        return 1;
    }
    return 0;
}
