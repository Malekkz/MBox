#include "wait_set.h"

#include "attachment.h"
#include "change_ring.h"
#include "futex.h"
#include "proc_region.h"
#include "shared_memory.h"
#include "subscription.h"

#include "scada/common/clock.h"
#include "scada/common/fd.h"
#include "scada/tag/scada_tag.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace scada::tag {

namespace {

using Seen = WaitSetSeen;

// The current count of `cycle`, a field of a mapped `proc` region.
std::uint32_t cycle_count(std::uint32_t& cycle) noexcept {
    // Acquire: pairs with the release increment of scada_tag_notify.
    return load_shared(cycle, std::memory_order_acquire);
}

// What `source` shows now: a ring of an owner, or a cycle counter.
Seen look_at(Attachment& attachment, bool ring, std::size_t index) noexcept {
    Seen seen;
    if (ring) {
        const std::span<std::byte> bytes = attachment.changes_of(index);
        seen.mapping = bytes.data();
        if (!bytes.empty()) {
            seen.epoch = ring_epoch(bytes);
            seen.word = futex_word_of(ring_write_seq(bytes));
        }
        return seen;
    }
    // In range: the index was checked by `add_cycle`.
    const std::span<std::byte> bytes =
        attachment.proc_of(static_cast<std::uint16_t>(index));
    seen.mapping = bytes.data();
    if (std::uint32_t* field = cycle_field(bytes)) {
        seen.word = cycle_count(*field);
    }
    return seen;
}

// Makes `event_fd` readable. An eventfd counter does not overflow from
// this; a failed write leaves it readable from an earlier one.
void signal(int event_fd) noexcept {
    const std::uint64_t one = 1;
    static_cast<void>(::write(event_fd, &one, sizeof(one)));
}

}  // namespace

WaitSet::~WaitSet() {
    if (watcher_.joinable()) {
        // Release: the thread loads it with acquire.
        stop_.store(1, std::memory_order_release);
        // A failed wake leaves the thread to see the word within a slice.
        static_cast<void>(futex_wake_all(&stop_, true));
        watcher_.join();
    }
    for (Subscription* subscription : subscriptions_) {
        subscription->set_wait_set(nullptr);
    }
}

int WaitSet::add_sub(Subscription& subscription) {
    if (&subscription.attachment() != &attachment_ ||
        subscription.wait_set() != nullptr || full() || event_.valid()) {
        return SCADA_E_INVALID;
    }
    subscriptions_.push_back(&subscription);
    subscription.set_wait_set(this);
    return SCADA_OK;
}

int WaitSet::add_cycle(std::uint16_t process_index) {
    if (process_index >= attachment_.process_count() || full() ||
        event_.valid()) {
        return SCADA_E_INVALID;
    }
    // Mapped and brought into memory here (section 11.2).
    attachment_.want_proc(process_index);
    Cycle cycle{.process_index = process_index, .seen = 0};
    if (std::uint32_t* field = cycle_field(attachment_.proc_of(process_index));
        field != nullptr) {
        cycle.seen = cycle_count(*field);
    }
    cycles_.push_back(cycle);
    return SCADA_OK;
}

void WaitSet::remove(const Subscription& subscription) noexcept {
    std::erase(subscriptions_, &subscription);
}

bool WaitSet::take_ready() noexcept {
    bool ready = std::ranges::any_of(
        subscriptions_,
        [](const Subscription* subscription) { return subscription->ready(); });
    for (Cycle& cycle : cycles_) {
        std::uint32_t* field =
            cycle_field(attachment_.proc_of(cycle.process_index));
        if (field == nullptr) {
            continue;
        }
        const std::uint32_t count = cycle_count(*field);
        if (count != cycle.seen) {
            cycle.seen = count;
            ready = true;
        }
    }
    return ready;
}

