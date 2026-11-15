#ifndef SCADA_TAG_ATTACHMENT_H
#define SCADA_TAG_ATTACHMENT_H

// One process attached to the tag database
// (docs/contracts/tag-database.md, sections 3, 6.3, 10 and 11.2). The C
// interface of scada/tag/scada_tag.h is a thin layer over this class.
// Private to libscada-tag.

#include "header_view.h"
#include "layout.h"
#include "own_region.h"
#include "peer_regions.h"
#include "proc_region.h"
#include "queue_region.h"
#include "retain_file.h"
#include "tag_record.h"

#include "scada/common/fd.h"
#include "scada/common/realtime.h"
#include "scada/common/result.h"
#include "scada/tag/liveness.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scada::tag {

// Defaults of the contract, sections 3 and 12.
inline constexpr std::string_view DEFAULT_RETAIN_DIR = "/var/lib/scada/retain";

// The user whose uid owns the base directory and the header (section 3.1).
inline constexpr std::string_view TAGD_USER_NAME = "scada-tagd";

struct AttachOptions {
    // The database directory; empty: /dev/shm/scada.
    std::string base_dir;
    // The directory of the retained values (section 12); empty:
    // /var/lib/scada/retain. A directory that cannot be opened restores
    // nothing.
    std::string retain_dir;
    // The uid of scada-tagd; nothing: the uid of the user `scada-tagd`.
    std::optional<std::uint32_t> tagd_uid;
};

// The process index of an observer, which is no process (section 11.4).
inline constexpr std::uint16_t NO_PROCESS_INDEX = 0xFFFF;

// The shortest time between two readings of `current` by an observer, in
// ns (section 11.4: at most once a second).
inline constexpr std::int64_t CURRENT_RECHECK_INTERVAL_NS = 1'000'000'000;

// The longest an attach waits for scada-tagd to write the FINAL retain files
// of the owners that held its retained tags in a RETIRING generation
// (decision D-45), and the pause between two looks, in ns. scada-tagd
// writes such a file at its next liveness check after the owner's process
// left, 250 ms by default.
inline constexpr std::int64_t RETAIN_FINAL_WAIT_NS = 2'000'000'000;
inline constexpr std::int64_t RETAIN_FINAL_POLL_NS = 20'000'000;

// Looks in a row at which an observer finds no readable `current` (or no
// base directory) before it counts the generation as gone (section 11.4).
inline constexpr std::uint32_t CURRENT_UNREADABLE_LOOKS = 2;

// What `Attachment::status` reports.
struct AttachmentStatus {
    std::uint64_t generation = 0;
    // `GlobalHeader.state` now; nothing for a value that is no state.
    std::optional<GenerationState> state;
    // For an observer: true once `current` was found to name another
    // generation than this one. Always false for a process.
    bool moved = false;
    std::uint16_t process_index = 0;  // NO_PROCESS_INDEX for an observer
    // Whether scada-tagd is down, by the rules of section 10.1 applied to
    // `proc.tagd`. REGION_MISSING also if the generation has no process
    // `tagd`.
    DownReason tagd = DownReason::REGION_MISSING;
    // What attach did with the regions this process writes.
    std::uint32_t regions_created = 0;
    std::uint32_t regions_reused = 0;
    // Tag records found with an odd sequence number and repaired
    // (section 6.3).
    std::uint32_t records_repaired = 0;
    // Retained values loaded into new `tags` regions (section 12), and the
    // waits for a FINAL retain file that ran out (decision D-45). Not in
    // scada_status, which keeps its size.
    std::uint32_t values_restored = 0;
    std::uint32_t final_waits_expired = 0;
    // Retain files that failed their checks and were ignored.
    std::uint32_t files_rejected = 0;
    // Owners whose retained values were not restored because the retain
    // directory could not be used (missing, or another owner, mode or
    // group).
    std::uint32_t directory_unusable = 0;
    // True if reading the clocks was found to be a system call on this
    // machine (section 13.1).
    bool clock_syscall = false;
};

// The tag regions of one owner of the attached process, mapped read-write.
struct OwnedOwner {
    std::size_t owner_index = 0;
    OwnRegion tags;
    OwnRegion strings;  // holds no mapping if the owner has no string slots
    OwnRegion changes;
};

// A reliable queue the attached process produces, mapped read-write.
struct OwnedQueue {
    std::size_t queue_id = 0;
    OwnRegion region;
};

