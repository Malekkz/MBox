#ifndef SCADA_TAG_RETAIN_KEEPER_H
#define SCADA_TAG_RETAIN_KEEPER_H

// The side of scada-tagd in retained values and retired generations
// (docs/contracts/tag-database.md, sections 11.3 and 12; decision D-45). It
// reads the RETAIN tags of the current generation with the read of section
// 6.2, without the owner-down overlay, has the retain files written by a
// thread of its own, and finishes the generations that were retired: a
// FINAL retain file for every owner once it left, then removal. Used by
// GenerationHost from the main loop of scada-tagd; only the writer thread
// runs beside it. Private to libscada-tag.

#include "header_view.h"
#include "peer_regions.h"
#include "proc_region.h"
#include "retain_file.h"
#include "tag_record.h"

#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/generation_host.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace scada::tag {

// The shortest time between two attempts to write a retain file that could
// not be written, in ns.
inline constexpr std::int64_t RETAIN_RETRY_INTERVAL_NS = 1'000'000'000;

// The longest scada-tagd waits for the writer thread: at a stop, and at
// start before it removes a generation it finished. Below the 10 s of the
// service watchdog.
inline constexpr std::chrono::milliseconds RETAIN_WAIT_LIMIT{5000};

// The bound of contract 12 for a RETAIN_NOW change, from the change to its
// file on disk, in ns.
inline constexpr std::int64_t RETAIN_NOW_BOUND_NS = 1'000'000'000;
// W of decision D-62: the time one retain write, with the writes queued
// before it, may take for the bound to hold.
inline constexpr std::int64_t RETAIN_WRITE_BUDGET_NS = 200'000'000;

// K of decision D-62: the checks that must pass after a file of an owner
// was handed to the writer before the next one may be, when the checks come
// every `check_period_ns` (taken as 1 ns if not positive).
// K = max(1, floor((RETAIN_NOW_BOUND_NS - RETAIN_WRITE_BUDGET_NS) / P) - 1),
// so that K checks plus less than one period of lateness of a check, plus
// W, stay within the bound. Counting checks, not ns, keeps a check that
// wakes a little late from pushing a write a whole period later.
[[nodiscard]] constexpr std::int64_t checks_between_writes(
    std::int64_t check_period_ns) noexcept {
    const std::int64_t period = check_period_ns > 0 ? check_period_ns : 1;
    const std::int64_t checks =
        ((RETAIN_NOW_BOUND_NS - RETAIN_WRITE_BUDGET_NS) / period) - 1;
    return checks > 1 ? checks : 1;
}

// Writes retain files on a thread of its own, so that a slow fsync never
// holds up the liveness loop of scada-tagd (decision D-45). Of several
// files of one owner waiting, only the newest is written.
class RetainWriter {
public:
    struct Done {
        std::string owner;
        std::uint64_t token = 0;  // as submitted
        bool written = false;
        std::string detail;  // why not
        std::size_t values = 0;
        bool final = false;
        GenerationIdentity source;
        // A removal of retain files instead of a write: the files removed;
        // `written` false if the directory could not be synced.
        bool removal = false;
        std::vector<std::string> removed;
        // The written_time_ns of the file written.
        std::int64_t written_time_ns = 0;
        // From the submit of the file to the end of its write, by the clock
        // of RetainFileOps (decision D-62).
        std::int64_t took_ns = 0;
    };

    // `dir_fd` and `ops` are borrowed and stay valid while this object
    // lives.
    explicit RetainWriter(int dir_fd,
                          RetainFileOps& ops = system_retain_file_ops());
    // Stops the thread after the file it is writing, if any; files still
    // waiting are not written.
    ~RetainWriter();
    RetainWriter(const RetainWriter&) = delete;
    RetainWriter& operator=(const RetainWriter&) = delete;
    RetainWriter(RetainWriter&&) = delete;
    RetainWriter& operator=(RetainWriter&&) = delete;

    // Queues `file` for `owner`, replacing a file of `owner` still waiting.
    void submit(const std::string& owner, RetainFile file, std::uint64_t token);
    // Queues the removal of the files `names` of the retain directory, then
    // an fsync of the directory.
    void submit_removal(std::vector<std::string> names);
    // What was written or failed since the previous call.
    [[nodiscard]] std::vector<Done> take_done();
    // Results that could not be recorded (a failed allocation), since the
    // start.
    [[nodiscard]] std::uint64_t lost() const;
    // The owner whose file is being written now; empty if none.
    [[nodiscard]] std::string in_flight() const;
    // Waits, at most `limit`, until nothing waits and nothing is being
    // written. False if the limit was reached.
    [[nodiscard]] bool wait_idle(std::chrono::milliseconds limit);

private:
    struct Job {
        RetainFile file;
        std::uint64_t token = 0;
        std::int64_t submitted_ns = 0;  // by the clock of ops_
    };

