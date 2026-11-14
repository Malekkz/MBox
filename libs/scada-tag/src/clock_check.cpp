#include "clock_check.h"

#include "region_file.h"

#include "scada/common/clock.h"
#include "scada/common/fd.h"

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <string_view>

namespace scada::tag {

namespace {

// Where Linux names the clock source in use.
constexpr std::string_view CLOCK_SOURCE_PATH =
    "/sys/devices/system/clocksource/clocksource0/current_clocksource";

// Longer than any clock source name of the kernel (at most 32 bytes).
constexpr std::size_t CLOCK_SOURCE_BUFFER = 64;

// The clock sources the vDSO reads without a system call.
constexpr std::array<std::string_view, 5> VDSO_CLOCK_SOURCES{
    "tsc", "tsc-early", "kvm-clock", "hyperv_clocksource_tsc_page",
    "arch_sys_counter"};

// Reads are timed in batches, and the fastest batch counts: a batch that
// was interrupted says nothing about the clock.
constexpr int BATCHES = 4;
constexpr std::int64_t READS_PER_BATCH = 256;

// The name of the current clock source, or an empty text if the kernel
// does not tell (a container may hide the file).
std::string read_clock_source() {
    const Fd file =
        open_at(AT_FDCWD, std::string(CLOCK_SOURCE_PATH), O_RDONLY | O_CLOEXEC);
    if (!file.valid()) {
        return {};
    }
    std::array<char, CLOCK_SOURCE_BUFFER> buffer{};
    const ssize_t length = ::read(file.get(), buffer.data(), buffer.size());
    if (length <= 0) {
        return {};
    }
    // In range: positive and at most the size of the buffer.
    const std::string_view text(buffer.data(),
                                static_cast<std::size_t>(length));
    return std::string(text.substr(0, text.find('\n')));
}

// The time one read of CLOCK_MONOTONIC takes, in ns: the mean of the
// fastest of BATCHES batches.
std::int64_t time_clock_read() noexcept {
    std::int64_t fastest = std::numeric_limits<std::int64_t>::max();
    for (int batch = 0; batch < BATCHES; ++batch) {
        const std::int64_t begin = monotonic_ns();
        std::int64_t last = begin;
        for (std::int64_t read = 0; read < READS_PER_BATCH; ++read) {
            last = monotonic_ns();
        }
        fastest = std::min(fastest, (last - begin) / READS_PER_BATCH);
    }
    return fastest;
}

}  // namespace

bool clock_reads_enter_kernel(std::string_view clock_source,
                              std::int64_t ns_per_read) noexcept {
    const bool known_source =
        clock_source.empty() ||
        std::ranges::find(VDSO_CLOCK_SOURCES, clock_source) !=
            VDSO_CLOCK_SOURCES.end();
    return !known_source || ns_per_read > CLOCK_READ_LIMIT_NS;
}

bool clock_reads_enter_kernel_now() noexcept {
    try {
        return clock_reads_enter_kernel(read_clock_source(), time_clock_read());
    } catch (const std::exception&) {
        // No memory for the name: nothing can be said about the clock, and
        // a failed check must not fail attach (section 13.1).
        return false;
    }
}

}  // namespace scada::tag
