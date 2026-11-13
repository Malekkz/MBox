// Breaks no rule: every check must pass on this file.

#include "scada/common/realtime.h"

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>

namespace fixture {

namespace {

constexpr std::size_t SAMPLE_COUNT = 4;

// Real-time: only arithmetic on memory the caller owns.
SCADA_RT int sum(std::span<const int> samples) noexcept {
    int total = 0;
    for (const int sample : samples) {
        total += sample;
    }
    return total;
}

// Not real-time, so allocation and locking are allowed here.
int locked_sum(std::mutex& guard) {
    const std::scoped_lock lock(guard);
    const auto samples =
        std::make_unique<std::array<int, SAMPLE_COUNT>>(std::array{1, 2, 3, 4});
    return sum(*samples);
}

}  // namespace

int run() {
    std::mutex guard;
    return locked_sum(guard);
}

}  // namespace fixture
