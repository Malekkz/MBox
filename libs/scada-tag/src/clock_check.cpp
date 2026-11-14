#include "clock_check.h"

#include "region_file.h"

#include "scada/common/clock.h"
#include "scada/common/fd.h"

#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <limits>
#include <optional>
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
// Fewer raw system calls: each takes several times a vDSO read, and the
// check is repeated once a minute.
constexpr std::int64_t SYSTEM_CALLS_PER_BATCH = 64;

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

// The time one call of `read` takes, in ns: the mean of the fastest of
// BATCHES batches of `per_batch` calls. Empty if a call failed.
template <typename Read>
std::optional<std::int64_t> time_reads(std::int64_t per_batch,
                                       Read read) noexcept {
    std::int64_t fastest = std::numeric_limits<std::int64_t>::max();
    for (int batch = 0; batch < BATCHES; ++batch) {
        const std::int64_t begin = monotonic_ns();
        for (std::int64_t call = 0; call < per_batch; ++call) {
            if (!read()) {
                return std::nullopt;
            }
        }
        fastest = std::min(fastest, (monotonic_ns() - begin) / per_batch);
    }
    return fastest;
}

// One read of CLOCK_MONOTONIC as the library does it (through the C
// library, which uses the vDSO where it can).
bool library_read() noexcept {
    // The result is used, so that the compiler keeps the call.
    return monotonic_ns() != std::numeric_limits<std::int64_t>::min();
}

// One read of CLOCK_MONOTONIC as a system call, bypassing the vDSO. False
// if the kernel refuses it, for example under a seccomp filter.
bool system_call_read() noexcept {
    std::timespec time{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): syscall has no other form
    return ::syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &time) == 0;
}

}  // namespace

bool clock_reads_enter_kernel(
    std::string_view clock_source, std::int64_t ns_per_read,
    std::optional<std::int64_t> ns_per_system_call) noexcept {
    const bool known_source =
        clock_source.empty() ||
        std::ranges::find(VDSO_CLOCK_SOURCES, clock_source) !=
            VDSO_CLOCK_SOURCES.end();
    // Divided rather than multiplied, so that no value can overflow.
    const bool not_clearly_faster =
        ns_per_system_call.has_value() &&
        ns_per_read > *ns_per_system_call / CLOCK_READ_SPEEDUP;
    return !known_source || not_clearly_faster;
}

std::int64_t time_library_read() noexcept {
    // The library reads always succeed: `monotonic_ns` ends the process
    // otherwise.
    return time_reads(READS_PER_BATCH, library_read).value_or(0);
}

std::optional<std::int64_t> time_system_call_read() noexcept {
    return time_reads(SYSTEM_CALLS_PER_BATCH, system_call_read);
}

bool clock_reads_enter_kernel_now() noexcept {
    try {
        const std::string source = read_clock_source();
        return clock_reads_enter_kernel(source, time_library_read(),
                                        time_system_call_read());
    } catch (const std::exception&) {
        // No memory for the name: nothing can be said about the clock, and
        // a failed check must not fail attach (section 13.1).
        return false;
    }
}

}  // namespace scada::tag
