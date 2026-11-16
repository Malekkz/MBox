// The parts of Attachment that serve the reliable queues
// (docs/contracts/tag-database.md, section 9): the queue table, the post of
// section 9.1, and what attach does with the cursors and the `cmd` queues
// (sections 9.2 and 11.2, step 5).

#include "attachment.h"
#include "futex.h"
#include "header_view.h"
#include "layout.h"
#include "peer_regions.h"
#include "proc_region.h"
#include "queue_region.h"
#include "region_file.h"
#include "shared_memory.h"
#include "thread_mark.h"

#include "scada/common/clock.h"
#include "scada/common/panic.h"
#include "scada/tag/liveness.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/types.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace scada::tag {

namespace {

constexpr std::string_view CMD_PREFIX = "cmd.";
constexpr std::string_view RESULT_PREFIX = "result.";

constexpr std::uint8_t OPEN_PRODUCER = 1U << 0U;
constexpr std::uint8_t OPEN_CONSUMER = 1U << 1U;

// The answer queue of the CMD queue `cmd` in `contents` (QueueAccess).
std::optional<std::size_t> answer_queue_of(const HeaderContents& contents,
                                           const QueueInfo& cmd) {
    const std::string_view rest =
        std::string_view(cmd.name).substr(CMD_PREFIX.size());
    for (std::size_t i = 0; i < contents.queues.size(); ++i) {
        const QueueInfo& queue = contents.queues[i];
        if (queue.kind == QueueKind::RESULT &&
            std::string_view(queue.name).substr(RESULT_PREFIX.size()) == rest &&
            queue.producer_process == cmd.consumer_process &&
            queue.consumer_process == cmd.producer_process) {
            return i;
        }
    }
    return std::nullopt;
}

// True if a cursor `read` and a `write_seq` `written` of a queue of
// `capacity` records are no valid pair (section 9.1).
SCADA_RT bool out_of_range(std::uint64_t read, std::uint64_t written,
                           std::uint32_t capacity) noexcept {
    return read > written || written - read > capacity;
}

}  // namespace

void Attachment::prepare_queues() {
    const HeaderContents& contents = header_.contents();
    const std::size_t count = contents.queues.size();
    queue_threads_ = std::vector<std::atomic<const void*>>(count);
    queue_woken_seq_ = std::vector<std::atomic<std::uint64_t>>(count);
    queue_open_ = std::vector<std::uint8_t>(count, 0);
    queue_counters_ = std::vector<QueueCounters>(count);
    queue_in_use_ = std::vector<std::atomic<const PeerMapping*>>(count);
    queue_dead_ = std::vector<std::atomic<bool>>(count);
    queue_access_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const QueueInfo& info = contents.queues[i];
        QueueAccess access;
        access.id = i;
        access.kind = info.kind;
        access.capacity = info.capacity;
        access.record_size = info.record_size;
        access.producer = info.producer_process;
        access.consumer = info.consumer_process;
        access.consumer_timeout_ms =
            contents.processes.at(info.consumer_process).heartbeat_timeout_ms;
        access.produced = info.producer_process == process_index_;
        access.consumed = info.consumer_process == process_index_;
        access.queue_slot = peers_.queue_slot(i);
        access.consumer_proc_slot = peers_.proc_slot(info.consumer_process);
        if (info.kind == QueueKind::CMD) {
            access.answer_queue = answer_queue_of(contents, info);
        }
        queue_access_.push_back(access);
    }
    for (const OwnedQueue& owned : queues_) {
        queue_access_.at(owned.queue_id).own_region =
            owned.region.memory.bytes();
    }
    // Mapped here rather than at scada_queue_open, because the drain and the
    // cursors below need them (section 11.2: never in a real-time function).
    for (const QueueAccess& access : queue_access_) {
        if (access.produced || access.consumed) {
            want_queue(access.id);
        }
    }
    // A queue region created while its consumer's cursor is not 0 was
    // created anew inside the generation (decision D-40): the consumer will
    // find its cursor out of range or the file replaced. Counted, so that
    // the process can log it; the library does not log.
    for (const OwnedQueue& owned : queues_) {
        const QueueAccess& access = queue_access_.at(owned.queue_id);
        if (owned.region.reused || access.consumer == process_index_) {
            continue;
        }
        if (std::uint64_t* cursor = cursor_field(
                peers_.bytes(access.consumer_proc_slot), access.id);
            cursor != nullptr &&
            load_shared(*cursor, std::memory_order_acquire) != 0) {
            count_one(counters(access.id).replaced);
        }
    }
}

