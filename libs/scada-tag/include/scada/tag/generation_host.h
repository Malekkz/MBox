#ifndef SCADA_TAG_GENERATION_HOST_H
#define SCADA_TAG_GENERATION_HOST_H

// The side of scada-tagd in the tag database
// (docs/contracts/tag-database.md, sections 3, 10 and 11.1): it creates or
// reuses the generation, owns `header` and `proc.tagd`, checks the liveness
// of every process and keeps the generation directory clean.
//
// Only scada-tagd uses this header. It lives in libscada-tag because the
// binary layout is private to this library (decision D-25).

#include "scada/common/result.h"
#include "scada/tag/liveness.h"
#include "scada/tag/tag_set.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scada::tag {

// Defaults of the contract, section 3.
inline constexpr std::string_view DEFAULT_BASE_DIR = "/dev/shm/scada";

// The process name of scada-tagd in the tag set; its region is `proc.tagd`
// (section 10.1).
inline constexpr std::string_view TAGD_PROCESS_NAME = "tagd";

struct HostOptions {
    // The database directory. It must exist, created by the platform
    // (section 3.1).
    std::string base_dir{DEFAULT_BASE_DIR};
    // The user id scada-tagd runs as: the owner the base directory must
    // have, and the uid of the process `tagd` in the tag set.
    std::uint32_t tagd_uid = 0;
};

// A process whose `down` flag changed at a liveness check.
struct ProcessChange {
    std::string process;
    // Why it is down now; NONE if it is no longer down.
    DownReason reason = DownReason::NONE;
};

// An entry of the generation directory that the header does not list, or
// that belongs to the wrong user (section 3.1).
struct SweptEntry {
    // The name as found in the directory. It is outside input: any bytes
    // but '/' and NUL, up to 255 of them.
    std::string name;
    // False if it could not be removed. Such an entry is reported once, not
    // at every check.
    bool removed = false;
};

// Late heartbeats a process recorded itself (section 10.1): heartbeats
// that came strictly more than its timeout after the one before, so that
// readers saw it down, possibly between two liveness checks. For diagnosis
// only. The figures are input: the process writes them.
struct LateHeartbeats {
    std::string process;
    // Late heartbeats since the previous check, or, with `baseline`, the
    // whole count found at the first look at this region.
    std::uint32_t added = 0;
    std::uint32_t total = 0;             // since the region was created; wraps
    std::int64_t longest_gap_ns = 0;     // between any two heartbeats
    std::int64_t last_late_mono_ns = 0;  // CLOCK_MONOTONIC; end of the gap
    bool baseline = false;
};

// What one liveness check changed.
struct CheckReport {
    std::vector<ProcessChange> changes;
    std::vector<SweptEntry> swept;
    std::vector<LateHeartbeats> late;
    // Only at the first check of a new generation: the processes that have
    // no usable region yet (DownReason::REGION_MISSING), in process index
    // order. Their `down` flag starts at 1 and stays, so they are not in
    // `changes`; this is their one trace (issue #60). Empty at every later
    // check and in a reused generation.
    std::vector<std::string> not_attached;
};

// One running generation, as scada-tagd holds it. Move-only. Destroying it
// releases the mappings and the locks; the generation and its files stay,
// so that a restart of scada-tagd does not disturb the data plane.
class GenerationHost {
public:
    // Starts as section 11.1 describes:
    // 1. Checks the base directory (section 3.1) and takes an exclusive
    //    lock on it, so that two instances cannot run on one database.
    // 2. Reuses the generation `current` points to if its `header` is ready
    //    and RUNNING, has the layout version and config hash of `tag_set`,
    //    and equals byte for byte the header this library builds for
    //    `tag_set`.
    // 3. Otherwise removes every other generation directory in which no
    //    process holds the lock on its `proc` region, checks that the file
    //    system has room for all regions, creates the next generation with
    //    its `header`, in which every process but `tagd` is down, switches
    //    `current` to it, and sets the state of the generation `current`
    //    pointed to before to RETIRING. A reused generation keeps the
    //    `down` flags it has.
    // 4. Attaches `proc.tagd`, sets it RUNNING and writes the first
    //    heartbeat.
    //
    // `tag_set` must hold a process named `tagd` with the uid
    // `options.tagd_uid`.
    //
    // INVALID_ARGUMENT and the other codes of `validate` for a tag set that
    // is not valid or has no such process; NOT_FOUND or PERMISSION_DENIED
    // for the base directory; ALREADY_EXISTS if another instance runs;
    // RESOURCE_EXHAUSTED if the file system has no room; IO_ERROR
    // otherwise. On an error `detail` says what failed.
    [[nodiscard]] static Result<GenerationHost> start(
        const TagSet& tag_set, const HostOptions& options, std::string& detail);

    ~GenerationHost();
    GenerationHost(GenerationHost&&) noexcept;
    GenerationHost& operator=(GenerationHost&&) noexcept;
    GenerationHost(const GenerationHost&) = delete;
    GenerationHost& operator=(const GenerationHost&) = delete;

    // The number of the generation.
    [[nodiscard]] std::uint64_t generation() const noexcept;

    // True if `start` reused an existing generation.
    [[nodiscard]] bool reused() const noexcept;

    // What `start` found and did besides, one sentence each, for the log:
    // why a generation was not reused, which directories were removed or
    // could not be removed.
    [[nodiscard]] const std::vector<std::string>& notes() const noexcept;

    // How often to call `check`, in ms (GlobalHeader.heartbeat_check_ms).
    [[nodiscard]] std::uint32_t heartbeat_check_ms() const noexcept;

    // The heartbeat timeout of the process `tagd`, in ms. `heartbeat` must
    // be called at least three times in that time (section 10.1).
    [[nodiscard]] std::uint32_t heartbeat_timeout_ms() const noexcept;

    // Writes the heartbeat of `proc.tagd`, taken at `now_mono_ns`
    // (CLOCK_MONOTONIC). Call it from the main loop only: a hung main loop
    // must stop the heartbeat.
    void heartbeat(std::int64_t now_mono_ns) noexcept;

    // One liveness check at `now_mono_ns` (CLOCK_MONOTONIC), section 10.1:
    // sets `down` in the process table for every process that is down and
    // clears it for every process that is not, reads the late heartbeats
    // every process recorded, then removes every entry of the generation
    // directory that the header does not list or whose owner is wrong
    // (section 3.1). Returns what changed, and at the first check of a new
    // generation the processes that have not attached yet.
    //
    // A temporary file `.tmp.<region>.<pid>` of a listed region, owned by
    // the user of that region, is a region being created and stays.
    [[nodiscard]] CheckReport check(std::int64_t now_mono_ns);

    // The `down` flag of `process` as other processes see it, or nothing if
    // the tag set has no such process.
    [[nodiscard]] std::optional<bool> is_down(
        std::string_view process) const noexcept;

    // Marks scada-tagd as stopping in `proc.tagd` (a clean stop is seen at
    // once, section 10.1). The generation keeps running.
    void stop() noexcept;

private:
    class Impl;
    explicit GenerationHost(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace scada::tag

#endif  // SCADA_TAG_GENERATION_HOST_H
