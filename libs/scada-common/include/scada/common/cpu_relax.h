#ifndef SCADA_COMMON_CPU_RELAX_H
#define SCADA_COMMON_CPU_RELAX_H

#include "scada/common/realtime.h"

#include <string_view>

namespace scada {

// Tells the processor that the caller is in a short retry loop, waiting for
// another thread or process to finish a few stores: the "pause instruction"
// of the tag database contract, section 6.2. It changes no register and no
// memory, makes no system call and takes a few nanoseconds to a few tens of
// nanoseconds.
//
// This is the one place outside the I/O HAL that names a processor
// instruction (docs/decisions.md, D-29): C++20 has no portable spelling for
// it. A target that is neither of the two supported ones does not compile.
//
// On ARM64 the build option SCADA_ARM64_PAUSE chooses the instruction:
// `yield` (the default) or `isb`. The choice exists to measure both on
// ARM64 hardware (issue #40, decision D-31); x86-64 always uses `pause`.
SCADA_RT inline void cpu_relax() noexcept {
#if defined(__x86_64__)
    asm("pause");
#elif defined(__aarch64__) && defined(SCADA_ARM64_PAUSE_ISB)
    asm("isb");
#elif defined(__aarch64__)
    asm("yield");
#else
#error "cpu_relax: only x86-64 and ARM64 are supported (contract, section 1)"
#endif
}

// The name of the instruction `cpu_relax` executes in this build, for
// measurement reports.
inline constexpr std::string_view CPU_RELAX_INSTRUCTION =
#if defined(__x86_64__)
    "pause";
#elif defined(__aarch64__) && defined(SCADA_ARM64_PAUSE_ISB)
    "isb";
#else
    "yield";
#endif

}  // namespace scada

#endif  // SCADA_COMMON_CPU_RELAX_H