// What the queue functions need to know about one queue of the generation.
// Filled once by `attach`, then only read.
struct QueueAccess {
    std::size_t id = 0;
    QueueKind kind = QueueKind::EVENTS;
    std::uint32_t capacity = 0;     // from the queue table
    std::uint32_t record_size = 0;  // from the queue table
    std::uint16_t producer = 0;     // process index
    std::uint16_t consumer = 0;     // process index
    std::uint32_t consumer_timeout_ms = 0;
    // Whether the attached process is the producer or the consumer.
    bool produced = false;
    bool consumed = false;
    // The region, mapped read-write, if `produced`. Refers to a mapping
    // `Attachment::queues()` holds, which lives as long as the attachment.
    std::span<std::byte> own_region;
    // The slots in the regions of other processes: the queue region and the
    // `proc` region of the consumer.
    std::size_t queue_slot = 0;
    std::size_t consumer_proc_slot = 0;
    // For a CMD queue: the RESULT queue its answers go to, the one of the
    // same name after the prefix whose producer and consumer are the
    // consumer and producer of this one. Nothing if there is none.
    std::optional<std::size_t> answer_queue;
};

// The counts of one queue in one context, as scada_queue_stats reports
// them. Each is changed by the one thread of its side of the queue, or by
// attach before any; relaxed atomics, because scada_queue_stats may be
// called from another thread.
struct QueueCounters {
    std::atomic<std::uint64_t> posted{0};
    std::atomic<std::uint64_t> delivered{0};
    std::atomic<std::uint64_t> expired{0};
    std::atomic<std::uint64_t> aborted{0};
    std::atomic<std::uint64_t> skipped{0};
    std::atomic<std::uint64_t> answers_dropped{0};
    std::atomic<std::uint64_t> invalid{0};
    std::atomic<std::uint64_t> out_of_range{0};
    std::atomic<std::uint64_t> replaced{0};
};

// Adds one to `counter`. Real-time safe: one atomic addition.
SCADA_RT inline void count_one(std::atomic<std::uint64_t>& counter,
                               std::uint64_t amount = 1) noexcept {
    // Relaxed: a count on its own.
    counter.fetch_add(amount, std::memory_order_relaxed);
}

class Attachment {
public:
    // Attaches the calling process as `process` (section 11.2):
    // 1. Checks the base directory (section 3.1), resolves `current` once,
    //    checks the generation directory and opens and validates `header`.
    // 2. Finds `process` in the process table; its uid must be the
    //    effective user id of the caller.
    // 3. Attaches `proc.<process>` and takes its lock. Then, for every
    //    owner that is a thread of the process, `tags.<owner>`,
    //    `strings.<owner>` if it has string slots and `changes.<owner>`,
    //    and every queue the process produces: a usable region is reused, a
    //    reused `tags` region is repaired and the `epoch` of a reused ring
    //    is incremented (section 6.3); an unusable region of the caller is
    //    removed; an absent one is created (section 3.2).
    // 4. Section 9.2: maps the regions of other processes its queues
    //    need; if `proc.<process>` was created, starts every cursor at the
    //    end of its queue; answers every request still in a `cmd` queue of
    //    the process ABORTED and passes over it.
    // 5. Brings the regions into memory, sets the state RUNNING and writes
    //    the first heartbeat.
    // 6. Starts again from step 1, a limited number of times, if the
    //    generation is RETIRING.
    //
    // The lock is taken before the other regions are touched, not after as
    // section 11.2 lists it, so that a second process of the same name
    // fails before it changes anything.
    //
    // Before step 3 the tag directory is read through every slot table
    // (`read_directory`), and after step 5 the clocks are checked (section
    // 13.1).
    //
    // A `tags` region that is created gets the retained values of its
    // owner's RETAIN tags before it is published (section 12,
    // `restore_retained`).
    //
    // INVALID_ARGUMENT for a name that is no process name; NOT_FOUND if the
    // process is not in the process table; PERMISSION_DENIED for a wrong
    // uid, or a directory, header or own region with the wrong owner or
    // mode; ALREADY_EXISTS if the process name is already attached;
    // UNSUPPORTED if magic or layout version differ; DATA_CORRUPT for a
    // header or `current` that is not valid; UNAVAILABLE if there is no base
    // directory, no generation or no header, or the generation is not
    // RUNNING; FAILED_PRECONDITION if every
    // generation found was RETIRING; RESOURCE_EXHAUSTED if the file system
    // has no room; IO_ERROR otherwise. On an error `detail` says what was
    // found.
    [[nodiscard]] static Result<std::unique_ptr<Attachment>> attach(
        std::string_view process, const AttachOptions& options,
        std::string& detail);

