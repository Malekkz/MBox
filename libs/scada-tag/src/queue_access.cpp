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
    queue_open_ = std::vector<std::atomic<std::uint8_t>>(count);
    queue_counters_ = std::vector<QueueCounters>(count);
    queue_mappings_ = std::vector<std::atomic<const std::byte*>>(count);
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
        if (access.consumed && !access.produced) {
            // The mapping the queue starts with in this context.
            static_cast<void>(queue_dead(access.id, queue_bytes(access.id)));
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

bool Attachment::queue_dead(std::size_t queue_id,
                            std::span<std::byte> bytes) noexcept {
    static_cast<void>(queue(queue_id));
    // Relaxed: one thread uses the consumer's side; attach is before it.
    if (queue_dead_[queue_id].load(std::memory_order_relaxed)) {
        return true;
    }
    std::atomic<const std::byte*>& first = queue_mappings_[queue_id];
    const std::byte* seen = first.load(std::memory_order_relaxed);
    if (seen == nullptr) {
        if (!bytes.empty()) {
            first.store(bytes.data(), std::memory_order_relaxed);
        }
        return false;
    }
    if (bytes.data() == seen) {
        return false;
    }
    queue_dead_[queue_id].store(true, std::memory_order_relaxed);
    count_one(counters(queue_id).replaced);
    // Release: the producer loads the cursor with acquire.
    store_shared(own_cursor(queue_id), DEAD_CURSOR, std::memory_order_release);
    return true;
}

void Attachment::start_cursors() {
    for (const QueueAccess& access : queue_access_) {
        if (!access.consumed) {
            continue;
        }
        const std::span<std::byte> bytes = queue_bytes(access.id);
        const std::uint64_t written =
            bytes.empty() ? 0 : queue_write_seq(bytes);
        count_one(counters(access.id).skipped, written);
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

bool Attachment::drain_queue(const QueueAccess& access) noexcept {
    const std::span<std::byte> bytes = queue_bytes(access.id);
    if (bytes.empty() || queue_dead(access.id, bytes)) {
        return false;  // no region, no request
    }
    QueueCounters& counts = counters(access.id);
    std::uint64_t& cursor = own_cursor(access.id);
    // Relaxed: this process is the only writer of its cursor.
    const std::uint64_t read = load_shared(cursor, std::memory_order_relaxed);
    const std::uint64_t written = queue_write_seq(bytes);
    if (out_of_range(read, written, access.capacity)) {
        // Not followed (section 9.2): the queue stays stopped. Setting the
        // cursor to `write_seq` could give ids that were given before in
        // this generation (decision D-40).
        count_one(counts.out_of_range);
        return false;
    }
    bool posted = false;
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
    return posted;
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

bool Attachment::claim_queue(std::size_t queue_id, bool producer) noexcept {
    const std::uint8_t bit = producer ? OPEN_PRODUCER : OPEN_CONSUMER;
    static_cast<void>(queue(queue_id));
    // Relaxed: only the bit itself matters.
    return (queue_open_[queue_id].fetch_or(bit, std::memory_order_relaxed) &
            bit) == 0;
}

void Attachment::release_queue(std::size_t queue_id, bool producer) noexcept {
    const std::uint8_t bit = producer ? OPEN_PRODUCER : OPEN_CONSUMER;
    static_cast<void>(queue(queue_id));
    // Relaxed: see `claim_queue`.
    queue_open_[queue_id].fetch_and(static_cast<std::uint8_t>(~bit),
                                    std::memory_order_relaxed);
}

bool Attachment::queue_open(std::size_t queue_id,
                            bool producer) const noexcept {
    const std::uint8_t bit = producer ? OPEN_PRODUCER : OPEN_CONSUMER;
    static_cast<void>(queue(queue_id));
    // Relaxed: see `claim_queue`.
    return (queue_open_[queue_id].load(std::memory_order_relaxed) & bit) != 0;
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
    return access.produced ? access.own_region
                           : peers_.bytes(access.queue_slot);
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

    // Section 9.1, judged by this process (section 10.1).
    const std::span<std::byte> consumer =
        access.consumer == process_index_
            ? proc_.bytes()
            : peers_.bytes(access.consumer_proc_slot);
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
    if (out_of_range(read, written, access.capacity)) {
        return SCADA_E_PEER_DOWN;  // the cursor is not valid
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
    store_record(access.own_region, access.capacity, access.record_size,
                 written, record);
    // Release: publishes the record.
    store_shared(header.write_seq, written + 1, std::memory_order_release);
    count_one(counters(queue_id).posted);
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

bool Attachment::check_queue_thread(std::size_t queue_id) noexcept {
    if (bind_queue_thread(queue_id)) {
        return true;
    }
#ifndef NDEBUG
    panic("one queue posted from two threads (contract 13.2)");
#endif
    return false;
}

void Attachment::release_queue_thread(std::size_t queue_id) noexcept {
    static_cast<void>(queue(queue_id));
    // Relaxed: as in `check_writer_thread`.
    queue_threads_[queue_id].store(nullptr, std::memory_order_relaxed);
}

bool Attachment::answer_queue_in_use(std::size_t queue_id) const noexcept {
    return std::ranges::any_of(queue_access_, [this, queue_id](
                                                  const QueueAccess& access) {
        return access.answer_queue == queue_id && queue_open(access.id, false);
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
