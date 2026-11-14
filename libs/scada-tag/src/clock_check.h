#ifndef SCADA_TAG_CLOCK_CHECK_H
#define SCADA_TAG_CLOCK_CHECK_H

// The clock check of docs/contracts/tag-database.md, section 13.1: the
// functions marked real-time safe read CLOCK_REALTIME and CLOCK_MONOTONIC,
// which is no system call only while the kernel's clock source can be read
// from the vDSO. Private to libscada-tag.

#include <cstdint>
#include <string_view>

namespace scada::tag {

// A clock read that takes longer than this, in ns, is taken to enter the
// kernel. A read from the vDSO takes 15 ns to 100 ns on the supported
// targets.
inline constexpr std::int64_t CLOCK_READ_LIMIT_NS = 200;

// How long a result of the check is used before the clocks are checked
// again, in ns (section 13.1: once a minute).
inline constexpr std::int64_t CLOCK_CHECK_INTERVAL_NS = 60'000'000'000;

// True if reading the clocks is a system call, judged from the name of the
// kernel's current clock source and the time one read takes:
// - `clock_source` is not one of the sources the vDSO of x86-64 and ARM64
//   reads (`tsc`, `kvm-clock`, `hyperv_clocksource_tsc_page`,
//   `arch_sys_counter`); an empty name, for a machine that does not tell,
//   decides nothing;
// - or `ns_per_read` is more than CLOCK_READ_LIMIT_NS.
[[nodiscard]] bool clock_reads_enter_kernel(std::string_view clock_source,
                                            std::int64_t ns_per_read) noexcept;

// Reads the name of the current clock source and times a batch of reads of
// CLOCK_MONOTONIC, and returns `clock_reads_enter_kernel` of the two. A few
// tens of microseconds; not real-time.
[[nodiscard]] bool clock_reads_enter_kernel_now() noexcept;

}  // namespace scada::tag

#endif  // SCADA_TAG_CLOCK_CHECK_H