bool Attachment::queue_is_dead(std::size_t queue_id) const noexcept {
    static_cast<void>(queue(queue_id));
    // Relaxed: see `queue_dead`.
    return queue_dead_[queue_id].load(std::memory_order_relaxed);
}

void Attachment::kill_queue(std::size_t queue_id) noexcept {
    // Relaxed: one thread uses the consumer's side; attach is before it.
    if (queue_dead_[queue_id].exchange(true, std::memory_order_relaxed)) {
        return;
    }
    count_one(counters(queue_id).replaced);
    // Release: the producer loads the cursor with acquire.
    store_shared(own_cursor(queue_id), DEAD_CURSOR, std::memory_order_release);
}

void Attachment::use_queue_file(std::size_t queue_id,
                                const PeerMapping* mapping) noexcept {
    // Relaxed: see `queue_in_use_`.
    queue_in_use_[queue_id].store(mapping, std::memory_order_relaxed);
}

void Attachment::jump_cursor(std::size_t queue_id, std::uint64_t read,
                             std::uint64_t base) noexcept {
    if (base <= read) {
        return;  // never back (issue #53, condition C1)
    }
    count_one(counters(queue_id).skipped, base - read);
    // Release: the producer loads the cursor with acquire and may then use
    // the slots before `base`, which this consumer will never read.
    store_shared(own_cursor(queue_id), base, std::memory_order_release);
}

namespace {

// True if the two mappings are of the same file (issue #53, review of PR
// #77): a file mapped again, after a look that failed, is the file in use.
bool same_file(const PeerMapping& one, const PeerMapping& other) noexcept {
    return one.region.device == other.region.device &&
           one.region.inode == other.region.inode;
}

}  // namespace

bool Attachment::follow_queue(std::size_t queue_id) noexcept {
    const QueueAccess& access = queue(queue_id);
    if (queue_is_dead(queue_id)) {
        return false;
    }
    if (peers_.at_mapping_limit(access.queue_slot)) {
        kill_queue(queue_id);  // decision D-49, Q2
        return false;
    }
    // Relaxed: see `queue_in_use_`.
    const PeerMapping* const used =
        queue_in_use_[queue_id].load(std::memory_order_relaxed);
    // `current` first, then `whole` (review of PR #77, F3): a look marks an
    // old mapping before it publishes a newer one, so a `whole` loaded
    // after `current` (both acquire) is at least as new as that look.
    const PeerMapping* const current = peers_.mapping(access.queue_slot);
    if (used != nullptr && !used->whole.load(std::memory_order_acquire)) {
        // The file in use changed size: its mapping must not be read again
        // (section 3.3).
        kill_queue(queue_id);
        return false;
    }
    std::uint64_t& cursor = own_cursor(queue_id);
    // Relaxed: this thread is the only writer of the cursor.
    const std::uint64_t read = load_shared(cursor, std::memory_order_relaxed);
    if (used == nullptr) {
        if (current == nullptr) {
            return true;  // not created yet
        }
        std::uint64_t base = 0;
        const QueueBase kind =
            queue_base_of(current->region.memory.bytes(), base);
        if (kind == QueueBase::BROKEN) {
            kill_queue(queue_id);  // outside input (condition B5)
            return false;
        }
        use_queue_file(queue_id, current);
        if (kind == QueueBase::SET) {
            // Positions below the base are in no file this consumer can
            // reach: a cursor below it always jumps (condition C3).
            jump_cursor(queue_id, read, base);
        }
        return true;
    }
    if (current == nullptr || current == used || same_file(*current, *used)) {
        return true;  // the name is missing, or the same file: keep it
    }
    // Another file under the name (issue #53).
    std::uint64_t base = 0;
    if (queue_base_of(current->region.memory.bytes(), base) != QueueBase::SET) {
        // No base (an older build, decision D-40) or one that is not valid.
        kill_queue(queue_id);
        return false;
    }
    // Read again now that the new file is ready (condition C2): only the
    // producer writes `write_seq`, and it created the new file at attach,
    // so the file in use is final unless two instances of it live
    // (contract 3.3). It is whole: `check_kept` ran in the look that
    // published the new file.
    const std::uint64_t old_written =
        queue_write_seq(used->region.memory.bytes());
    if (base < old_written) {
        kill_queue(queue_id);  // ids of the new file would repeat
        return false;
    }
    if (read < old_written &&
        !out_of_range(read, old_written, access.capacity)) {
        return true;  // deliver what the file in use holds first
    }
    jump_cursor(queue_id, read, base);
    use_queue_file(queue_id, current);
    count_one(counters(queue_id).replaced);
    return true;
}