    // Opens an observer of the current generation (section 11.4): step 1
    // of `attach`, the reading of the tag directory and the clock check,
    // and nothing else. An observer has the process index
    // NO_PROCESS_INDEX, owns no owner and no region, takes no lock and
    // writes nothing to shared memory; every region of another process is
    // mapped read-only by `PeerRegions` when it is asked for. It keeps the
    // base directory open, to read `current` again in `status`. Starts
    // again from step 1, a limited number of times, if the generation is
    // RETIRING.
    //
    // PERMISSION_DENIED if the caller may not open the base directory or
    // the generation directory, or a directory or the header has the
    // wrong owner or mode; the other errors as for `attach`, except
    // NOT_FOUND and ALREADY_EXISTS, which an observer never gets.
    [[nodiscard]] static Result<std::unique_ptr<Attachment>> observe(
        const AttachOptions& options, std::string& detail);

    // True for an observer.
    [[nodiscard]] bool observer() const noexcept {
        return process_index_ == NO_PROCESS_INDEX;
    }

    // Detaches: the state becomes STOPPING, so that the process reads as
    // down at once (section 10.1), and mappings and the lock are released.
    // The region files stay.
    ~Attachment();
    Attachment(const Attachment&) = delete;
    Attachment& operator=(const Attachment&) = delete;
    Attachment(Attachment&&) = delete;
    Attachment& operator=(Attachment&&) = delete;

    // Writes the heartbeat of the process, taken now. SCADA_E_RETIRING if
    // the generation is RETIRING: the caller detaches and attaches again
    // (section 11.3). SCADA_E_INVALID for an observer, which has no
    // heartbeat, and writes nothing. Call it from the main loop or cycle of
    // the process.
    SCADA_RT [[nodiscard]] int heartbeat() noexcept;

    // The state of the attachment now. It also looks again for the regions
    // of other processes that were asked for and are missing
    // (`PeerRegions::refresh`), and checks the clocks again at most once a
    // minute (section 13.1). An observer also reads `current` again, at
    // most every CURRENT_RECHECK_INTERVAL_NS, and reports in `moved` that
    // it names another generation. Not real-time.
    [[nodiscard]] AttachmentStatus status();

    // The tag functions of section 13. Each returns a return code of
    // scada/tag/scada_tag.h, where the functions of the same names state
    // their contracts; what is said here is what differs.

    // The handle of the tag `name`. If another process owns the tag, the
    // `tags` and `strings` regions of its owner and the `proc` region of
    // that process are asked for (`PeerRegions::want`). On an error
    // `handle` is INVALID_HANDLE. Not real-time.
    [[nodiscard]] int resolve(std::string_view name, std::uint32_t& handle);

    // Name, type, flags and owner of the tag `handle`. Not real-time.
    [[nodiscard]] int info(std::uint32_t handle, scada_tag_info_t& out) const;

    // Name, tag count, ring capacity and process of the owner
    // `owner_index`. Not real-time.
    [[nodiscard]] int owner_info(std::uint16_t owner_index,
                                 scada_owner_info_t& out) const;

    // The index of the process `name`; asks for its `proc` region unless it
    // is the attached process. Not real-time.
    [[nodiscard]] int proc_resolve(std::string_view name, std::uint16_t& index);

    // The state of the process `index` now (section 10.1).
    SCADA_RT [[nodiscard]] int proc_status(
        std::uint16_t index, scada_proc_status_t& out) const noexcept;

    // Section 6.2 and the overlay of the owner state.
    SCADA_RT [[nodiscard]] int read(std::uint32_t handle,
                                    scada_value& out) noexcept;

    // `out` has the size of `handles`; `status` has that size or is empty.
    // Other sizes end the process.
    SCADA_RT [[nodiscard]] int read_many(std::span<const std::uint32_t> handles,
                                         std::span<scada_value> out,
                                         std::span<int> status) noexcept;

    // Section 6.1. A write that changes value, quality or flags appends a
    // change entry to the owner's ring (section 8.1).
    SCADA_RT [[nodiscard]] int write(std::uint32_t handle,
                                     const scada_value& value) noexcept;

