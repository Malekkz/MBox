// Breaks the naming rules of CLAUDE.md and nothing else. The names in
// `good` follow the rules and must not be reported.

#include <cstdint>

namespace fixture {

namespace good {

constexpr int MAXIMUM_COUNT = 8;

enum class Colour : std::uint8_t { RED, GREEN };

class Counter {
public:
    void count_up(int step_size) { total_count_ += step_size; }
    [[nodiscard]] int total() const { return total_count_; }

private:
    int total_count_ = 0;
};

struct Sample {
    int raw_value = 0;
};

int scale(int raw_value) {
    const int scaled_value = raw_value * MAXIMUM_COUNT;
    return scaled_value;
}

}  // namespace good

constexpr int bad_constant = 8;

enum class Colour : std::uint8_t { bad_value };

struct Sample {
    int BadMember = 0;
};

class bad_type {
public:
    [[nodiscard]] int value() const { return missing_suffix; }

private:
    int missing_suffix = 0;
};

int BadFunction(int BadParameter) {
    return BadParameter * bad_constant;
}

}  // namespace fixture