std::optional<std::uint64_t> Attachment::base_for_new_queue(
    std::size_t queue_id) noexcept {
    const HeaderContents& contents = header_.contents();
    const QueueInfo& info = contents.queues.at(queue_id);
    const std::uint64_t capacity = info.capacity;
    // R': every id ever given in this generation is below the consumer's
    // cursor at that post plus the capacity (9.1), and the cursor only
    // grows. A cursor of 0 is left out: in a new generation the consumer
    // may attach first, and the queue then starts at 0 (issue #17).
    std::uint64_t from_cursor = 0;
    const std::size_t slot = peers_.proc_slot(info.consumer_process);
    peers_.want(directory_.get(), slot, monotonic_ns());
    if (const std::uint64_t* cursor =
            cursor_field(peers_.bytes(slot), queue_id);
        cursor != nullptr) {
        const std::uint64_t read =
            load_shared(*cursor, std::memory_order_acquire);
        if (read != 0 && read != DEAD_CURSOR &&
            read <= DEAD_CURSOR - capacity) {
            from_cursor = read + capacity;
        }
    }
    // H': the next position this process would have given, if its `proc`
    // region is the one it posted with. It is input: one above R' cannot
    // be (every id is below R'), so it is taken only when R' is unknown or
    // not below it.
    std::uint64_t from_mark = 0;
    if (proc_.reused()) {
        if (const std::uint64_t* mark = cursor_field(proc_.bytes(), queue_id);
            mark != nullptr) {
            from_mark = load_shared(*mark, std::memory_order_relaxed);
        }
        if (from_cursor != 0 && from_mark > from_cursor) {
            from_mark = 0;
        }
    }
    const std::uint64_t base = std::max(from_mark, from_cursor);
    // Room for a capacity of ids: below the reserved all-ones cursor, and
    // for CMD below 2^48 (section 9.4).
    const std::uint64_t limit =
        info.kind == QueueKind::CMD ? MAX_REQUEST_SEQ : DEAD_CURSOR - 1;
    if (base > limit - capacity) {
        return std::nullopt;
    }
    return base;
}

void Attachment::follow_queues() {
    for (const QueueAccess& access : queue_access_) {
        if (access.consumed && !access.produced) {
            static_cast<void>(follow_queue(access.id));
        }
    }
}

