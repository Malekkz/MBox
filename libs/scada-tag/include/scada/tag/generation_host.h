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

// The default retain interval and directory (section 12).
inline constexpr std::uint32_t DEFAULT_RETAIN_INTERVAL_MS = 5000;
inline constexpr std::string_view DEFAULT_RETAIN_DIR_PATH =
    "/var/lib/scada/retain";

struct HostOptions {
    // The database directory. It must exist, created by the platform
    // (section 3.1).
    std::string base_dir{DEFAULT_BASE_DIR};
    // The user id scada-tagd runs as: the owner the base directory must
    // have, and the uid of the process `tagd` in the tag set.
    std::uint32_t tagd_uid = 0;
    // The directory of the retained values (section 12). It must exist,
    // owned by `tagd_uid` with mode 2750. Empty: no value is persisted;
    // retired generations are still finished and removed.
    std::string retain_dir;
    // Values that changed are written at most this often, in ms (section
    // 12); a RETAIN_NOW tag is written at the next check after its change.
    std::uint32_t retain_interval_ms = DEFAULT_RETAIN_INTERVAL_MS;
};

// How long a retired generation may keep scada-tagd waiting for its
// processes to leave before it is removed anyway, in ms (section 11.3).
inline constexpr std::uint32_t RETIRED_GENERATION_TIMEOUT_MS = 30'000;

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

// A retain file written (section 12).
struct RetainWrite {
    std::string owner;
    std::size_t values = 0;
    // True if the file marks that the owner left a retired generation
    // (decision D-45).
    bool final = false;
};

// A retain file that was ignored because it failed its checks, or one that
// could not be written. Reported once until the problem goes away.
struct RetainProblem {
    // The file name or the owner. Outside input for a file name: any
    // bytes but '/' and NUL.
    std::string subject;
    std::string detail;
};

// A retired generation that scada-tagd finished with (section 11.3, step
// 3).
struct RetiredGeneration {
    std::uint64_t generation = 0;
    // False if something of it stays; tried again at the next check, and
    // reported again only once it is gone.
    bool removed = false;
    // True if it was removed because RETIRED_GENERATION_TIMEOUT_MS passed,
    // not because every process had left.
    bool timed_out = false;
    // On a timeout: the processes that still held the lock of their `proc`
    // region, as "<process> (pid <pid>)". Their values were written, but
    // not marked FINAL.
    std::vector<std::string> still_attached;
    // True if it could not be removed by twice the timeout: it is no
    // longer tried and stays until the next boot (section 3.3).
    bool abandoned = false;
};

// Lives a process started in its region since the previous sample, from
// the `starts` count of its `proc` region (section 10.1). A process that
// restarts into its reused region between two checks is otherwise not seen
// by them. For diagnosis only. The figures are input: the process writes
// them, and a life of a library before contract 0.14 counts nothing, so
// they are a lower bound.
struct Restarts {
    std::string process;
    // Lives started since the previous sample, at least 1.
    std::uint32_t added = 0;
    // Of those, the lives no check saw as down and up again: `added` minus
    // the checks since the previous sample that found the process up after
    // it was down, at least 0. A lower bound: a heartbeat that came back
    // without a restart is such a check too.
    std::uint32_t unseen = 0;
    std::uint32_t total = 0;  // since the region was created; wraps
};

// What changed about a reliable queue at a liveness check (issue #53).
enum class QueueEvent : std::uint8_t {
    // Its consumer's cursor and its `write_seq` became a pair out of range
    // (section 9.2). Such a queue is stopped: its consumer gets
    // SCADA_E_LAYOUT and its producer SCADA_E_PEER_DOWN.
    BROKEN,
    // A broken queue's pair is in range again.
    NO_LONGER_BROKEN,
    // An events or command queue has had no usable file at two checks in a
    // row while its producer was not down. A producer creates its queue
    // files when it attaches, so the file was removed or damaged.
    MISSING,
    // A queue reported MISSING has a usable file again.
    FOUND,
    // The queue's file is another one than at the previous check: its
    // producer created it anew inside the generation (issue #53).
    // Records that were in the old file and that its consumer had not
    // taken are lost unless the consumer still holds the old file.
    RECREATED,
};

