#include "scada/driver/diagnostics.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::driver {
namespace {

constexpr std::int64_t NS_PER_S = 1'000'000'000;
constexpr double NS_PER_MS = 1'000'000.0;

}  // namespace

void ResponseWindow::add(std::int64_t duration_ns) noexcept {
    // `next_` is below RESPONSE_SAMPLES: it is kept modulo that.
    const std::span<std::int64_t> samples(samples_);
    samples[next_] = std::max<std::int64_t>(duration_ns, 0);
    next_ = (next_ + 1) % RESPONSE_SAMPLES;
    count_ = std::min(count_ + 1, RESPONSE_SAMPLES);
}

double ResponseWindow::average_ms() const noexcept {
    if (count_ == 0) {
        return 0.0;
    }
    // In double: 100 samples of up to 2^63 ns would overflow an integer sum.
    double sum = 0.0;
    for (const std::int64_t sample : std::span(samples_).first(count_)) {
        sum += static_cast<double>(sample);
    }
    return sum / static_cast<double>(count_) / NS_PER_MS;
}

void RateWindow::advance(std::int64_t now_ns) noexcept {
    // A monotonic time is never negative; one that is counts as 0, so that
    // every bucket index is.
    const std::int64_t second = std::max<std::int64_t>(now_ns, 0) / NS_PER_S;
    if (!started_) {
        started_ = true;
        second_ = second;
        return;
    }
    if (second <= second_) {
        return;  // the same second, or a clock that did not move forward
    }
    const std::int64_t passed = std::min(second - second_, RATE_SECONDS);
    // Every index is below RATE_SECONDS: `second_` is never negative.
    const std::span<std::uint64_t> buckets(buckets_);
    for (std::int64_t step = 1; step <= passed; ++step) {
        buckets[static_cast<std::size_t>((second_ + step) % RATE_SECONDS)] = 0;
    }
    second_ = second;
}

void RateWindow::add(std::int64_t now_ns) noexcept {
    advance(now_ns);
    const std::span<std::uint64_t> buckets(buckets_);
    ++buckets[static_cast<std::size_t>(second_ % RATE_SECONDS)];
}

double RateWindow::per_second(std::int64_t now_ns) noexcept {
    advance(now_ns);
    std::uint64_t total = 0;
    for (const std::uint64_t count : buckets_) {
        total += count;
    }
    return static_cast<double>(total) / static_cast<double>(RATE_SECONDS);
}

}  // namespace scada::driver
