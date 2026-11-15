#ifndef SCADA_TAG_PROC_REGION_H
#define SCADA_TAG_PROC_REGION_H

// The `proc.<process>` region (docs/contracts/tag-database.md, section 10):
// the side of the process that owns it, and the side of a process that
// watches it. Private to libscada-tag.

#include "layout.h"
#include "region_file.h"

#include "scada/common/fd.h"
#include "scada/common/mapped_region.h"
#include "scada/common/realtime.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/liveness.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace scada::tag {

// What identifies the `proc` region of one process in one generation.
struct ProcRegionParameters {
    std::string process;  // validated process name
    std::uint16_t process_index = 0;
    std::uint64_t generation = 0;
    std::size_t queue_count = 0;  // queues of the generation
    std::uint32_t uid = 0;        // user the process runs as
};

// The RegionIdentity of that region.
[[nodiscard]] RegionIdentity proc_region_identity(
    const ProcRegionParameters& parameters) noexcept;

// The `proc` region of the calling process, mapped read-write and locked
// (section 10: an exclusive lock for the whole life of the process). Move-only;
// destroying it releases the lock and the mapping, and leaves the file.
class OwnProcRegion {
public:
    // Holds no region.
    OwnProcRegion() noexcept = default;

    // Attaches to the region in the generation directory open as `dir_fd`
    // (section 11.2, step 3 and the lock of step 4):
    // - a region that exists with the right owner, size and header and is
    //   ready is reused;
    // - an entry of that name owned by `parameters.uid` that is not such a
    //   region is removed and the region is created;
    // - an absent region is created (section 3.2), after the caller's stale
    //   temporary files were removed.
    // The state is STARTING afterwards and the process id is the caller's.
    // The cursors of a new region are 0.
    //
    // ALREADY_EXISTS if another process holds the lock (the process name is
    // already attached); PERMISSION_DENIED if the entry belongs to another
    // user; RESOURCE_EXHAUSTED if the file system has no room; IO_ERROR
    // otherwise. On an error `detail` says what was found.
    [[nodiscard]] static Result<OwnProcRegion> attach(
        int dir_fd, const ProcRegionParameters& parameters,
        std::string& detail);

    // True if a region is held.
    [[nodiscard]] bool valid() const noexcept { return memory_.valid(); }

    // True if `attach` found the region and reused it.
    [[nodiscard]] bool reused() const noexcept { return reused_; }

    // Writes one heartbeat taken at `mono_ns` (CLOCK_MONOTONIC). Called
    // from one thread: the process's own main loop or cycle (section 10.1).
    // Precondition: `valid()`.
    SCADA_RT void heartbeat(std::int64_t mono_ns) noexcept;

    // Brings the region into memory (see MappedRegion::prefault).
    [[nodiscard]] Status prefault() const noexcept {
        return memory_.prefault();
    }

    // The mapped bytes of the region, for `proc_liveness`; empty if no
    // region is held. Valid as long as this object holds the region.
    SCADA_RT [[nodiscard]] std::span<std::byte> bytes() const noexcept {
        return memory_.bytes();
    }

    // Sets the state other processes see. Precondition: `valid()`.
    SCADA_RT void set_state(ProcessState state) noexcept;

    // Increments `cycle_seq` (section 10) and returns the word to wake.
    // Safe from several threads: no increment is lost. Precondition:
    // `valid()`.
    SCADA_RT const std::uint32_t* advance_cycle() noexcept;

private:
    OwnProcRegion(Fd file, MappedRegion memory, bool reused) noexcept;

    [[nodiscard]] ProcHeader& header() const noexcept;

    Fd file_;  // holds the lock
    MappedRegion memory_;
    bool reused_ = false;
};

// Evaluates the second and third rule of section 10.1 on `region`, the
// mapped `proc` region of a process that passed the checks of section 3.1,
// at the time `now_mono_ns` (CLOCK_MONOTONIC): NOT_RUNNING, HEARTBEAT_STALE
// or NONE. A heartbeat exactly as old as the timeout is not yet stale. One
// newer than `now_mono_ns` is fresh up to the timeout ahead of it and stale
// beyond (section 10.1). REGION_MISSING if `region` is too
// small to be a `proc` region, which includes an empty span.
SCADA_RT [[nodiscard]] DownReason proc_liveness(
    std::span<std::byte> region, std::uint32_t heartbeat_timeout_ms,
    std::int64_t now_mono_ns) noexcept;

// The fields of a `proc` region as a watcher reads them, each on its own.
struct ProcFields {
    std::uint64_t heartbeat = 0;
    std::int64_t heartbeat_mono_ns = 0;
    std::uint32_t pid = 0;
    std::uint32_t state = 0;  // as stored; may be no ProcessState
    std::uint32_t cycle_seq = 0;
};

// The fields of `region`, the mapped `proc` region of a process, or
// nothing if `region` is too small to be one (an empty span too). Each
// field is read with `load_shared`; together they need not belong to one
// heartbeat.
SCADA_RT [[nodiscard]] std::optional<ProcFields> read_proc_fields(
    std::span<std::byte> region) noexcept;

// The `cycle_seq` field of `region`, the mapped `proc` region of a process,
// or null if `region` is too small to be one (an empty span too). The
// pointer is valid as long as the mapping is; the field is read with
// `load_shared` only.
SCADA_RT [[nodiscard]] std::uint32_t* cycle_field(
    std::span<std::byte> region) noexcept;

// Watches the `proc` region of another process, mapped read-only. The
// mapping is kept between calls and replaced when the name refers to another
// file.
class ProcProbe {
public:
    // Evaluates the first three rules of section 10.1 for the process whose
    // region is described by `parameters`, in the generation directory open
    // as `dir_fd`, at the time `now_mono_ns` (CLOCK_MONOTONIC). A heartbeat
    // newer than `now_mono_ns` is fresh.
    //
    // A region that is absent or fails a check of section 3.1 is reported
    // as REGION_MISSING; it is looked for again at the next call.
    [[nodiscard]] DownReason observe(int dir_fd,
                                     const ProcRegionParameters& parameters,
                                     std::uint32_t heartbeat_timeout_ms,
                                     std::int64_t now_mono_ns);

private:
    std::optional<OpenedRegion> region_;
};

}  // namespace scada::tag

#endif  // SCADA_TAG_PROC_REGION_H
