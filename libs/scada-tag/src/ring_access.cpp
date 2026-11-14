// The parts of Attachment that serve the change rings and the wake-up
// (docs/contracts/tag-database.md, sections 8 and 10): notify, and what
// subscriptions and wait sets ask of the attachment.

#include "attachment.h"
#include "change_ring.h"
#include "futex.h"
#include "header_view.h"
#include "layout.h"
#include "peer_regions.h"
#include "proc_region.h"
#include "shared_memory.h"
#include "thread_mark.h"

#include "scada/common/clock.h"
#include "scada/tag/scada_tag.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace scada::tag {

int Attachment::notify() {
    const void* const self = thread_mark();
    int error = 0;
    for (const OwnedOwner& owned : owners_) {
        const std::size_t index = owned.owner_index;
        // Relaxed: compared only. A ring nobody wrote yet has no writer.
        if (writer_threads_[index].load(std::memory_order_relaxed) != self) {
            continue;
        }
        ChangeRingHeader& ring = ring_header(access_[index].own_changes);
        // Relaxed: this thread is the writer of the ring.
        const std::uint64_t written =
            load_shared(ring.write_seq, std::memory_order_relaxed);
        // Relaxed: see the declaration of `woken_seq_`.
        if (woken_seq_[index].exchange(written, std::memory_order_relaxed) ==
            written) {
            continue;  // nothing written since the last wake
        }
        if (!futex_wake_all(&ring.write_seq)) {
            error = errno;
        }
    }
    if (!futex_wake_all(proc_.advance_cycle())) {
        error = errno;
    }

    const std::int64_t now = monotonic_ns();
    static_cast<void>(peers_.refresh(directory_.get(), now));
    // Relaxed: see the declaration.
    if (now >= next_clock_check_mono_ns_.load(std::memory_order_relaxed)) {
        check_clock(now);
    }
    if (error != 0) {
        errno = error;
        return SCADA_E_IO;
    }
    return SCADA_OK;
}

std::optional<std::size_t> Attachment::find_owner(
    std::string_view name) const noexcept {
    const HeaderContents& contents = header_.contents();
    for (std::size_t i = 0; i < contents.owners.size(); ++i) {
        if (contents.owners[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

void Attachment::want_changes(std::size_t owner_index) {
    const OwnerAccess& owner = access_.at(owner_index);
    if (owner.own) {
        return;
    }
    const std::int64_t now = monotonic_ns();
    peers_.want(directory_.get(), owner.changes_slot, now);
    if (owner.process_index != process_index_) {
        peers_.want(directory_.get(), owner.proc_slot, now);
    }
}

void Attachment::want_proc(std::uint16_t process_index) {
    if (process_index == process_index_) {
        return;
    }
    peers_.want(directory_.get(), peers_.proc_slot(process_index),
                monotonic_ns());
}

std::span<std::byte> Attachment::changes_of(
    std::size_t owner_index) const noexcept {
    const OwnerAccess& owner = access_.at(owner_index);
    return owner.own ? owner.own_changes : peers_.bytes(owner.changes_slot);
}

std::span<std::byte> Attachment::proc_of(
    std::uint16_t process_index) const noexcept {
    if (process_index == process_index_) {
        return proc_.bytes();
    }
    return peers_.bytes(peers_.proc_slot(process_index));
}

std::uint32_t Attachment::ring_capacity(
    std::size_t owner_index) const noexcept {
    return access_.at(owner_index).ring_capacity;
}

void Attachment::refresh_peers() {
    static_cast<void>(peers_.refresh(directory_.get(), monotonic_ns()));
}

}  // namespace scada::tag