    // `values` has the size of `handles`; another size ends the process.
    SCADA_RT [[nodiscard]] int write_many(
        std::span<const std::uint32_t> handles,
        std::span<const scada_value> values) noexcept;

    // Section 7. The text goes to the start of `buffer` and its length to
    // `length`; `meta` may be null. A `buffer` with a null address asks for
    // the length and `meta` alone, and gets them as a large enough buffer
    // would. Not real-time.
    [[nodiscard]] int read_string(std::uint32_t handle, std::span<char> buffer,
                                  std::size_t& length,
                                  scada_value* meta) noexcept;

    // Section 7. Not real-time.
    [[nodiscard]] int write_string(std::uint32_t handle, std::string_view text,
                                   std::uint8_t quality, std::uint16_t flags,
                                   std::int64_t source_time_ns) noexcept;

    // Section 8.4: wakes the consumers of the rings the calling thread
    // wrote since its previous call, advances `cycle_seq` and wakes its
    // waiters; then does what `status` does to missing regions and the
    // clock check. SCADA_OK, or SCADA_E_IO with errno set if a wake was
    // refused (every wake is still tried). An observer wakes nobody: it
    // gets SCADA_E_INVALID, and nothing is done. Not real-time.
    [[nodiscard]] int notify();

    // What the subscriptions and wait sets need (section 8). Each owner or
    // process index must be below the count of the header; another one
    // ends the process.

    // The index of the owner `name`, or nothing.
    [[nodiscard]] std::optional<std::size_t> find_owner(
        std::string_view name) const noexcept;

    // Asks for the `changes` region of `owner_index` and the `proc` region
    // of its process, unless they are this process's own. Not real-time.
    void want_changes(std::size_t owner_index);

    // Asks for the `proc` region of `process_index`, unless it is this
    // process's own, and records that it was asked for. Not real-time.
    void want_proc(std::uint16_t process_index);

    // True if the `proc` region of `process_index` was asked for with
    // `want_proc`, or is this process's own: then `proc_of` tells whether
    // it is there. False for a process index that is none.
    SCADA_RT [[nodiscard]] bool proc_asked(
        std::uint16_t process_index) const noexcept;

    // The mapped `changes` region of `owner_index`, or an empty span while
    // it is missing. For an owner of another process the span may change
    // when the region is mapped anew; each span stays valid as long as this
    // object. Safe from any thread.
    [[nodiscard]] std::span<std::byte> changes_of(
        std::size_t owner_index) const noexcept;

    // The mapped `proc` region of `process_index`, or an empty span, with
    // the guarantees of `changes_of`.
    SCADA_RT [[nodiscard]] std::span<std::byte> proc_of(
        std::uint16_t process_index) const noexcept;

    // True if the process of the owner `owner_index` is down now, by the
    // three rules of section 10.1 (as the overlay of a read judges it).
    [[nodiscard]] bool owner_process_down(
        std::size_t owner_index) const noexcept;

    // The capacity of the ring of `owner_index`, from the owner table.
    [[nodiscard]] std::uint32_t ring_capacity(
        std::size_t owner_index) const noexcept;

    // True if `entry` is what the ring of `owner_index` may hold: a handle
    // of that owner, the type of its tag, a quality and flags an owner may
    // store, 0 or 1 for a BOOL and the slot number for a STRING.
    [[nodiscard]] bool change_valid(std::size_t owner_index,
                                    const ChangeEntry& entry) const noexcept;

    // Looks again for missing regions of other processes that were asked
    // for (`PeerRegions::refresh`, at most once a second for each). Not
    // real-time.
    void refresh_peers();

    // What the reliable queues need (section 9). Each queue id must be
    // below the count of the header; another one ends the process.

    // The id of the queue `name`, or nothing.
    [[nodiscard]] std::optional<std::size_t> find_queue(
        std::string_view name) const noexcept;

    // What is known of the queue `queue_id`.
    [[nodiscard]] const QueueAccess& queue(std::size_t queue_id) const noexcept;

