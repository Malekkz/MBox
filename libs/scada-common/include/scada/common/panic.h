#ifndef SCADA_COMMON_PANIC_H
#define SCADA_COMMON_PANIC_H

#include "scada/common/static_string.h"

namespace scada {

// Ends the process after a broken invariant (coding standard, section 7):
// writes "scada: fatal: <message>" to the standard error stream and calls
// `std::abort`. It never returns, does not allocate, and does not run
// destructors. The service supervisor is expected to restart the process.
//
// Use it for programmer errors only. Invalid outside input must be reported
// with a `Status` instead.
[[noreturn]] void panic(StaticString message) noexcept;

}  // namespace scada

#endif  // SCADA_COMMON_PANIC_H
