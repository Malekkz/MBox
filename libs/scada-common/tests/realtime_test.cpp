// Unit tests for libscada-common.
//
// SCADA_RT has no effect at run time. These tests prove that a marked
// declaration, definition and member function compile with every supported
// compiler and still behave as written.

#include "scada/common/realtime.h"

#include <gtest/gtest.h>

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

TEST(Realtime, MarkedFreeFunctionBehavesAsWritten) {
    EXPECT_EQ(add(1, 2), 3);
}

TEST(Realtime, MarkedMemberFunctionBehavesAsWritten) {
    Counter counter;
    counter.count();
    EXPECT_EQ(counter.value(), 1);
}

}  // namespace
