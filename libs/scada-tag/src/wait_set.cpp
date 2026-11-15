#include "wait_set.h"

#include "attachment.h"
#include "change_ring.h"
#include "futex.h"
#include "proc_region.h"
#include "queue.h"
#include "queue_region.h"
#include "shared_memory.h"
#include "subscription.h"

#include "scada/common/clock.h"
#include "scada/common/fd.h"
#include "scada/tag/scada_tag.h"

#include <sys/eventfd.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
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

using WatchKind = WaitSetWatch::Kind;

// What a source shows now: the ring of owner `index`, the cycle counter of
// process `index` or the queue `index`.
Seen look_at(Attachment& attachment, WatchKind kind,
             std::size_t index) noexcept {
    Seen seen;
    if (kind == WatchKind::QUEUE) {
        const std::span<std::byte> bytes = attachment.queue_bytes(index);
        seen.mapping = bytes.data();
        if (!bytes.empty()) {
            seen.word = futex_word_of(queue_write_seq(bytes));
        }
        return seen;
    }
    if (kind == WatchKind::RING) {
        const std::span<std::byte> bytes = attachment.changes_of(index);
        seen.mapping = bytes.data();
        if (!bytes.empty()) {
            seen.epoch = ring_epoch(bytes);
            seen.word = futex_word_of(ring_write_seq(bytes));
        }
        // A dead owner wakes nobody: the slice of the wait finds it.
        seen.owner_down = attachment.owner_process_down(index);
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

// The futex word of `watch` in its mapping now, if that is still the
// mapping `seen` belongs to; null otherwise.
const void* watched_word(Attachment& attachment, const WaitSetWatch& watch,
                         const Seen& seen) noexcept {
    std::span<std::byte> bytes;
    switch (watch.kind) {
        case WatchKind::RING:
            bytes = attachment.changes_of(watch.index);
            break;
        case WatchKind::QUEUE:
            bytes = attachment.queue_bytes(watch.index);
            break;
        case WatchKind::CYCLE:
            // In range: the index was checked by `add_cycle`.
            bytes = attachment.proc_of(static_cast<std::uint16_t>(watch.index));
            break;
    }
    if (bytes.empty() || bytes.data() != seen.mapping) {
        return nullptr;
    }
    switch (watch.kind) {
        case WatchKind::RING:
            return &ring_header(bytes).write_seq;
        case WatchKind::QUEUE:
            return &queue_header(bytes).write_seq;
        case WatchKind::CYCLE:
            break;
    }
    return cycle_field(bytes);
}

// Looks at every source of `watches` again and stores what it shows in
// `seen`. True if any changed.
bool look_again(Attachment& attachment, std::span<const WaitSetWatch> watches,
                std::span<Seen> seen) noexcept {
    bool changed = false;
    for (std::size_t i = 0; i < watches.size(); ++i) {
        const Seen now = look_at(attachment, watches[i].kind, watches[i].index);
        if (now != seen[i]) {
            seen[i] = now;
            changed = true;
        }
    }
    return changed;
}

// Makes `event_fd` readable. An eventfd counter does not overflow from
// this; a failed write leaves it readable from an earlier one.
void signal(int event_fd) noexcept {
    const std::uint64_t one = 1;
    const ssize_t written = ::write(event_fd, &one, sizeof(one));
    static_cast<void>(written);
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
    for (Queue* queue : queues_) {
        queue->set_wait_set(nullptr);
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

int WaitSet::add_queue(Queue& queue) {
    if (&queue.attachment() != &attachment_ || queue.producer() ||
        queue.wait_set() != nullptr || full() || event_.valid()) {
        return SCADA_E_INVALID;
    }
    queues_.push_back(&queue);
    queue.set_wait_set(this);
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

void WaitSet::remove(const Queue& queue) noexcept {
    std::erase(queues_, &queue);
}

bool WaitSet::take_ready() noexcept {
    bool ready = std::ranges::any_of(
        subscriptions_,
        [](const Subscription* subscription) { return subscription->ready(); });
    ready = ready || std::ranges::any_of(queues_, [](const Queue* queue) {
                return queue->ready();
            });
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
        bool missing = false;
        const std::size_t count = collect_waiters(waiters, missing);
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

std::size_t WaitSet::collect_waiters(std::span<FutexWaiter> waiters,
                                     bool& missing) const noexcept {
    std::size_t count = 0;
    missing = false;
    for (const Subscription* subscription : subscriptions_) {
        if (const std::optional<FutexWaiter> word = subscription->waiter()) {
            waiters[count++] = *word;
        } else {
            missing = true;
        }
    }
    for (const Queue* queue : queues_) {
        if (const std::optional<FutexWaiter> word = queue->waiter()) {
            waiters[count++] = *word;
        } else {
            missing = true;
        }
    }
    for (const Cycle& cycle : cycles_) {
        if (const std::uint32_t* field =
                cycle_field(attachment_.proc_of(cycle.process_index))) {
            waiters[count++] = {.word = field,
                                .expected = cycle.seen,
                                .process_private = false};
        } else {
            missing = true;
        }
    }
    return count;
}

int WaitSet::fd(int& out) {
    if (!event_.valid()) {
        Fd event(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
        if (!event.valid()) {
            return SCADA_E_IO;
        }
        std::vector<Watch> watches;
        watches.reserve(subscriptions_.size() + queues_.size() +
                        cycles_.size());
        for (const Subscription* subscription : subscriptions_) {
            watches.push_back({.kind = WatchKind::RING,
                               .index = subscription->owner_index()});
        }
        for (const Queue* queue : queues_) {
            watches.push_back({.kind = WatchKind::QUEUE, .index = queue->id()});
        }
        for (const Cycle& cycle : cycles_) {
            watches.push_back(
                {.kind = WatchKind::CYCLE, .index = cycle.process_index});
        }
        // What the thread compares against, taken before the look at what
        // is ready already: a change after the look is then a change for
        // the thread, and one before it is ready here.
        std::vector<Seen> seen;
        seen.reserve(watches.size());
        for (const Watch& watch : watches) {
            seen.push_back(look_at(attachment_, watch.kind, watch.index));
        }
        const bool ready_now =
            std::ranges::any_of(subscriptions_,
                                [](const Subscription* subscription) {
                                    return subscription->ready();
                                }) ||
            std::ranges::any_of(
                queues_, [](const Queue* queue) { return queue->ready(); });
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
            const void* word = watched_word(attachment, watches[i], seen[i]);
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
        if (look_again(attachment, watches, seen)) {
            signal(event_fd);
        }
    }
}

}  // namespace scada::tag