void Attachment::start_cursors() {
    for (const QueueAccess& access : queue_access_) {
        if (!access.consumed) {
            continue;
        }
        // A new `proc` region has lost its cursors: each starts at the
        // queue's `write_seq`, and the records before it, from the file's
        // base on, are skipped (section 9.2).
        const PeerMapping* const current = peers_.mapping(access.queue_slot);
        std::uint64_t written = 0;
        std::uint64_t first = 0;
        if (current != nullptr) {
            const std::span<std::byte> bytes = current->region.memory.bytes();
            std::uint64_t base = 0;
            if (queue_base_of(bytes, base) == QueueBase::BROKEN) {
                kill_queue(access.id);
                continue;
            }
            use_queue_file(access.id, current);
            written = queue_write_seq(bytes);
            first = std::min(base, written);
        }
        count_one(counters(access.id).skipped, written - first);
        // Release: the producer loads the cursor with acquire.
        store_shared(own_cursor(access.id), written, std::memory_order_release);
    }
}

void Attachment::drain_requests() {
    std::vector<std::size_t> answered;
    for (const QueueAccess& access : queue_access_) {
        if (access.consumed && access.kind == QueueKind::CMD &&
            drain_queue(access) && access.answer_queue.has_value()) {
            answered.push_back(*access.answer_queue);
        }
    }
    // The answers were posted by the attaching thread, which is not
    // recorded as the thread of their queues; it wakes them here instead
    // of in scada_tag_notify. A failed wake delays them by one wait slice.
    for (const std::size_t queue_id : answered) {
        static_cast<void>(futex_wake_all(
            &queue_header(queue_access_.at(queue_id).own_region).write_seq));
    }
}

void Attachment::drain_retiring_requests() noexcept {
    if (!header_.retiring()) {
        return;
    }
    for (const QueueAccess& access : queue_access_) {
        if (access.consumed && access.kind == QueueKind::CMD) {
            // A requester that created its queue after this process last
            // looked may have posted to it (review of PR #82, B2).
            static_cast<void>(look_for_queue_now(access.id));
        }
        if (access.consumed && access.kind == QueueKind::CMD &&
            drain_queue(access) && access.answer_queue.has_value()) {
            // As in drain_requests: a failed wake delays the answers by one
            // wait slice of their requester.
            static_cast<void>(futex_wake_all(
                &queue_header(queue_access_.at(*access.answer_queue).own_region)
                     .write_seq));
        }
    }
}

bool Attachment::look_for_queue_now(std::size_t queue_id) noexcept {
    const QueueAccess& access = queue(queue_id);
    // Also when a file is mapped: its producer may have created it anew
    // under the name since the last look (review of PR #82, after #77).
    if (access.produced) {
        return true;
    }
    try {
        static_cast<void>(peers_.look_now(directory_.get(), access.queue_slot));
    } catch (const std::exception&) {
        // A failed allocation: the region stays unmapped, as if missing.
        return false;
    }
    return true;
}

void Attachment::drain_request_queue(std::size_t queue_id) noexcept {
    const QueueAccess& access = queue(queue_id);
    if (access.consumed && access.kind == QueueKind::CMD &&
        drain_queue(access) && access.answer_queue.has_value()) {
        static_cast<void>(futex_wake_all(
            &queue_header(queue_access_.at(*access.answer_queue).own_region)
                 .write_seq));
    }
}

bool Attachment::drain_queue(const QueueAccess& access) noexcept {
    // The file in use, then a file created anew under the name, which
    // `follow_queue` moves to once the one in use is drained (issue #53):
    // a drain must not end on the old file (review of PR #82, after #77).
    bool posted = false;
    for (int file = 0; file < 2; ++file) {
        // Relaxed: see `queue_in_use_`.
        const PeerMapping* const before =
            queue_in_use_[access.id].load(std::memory_order_relaxed);
        if (!follow_queue(access.id)) {
            return posted;  // dead
        }
        if (file > 0 && queue_in_use_[access.id].load(
                            std::memory_order_relaxed) == before) {
            break;  // no newer file
        }
        if (!drain_file(access, posted)) {
            break;
        }
    }
    return posted;
}

