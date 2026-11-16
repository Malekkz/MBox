#ifndef SCADA_TAGD_WAIT_H
#define SCADA_TAGD_WAIT_H

#include <signal.h>  // NOLINT(modernize-deprecated-headers): sigset_t is declared here, not in <csignal>

#include <cstdint>

namespace scada::tagd {

// What ended one wait of the main loop.
enum class Wake : std::uint8_t { CHECK_DUE, STOP, ERROR };

// The calls `wait_until` makes; tests replace them.
class WaitCalls {
public:
    WaitCalls() = default;
    virtual ~WaitCalls() = default;
    WaitCalls(const WaitCalls&) = delete;
    WaitCalls& operator=(const WaitCalls&) = delete;
    WaitCalls(WaitCalls&&) = delete;
    WaitCalls& operator=(WaitCalls&&) = delete;

    // CLOCK_MONOTONIC in ns.
    [[nodiscard]] virtual std::int64_t now_ns() noexcept = 0;
    // sigtimedwait(2) for `signals` with a timeout of `timeout_ns` > 0: the
    // signal number, or -1 with errno set.
    [[nodiscard]] virtual int timed_wait(const sigset_t& signals,
                                         std::int64_t timeout_ns) noexcept = 0;
};

// The operating system's calls.
[[nodiscard]] WaitCalls& system_wait_calls() noexcept;

// Waits for one of the signals in `signals`, which must be blocked, until
// the monotonic time `deadline_ns`. A wait that another signal interrupts
// (EINTR) is waited again for the time left: a check that came early would
// use up the slot of the next one and break the bound of RETAIN_NOW
// (contract 12, decision D-62). STOP if a signal of `signals` arrived,
// CHECK_DUE at the deadline, ERROR if the wait failed otherwise.
[[nodiscard]] Wake wait_until(const sigset_t& signals, std::int64_t deadline_ns,
                              WaitCalls& calls = system_wait_calls()) noexcept;

}  // namespace scada::tagd

#endif  // SCADA_TAGD_WAIT_H