    // Marks the producing (`producer`) or consuming side of `queue_id` as
    // open in this context. False if it is open already: one handle for each
    // side (section 13.2: one thread). Safe from any thread; not real-time.
    [[nodiscard]] bool claim_queue(std::size_t queue_id, bool producer);
    // Undoes `claim_queue`. If then no side that posts to the queue the
    // closed side posts to is open (its producer side, and for an answer
    // queue the side that consumes its `cmd` queue), forgets the thread
    // recorded for it, in one step with the test. Safe from any thread;
    // not real-time.
    void close_queue(std::size_t queue_id, bool producer);

    // Asks for the regions of other processes that the open side of
    // `queue_id` needs: the consumer's `proc` region for the producer, the
    // queue region for the consumer. Not real-time.
    void want_queue(std::size_t queue_id);

    // The mapped region of `queue_id`: the own mapping if this process
    // produces it, otherwise the read-only one, or an empty span while it
    // is missing, with the guarantees of `changes_of`.
    [[nodiscard]] std::span<std::byte> queue_bytes(
        std::size_t queue_id) const noexcept;

    // The cursor of `queue_id` in this process's own `proc` region.
    SCADA_RT [[nodiscard]] std::uint64_t& own_cursor(
        std::size_t queue_id) const noexcept;

    // Section 9.1: posts `record` to `queue_id`, which this process
    // produces, after the checks of `queue_region.h`; sets its id from
    // `write_seq` (EventRecord.seq, WriteRequest.request_id) and, for a
    // request, `posted_mono_ns` to now. A return code as scada_queue_post
    // states. Real-time safe. One thread posts to a queue.
    SCADA_RT [[nodiscard]] int post_record(std::size_t queue_id,
                                           RecordWords& record) noexcept;

    // Records the calling thread as the one that posts to `queue_id` if no
    // thread is recorded, so that `notify` wakes the consumer. True if the
    // calling thread is the recorded one. Otherwise a build without NDEBUG
    // stops the process (section 13.2) and another build returns false: two
    // threads that post to one queue can lose a record or tear one, so the
    // rule holds in every build (decision D-40).
    SCADA_RT [[nodiscard]] bool check_queue_thread(
        std::size_t queue_id) noexcept;

    // As `check_queue_thread`, but never stops the process: for the answers
    // `scada_queue_next` posts itself, which a thread that is not the
    // recorded one drops and counts in every build (decision D-40).
    SCADA_RT [[nodiscard]] bool bind_queue_thread(
        std::size_t queue_id) noexcept;

    // True if a thread is recorded as the one that posts to `queue_id`.
    [[nodiscard]] bool queue_thread_recorded(
        std::size_t queue_id) const noexcept;

    // The counts of `queue_id` in this context.
    [[nodiscard]] QueueCounters& counters(std::size_t queue_id) noexcept;

    // For a queue this process consumes and does not produce: true if the
    // queue is dead because its region was replaced inside the generation
    // (decision D-40). `bytes` is what `queue_bytes` gives now. The first
    // mapping this context sees is the queue's; another one later, or none,
    // means the file was removed, shrunk or created anew, and the records
    // and ids of the two files cannot be told apart. The first time, the
    // own cursor is set out of range, so that the producer gets
    // SCADA_E_PEER_DOWN by the rule of section 9.1, also after a restart of
    // either process, and `replaced` is counted. Sticky until detach.
    [[nodiscard]] bool queue_dead(std::size_t queue_id,
                                  std::span<std::byte> bytes) noexcept;

    // True if `queue_dead` found the queue dead. Changes nothing.
    [[nodiscard]] bool queue_is_dead(std::size_t queue_id) const noexcept;

    // Answers the request `request_id` of the CMD queue `queue_id`, which
    // this process consumes, with `status` in its answer queue. A return
    // code of `post_record`; SCADA_E_NOT_FOUND if the queue has no answer
    // queue. The answer is dropped on any error (section 9.2) and counted
    // in `answers_dropped` of `queue_id`. The caller has made sure that the
    // calling thread may post to the answer queue.
    SCADA_RT [[nodiscard]] int answer(std::size_t queue_id,
                                      std::uint64_t request_id,
                                      WriteStatus status) noexcept;

    // The number of processes of the generation.
    [[nodiscard]] std::size_t process_count() const noexcept {
        return header_.contents().processes.size();
    }

    [[nodiscard]] const HeaderView& header() const noexcept { return header_; }

    // The regions this process writes.
    [[nodiscard]] const std::vector<OwnedOwner>& owners() const noexcept {
        return owners_;
    }
    [[nodiscard]] const std::vector<OwnedQueue>& queues() const noexcept {
        return queues_;
    }

