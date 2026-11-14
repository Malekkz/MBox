#ifndef SCADA_TAG_FUTEX_H
#define SCADA_TAG_FUTEX_H

// Waiting and waking on 32-bit words of shared memory
// (docs/contracts/tag-database.md, section 8.4; decision D-32). The words
// lie in mappings of region files that other processes map too, so the
// futexes are shared, not private: the kernel identifies them by file and
// offset, which makes a wait on a read-only mapping meet a wake on another
// process's read-write mapping. Private to libscada-tag.
//
// The words are the low 32 bits of a ring's `write_seq` and a process's
// `cycle_seq`. The targets are little-endian (contract, section 1), so the
// low 32 bits of a 64-bit field lie at the address of the field.

#include <bit>
#include <cstdint>
#include <span>

namespace scada::tag {

static_assert(std::endian::native == std::endian::little,
              "the futex word of a 64-bit field is its low half");

// The most words one `futex_wait_any` waits on (the kernel's limit).
inline constexpr std::size_t MAX_FUTEX_WAITERS = 128;

// What a wait ended with.
enum class FutexWait : std::uint8_t {
    WOKEN,          // a wake arrived, or a signal interrupted the wait
    VALUE_CHANGED,  // a word did not hold its expected value
    TIMED_OUT,      // the deadline passed
    FAILED,         // the kernel refused the wait; errno is set
};

// One word to wait on. `word` points into a mapping, read-only or
// read-write, that stays mapped during the wait.
struct FutexWaiter {
    const void* word = nullptr;
    std::uint32_t expected = 0;
    // True for a word in memory private to this process, which only threads
    // of this process wake.
    bool process_private = false;
};

// Waits until the 32-bit word at `word` is woken, if it holds `expected`
// when the wait starts, or until `deadline_mono_ns` (CLOCK_MONOTONIC)
// passes. Not real-time: a system call that blocks.
[[nodiscard]] FutexWait futex_wait(const void* word, std::uint32_t expected,
                                   std::int64_t deadline_mono_ns) noexcept;

// As `futex_wait` for several words at once (`futex_waitv`, Linux 5.16):
// it ends when any of them is woken or does not hold its expected value.
// `waiters` holds 1 to MAX_FUTEX_WAITERS words; another count ends the
// process. Not real-time.
[[nodiscard]] FutexWait futex_wait_any(std::span<const FutexWaiter> waiters,
                                       std::int64_t deadline_mono_ns) noexcept;

// Wakes every thread of every process that waits on the word at `word`.
// `process_private` as in FutexWaiter. Returns false with errno set if the
// kernel refused. One system call that does not block; not real-time.
[[nodiscard]] bool futex_wake_all(const void* word,
                                  bool process_private = false) noexcept;

// The futex word of a 64-bit counter: its low 32 bits, which is what a
// waiter passes as expected value.
[[nodiscard]] constexpr std::uint32_t futex_word_of(
    std::uint64_t counter) noexcept {
    // Wanted: the low half; the high half is not part of the word.
    return static_cast<std::uint32_t>(counter);
}

}  // namespace scada::tag

#endif  // SCADA_TAG_FUTEX_H