bool Attachment::drain_file(const QueueAccess& access, bool& posted) noexcept {
    const std::span<std::byte> bytes = queue_bytes(access.id);
    if (bytes.empty()) {
        return false;  // no region, no request
    }
    QueueCounters& counts = counters(access.id);
    std::uint64_t& cursor = own_cursor(access.id);
    // Relaxed: this process is the only writer of its cursor.
    const std::uint64_t read = load_shared(cursor, std::memory_order_relaxed);
    // seq_cst: after a seq_cst load of the state that saw RETIRING, every
    // request a post refused nothing for is seen (decision D-54).
    const std::uint64_t written = queue_write_seq_sc(bytes);
    if (out_of_range(read, written, access.capacity)) {
        // Not followed (section 9.2): the queue stays stopped. Setting the
        // cursor to `write_seq` could give ids that were given before in
        // this generation (decision D-40).
        count_one(counts.out_of_range);
        return false;
    }
    for (std::uint64_t position = read; position < written; ++position) {
        RecordWords words;
        load_record(bytes, access.capacity, access.record_size, position,
                    words);
        const WriteRequest request = request_of(words);
        if (request.request_id != request_id_of(access.id, position) ||
            !request_valid(request)) {
            count_one(counts.invalid);
            continue;
        }
        count_one(counts.aborted);
        // Dropped and counted if it cannot be posted (section 9.2).
        posted = answer(access.id, request.request_id, WriteStatus::ABORTED) ==
                     SCADA_OK ||
                 posted;
    }
    // Release: the producer loads the cursor with acquire.
    store_shared(cursor, written, std::memory_order_release);
    return true;
}

std::optional<std::size_t> Attachment::find_queue(
    std::string_view name) const noexcept {
    for (const QueueAccess& access : queue_access_) {
        if (header_.contents().queues[access.id].name == name) {
            return access.id;
        }
    }
    return std::nullopt;
}

const QueueAccess& Attachment::queue(std::size_t queue_id) const noexcept {
    if (queue_id >= queue_access_.size()) {
        panic("attachment: no such queue");
    }
    return queue_access_[queue_id];
}

bool Attachment::claim_queue(std::size_t queue_id, bool producer) {
    const std::uint8_t bit = producer ? OPEN_PRODUCER : OPEN_CONSUMER;
    static_cast<void>(queue(queue_id));
    // Under the mutex, so that `close_queue` sees the side open or clears
    // the binding before this side's thread can bind (decision D-40).
    const std::scoped_lock lock(queue_sides_mutex_);
    std::uint8_t& open = queue_open_[queue_id];
    if ((open & bit) != 0) {
        return false;
    }
    open = static_cast<std::uint8_t>(open | bit);
    return true;
}

void Attachment::close_queue(std::size_t queue_id, bool producer) {
    const std::uint8_t bit = producer ? OPEN_PRODUCER : OPEN_CONSUMER;
    const QueueAccess& access = queue(queue_id);
    const std::optional<std::size_t> posted =
        producer ? std::optional<std::size_t>(queue_id) : access.answer_queue;
    // One step with `claim_queue`: no side can open between the test that
    // no posting side is open and the clear (review of PR #56). A lock that
    // fails ends the process: the destructor that calls this cannot throw.
    const std::scoped_lock lock(queue_sides_mutex_);
    std::uint8_t& open = queue_open_[queue_id];
    open = static_cast<std::uint8_t>(open & static_cast<std::uint8_t>(~bit));
    if (!posted.has_value() || queue_side_open(*posted, true) ||
        answer_queue_in_use(*posted)) {
        return;
    }
    // Relaxed: every bind is made by the thread of an open side, before
    // that side's close takes the mutex; the mutex orders it before this
    // store.
    queue_threads_[*posted].store(nullptr, std::memory_order_relaxed);
}

bool Attachment::queue_side_open(std::size_t queue_id,
                                 bool producer) const noexcept {
    const std::uint8_t bit = producer ? OPEN_PRODUCER : OPEN_CONSUMER;
    return (queue_open_[queue_id] & bit) != 0;
}

