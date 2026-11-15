#include "peer_regions.h"

#include "header_view.h"
#include "layout.h"
#include "region_file.h"

#include "scada/common/mapped_region.h"
#include "scada/common/panic.h"
#include "scada/common/result.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>

namespace scada::tag {

namespace {

// Slots of one owner: tags, strings, changes.
constexpr std::size_t SLOTS_PER_OWNER = 3;
constexpr std::size_t TAGS_PART = 0;
constexpr std::size_t STRINGS_PART = 1;
constexpr std::size_t CHANGES_PART = 2;

RegionKind region_kind_of(QueueKind kind) noexcept {
    switch (kind) {
        case QueueKind::EVENTS:
            return RegionKind::EVENTS;
        case QueueKind::CMD:
            return RegionKind::CMD;
        case QueueKind::RESULT:
            return RegionKind::RESULT;
    }
    return RegionKind::EVENTS;
}

}  // namespace

PeerRegions::PeerRegions(const HeaderContents& contents)
    : process_count_(contents.processes.size()),
      owner_count_(contents.owners.size()),
      slots_(contents.processes.size() +
             (contents.owners.size() * SLOTS_PER_OWNER) +
             contents.queues.size()) {
    const auto describe = [&](std::size_t index, std::string name,
                              RegionKind kind, std::uint64_t size,
                              std::size_t owner_index, std::uint16_t queue_id,
                              std::uint32_t uid) {
        Slot& slot = slots_.at(index);
        slot.name = std::move(name);
        slot.identity.kind = kind;
        slot.identity.generation = contents.generation;
        slot.identity.size_bytes = size;
        // Below MAX_PROCESSES or MAX_OWNERS, which fit 16 bits.
        slot.identity.owner_index = static_cast<std::uint16_t>(owner_index);
        slot.identity.queue_id = queue_id;
        slot.identity.uid = uid;
        slot.exists = true;
    };
    for (std::size_t i = 0; i < contents.processes.size(); ++i) {
        const ProcessInfo& process = contents.processes[i];
        describe(i, proc_region_name(process.name), RegionKind::PROC,
                 proc_region_size(contents.queues.size()), i, NO_QUEUE_ID,
                 process.uid);
    }
    for (std::size_t i = 0; i < contents.owners.size(); ++i) {
        const OwnerInfo& owner = contents.owners[i];
        const std::size_t first = process_count_ + (i * SLOTS_PER_OWNER);
        describe(first + TAGS_PART, tags_region_name(owner.name),
                 RegionKind::TAGS, tags_region_size(owner.tag_count), i,
                 NO_QUEUE_ID, owner.uid);
        if (owner.string_slots != 0) {
            describe(first + STRINGS_PART, strings_region_name(owner.name),
                     RegionKind::STRINGS,
                     strings_region_size(owner.string_slots), i, NO_QUEUE_ID,
                     owner.uid);
        }
        describe(first + CHANGES_PART, changes_region_name(owner.name),
                 RegionKind::CHANGES,
                 changes_region_size(owner.change_ring_capacity), i,
                 NO_QUEUE_ID, owner.uid);
    }
    for (std::size_t i = 0; i < contents.queues.size(); ++i) {
        const QueueInfo& queue = contents.queues[i];
        // The region of a queue is written by its producer. Its
        // `owner_index` is the producer's process index. A queue id is
        // below MAX_QUEUES, which fits 16 bits.
        describe(process_count_ + (owner_count_ * SLOTS_PER_OWNER) + i,
                 queue.name, region_kind_of(queue.kind),
                 queue_region_size(queue.capacity, queue.record_size),
                 queue.producer_process, static_cast<std::uint16_t>(i),
                 contents.processes.at(queue.producer_process).uid);
    }
}

PeerRegions::~PeerRegions() = default;

std::size_t PeerRegions::checked(std::size_t slot) const {
    if (slot >= slots_.size() || !slots_[slot].exists) {
        panic("peer regions: no such region in this generation");
    }
    return slot;
}

std::size_t PeerRegions::proc_slot(std::size_t process_index) const {
    if (process_index >= process_count_) {
        panic("peer regions: no such process");
    }
    return checked(process_index);
}

std::size_t PeerRegions::tags_slot(std::size_t owner_index) const {
    if (owner_index >= owner_count_) {
        panic("peer regions: no such owner");
    }
    return checked(process_count_ + (owner_index * SLOTS_PER_OWNER) +
                   TAGS_PART);
}

std::size_t PeerRegions::strings_slot(std::size_t owner_index) const {
    if (owner_index >= owner_count_) {
        panic("peer regions: no such owner");
    }
    return checked(process_count_ + (owner_index * SLOTS_PER_OWNER) +
                   STRINGS_PART);
}

std::size_t PeerRegions::changes_slot(std::size_t owner_index) const {
    if (owner_index >= owner_count_) {
        panic("peer regions: no such owner");
    }
    return checked(process_count_ + (owner_index * SLOTS_PER_OWNER) +
                   CHANGES_PART);
}

std::size_t PeerRegions::queue_slot(std::size_t queue_id) const {
    const std::size_t first = process_count_ + (owner_count_ * SLOTS_PER_OWNER);
    if (queue_id >= slots_.size() - first) {
        panic("peer regions: no such queue");
    }
    return checked(first + queue_id);
}

bool PeerRegions::look(int dir_fd, Slot& slot) {
    // Relaxed: only this function writes it, under the mutex.
    const OpenedRegion* const before =
        slot.current.load(std::memory_order_relaxed);
    const auto drop = [&] {
        // Release: readers load with acquire.
        slot.current.store(nullptr, std::memory_order_release);
        return before != nullptr;
    };

    struct stat info {};
    if (::fstatat(dir_fd, slot.name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        return drop();
    }
    // The same file as before, and not shrunk (section 3.3): nothing to do.
    if (before != nullptr && info.st_dev == before->device &&
        info.st_ino == before->inode && info.st_size >= 0 &&
        static_cast<std::uint64_t>(info.st_size) == slot.identity.size_bytes) {
        return false;
    }
    if (slot.mappings >= MAX_MAPPINGS_PER_REGION) {
        return drop();
    }
    std::string ignored;
    Result<OpenedRegion> opened = open_region(dir_fd, slot.name, slot.identity,
                                              MapAccess::READ_ONLY, ignored);
    // Brought into memory here, never inside a real-time function. A file
    // that was shrunk after the checks fails here with an error, not later
    // with a signal.
    if (!opened.ok() || !opened.value().memory.prefault().ok()) {
        return drop();
    }
    // The mapping stays without the descriptor.
    opened.value().file.reset();
    mappings_.push_back(
        std::make_unique<const OpenedRegion>(std::move(opened).value()));
    ++slot.mappings;
    // Release: publishes the mapping to readers, which load with acquire.
    slot.current.store(mappings_.back().get(), std::memory_order_release);
    return true;
}

void PeerRegions::want(int dir_fd, std::size_t slot, std::int64_t now_mono_ns) {
    Slot& wanted = slots_.at(checked(slot));
    const std::scoped_lock lock(mutex_);
    if (wanted.wanted) {
        return;
    }
    wanted.wanted = true;
    wanted_.push_back(slot);
    static_cast<void>(look(dir_fd, wanted));
    wanted.next_look_mono_ns = now_mono_ns + PEER_RETRY_INTERVAL_NS;
    // Release, after the first look: a reader that sees the flag with
    // acquire also sees the mapping that look published.
    wanted.asked.store(true, std::memory_order_release);
}

bool PeerRegions::asked(std::size_t slot) const noexcept {
    return slot < slots_.size() &&
           slots_[slot].asked.load(std::memory_order_acquire);
}

std::span<std::byte> PeerRegions::bytes(std::size_t slot) const noexcept {
    if (slot >= slots_.size()) {
        return {};
    }
    // Acquire: pairs with the release store that published the mapping.
    const OpenedRegion* const region =
        slots_[slot].current.load(std::memory_order_acquire);
    return region == nullptr ? std::span<std::byte>{} : region->memory.bytes();
}

std::size_t PeerRegions::refresh(int dir_fd, std::int64_t now_mono_ns) {
    const std::scoped_lock lock(mutex_);
    std::size_t changed = 0;
    for (const std::size_t index : wanted_) {
        Slot& slot = slots_.at(index);
        if (now_mono_ns < slot.next_look_mono_ns) {
            continue;
        }
        if (look(dir_fd, slot)) {
            ++changed;
        }
        slot.next_look_mono_ns = now_mono_ns + PEER_RETRY_INTERVAL_NS;
    }
    return changed;
}

}  // namespace scada::tag
