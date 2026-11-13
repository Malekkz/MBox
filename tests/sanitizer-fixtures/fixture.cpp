// Deliberate bugs, one per sanitizer finding the build must catch (issue
// P0-03). Each function breaks the coding standard on purpose. This program
// is built only in the sanitizer builds and run only by the tests that expect
// it to fail.
//
// Values that decide the bug are read through `volatile`, so the compiler
// can neither diagnose the bug at compile time nor optimise it away.

#include <cstddef>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>
#include <thread>

namespace {

constexpr std::size_t BUFFER_SIZE = 8;

// Writes one element past the end of a heap buffer.
int heap_overflow() {
    volatile std::size_t index = BUFFER_SIZE;
    const auto buffer = std::make_unique<int[]>(BUFFER_SIZE);
    buffer[index] = 1;
    return buffer[0];
}

// Reads a heap object after it was freed.
int use_after_free() {
    auto owner = std::make_unique<int>(1);
    int* volatile dangling = owner.get();
    owner.reset();
    return *dangling;
}

// Gives up the only pointer to a heap buffer.
int leak() {
    auto owner = std::make_unique<int[]>(BUFFER_SIZE);
    int* volatile lost = owner.release();
    lost = nullptr;
    return lost == nullptr ? 0 : 1;
}

// Adds one to the largest int.
int signed_overflow() {
    volatile int value = std::numeric_limits<int>::max();
    volatile int one = 1;
    return value + one;
}

// Two threads write one variable with no synchronisation.
int data_race() {
    constexpr int ROUNDS = 100'000;
    int shared = 0;
    const auto work = [&shared] {
        for (int i = 0; i < ROUNDS; ++i) {
            shared = shared + 1;
        }
    };
    {
        const std::jthread first(work);
        const std::jthread second(work);
    }
    return shared == 2 * ROUNDS ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string_view bug = argc == 2 ? argv[1] : "";
    int result = 0;
    if (bug == "heap-overflow") {
        result = heap_overflow();
    } else if (bug == "use-after-free") {
        result = use_after_free();
    } else if (bug == "leak") {
        result = leak();
    } else if (bug == "signed-overflow") {
        result = signed_overflow();
    } else if (bug == "data-race") {
        result = data_race();
    } else {
        std::cerr << "usage: scada-sanitizer-fixture heap-overflow|"
                     "use-after-free|leak|signed-overflow|data-race\n";
        return 2;
    }
    // Reaching this line means the sanitizer missed the bug. Exit with
    // success so that the test, which expects a failure, reports it.
    std::cout << "not caught (" << result << ")\n";
    return 0;
}