void Attachment::want_queue(std::size_t queue_id) {
    const QueueAccess& access = queue(queue_id);
    const std::int64_t now = monotonic_ns();
    if (access.produced && access.consumer != process_index_) {
        peers_.want(directory_.get(), access.consumer_proc_slot, now);
    }
    if (access.consumed && !access.produced) {
        peers_.want(directory_.get(), access.queue_slot, now);
    }
}

std::span<std::byte> Attachment::queue_bytes(
    std::size_t queue_id) const noexcept {
    const QueueAccess& access = queue(queue_id);
    if (access.produced) {
        return access.own_region;
    }
    // The file in use (`follow_queue`), which stays mapped as long as the
    // context. Relaxed: see `queue_in_use_`.
    const PeerMapping* const used =
        queue_in_use_[queue_id].load(std::memory_order_relaxed);
    return used == nullptr ? std::span<std::byte>{}
                           : used->region.memory.bytes();
}

std::uint64_t& Attachment::own_cursor(std::size_t queue_id) const noexcept {
    std::uint64_t* cursor = cursor_field(proc_.bytes(), queue_id);
    if (cursor == nullptr) {
        panic("attachment: the own proc region has no cursor for a queue");
    }
    return *cursor;
}

int Attachment::post_record(std::size_t queue_id,
                            RecordWords& record) noexcept {
    const QueueAccess& access = queue(queue_id);
    if (!access.produced) {
        panic("attachment: post to a queue of another process");
    }
    const std::int64_t now = monotonic_ns();
    EventRecord event;
    WriteRequest request;
    switch (access.kind) {
        case QueueKind::EVENTS:
            event = event_of(record);
            if (!event_valid(event)) {
                return SCADA_E_INVALID;
            }
            break;
        case QueueKind::CMD:
            request = request_of(record);
            // The library's own time, so that the lifetime check of the
            // consumer does not depend on what the poster says.
            request.posted_mono_ns = now;
            if (!request_valid(request) || request_expired(request, now)) {
                return SCADA_E_INVALID;
            }
            break;
        case QueueKind::RESULT:
            if (!result_valid(result_of(record))) {
                return SCADA_E_INVALID;
            }
            break;
    }

    // Decision D-54: nothing new is posted in a RETIRING generation, but
    // answers are, the library's own ABORTED and EXPIRED ones and a
    // driver's result for a request it already took. The first load of the
    // handshake.
    if (access.kind != QueueKind::RESULT && header_.retiring()) {
        return SCADA_E_RETIRING;
    }

    // The tag-set builder refuses a queue with one process at both ends,
    // and `scada_queue_open` refuses one from a header (decision D-40):
    // the cursor slot of a queue this process produces holds its
    // high-water mark below, which must not be a cursor (condition C5).
    if (access.consumer == process_index_) {
        panic("attachment: a queue with one process at both ends");
    }
    // Section 9.1, judged by this process (section 10.1).
    const std::span<std::byte> consumer =
        peers_.bytes(access.consumer_proc_slot);
    if (proc_liveness(consumer, access.consumer_timeout_ms, now) !=
        DownReason::NONE) {
        return SCADA_E_PEER_DOWN;
    }
    std::uint64_t* cursor = cursor_field(consumer, queue_id);
    if (cursor == nullptr) {
        return SCADA_E_PEER_DOWN;
    }
    // Acquire: the consumer has finished with the records before its
    // cursor; their slots may be written again.
    const std::uint64_t read = load_shared(*cursor, std::memory_order_acquire);
    QueueHeader& header = queue_header(access.own_region);
    // Relaxed: this thread is the only writer of `write_seq`.
    const std::uint64_t written =
        load_shared(header.write_seq, std::memory_order_relaxed);
    // A file created dead has `write_seq` all-ones; with an all-ones cursor
    // the pair is in range, and a post would wrap `write_seq` to 0 (review
    // of PR #77, F2). Such a queue takes no post.
    if (written == DEAD_CURSOR ||
        out_of_range(read, written, access.capacity)) {
        return SCADA_E_PEER_DOWN;  // the cursor or the queue is not valid
    }
    if (written - read == access.capacity) {
        return SCADA_E_FULL;
    }
    if (written + 1 == DEAD_CURSOR) {
        // DEAD_CURSOR is reserved (decision D-40): no `write_seq` reaches
        // it, so that it is never a valid cursor.
        return SCADA_E_FULL;
    }

    // Section 9.4: the id comes from `write_seq`.
    switch (access.kind) {
        case QueueKind::EVENTS:
            event.seq = written;
            record = words_of(event);
            break;
        case QueueKind::CMD:
            if (written > MAX_REQUEST_SEQ) {
                return SCADA_E_FULL;  // no id is left in this generation
            }
            request.request_id = request_id_of(queue_id, written);
            record = words_of(request);
            break;
        case QueueKind::RESULT:
            break;
    }
    // The high-water mark (decision D-49): after every refusal and before
    // the record, so that a crash between this store and `write_seq` only
    // burns the id, and a producer that creates the file anew never gives
    // it again. Relaxed: its only reader is a later attach of this process
    // name, after this one has gone (condition C4).
    store_shared(own_cursor(queue_id), written + 1, std::memory_order_relaxed);
    store_record(access.own_region, access.capacity, access.record_size,
                 written, record);
    // Publishes the record, then the second load of the state (decision
    // D-54).
    const bool retiring_after =
        publish_write_seq(access.own_region, written + 1, header_);
    count_one(counters(queue_id).posted);
    if (access.kind == QueueKind::EVENTS && retiring_after) {
        // RETIRING came while the record was published: the drain may or
        // may not see it. Refused, so that no action depends on it; if it
        // is delivered, the journal holds a record of an action that was
        // not taken, which is the safe side. A request is not checked
        // again: its requester must never be told "refused" for a request
        // a driver may execute.
        return SCADA_E_RETIRING;
    }
    return SCADA_OK;
}

