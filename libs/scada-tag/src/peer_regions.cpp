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
                 queue.name, queue_region_kind(queue.kind),
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

bool PeerRegions::is_queue(const Slot& slot) noexcept {
    const RegionKind kind = slot.identity.kind;
    return kind == RegionKind::EVENTS || kind == RegionKind::CMD ||
           kind == RegionKind::RESULT;
}

void PeerRegions::check_kept(const Slot& slot) noexcept {
    // Issue #53: a queue file can be shrunk in place and then replaced
    // under its name, which no look by name sees; its consumer may still
    // read the old mapping. Every kept mapping of the slot is checked by
    // its descriptor here, outside real-time code, and a changed one is
    // marked before any new mapping is published.
    for (const std::unique_ptr<PeerMapping>& mapping : mappings_) {
        if (mapping->slot != &slot || !mapping->region.file.valid() ||
            !mapping->whole.load(std::memory_order_relaxed)) {
            continue;
        }
        struct stat info {};
        if (::fstat(mapping->region.file.get(), &info) != 0 ||
            info.st_size < 0 ||
            static_cast<std::uint64_t>(info.st_size) !=
                slot.identity.size_bytes) {
            // Release: a reader that loads the slot's new mapping with
            // acquire, or this flag with acquire, sees it.
            mapping->whole.store(false, std::memory_order_release);
        }
    }
}

bool PeerRegions::look_slot(int dir_fd, Slot& slot) {
    // Relaxed: only this function writes it, under the mutex.
    const PeerMapping* const before =
        slot.current.load(std::memory_order_relaxed);
    const auto drop = [&] {
        // Release: readers load with acquire.
        slot.current.store(nullptr, std::memory_order_release);
        return before != nullptr;
    };
    if (is_queue(slot)) {
        check_kept(slot);
    }

    struct stat info {};
    if (::fstatat(dir_fd, slot.name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        return drop();
    }
    // The same file as before, and not shrunk (section 3.3): nothing to do.
    const bool same_file = before != nullptr &&
                           info.st_dev == before->region.device &&
                           info.st_ino == before->region.inode;
    if (same_file && info.st_size >= 0 &&
        static_cast<std::uint64_t>(info.st_size) == slot.identity.size_bytes) {
        return false;
    }
    if (same_file) {
        // The mapped file itself changed size (issue #53): its mapping must
        // not be read again. Release: as in `check_kept`.
        before->whole.store(false, std::memory_order_release);
        return drop();
    }
    // The same file as one mapped before, after a look that failed (for
    // example a transient fstatat error) or while it was under another
    // name: its kept mapping is used again, and no mapping slot is spent
    // (decision D-51). Only a mapping that is still whole. It skips the
    // header checks of section 3.1 that a fresh open makes: they passed
    // when it was first mapped, and the checks that matter for a queue
    // reused this way are the consumer's range rule and base (`follow_queue`),
    // which stop a queue whose file was shrunk and grown again to its size
    // between two looks (review of PR #86, N4).
    for (const std::unique_ptr<PeerMapping>& kept : mappings_) {
        if (kept->slot == &slot && kept->region.device == info.st_dev &&
            kept->region.inode == info.st_ino && info.st_size >= 0 &&
            static_cast<std::uint64_t>(info.st_size) ==
                slot.identity.size_bytes &&
            kept->whole.load(std::memory_order_relaxed)) {
            // Release: publishes the mapping to readers, which load with
            // acquire.
            slot.current.store(kept.get(), std::memory_order_release);
            return before != kept.get();
        }
    }
    if (slot.mappings >= MAX_MAPPINGS_PER_REGION) {
        // Relaxed: only this function writes it, under the mutex, and
        // readers judge it on its own.
        slot.at_limit.store(true, std::memory_order_relaxed);
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
    // The mapping stays without the descriptor, except for a queue region,
    // whose descriptor `check_kept` uses.
    if (!is_queue(slot)) {
        opened.value().file.reset();
    }
    auto mapping = std::make_unique<PeerMapping>();
    mapping->region = std::move(opened).value();
    mapping->slot = &slot;
    mappings_.push_back(std::move(mapping));
    ++slot.mappings;
    // Release: publishes the mapping to readers, which load with acquire.
    slot.current.store(mappings_.back().get(), std::memory_order_release);
    return true;
}

bool PeerRegions::look(int dir_fd, Slot& slot) {
    const bool changed = look_slot(dir_fd, slot);
    // Relaxed: only `look_slot` writes it, under the mutex, which this
    // thread holds.
    if (changed && slot.current.load(std::memory_order_relaxed) != nullptr) {
        look_at_producer(dir_fd, slot);
    }
    return changed;
}

void PeerRegions::look_at_producer(int dir_fd, const Slot& slot) {
    if (!is_queue(slot)) {
        return;
    }
    // The producer created its `proc` region, complete, before the queue
    // file (contract 11.2, step 3), so it is there unless the producer has
    // gone since: a consumer that can see a record of the queue can see the
    // producer, which its answers and its liveness need (section 9.2,
    // decision D-61). Without this, the two slots are looked at on timers
    // of their own, up to a second apart (issue #92). Asked for here, not
    // when the queue is opened: a slot asked for while missing is not
    // looked at again by a later `want` (a resolve), only by `refresh`.
    const std::size_t index = slot.identity.owner_index;
    Slot& producer = slots_.at(index);
    if (!producer.wanted) {
        producer.wanted = true;
        wanted_.push_back(index);
    }
    static_cast<void>(look_slot(dir_fd, producer));
    // Release, after the look: as in `want`.
    producer.asked.store(true, std::memory_order_release);
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
    const PeerMapping* const current = mapping(slot);
    return current == nullptr ? std::span<std::byte>{}
                              : current->region.memory.bytes();
}

const PeerMapping* PeerRegions::mapping(std::size_t slot) const noexcept {
    if (slot >= slots_.size()) {
        return nullptr;
    }
    // Acquire: pairs with the release store that published the mapping.
    return slots_[slot].current.load(std::memory_order_acquire);
}

bool PeerRegions::at_mapping_limit(std::size_t slot) const noexcept {
    return slot < slots_.size() &&
           slots_[slot].at_limit.load(std::memory_order_relaxed);
}

std::size_t PeerRegions::refresh(int dir_fd, std::int64_t now_mono_ns) {
    const std::scoped_lock lock(mutex_);
    return refresh_locked(dir_fd, now_mono_ns);
}

std::size_t PeerRegions::try_refresh(int dir_fd, std::int64_t now_mono_ns) {
    // Another thread is looking, mapping or bringing a region into memory:
    // its look covers this one, and the caller must not wait for it
    // (decision D-51).
    const std::unique_lock lock(mutex_, std::try_to_lock);
    return lock.owns_lock() ? refresh_locked(dir_fd, now_mono_ns) : 0;
}

std::size_t PeerRegions::refresh_locked(int dir_fd, std::int64_t now_mono_ns) {
    std::size_t changed = 0;
    // By position: a look can ask for a slot (`look_at_producer`), which
    // appends to `wanted_`.
    // NOLINTNEXTLINE(modernize-loop-convert): the body can append to `wanted_` (look_at_producer), which a range-based loop must not see
    for (std::size_t i = 0; i < wanted_.size(); ++i) {
        Slot& slot = slots_.at(wanted_[i]);
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

bool PeerRegions::look_now(int dir_fd, std::size_t slot) {
    Slot& wanted = slots_.at(checked(slot));
    const std::scoped_lock lock(mutex_);
    return wanted.wanted && look(dir_fd, wanted);
}

}  // namespace scada::tag