// A reliable queue whose state changed at a liveness check. The values are
// input: the producer and the consumer write them.
struct QueueChange {
    std::string queue;
    QueueEvent event = QueueEvent::BROKEN;
    // BROKEN and NO_LONGER_BROKEN: the values that were judged.
    std::uint64_t cursor = 0;
    std::uint64_t write_seq = 0;
    std::uint32_t capacity = 0;
    // BROKEN: the cursor is the all-ones value a consumer stores when it
    // found the queue file replaced inside the generation (decision D-40).
    bool replaced = false;
    // RECREATED: the new file's base, if its creator set one; the
    // `write_seq` scada-tagd saw in the old file at its last check of it
    // (`old_write_seq`), and the consumer's cursor at that check, if it was
    // readable then (`cursor_known`, `cursor`); `write_seq` is the new
    // file's.
    std::optional<std::uint64_t> base_seq = std::nullopt;
    std::uint64_t old_write_seq = 0;
    bool cursor_known = false;
};

// The ids of the old file that a RECREATED change may have lost: from the
// consumer's cursor at the last check of the old file up to the new base,
// which is above every id the old file gave (decision D-49). `bounded` is
// false without a base or with a base below the old `write_seq` (the queue
// then stops): nothing bounds the posts after that check, and `until` is
// the largest of base, old `write_seq` and `from`. Never inverted.
struct LostIds {
    std::uint64_t from = 0;
    std::uint64_t until = 0;
    bool bounded = false;
};
[[nodiscard]] LostIds lost_ids(const QueueChange& change) noexcept;

// What one liveness check changed.
struct CheckReport {
    std::vector<ProcessChange> changes;
    std::vector<SweptEntry> swept;
    std::vector<LateHeartbeats> late;
    std::vector<RetainWrite> retain_written;
    std::vector<RetainProblem> retain_problems;
    std::vector<RetiredGeneration> retired;
    // Retain files of owners that are in no generation any more, removed
    // once every owner of the current generation has a file and no
    // generation is retired (decision D-45). Names found in the
    // directory: outside input.
    std::vector<std::string> retain_files_removed;
    std::vector<Restarts> restarts;
    std::vector<QueueChange> queues;
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
    // every process recorded and the lives every other process started
    // since the previous check, applies the range rule of section 9.2 to
    // every queue whose region and consumer's `proc` region are both there,
    // then removes every entry of the generation directory that the header
    // does not list or whose owner is wrong (section 3.1). Returns what
    // changed. A queue is reported when it becomes broken, when it is no
    // longer broken, when an events or command queue file has been missing
    // at two checks in a row while its producer was not down, and when it
    // is there again; not at every check.
    //
    // A temporary file `.tmp.<region>.<pid>` of a listed region, owned by
    // the user of that region, is a region being created and stays.
    //
    // Then, with a retain directory, reads the RETAIN tags of the
    // generation and writes the retain file of every owner whose values
    // changed: at the first check after a change of a RETAIN_NOW tag,
    // otherwise once `retain_interval_ms` passed since its last write
    // (section 12). A record counts as a value if its quality is GOOD or
    // UNCERTAIN and it is not NOT_INIT; otherwise the value on file stays.
    //
    // Then finishes the retired generations (section 11.3, step 3;
    // decision D-45): once the process of an owner has left one (down by
    // section 10.1, or no longer holding the lock of its `proc` region),
    // reads that owner's RETAIN tags there and writes its retain file
    // marked FINAL; once every owner is done and no `proc` region is
    // locked, removes the generation. After RETIRED_GENERATION_TIMEOUT_MS
    // it does both whatever the processes do.
    [[nodiscard]] CheckReport check(std::int64_t now_mono_ns);

    // Writes the retain file of every owner whose values changed since its
    // last write, now (section 12: on clean shutdown).
    [[nodiscard]] CheckReport flush_retained();

    // The retain files are written by a thread of scada-tagd's own, so that
    // a slow fsync does not hold up the liveness checks (decision D-45); a
    // check reports the files that thread finished since the check before.
    // This waits until it has finished every file it was given and reports
    // them, and gives it nothing new.
    [[nodiscard]] CheckReport wait_retained_written();

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