    void run(const std::stop_token& stop);
    [[nodiscard]] Done remove_files(const std::vector<std::string>& names);

    int dir_fd_;
    RetainFileOps& ops_;
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::map<std::string, Job, std::less<>> waiting_;  // guarded by mutex_
    std::vector<Done> done_;                           // guarded by mutex_
    std::vector<std::string> removals_;                // guarded by mutex_
    std::uint64_t lost_ = 0;                           // guarded by mutex_
    bool busy_ = false;                                // guarded by mutex_
    std::string in_flight_;                            // guarded by mutex_
    // Last: started after everything it uses, stopped and joined first.
    std::jthread thread_;
};

class RetainKeeper {
public:
    // `retain_dir` empty: no file is read or written, and the owners of a
    // retired generation count as done at once. Otherwise the directory
    // must be one scada-tagd may use (owned by `tagd_uid`, mode 2750, as
    // the base directory, and the group of the base directory `base_fd`;
    // it is not created); its stale temporary files are
    // removed. `config_hash` is that of the tag set scada-tagd runs with; it
    // goes into every file written. The codes of `open_base_directory` on
    // an error.
    // `ops`, borrowed for the life of the keeper, are the file system calls
    // of its writer.
    [[nodiscard]] static Result<std::unique_ptr<RetainKeeper>> open(
        const std::string& retain_dir, std::uint32_t tagd_uid, int base_fd,
        std::uint32_t interval_ms, const ConfigHash& config_hash,
        std::string& detail, RetainFileOps& ops = system_retain_file_ops());

private:
    // Only `open` can name it, so only `open` can construct.
    struct Key {
        explicit Key() = default;
    };

public:
    // For `open` alone.
    RetainKeeper(Key key, Fd directory, std::uint32_t tagd_uid,
                 std::int64_t interval_ns, const ConfigHash& config_hash,
                 RetainFileOps& ops);

    RetainKeeper(const RetainKeeper&) = delete;
    RetainKeeper& operator=(const RetainKeeper&) = delete;
    RetainKeeper(RetainKeeper&&) = delete;
    RetainKeeper& operator=(RetainKeeper&&) = delete;
    ~RetainKeeper();

    // Whether a retain directory is in use.
    [[nodiscard]] bool persists() const noexcept { return directory_.valid(); }

    // Finishes the generation `generation` of the base directory open as
    // `base_fd` at once, for a generation in which no process is attached:
    // the RETAIN tags of every owner are read and written FINAL, and this
    // returns once the files are on disk. Called at start, before scada-tagd
    // removes such a generation, so that what processes wrote there after
    // the last retain write is not lost.
    void finish_now(int base_fd, std::uint64_t generation,
                    std::int64_t now_mono_ns, CheckReport& report);

    // Watches the current generation, open as `dir_fd`, which must stay
    // open while this object lives. The codes of HeaderView::open.
    [[nodiscard]] Status watch_current(int dir_fd, std::uint64_t generation,
                                       std::string& detail);

    // Watches the retired generation `generation` of the base directory
    // open as `base_fd`, retired at `now_mono_ns`; call `watch_current`
    // first. Its owners are finished and it is removed by `check`. Returns
    // the state its header has, or nothing if it has no usable header; such
    // a generation is removed once no process holds a lock in it.
    std::optional<GenerationState> watch_retired(int base_fd,
                                                 std::uint64_t generation,
                                                 std::int64_t now_mono_ns);

    // Loads the values on file for every owner watched (section 12), so
    // that a file is written only when a value differs from it, and marks
    // for writing an owner that has a value but no file, or whose file
    // holds a tag it has in no watched generation. Files that fail their
    // checks are reported at the next `check`.
    void load();

    // One round of section 12 and 11.3, step 3, as GenerationHost::check
    // describes it. `base_fd` is the base directory.
    void check(int base_fd, std::int64_t now_mono_ns, CheckReport& report);

    // Reads the current generation and writes every owner whose values
    // changed, now, and waits until the files are on disk.
    void flush(CheckReport& report);

    // The period of the checks, `period_ns` > 0 (a value not positive is
    // taken as 1 ns), which sets how many checks pass between two writes of
    // one owner's file (`checks_between_writes`, decision D-62). Until it
    // is called, the period is the `heartbeat_check_ms` of the tag set.
    void set_check_period(std::int64_t period_ns) noexcept {
        checks_between_writes_ = checks_between_writes(period_ns);
    }

