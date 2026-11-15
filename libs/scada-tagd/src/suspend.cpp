#include "scada/tagd/suspend.h"

#include "scada/common/fd.h"

#include <fcntl.h>
#include <sys/types.h>
#include <time.h>  // NOLINT(modernize-deprecated-headers): clock_gettime and CLOCK_BOOTTIME are POSIX and Linux, declared here
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace scada::tagd {

namespace {

constexpr std::int64_t NANOSECONDS_PER_SECOND = 1'000'000'000;
constexpr std::size_t MAX_SLEEP_STATES_BYTES = 256;
constexpr char FIRST_PRINTABLE = ' ';
constexpr char LAST_PRINTABLE = '~';

std::int64_t read_clock(clockid_t clock) noexcept {
    timespec now{};
    // Cannot fail for these clocks on the supported kernels; 0 then makes
    // the sample look like no suspend, which only loses a warning.
    if (::clock_gettime(clock, &now) != 0) {
        return 0;
    }
    return (std::int64_t{now.tv_sec} * NANOSECONDS_PER_SECOND) + now.tv_nsec;
}

bool is_space(char character) noexcept {
    return character == ' ' || character == '\n' || character == '\t' ||
           character == '\r';
}

}  // namespace

ClockSample sample_clocks() noexcept {
    ClockSample sample;
    sample.monotonic_before_ns = read_clock(CLOCK_MONOTONIC);
    sample.boottime_ns = read_clock(CLOCK_BOOTTIME);
    sample.monotonic_after_ns = read_clock(CLOCK_MONOTONIC);
    return sample;
}

std::optional<SuspendFinding> SuspendWatch::observe(
    const ClockSample& sample) noexcept {
    if (sample.monotonic_after_ns < sample.monotonic_before_ns) {
        return std::nullopt;
    }
    // The clocks count from boot and stay far below 2^62 ns: no overflow.
    const std::int64_t lower = sample.boottime_ns - sample.monotonic_after_ns;
    const std::int64_t upper = sample.boottime_ns - sample.monotonic_before_ns;
    if (!started_) {
        started_ = true;
        upper_ns_ = upper;
        if (lower > 0) {
            return SuspendFinding{.at_start = true, .suspended_ns = lower};
        }
        return std::nullopt;
    }
    const std::int64_t previous = upper_ns_;
    upper_ns_ = upper;
    if (lower > previous) {
        return SuspendFinding{.at_start = false,
                              .suspended_ns = lower - previous};
    }
    return std::nullopt;
}

std::string read_sleep_states(const std::string& path) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open has no other form
    const Fd file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!file.valid()) {
        return {};
    }
    std::array<char, MAX_SLEEP_STATES_BYTES> buffer{};
    ssize_t got = -1;
    for (;;) {
        got = ::read(file.get(), buffer.data(), buffer.size());
        if (got >= 0 || errno != EINTR) {
            break;
        }
    }
    if (got <= 0) {
        return {};
    }
    // In range: read returns at most the size of the buffer.
    std::string text(buffer.data(), static_cast<std::size_t>(got));
    for (char& character : text) {
        if (!is_space(character) &&
            (character < FIRST_PRINTABLE || character > LAST_PRINTABLE)) {
            character = '?';
        }
    }
    const std::size_t first = text.find_first_not_of(" \n\t\r");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \n\t\r");
    return text.substr(first, last - first + 1);
}

}  // namespace scada::tagd