    // The regions of other processes, and the generation directory to pass
    // to `PeerRegions::want`. The descriptor is borrowed.
    [[nodiscard]] PeerRegions& peers() noexcept { return peers_; }
    [[nodiscard]] int directory() const noexcept { return directory_.get(); }

private:
    // Only `attach` can name it, so only `attach` can construct.
    struct Key {
        explicit Key() = default;
    };

public:
    // For `attach` alone.
    Attachment(Key key, Fd directory, HeaderView header,
               std::uint16_t process_index);

private:
    // True if that side of `queue_id` is open in this context. The caller
    // holds `queue_sides_mutex_`.
    [[nodiscard]] bool queue_side_open(std::size_t queue_id,
                                       bool producer) const noexcept;
    // True if the side that consumes a `cmd` queue whose answer queue is
    // `queue_id` is open in this context: its thread posts the answers. The
    // caller holds `queue_sides_mutex_`.
    [[nodiscard]] bool answer_queue_in_use(std::size_t queue_id) const noexcept;

    // What the tag functions need to know about one owner of the
    // generation. Filled once by `attach`, then only read.
    struct OwnerAccess {
        std::size_t index = 0;  // its owner index
        std::uint32_t tag_count = 0;
        std::uint32_t string_slots = 0;
        std::uint32_t heartbeat_timeout_ms = 0;  // of its process
        std::uint16_t process_index = 0;
        // True if the owner is a thread of the attached process.
        bool own = false;
        // Where the types of its slots start in `types_`, and where its
        // STRING slots start in `string_tags_`.
        std::size_t first_type = 0;
        std::size_t first_string = 0;
        // Its regions, mapped read-write, if `own`. The spans refer to the
        // mappings `owners_` holds, which live as long as this object.
        std::span<std::byte> own_tags;
        std::span<std::byte> own_strings;
        std::span<std::byte> own_changes;
        std::uint32_t ring_capacity = 0;  // from the owner table
        // The slots of its regions in `peers_`, if not `own`.
        // `strings_slot` means something only if it has string slots.
        std::size_t tags_slot = 0;
        std::size_t strings_slot = 0;
        std::size_t changes_slot = 0;
        std::size_t proc_slot = 0;
    };

    // A tag a handle refers to.
    struct TagRef {
        const OwnerAccess* owner = nullptr;  // into `access_`
        std::uint32_t slot = 0;
        TagType type = TagType::BOOL;
    };

    // Reads the tag directory through the slot table of every owner and
    // fills `access_`, `types_` and `string_tags_`. DATA_CORRUPT if a slot
    // has no valid entry.
    [[nodiscard]] Status read_directory(std::string& detail);

    // The types of the slots of owner `owner_index`, in slot order.
    [[nodiscard]] std::span<const TagType> types_of(
        std::size_t owner_index) const noexcept;

    // The tag `handle` refers to, or nothing if it is no handle of this
    // generation: two comparisons (section 13.1).
    SCADA_RT [[nodiscard]] std::optional<TagRef> tag_of(
        std::uint32_t handle) const noexcept;

    // The number of the string slot of the STRING tag `tag`: its rank among
    // the STRING tags of its owner.
    SCADA_RT [[nodiscard]] std::uint64_t string_slot_of(
        const TagRef& tag) const noexcept;

    // True if the process of `owner` is down at the time `now_mono_ns` by
    // the three rules of section 10.1 that the library evaluates itself:
    // its `proc` region is missing, its state is not RUNNING, or its
    // heartbeat is older than its timeout.
    SCADA_RT [[nodiscard]] bool owner_down(
        const OwnerAccess& owner, std::int64_t now_mono_ns) const noexcept;

    // True if a tag of `owner` may be read in this context: the owner is
    // one of this process, or a tag of it was resolved here (section 6.2).
    SCADA_RT [[nodiscard]] bool resolved(
        const OwnerAccess& owner) const noexcept;

    // The mapped `tags` and `strings` regions of `owner`; empty while a
    // region of another process is missing.
    SCADA_RT [[nodiscard]] std::span<std::byte> tags_of(
        const OwnerAccess& owner) const noexcept;
    SCADA_RT [[nodiscard]] std::span<std::byte> strings_of(
        const OwnerAccess& owner) const noexcept;

