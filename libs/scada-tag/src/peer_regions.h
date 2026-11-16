#ifndef SCADA_TAG_PEER_REGIONS_H
#define SCADA_TAG_PEER_REGIONS_H

// The regions of other processes, mapped read-only on demand
// (docs/contracts/tag-database.md, section 11.2, last paragraph). A region
// that is missing, not ready or fails a check of section 3.1 is not an
// error: it reads as missing and is looked for again later. Private to
// libscada-tag.

#include "header_view.h"
#include "region_file.h"

#include "scada/common/realtime.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace scada::tag {

// The shortest time between two looks at one region, in ns (section 11.2:
// at most once a second for each missing region).
inline constexpr std::int64_t PEER_RETRY_INTERVAL_NS = 1'000'000'000;

// How often one region may be mapped anew because its file was replaced. A
// replaced mapping stays until this object is destroyed, because a reader
// may still be using it; the limit bounds the memory a process that keeps
// replacing its region can take from its readers. Beyond it the region
// reads as missing until the reader attaches again.
inline constexpr std::uint32_t MAX_MAPPINGS_PER_REGION = 16;

// One mapping of a region. For a queue region its descriptor is kept, and
// `whole` turns false, for good, once a look found the file changed in size
// (by name, or for a file no longer under the name by `fstat` on that
// descriptor): reading such a mapping can end the process with SIGBUS
// (section 3.3). Set before any newer mapping of the slot is published.
struct PeerMapping {
    OpenedRegion region;
    const void* slot = nullptr;  // the slot it belongs to
    mutable std::atomic<bool> whole{true};
};

// One slot for every region the header of a generation lists. Safe to use
// from several threads: `bytes` from any thread at any time, `want` and
// `refresh` from threads that may block.
class PeerRegions {
public:
    // Slots for the regions of the generation `contents` describes.
    // Nothing is mapped yet.
    explicit PeerRegions(const HeaderContents& contents);

    ~PeerRegions();
    PeerRegions(const PeerRegions&) = delete;
    PeerRegions& operator=(const PeerRegions&) = delete;
    PeerRegions(PeerRegions&&) = delete;
    PeerRegions& operator=(PeerRegions&&) = delete;

    // The slot of a region. Each index must be below the count of the
    // header; a slot of `strings_slot` exists only for an owner with string
    // slots. A wrong index ends the process.
    [[nodiscard]] std::size_t proc_slot(std::size_t process_index) const;
    [[nodiscard]] std::size_t tags_slot(std::size_t owner_index) const;
    [[nodiscard]] std::size_t strings_slot(std::size_t owner_index) const;
    [[nodiscard]] std::size_t changes_slot(std::size_t owner_index) const;
    [[nodiscard]] std::size_t queue_slot(std::size_t queue_id) const;

    // Asks for the region of `slot`: it is looked for now, in the
    // generation directory open as `dir_fd`, and from then on by `refresh`.
    // Calling it again for the same slot does nothing. Not real-time.
    void want(int dir_fd, std::size_t slot, std::int64_t now_mono_ns);

    // True once `want` was called for `slot`, whether or not the region is
    // there. Tells a tag that was never resolved in this context from one
    // whose owner is down (contract, section 6.2). False for a slot that is
    // none.
    SCADA_RT [[nodiscard]] bool asked(std::size_t slot) const noexcept;

    // The mapped bytes of the region of `slot`, read-only and brought into
    // memory, or an empty span while the region is missing. The span stays
    // valid until this object is destroyed, also when the region is mapped
    // anew meanwhile. Storing through it ends the process with a signal.
    SCADA_RT [[nodiscard]] std::span<std::byte> bytes(
        std::size_t slot) const noexcept;

    // The mapping of `slot` that `bytes` gives, or null while the region is
    // missing (and for a slot that is none). Valid until this object is
    // destroyed, also when the region is mapped anew meanwhile.
    SCADA_RT [[nodiscard]] const PeerMapping* mapping(
        std::size_t slot) const noexcept;

    // True once `slot` reached MAX_MAPPINGS_PER_REGION and a further file
    // under its name was not mapped. Never cleared. False for a slot that is
    // none.
    SCADA_RT [[nodiscard]] bool at_mapping_limit(
        std::size_t slot) const noexcept;

    // Looks again at every region that was asked for and not looked at for
    // PEER_RETRY_INTERVAL_NS: a missing one is mapped if it is there now, a
    // mapped one whose file is gone or was replaced is dropped or mapped
    // anew. Returns how many slots changed. Called from scada_tag_notify
    // and scada_tag_status; not real-time.
    std::size_t refresh(int dir_fd, std::int64_t now_mono_ns);

    // As `refresh`, but looks only if no other thread is looking: returns
    // 0 at once while another one holds the lock. For scada_tag_notify,
    // which must not wait while a consumer maps and prefaults a region.
    std::size_t try_refresh(int dir_fd, std::int64_t now_mono_ns);

    // Looks at the region of `slot`, which was asked for with `want`, now,
    // whenever `refresh` would look next: for scada-tagd, which reads the
    // RETAIN tags at every liveness check and must find a region that has
    // just appeared within that time (contract, section 12: RETAIN_NOW
    // within 1 second). True if what `bytes` returns changed. Nothing for a
    // slot that was not asked for. Not real-time.
    bool look_now(int dir_fd, std::size_t slot);

private:
    struct Slot {
        std::string name;
        RegionIdentity identity;
        bool exists = false;  // the header lists this region
        // What follows is guarded by `mutex_`.
        bool wanted = false;
        std::int64_t next_look_mono_ns = 0;
        std::uint32_t mappings = 0;
        // Points into `mappings_`, or null while the region is missing.
        // Written under `mutex_`, read without it.
        std::atomic<const PeerMapping*> current{nullptr};
        // Set by `want` under `mutex_`, read without it; never cleared.
        std::atomic<bool> asked{false};
        // Written by `look` under `mutex_`, read without it.
        std::atomic<bool> at_limit{false};
    };

    [[nodiscard]] std::size_t checked(std::size_t slot) const;

    // The loop of `refresh`, with `mutex_` held.
    std::size_t refresh_locked(int dir_fd, std::int64_t now_mono_ns);

    // Looks at one slot. True if what `bytes` returns changed. A queue
    // slot that gets a new mapping also looks at its producer's `proc`
    // slot (`look_at_producer`).
    bool look(int dir_fd, Slot& slot);
    // Asks for the `proc` slot of the producer of the queue slot `slot`, if
    // nobody has, and looks at it now; nothing for another slot. Under
    // `mutex_`.
    void look_at_producer(int dir_fd, const Slot& slot);
    [[nodiscard]] static bool is_queue(const Slot& slot) noexcept;
    // Marks every kept mapping of the queue slot `slot` whose file changed
    // size (`PeerMapping::whole`). Under `mutex_`.
    void check_kept(const Slot& slot) noexcept;

    std::size_t process_count_ = 0;
    std::size_t owner_count_ = 0;
    std::vector<Slot> slots_;  // never resized after construction

    std::mutex mutex_;
    friend class PeerRegionsTestAccess;  // holds `mutex_` in a unit test
    std::vector<std::size_t> wanted_;    // guarded by mutex_
    // Every mapping ever made, kept until destruction. Guarded by mutex_.
    std::vector<std::unique_ptr<PeerMapping>> mappings_;
};

}  // namespace scada::tag

#endif  // SCADA_TAG_PEER_REGIONS_H
