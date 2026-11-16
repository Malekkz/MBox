#ifndef SCADA_TEST_CPU_AFFINITY_H
#define SCADA_TEST_CPU_AFFINITY_H

// Test support: the processors a thread may run on. Linux only.
//
// Conformance tests 3 and 26 of the tag database contract put writers and
// readers on chosen processors. A thread inherits the set of the thread
// that creates it, and a process the set of the thread that starts it, so
// a test sets the set of its main thread before it starts a process or a
// thread, and puts it back afterwards (`PinnedThread`).

#include <optional>
#include <span>
#include <vector>

namespace scada::test {

// The processors the calling thread may run on, in increasing order;
// empty if the kernel refused to tell.
[[nodiscard]] std::vector<int> allowed_cpus();

// Lets the calling thread run only on `cpus`. False if `cpus` is empty,
// names a processor out of range, or the kernel refused.
[[nodiscard]] bool pin_this_thread(std::span<const int> cpus);

// The processors the thread or process `tid` may run on, in increasing
// order; nothing if the kernel refused to tell, for example because `tid`
// has ended.
[[nodiscard]] std::optional<std::vector<int>> cpus_of(int tid);

// Pins the calling thread to a set and puts its earlier set back when it
// goes out of scope. Create and destroy it on the same thread.
class PinnedThread {
public:
    // Pins the calling thread to `cpus`; `pinned()` says whether it worked.
    explicit PinnedThread(std::span<const int> cpus);
    ~PinnedThread();
    PinnedThread(const PinnedThread&) = delete;
    PinnedThread& operator=(const PinnedThread&) = delete;
    PinnedThread(PinnedThread&&) = delete;
    PinnedThread& operator=(PinnedThread&&) = delete;

    [[nodiscard]] bool pinned() const noexcept { return pinned_; }

    // Pins the calling thread to `cpus` instead; the set put back at the
    // end stays the one from before the constructor. False if it failed.
    [[nodiscard]] bool move_to(std::span<const int> cpus);

private:
    std::vector<int> before_;
    bool pinned_ = false;
};

}  // namespace scada::test

#endif  // SCADA_TEST_CPU_AFFINITY_H