int WaitSet::wait(int timeout_ms) {
    if (timeout_ms < 0) {
        return SCADA_E_INVALID;
    }
    const std::int64_t deadline = deadline_after(monotonic_ns(), timeout_ms);
    for (;;) {
        if (take_ready()) {
            return SCADA_OK;
        }
        const std::int64_t now = monotonic_ns();
        if (now >= deadline) {
            return SCADA_E_TIMEOUT;
        }
        const std::int64_t slice_end =
            deadline - now < WAIT_SLICE_NS ? deadline : now + WAIT_SLICE_NS;

        std::array<FutexWaiter, MAX_FUTEX_WAITERS> waiters{};
        std::size_t count = 0;
        bool missing = false;
        for (const Subscription* subscription : subscriptions_) {
            if (const std::optional<FutexWaiter> word =
                    subscription->waiter()) {
                waiters.at(count++) = *word;
            } else {
                missing = true;
            }
        }
        for (const Cycle& cycle : cycles_) {
            if (const std::uint32_t* field =
                    cycle_field(attachment_.proc_of(cycle.process_index))) {
                waiters.at(count++) = {.word = field,
                                       .expected = cycle.seen,
                                       .process_private = false};
            } else {
                missing = true;
            }
        }
        if (missing) {
            // Look for missing regions again, at most once a second each.
            attachment_.refresh_peers();
        }
        if (count == 0) {
            sleep_until(slice_end);
            continue;
        }
        if (futex_wait_any(std::span<const FutexWaiter>(waiters).first(count),
                           slice_end) == FutexWait::FAILED) {
            return SCADA_E_IO;
        }
    }
}

int WaitSet::fd(int& out) {
    if (!event_.valid()) {
        Fd event(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
        if (!event.valid()) {
            return SCADA_E_IO;
        }
        std::vector<Watch> watches;
        watches.reserve(subscriptions_.size() + cycles_.size());
        for (const Subscription* subscription : subscriptions_) {
            watches.push_back(
                {.ring = true, .index = subscription->owner_index()});
        }
        for (const Cycle& cycle : cycles_) {
            watches.push_back({.ring = false, .index = cycle.process_index});
        }
        // What the thread compares against, taken before the look at what
        // is ready already: a change after the look is then a change for
        // the thread, and one before it is ready here.
        std::vector<Seen> seen;
        seen.reserve(watches.size());
        for (const Watch& watch : watches) {
            seen.push_back(look_at(attachment_, watch.ring, watch.index));
        }
        const bool ready_now = std::ranges::any_of(
            subscriptions_, [](const Subscription* subscription) {
                return subscription->ready();
            });
        try {
            // The thread is joined by the destructor, before `event_` and
            // `stop_` go; the references stay valid for its whole life.
            watcher_ = std::jthread(
                [&attachment = attachment_, watches = std::move(watches),
                 seen = std::move(seen), event_fd = event.get(),
                 &stop = stop_]() mutable {
                    watch(attachment, watches, std::move(seen), event_fd, stop);
                });
        } catch (const std::system_error& error) {
            errno = error.code().value();
            return SCADA_E_IO;
        }
        event_ = std::move(event);
        if (ready_now) {
            signal(event_.get());
        }
    }
    out = event_.get();
    return SCADA_OK;
}

void WaitSet::watch(Attachment& attachment, std::span<const Watch> watches,
                    std::vector<Seen> seen, int event_fd,
                    const std::atomic<std::uint32_t>& stop) {
    std::array<FutexWaiter, MAX_FUTEX_WAITERS> waiters{};
    // Acquire: pairs with the release store of the destructor.
    while (stop.load(std::memory_order_acquire) == 0) {
        std::size_t count = 0;
        waiters.at(count++) = {
            .word = &stop, .expected = 0, .process_private = true};
        bool missing = false;
        for (std::size_t i = 0; i < watches.size(); ++i) {
            const Watch& watch = watches[i];
            const std::span<std::byte> bytes =
                watch.ring ? attachment.changes_of(watch.index)
                           : attachment.proc_of(
                                 static_cast<std::uint16_t>(watch.index));
            if (bytes.empty() || bytes.data() != seen[i].mapping) {
                missing = true;
                continue;
            }
            const void* word =
                watch.ring
                    ? static_cast<const void*>(&ring_header(bytes).write_seq)
                    : static_cast<const void*>(cycle_field(bytes));
            if (word == nullptr) {
                missing = true;
                continue;
            }
            waiters.at(count++) = {.word = word,
                                   .expected = seen[i].word,
                                   .process_private = false};
        }
        if (missing) {
            attachment.refresh_peers();
        }
        // A failed wait is retried after the look below; the slice bounds
        // how often.
        const FutexWait result =
            futex_wait_any(std::span<const FutexWaiter>(waiters).first(count),
                           monotonic_ns() + WAIT_SLICE_NS);
        if (result == FutexWait::FAILED) {
            sleep_until(monotonic_ns() + WAIT_SLICE_NS);
        }
        bool changed = false;
        for (std::size_t i = 0; i < watches.size(); ++i) {
            const Seen now =
                look_at(attachment, watches[i].ring, watches[i].index);
            if (now != seen[i]) {
                seen[i] = now;
                changed = true;
            }
        }
        if (changed) {
            signal(event_fd);
        }
    }
}

}  // namespace scada::tag