    // Waits until the writer has finished every file handed to it, and
    // reports what it did; hands it nothing new.
    void wait_written(CheckReport& report);

    // The retired generations still watched, in ascending order.
    [[nodiscard]] std::vector<std::uint64_t> retired() const;

private:
    struct RetainedTag {
        std::string name;
        std::uint32_t slot = 0;
        TagType type = TagType::BOOL;
        bool now = false;  // RETAIN_NOW
    };
    struct WatchedOwner {
        std::size_t index = 0;
        std::string name;
        std::uint16_t process_index = 0;
        std::vector<RetainedTag> tags;
        // Retired generations: what it left there is on disk (or, after the
        // timeout, what could be read), or it has a region in the current
        // generation already.
        bool done = false;
        // Retired generations: the FINAL file was handed to the writer.
        bool final_submitted = false;
    };
    struct WatchedProcess {
        ProcRegionParameters region;
        std::uint32_t heartbeat_timeout_ms = 0;
        ProcProbe probe;
    };
    struct Watched {
        std::uint64_t generation = 0;
        GenerationIdentity identity;
        Fd directory;  // retired generations only; the current one is borrowed
        int dir_fd = -1;
        std::unique_ptr<HeaderView> header;
        std::unique_ptr<PeerRegions> peers;
        std::vector<WatchedOwner> owners;
        std::vector<WatchedProcess> processes;
        std::int64_t retired_mono_ns = 0;
        bool removal_reported = false;
    };
    struct OwnerState {
        std::map<std::string, RetainedValue, std::less<>> values;
        bool dirty = false;
        bool urgent = false;  // a RETAIN_NOW tag changed
        // When the last file was handed to the writer.
        std::optional<std::int64_t> last_write_mono_ns;
        // Checks since then, up to checks_between_writes_ (decision D-62).
        std::int64_t checks_since_write = 0;
        // Decision D-62: the earliest time the oldest RETAIN_NOW change can
        // have happened (the check before the one that saw it), while it is
        // not handed to the writer, and once it is, until it is on disk; and
        // whether a late write was reported and not yet followed by one
        // within the bound.
        std::optional<std::int64_t> urgent_since_mono_ns;
        std::optional<std::int64_t> submitted_since_mono_ns;
        bool late_reported = false;
        std::int64_t next_retry_mono_ns = 0;
        // The newest retired generation this owner was seen to leave, whose
        // values the file therefore holds; sticky (decision D-45).
        std::optional<GenerationIdentity> final_source;
        // The FINAL mark of the last file on disk.
        std::optional<GenerationIdentity> final_on_disk;
        bool has_file = false;         // a valid file of it is on disk
        std::uint64_t token = 0;       // of the last file submitted
        std::uint64_t done_token = 0;  // of the last file the writer finished
        bool failing = false;          // the last write failed and was reported
        // The values the owner has not written since they were loaded or
        // restored, with the stamp of the file they came from (decision
        // D-59); a write of the owner removes the entry.
        std::map<std::string, RetainStamp, std::less<>> not_written;
        // The written_time_ns of the owner's file on disk, as last read or
        // written; nothing if it has none known (review D4 of #93).
        std::optional<std::int64_t> written_time_on_disk;
    };

    // Fills `watched.owners` and `watched.processes` from its header.
    void describe(Watched& watched, bool retired,
                  std::int64_t now_mono_ns) const;
    // Loads the values on file for the owners of `watched`; with
    // `own_only` from each owner's own file only. Names of the owner's own
    // file go to `own_names`.
    void load_watched(
        const Watched& watched,
        std::map<std::string, std::vector<std::string>, std::less<>>& own_names,
        bool own_only);

    // Reads the RETAIN tags of `owner` in `watched` into the state of its
    // name. False if its `tags` region is missing.
    bool read_owner(const Watched& watched, const WatchedOwner& owner);
    // What `read_owner` learns from `copy`, a record of `tag` that holds a
    // value: a change of value, quality or flags makes the owner dirty; a
    // write of the owner, unlike the restore, ends the stamp it came with.
    // True for a restore of a value this keeper does not have as it is on
    // file, which `note_restores` takes.
    [[nodiscard]] static bool note_value(OwnerState& state,
                                         const RetainedTag& tag,
                                         const RecordCopy& copy);
    // Takes the restored records `copies` of `tags` of `owner` with the
    // value, quality, flags and stamp of the file they came from, marked
    // adopted when that file is another owner's (decision D-59, review B2,
    // B3); a source no longer found gives an unknown age and SOURCE_NOT_FINAL.
    void note_restores(const std::string& owner,
                       std::span<const RetainedTag> tags,
                       std::span<const RecordCopy> copies);
    void read_current();