bool Attachment::bind_queue_thread(std::size_t queue_id) noexcept {
    static_cast<void>(queue(queue_id));
    std::atomic<const void*>& writer = queue_threads_[queue_id];
    const void* const self = thread_mark();
    // Relaxed: as in `check_writer_thread`.
    const void* first = writer.load(std::memory_order_relaxed);
    if (first == self) {
        return true;
    }
    return first == nullptr && writer.compare_exchange_strong(
                                   first, self, std::memory_order_relaxed);
}

bool Attachment::queue_thread_recorded(std::size_t queue_id) const noexcept {
    static_cast<void>(queue(queue_id));
    // Relaxed: as in `check_writer_thread`.
    return queue_threads_[queue_id].load(std::memory_order_relaxed) != nullptr;
}

bool Attachment::check_queue_thread(std::size_t queue_id) noexcept {
    if (bind_queue_thread(queue_id)) {
        return true;
    }
#ifndef NDEBUG
    panic("one queue posted from two threads (contract 13.2)");
#endif
    return false;
}

bool Attachment::answer_queue_in_use(std::size_t queue_id) const noexcept {
    return std::ranges::any_of(queue_access_,
                               [this, queue_id](const QueueAccess& access) {
                                   return access.answer_queue == queue_id &&
                                          queue_side_open(access.id, false);
                               });
}

QueueCounters& Attachment::counters(std::size_t queue_id) noexcept {
    static_cast<void>(queue(queue_id));
    return queue_counters_[queue_id];
}

int Attachment::answer(std::size_t queue_id, std::uint64_t request_id,
                       WriteStatus status) noexcept {
    const QueueAccess& access = queue(queue_id);
    if (!access.answer_queue.has_value()) {
        count_one(counters(queue_id).answers_dropped);
        return SCADA_E_NOT_FOUND;
    }
    WriteResult result;
    result.request_id = request_id;
    result.status = to_raw(status);
    result.completed_time_ns = realtime_ns();
    RecordWords words = words_of(result);
    const int code = post_record(*access.answer_queue, words);
    if (code != SCADA_OK) {
        count_one(counters(queue_id).answers_dropped);
    }
    return code;
}

}  // namespace scada::tag