    SCADA_RT [[nodiscard]] int read_at(std::uint32_t handle, scada_value& out,
                                       std::int64_t now_mono_ns) noexcept;
    // The checks of `write` that need no clock.
    SCADA_RT [[nodiscard]] int check_write(std::uint32_t handle,
                                           const scada_value& value,
                                           TagRef& tag) const noexcept;

    // Records the calling thread as the writer of `owner` if it writes
    // first, so that `notify` knows which rings the thread wrote. Section
    // 13.2, in a build without NDEBUG: stops the process if the tags of
    // `owner` are written from a thread other than that first one.
    SCADA_RT void check_writer_thread(const OwnerAccess& owner) noexcept;

    // Appends the change of the tag `handle` that `update` wrote (section
    // 8.1). `tag` is the tag `handle` refers to, of an own owner.
    SCADA_RT static void append_change_of(std::uint32_t handle,
                                          const TagRef& tag,
                                          const RecordUpdate& update) noexcept;

    // Reads the clock source and times the clocks (section 13.1).
    void check_clock(std::int64_t now_mono_ns) noexcept;

    // Steps 3 and 4 of `attach`.
    [[nodiscard]] Status attach_regions(std::string& detail);
    [[nodiscard]] Status attach_owner(std::size_t owner_index,
                                      std::string& detail);
    [[nodiscard]] Status attach_queue(std::size_t queue_id,
                                      std::string& detail);
    void count(const OwnRegion& region) noexcept;

    // Section 12 and decision D-45: loads the retained values of the RETAIN
    // tags of `owner_index` into `tags`, a new `tags` region whose records
    // are in their initial state and which nobody else sees yet. First
    // waits, at most RETAIN_FINAL_WAIT_NS, until every owner that held one
    // of these tags in a RETIRING generation still in the base directory
    // has its FINAL file of the newest such generation, or that
    // generation's directory is gone. A value restored has
    // quality UNCERTAIN, flag RESTORED, its stored source time, and the
    // time of the restore as update time. Nothing is restored when the
    // retain directory or the file cannot be read.
    void restore_retained(std::span<std::byte> tags, std::size_t owner_index);
    // A FINAL retain file an attach waits for: that of `owner` for the
    // RETIRING generation `generation`, whose directory is `directory`.
    struct FinalWanted {
        std::string owner;
        GenerationIdentity generation;
        std::string directory;
    };
    // For each owner that holds one of the tags `names` in a RETIRING
    // generation older than this one, the newest such generation.
    [[nodiscard]] std::vector<FinalWanted> finals_wanted(
        std::span<const WantedValue> names) const;

    // Fills `queue_access_` once the own queues are attached, and asks for
    // the regions of other processes the queues of this process need.
    void prepare_queues();
    // Section 9.2: the cursors of a new `proc` region start at the end of
    // their queues; every request still in a `cmd` queue of the process is
    // answered ABORTED and passed over.
    void start_cursors();
    void drain_requests();
    // The drain of one `cmd` queue the process consumes. True if an answer
    // was posted.
    [[nodiscard]] bool drain_queue(const QueueAccess& access) noexcept;
    // For an observer: true if `current` names another generation now,
    // looked at no more than every CURRENT_RECHECK_INTERVAL_NS.
    [[nodiscard]] bool current_moved(std::int64_t now_mono_ns);
    // One look: true if the base directory under its path is another one,
    // `current` names another generation, or neither could be read for
    // CURRENT_UNREADABLE_LOOKS looks in a row.
    [[nodiscard]] bool current_names_another_generation();

    Fd directory_;  // the generation directory
    HeaderView header_;
    // The base directory as it was opened. An observer looks at `current`
    // again through it and `base_path_`; a process looks for RETIRING
    // generations when it restores retained values.
    Fd base_;
    std::string base_path_;
    std::uint16_t process_index_ = 0;
    std::optional<std::uint16_t> tagd_index_;
    OwnProcRegion proc_;
    std::vector<OwnedOwner> owners_;
    std::vector<OwnedQueue> queues_;
    PeerRegions peers_;
    std::uint32_t regions_created_ = 0;
    std::uint32_t regions_reused_ = 0;
    std::uint32_t records_repaired_ = 0;
    std::uint32_t tagd_uid_ = 0;
    std::string retain_dir_;  // for a process: where retained values are
    std::uint32_t values_restored_ = 0;
    std::uint32_t final_waits_expired_ = 0;
    std::uint32_t files_rejected_ = 0;
    std::uint32_t directory_unusable_ = 0;
    // When the waits for FINAL files of this attach end (one deadline for
    // all owners); set by the first wait.
    std::optional<std::int64_t> final_deadline_;