    // True if the current generation has a ready `tags` region of the
    // owner `name` (decision D-45: then no file is written for it from an
    // older generation).
    [[nodiscard]] bool current_has_region(const std::string& name);

    // Hands the file of `name` to the writer.
    void submit(const std::string& name, std::int64_t now_mono_ns);
    // Applies what the writer did.
    void collect(std::int64_t now_mono_ns, CheckReport& report);
    // A file the writer wrote: the state of its owner, and the report.
    static void note_written(const RetainWriter::Done& done, OwnerState& state,
                             CheckReport& report);
    // Decision D-62: reports, once until a later one is within the bound, a
    // RETAIN_NOW change that took longer than RETAIN_NOW_BOUND_NS to reach
    // the disk; `took_ns` is the writer's part.
    static void note_time_to_disk(const std::string& owner, OwnerState& state,
                                  std::int64_t took_ns, CheckReport& report);

    // True if a generation newer than `generation` in the base directory
    // `base_fd` has a ready `tags` region of `owner`, or the owner's file
    // already marks a newer generation as left (decision D-45).
    [[nodiscard]] bool newer_region_exists(int base_fd,
                                           std::uint64_t generation,
                                           const std::string& owner,
                                           std::int64_t now_mono_ns) const;

    // The processes of `watched` that still hold the lock of their `proc`
    // region, as "<process> (pid <pid>)".
    [[nodiscard]] static std::vector<std::string> still_attached(
        Watched& watched, std::int64_t now_mono_ns);
    // Makes every count of `queues` unknown.
    static void forget_counts(std::vector<UnconsumedQueue>& queues);
    // The consumer's cursor and the `write_seq` of queue `queue_id` of
    // `watched`, or nothing if either cannot be read.
    [[nodiscard]] static std::optional<std::pair<std::uint64_t, std::uint64_t>>
    queue_pair(Watched& watched, std::size_t queue_id,
               std::int64_t now_mono_ns);
    // The queues of `watched` that still hold records (decision D-54).
    [[nodiscard]] static std::vector<UnconsumedQueue> unconsumed_records(
        Watched& watched, std::int64_t now_mono_ns);
    // True if `owner` has left the retired generation `watched`.
    [[nodiscard]] static bool has_left(Watched& watched,
                                       const WatchedOwner& owner,
                                       std::int64_t now_mono_ns);
    // A retired generation, one round.
    void finish(int base_fd, Watched& watched, std::int64_t now_mono_ns);

    // Waits for the writer, at most RETAIN_WAIT_LIMIT; reports reaching it.
    void wait_bounded(CheckReport& report);
    // Removes the retired generations that are done.
    void remove_done(int base_fd, std::int64_t now_mono_ns,
                     CheckReport& report);
    // Takes into the state of every owner of the current generation that
    // has no region there the values of its RETAIN tags that only other
    // owners' files hold (a tag that moved, decision D-59); those owners are
    // then written. False if not every file could be looked at (review B5).
    [[nodiscard]] bool adopt_moved_values();
    // Removes the files of owners that are in no generation any more, once
    // `adopt_moved_values` ran and every owner of the current generation
    // with values has its file on disk.
    void remove_files_of_gone_owners();

    Fd directory_;  // the retain directory; invalid if none
    std::uint32_t tagd_uid_ = 0;
    std::int64_t interval_ns_ = 0;
    std::int64_t checks_between_writes_ =
        checks_between_writes(RETAIN_NOW_BOUND_NS / 4);
    // The time of the previous check, nothing before the first.
    std::optional<std::int64_t> last_check_mono_ns_;
    ConfigHash config_hash_{};
    std::unique_ptr<Watched> current_;
    std::vector<std::unique_ptr<Watched>> retired_;
    std::map<std::string, OwnerState, std::less<>> states_;
    // Files reported as failing their checks, so that each is reported once.
    std::set<std::string, std::less<>> rejected_seen_;
    std::vector<RetainProblem> pending_problems_;
    bool gone_owners_removed_ = false;
    bool moved_values_adopted_ = false;
    bool adoption_reported_ = false;          // an incomplete look, logged once
    std::int64_t next_adoption_mono_ns_ = 0;  // the next look, if incomplete
    std::uint64_t writer_lost_ = 0;  // RetainWriter::lost at the last look
    // Last: destroyed first, while the directory is still open.
    std::unique_ptr<RetainWriter> writer_;
};

}  // namespace scada::tag

#endif  // SCADA_TAG_RETAIN_KEEPER_H
