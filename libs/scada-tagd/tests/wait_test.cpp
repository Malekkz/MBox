#include "wait.h"

#include <gtest/gtest.h>

#include <signal.h>  // NOLINT(modernize-deprecated-headers): sigset_t is declared here, not in <csignal>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using scada::tagd::wait_until;
using scada::tagd::WaitCalls;
using scada::tagd::Wake;

constexpr std::int64_t NS_PER_MS = 1'000'000;
constexpr std::int64_t START_NS = 1'000 * NS_PER_MS;
constexpr std::int64_t PERIOD_NS = 250 * NS_PER_MS;
constexpr std::int64_t DEADLINE_NS = START_NS + PERIOD_NS;
constexpr std::int64_t INTERRUPTED_AFTER_NS = 3 * NS_PER_MS;
constexpr int SIGNAL = SIGTERM;

// A clock that moves only when a wait ends, and waits that end as told.
class ScriptedWaits final : public WaitCalls {
public:
    // What a wait does: it ends after `after_ns` (at most its timeout),
    // with `result` and `error`.
    struct Step {
        std::int64_t after_ns = 0;
        int result = -1;
        int error = EAGAIN;
    };

    explicit ScriptedWaits(std::vector<Step> steps)
        : steps_(std::move(steps)) {}

    std::int64_t now_ns() noexcept override { return now_; }

    int timed_wait(const sigset_t& /*signals*/,
                   std::int64_t timeout_ns) noexcept override {
        timeouts_.push_back(timeout_ns);
        if (next_ >= steps_.size()) {
            now_ += timeout_ns;  // the timeout
            errno = EAGAIN;
            return -1;
        }
        const Step step = steps_[next_++];
        now_ += step.after_ns < timeout_ns ? step.after_ns : timeout_ns;
        errno = step.error;
        return step.result;
    }

    [[nodiscard]] const std::vector<std::int64_t>& timeouts() const {
        return timeouts_;
    }
    [[nodiscard]] std::int64_t now() const { return now_; }

private:
    std::vector<Step> steps_;
    std::size_t next_ = 0;
    std::int64_t now_ = START_NS;
    std::vector<std::int64_t> timeouts_;
};

sigset_t stop_signals() {
    sigset_t signals{};
    sigemptyset(&signals);
    sigaddset(&signals, SIGNAL);
    return signals;
}

// F2 of the review of #111: an interrupted wait (EINTR, for example after
// SIGSTOP and SIGCONT) is waited again for the time left, so the check
// never comes early.
TEST(WaitUntil, AnInterruptedWaitWaitsAgainUntilTheDeadline) {
    ScriptedWaits calls({{INTERRUPTED_AFTER_NS, -1, EINTR}});
    EXPECT_EQ(wait_until(stop_signals(), DEADLINE_NS, calls), Wake::CHECK_DUE);
    EXPECT_EQ(calls.now(), DEADLINE_NS);
    ASSERT_EQ(calls.timeouts().size(), 2U);
    EXPECT_EQ(calls.timeouts()[0], PERIOD_NS);
    EXPECT_EQ(calls.timeouts()[1], PERIOD_NS - INTERRUPTED_AFTER_NS);
}

TEST(WaitUntil, ATimeoutIsTheCheck) {
    ScriptedWaits calls({});
    EXPECT_EQ(wait_until(stop_signals(), DEADLINE_NS, calls), Wake::CHECK_DUE);
    EXPECT_EQ(calls.timeouts().size(), 1U);
}

TEST(WaitUntil, ADeadlineThatHasPassedDoesNotWait) {
    ScriptedWaits calls({});
    EXPECT_EQ(wait_until(stop_signals(), START_NS, calls), Wake::CHECK_DUE);
    EXPECT_TRUE(calls.timeouts().empty());
}

TEST(WaitUntil, AStopSignalEndsTheWaitAtOnce) {
    ScriptedWaits calls(
        {{INTERRUPTED_AFTER_NS, -1, EINTR}, {INTERRUPTED_AFTER_NS, SIGNAL, 0}});
    EXPECT_EQ(wait_until(stop_signals(), DEADLINE_NS, calls), Wake::STOP);
    EXPECT_EQ(calls.now(), START_NS + (2 * INTERRUPTED_AFTER_NS));
}

TEST(WaitUntil, AnotherErrorIsReported) {
    ScriptedWaits calls({{0, -1, EINVAL}});
    EXPECT_EQ(wait_until(stop_signals(), DEADLINE_NS, calls), Wake::ERROR);
}

}  // namespace
