#ifndef SCADA_TAG_WAIT_SET_H
#define SCADA_TAG_WAIT_SET_H

// Waiting on several change rings and cycle counters at once
// (docs/contracts/tag-database.md, section 8.4): one `futex_waitv` for all
// of them, and on request a thread of the library that turns their wakes
// into a process-local eventfd for poll and epoll. The C interface
// `scada_waitset_*` is a thin layer over this class. Private to
// libscada-tag.

#include "attachment.h"
#include "futex.h"
#include "subscription.h"

#include "scada/common/fd.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

namespace scada::tag {

// The most sources of one wait set: the kernel's limit for one wait, less
// the word that stops the thread of `fd`.
inline constexpr std::size_t MAX_WAIT_SET_SOURCES = MAX_FUTEX_WAITERS - 1;

// What the thread of `WaitSet::fd` last saw of one source. A change of any
// field makes the descriptor readable.
struct WaitSetSeen {
    const std::byte* mapping = nullptr;
    std::uint32_t epoch = 0;
    std::uint32_t word = 0;  // the futex word: write_seq or cycle_seq

    friend bool operator==(const WaitSetSeen&, const WaitSetSeen&) = default;
};

// What the thread of `WaitSet::fd` watches: copied when it starts, so that
// it shares nothing with the thread that uses the set.
struct WaitSetWatch {
    bool ring = false;      // a ring of an owner, or a cycle counter
    std::size_t index = 0;  // owner index or process index
};

// One thread uses a wait set and its subscriptions (section 13.2). The
// attachment must outlive it.
class WaitSet {
public:
    explicit WaitSet(Attachment& attachment) noexcept
        : attachment_(attachment) {}

    // Stops the thread of `fd`, if it runs, and takes the subscriptions out.
    ~WaitSet();
    WaitSet(const WaitSet&) = delete;
    WaitSet& operator=(const WaitSet&) = delete;
    WaitSet(WaitSet&&) = delete;
    WaitSet& operator=(WaitSet&&) = delete;

    // As scada_waitset_add_sub. The subscription is borrowed: it leaves the
    // set when it is destroyed (`remove`).
    [[nodiscard]] int add_sub(Subscription& subscription);

    // As scada_waitset_add_cycle.
    [[nodiscard]] int add_cycle(std::uint16_t process_index);

    // As scada_waitset_wait.
    [[nodiscard]] int wait(int timeout_ms);

    // As scada_waitset_fd.
    [[nodiscard]] int fd(int& out);

    // Takes `subscription` out of the set. Called by its destructor.
    void remove(const Subscription& subscription) noexcept;

private:
    // A cycle counter of the set: the process, and the count the last
    // return of `wait` saw.
    struct Cycle {
        std::uint16_t process_index = 0;
        std::uint32_t seen = 0;
    };

    using Watch = WaitSetWatch;

    // True if a source is ready now. Cycle counters that advanced count as
    // seen from here on.
    [[nodiscard]] bool take_ready() noexcept;

    // Puts the word of every source that is mapped into `waiters` and
    // returns how many; `missing` tells whether a source is not mapped.
    [[nodiscard]] std::size_t collect_waiters(std::span<FutexWaiter> waiters,
                                              bool& missing) const noexcept;

    // The body of the thread of `fd`: waits on the sources, compares them
    // with `seen` and makes `event_fd` readable when one changed, until
    // `stop` is 1.
    static void watch(Attachment& attachment, std::span<const Watch> watches,
                      std::vector<WaitSetSeen> seen, int event_fd,
                      const std::atomic<std::uint32_t>& stop);

    [[nodiscard]] bool full() const noexcept {
        return subscriptions_.size() + cycles_.size() >= MAX_WAIT_SET_SOURCES;
    }

    Attachment& attachment_;
    std::vector<Subscription*> subscriptions_;  // borrowed
    std::vector<Cycle> cycles_;

    // The eventfd of `fd`, and the word that stops its thread: 1 tells it
    // to end. Private to this process; the thread waits on it too.
    Fd event_;
    std::atomic<std::uint32_t> stop_{0};
    std::jthread watcher_;
};

}  // namespace scada::tag

#endif  // SCADA_TAG_WAIT_SET_H
