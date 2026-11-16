#include "scada/test/cpu_affinity.h"

#include <sched.h>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace scada::test {

namespace {

// The members of `set`, in increasing order.
std::vector<int> members(const cpu_set_t& set) {
    std::vector<int> cpus;
    for (std::size_t cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        // NOLINTNEXTLINE(scada-banned,hicpp-signed-bitwise,cppcoreguidelines-pro-bounds-pointer-arithmetic): the glibc macro, which casts; `cpu` is in range
        if (CPU_ISSET(cpu, &set)) {
            // In range: below CPU_SETSIZE.
            cpus.push_back(static_cast<int>(cpu));
        }
    }
    return cpus;
}

}  // namespace

std::optional<std::vector<int>> cpus_of(int tid) {
    cpu_set_t set;
    // NOLINTNEXTLINE(scada-banned,hicpp-signed-bitwise,cppcoreguidelines-pro-bounds-pointer-arithmetic): the glibc macro, which casts
    CPU_ZERO(&set);
    if (::sched_getaffinity(tid, sizeof(set), &set) != 0) {
        return std::nullopt;
    }
    return members(set);
}

std::vector<int> allowed_cpus() {
    return cpus_of(0).value_or(std::vector<int>{});
}

bool pin_this_thread(std::span<const int> cpus) {
    if (cpus.empty()) {
        return false;
    }
    cpu_set_t set;
    // NOLINTNEXTLINE(scada-banned,hicpp-signed-bitwise,cppcoreguidelines-pro-bounds-pointer-arithmetic): the glibc macro, which casts
    CPU_ZERO(&set);
    for (const int cpu : cpus) {
        if (cpu < 0 || cpu >= CPU_SETSIZE) {
            return false;
        }
        // NOLINTNEXTLINE(scada-banned,hicpp-signed-bitwise,cppcoreguidelines-pro-bounds-pointer-arithmetic): the glibc macro, which casts; `cpu` is in range
        CPU_SET(static_cast<std::size_t>(cpu), &set);
    }
    // 0: the calling thread, not the whole process.
    return ::sched_setaffinity(0, sizeof(set), &set) == 0;
}

PinnedThread::PinnedThread(std::span<const int> cpus)
    : before_(allowed_cpus()),
      pinned_(!before_.empty() && pin_this_thread(cpus)) {}

PinnedThread::~PinnedThread() {
    if (!before_.empty()) {
        static_cast<void>(pin_this_thread(before_));
    }
}

bool PinnedThread::move_to(std::span<const int> cpus) {
    pinned_ = !before_.empty() && pin_this_thread(cpus);
    return pinned_;
}

}  // namespace scada::test
