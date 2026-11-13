// The test framework registers each test with a `new` inside the body of its
// macro GTEST_TEST_. That one expression is not reported; every other `new`
// is. The macro below stands in for the framework's, so that the fixture
// compiles on its own.

#define GTEST_TEST_(name)  \
    int* name() {          \
        return new int(1); \
    }
#define OTHER_MACRO(name)  \
    int* name() {          \
        return new int(2); \
    }
#define PASS_THROUGH(expression) (expression)

namespace fixture {

GTEST_TEST_(registered_by_the_framework)
OTHER_MACRO(registered_by_something_else)  // expect: scada-banned

int* in_a_test_body() {
    return new int(3);  // expect: scada-banned
}

int* in_a_macro_argument() {
    return PASS_THROUGH(new int(4));  // expect: scada-banned
}

}  // namespace fixture
