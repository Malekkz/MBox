#include "futex.h"

#include "scada/common/clock.h"
#include "scada/common/panic.h"

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <span>

namespace scada::tag {

namespace {

// The absolute time `deadline_mono_ns` as the kernel takes it.
timespec timespec_of(std::int64_t deadline_mono_ns) noexcept {
    const std::int64_t deadline = deadline_mono_ns < 0 ? 0 : deadline_mono_ns;
    return {.tv_sec = deadline / NANOSECONDS_PER_SECOND,
            .tv_nsec = deadline % NANOSECONDS_PER_SECOND};
}

// What a wait that returned -1 with `error` ended with.
FutexWait wait_result_of(int error) noexcept {
    switch (error) {
        case EAGAIN:
            return FutexWait::VALUE_CHANGED;
        case ETIMEDOUT:
            return FutexWait::TIMED_OUT;
        case EINTR:
            // The caller looks at the words again in either case.
            return FutexWait::WOKEN;
        default:
            return FutexWait::FAILED;
    }
}

// The address the kernel waits on, as `futex_waitv` takes it.
std::uint64_t address_of(const void* word) noexcept {
    // NOLINTNEXTLINE(scada-banned,cppcoreguidelines-pro-type-reinterpret-cast): futex_waitv takes the address as an integer; the reviewed helper of the wake mechanism (coding standard, section 5)
    return reinterpret_cast<std::uintptr_t>(word);
}

}  // namespace

FutexWait futex_wait(const void* word, std::uint32_t expected,
                     std::int64_t deadline_mono_ns) noexcept {
    const timespec deadline = timespec_of(deadline_mono_ns);
    // FUTEX_WAIT_BITSET takes an absolute CLOCK_MONOTONIC deadline. Not
    // FUTEX_PRIVATE_FLAG: the word is shared with other processes.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the futex system call has no other form
    const long result = ::syscall(SYS_futex, word, FUTEX_WAIT_BITSET, expected,
                                  &deadline, nullptr, FUTEX_BITSET_MATCH_ANY);
    return result == 0 ? FutexWait::WOKEN : wait_result_of(errno);
}

FutexWait futex_wait_any(std::span<const FutexWaiter> waiters,
                         std::int64_t deadline_mono_ns) noexcept {
    if (waiters.empty() || waiters.size() > MAX_FUTEX_WAITERS) {
        panic("futex: a wait on no word or on too many");
    }
    std::array<futex_waitv, MAX_FUTEX_WAITERS> vector{};
    for (std::size_t i = 0; i < waiters.size(); ++i) {
        vector.at(i).val = waiters[i].expected;
        vector.at(i).uaddr = address_of(waiters[i].word);
        vector.at(i).flags = FUTEX2_SIZE_U32 |
                             (waiters[i].process_private ? FUTEX2_PRIVATE : 0U);
    }
    const timespec deadline = timespec_of(deadline_mono_ns);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the futex_waitv system call has no other form
    const long result = ::syscall(SYS_futex_waitv, vector.data(),
                                  static_cast<unsigned int>(waiters.size()), 0U,
                                  &deadline, CLOCK_MONOTONIC);
    return result >= 0 ? FutexWait::WOKEN : wait_result_of(errno);
}

bool futex_wake_all(const void* word, bool process_private) noexcept {
    const int operation =
        process_private ? (FUTEX_WAKE | FUTEX_PRIVATE_FLAG) : FUTEX_WAKE;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the futex system call has no other form
    return ::syscall(SYS_futex, word, operation, INT_MAX) >= 0;
}

}  // namespace scada::tag
