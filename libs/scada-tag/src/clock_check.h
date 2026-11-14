#ifndef SCADA_TAG_CLOCK_CHECK_H
#define SCADA_TAG_CLOCK_CHECK_H

// The clock check of docs/contracts/tag-database.md, section 13.1: the
// functions marked real-time safe read CLOCK_REALTIME and CLOCK_MONOTONIC,
// which is no system call only while the kernel's clock source can be read
// from the vDSO. Private to libscada-tag.

#include <cstdint>
#include <optional>
#include <string_view>

namespace scada::tag {

// A library clock read counts as no system call only if it takes at most
// 1 / CLOCK_READ_SPEEDUP of the time of a raw `clock_gettime` system call.
// A read from the vDSO takes 15 ns to 100 ns on the supported targets and a
// system call several times that; a vDSO that falls back to the system call
// takes as long as the call or longer.
inline constexpr std::int64_t CLOCK_READ_SPEEDUP = 2;

// How long a result of the check is used before the clocks are checked
// again, in ns (section 13.1: once a minute).
inline constexpr std::int64_t CLOCK_CHECK_INTERVAL_NS = 60'000'000'000;

// True if reading the clocks is a system call, judged from two signals:
// - `clock_source`, the name of the kernel's current clock source, is not
//   one of the sources the vDSO of x86-64 and ARM64 reads (`tsc`,
//   `tsc-early`, `kvm-clock`, `hyperv_clocksource_tsc_page`,
//   `arch_sys_counter`); an empty name, for a machine that does not tell,
//   decides nothing;
// - or `ns_per_read`, the time of one clock read through the library, is
//   more than `ns_per_system_call` / CLOCK_READ_SPEEDUP. An empty
//   `ns_per_system_call`, for a machine where the raw system call failed,
//   decides nothing.
// The time decides alone when the name is known: an ARM timer erratum
// workaround keeps the name `arch_sys_counter` while reads become system
// calls.
[[nodiscard]] bool clock_reads_enter_kernel(
    std::string_view clock_source, std::int64_t ns_per_read,
    std::optional<std::int64_t> ns_per_system_call) noexcept;

// The time one read of CLOCK_MONOTONIC through the library takes, in ns: the
// mean of the fastest of 4 batches of 256 reads. Not real-time.
[[nodiscard]] std::int64_t time_library_read() noexcept;

// The time one raw `clock_gettime` system call for CLOCK_MONOTONIC takes, in
// ns, bypassing the vDSO: the mean of the fastest of 4 batches of 64 calls.
// Empty if the kernel refuses the call (a seccomp filter). Not real-time.
[[nodiscard]] std::optional<std::int64_t> time_system_call_read() noexcept;

// Reads the name of the current clock source, times batches of reads of
// CLOCK_MONOTONIC through the library and through the raw system call, and
// returns `clock_reads_enter_kernel` of the three. Under 1 ms on the
// supported targets; makes system calls, so not real-time.
[[nodiscard]] bool clock_reads_enter_kernel_now() noexcept;

}  // namespace scada::tag

#endif  // SCADA_TAG_CLOCK_CHECK_H
