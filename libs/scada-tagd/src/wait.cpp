#include "wait.h"

#include "scada/common/clock.h"

#include <signal.h>  // NOLINT(modernize-deprecated-headers): sigtimedwait is declared here, not in <csignal>

#include <cerrno>
#include <cstdint>
#include <ctime>

namespace scada::tagd {

namespace {

constexpr std::int64_t NANOSECONDS_PER_SECOND = 1'000'000'000;

class SystemWaitCalls final : public WaitCalls {
public:
    std::int64_t now_ns() noexcept override { return monotonic_ns(); }

    int timed_wait(const sigset_t& signals,
                   std::int64_t timeout_ns) noexcept override {
        timespec limit{};
        limit.tv_sec =
            static_cast<std::time_t>(timeout_ns / NANOSECONDS_PER_SECOND);
        // Below one second, which fits every `long`.
        limit.tv_nsec = static_cast<long>(timeout_ns % NANOSECONDS_PER_SECOND);
        return ::sigtimedwait(&signals, nullptr, &limit);
    }
};

}  // namespace

WaitCalls& system_wait_calls() noexcept {
    // One stateless object for the process; it holds nothing that changes.
    static SystemWaitCalls
        calls;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): stateless; the interface needs a non-const object
    return calls;
}

Wake wait_until(const sigset_t& signals, std::int64_t deadline_ns,
                WaitCalls& calls) noexcept {
    for (;;) {
        const std::int64_t left = deadline_ns - calls.now_ns();
        if (left <= 0) {
            return Wake::CHECK_DUE;
        }
        if (calls.timed_wait(signals, left) >= 0) {
            return Wake::STOP;
        }
        // EAGAIN is the timeout, EINTR an unrelated signal: either way the
        // clock decides whether the deadline has come.
        if (errno != EAGAIN && errno != EINTR) {
            return Wake::ERROR;
        }
    }
}

}  // namespace scada::tagd