    // One entry for every owner of the generation, the type of every slot
    // of every owner, and the STRING slots of every owner in ascending
    // order. None of them changes after `attach`.
    std::vector<OwnerAccess> access_;
    std::vector<TagType> types_;
    std::vector<std::uint32_t> string_tags_;

    // For every owner, what tells the thread that wrote its tags first, or
    // null (`check_writer_thread`). Sized at `attach`, never resized.
    std::vector<std::atomic<const void*>> writer_threads_;

    // For every owner, the `write_seq` of its ring at the last wake by
    // `notify`. Atomic because `notify` may run in several threads;
    // relaxed, because only the exchange itself matters: a thread that
    // loses it leaves the wake to the one that wins. Sized at `attach`.
    std::vector<std::atomic<std::uint64_t>> woken_seq_;

    // For every queue of the generation: what `queue` returns, the thread
    // that posts to it (as `writer_threads_`), its `write_seq` at the last
    // wake (as `woken_seq_`), and which sides are open (`claim_queue`: bit
    // 0 producer, bit 1 consumer). Sized at `attach`, never resized.
    std::vector<QueueAccess> queue_access_;
    std::vector<std::atomic<const void*>> queue_threads_;
    std::vector<std::atomic<std::uint64_t>> queue_woken_seq_;
    // Which sides are open, and the binding rule of the queue threads
    // (decision D-40): a thread binds itself (`bind_queue_thread`) only
    // through a side that posts to the queue and is open, from its open to
    // its close; `close_queue` clears a binding only when no such side is
    // open, and tests and clears under `queue_sides_mutex_`, which
    // `claim_queue` also takes. Neither is held by real-time code.
    std::mutex queue_sides_mutex_;
    std::vector<std::uint8_t> queue_open_;  // guarded by queue_sides_mutex_
    std::vector<QueueCounters> queue_counters_;
    // The first mapping seen of each queue, and whether it died (see
    // `queue_dead`).
    std::vector<std::atomic<const std::byte*>> queue_mappings_;
    std::vector<std::atomic<bool>> queue_dead_;
    // For every process, whether its `proc` region was asked for by
    // `want_proc` (scada_proc_resolve, a wait set, scada-tagd at attach),
    // so that scada_proc_status can tell a process that is down from one
    // that was never resolved. Sized at construction, never resized; set
    // once, never cleared.
    std::vector<std::atomic<bool>> proc_asked_;

    // The result of the last clock check and when the next one is due.
    // Atomic because `status` may run in several threads; relaxed, because
    // each is a value on its own.
    std::atomic<std::int64_t> next_clock_check_mono_ns_{0};

    // For an observer: when `current` is read next, how many looks in a
    // row found it unreadable, and whether it was found to name another
    // generation, which does not change back. Atomic and relaxed for the
    // reasons of the clock check above. Ordered by size, not by topic.
    std::atomic<std::int64_t> next_current_check_mono_ns_{0};
    std::atomic<std::uint32_t> unreadable_looks_{0};
    std::atomic<bool> clock_syscall_{false};  // the last clock check, above
    std::atomic<bool> moved_{false};
};

// Initial state of the `tags` region `tags` of an owner whose slots have
// the types `types` (section 6): `seq` 0, quality BAD, flag NOT_INIT, times
// 0. The value of a STRING record is its string slot: its rank among the
// owner's STRING tags in slot order. `tags` is the whole region, large
// enough for `types.size()` records.
void initialise_tag_records(std::span<std::byte> tags,
                            std::span<const TagType> types) noexcept;

// Repairs the `tags` region `tags` of an owner after a restart (section
// 6.3): every record with an odd `seq` gets quality BAD and the flag
// NOT_INIT, the string slot of such a STRING record in `strings` gets
// length 0, and then `seq + 1` is stored. Returns the number of records
// repaired. Only the owner calls it, before it sets itself running.
// `strings` may be empty if `types` holds no STRING.
std::uint32_t repair_tag_records(std::span<std::byte> tags,
                                 std::span<std::byte> strings,
                                 std::span<const TagType> types) noexcept;

}  // namespace scada::tag

#endif  // SCADA_TAG_ATTACHMENT_H
