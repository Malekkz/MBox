#include "queue.h"

#include "attachment.h"
#include "futex.h"
#include "layout.h"
#include "queue_region.h"
#include "shared_memory.h"
#include "subscription.h"
#include "wait_set.h"

#include "scada/common/clock.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace scada::tag {

namespace {

using RecordBytes = std::array<std::byte, MAX_RECORD_SIZE>;

// A cursor `read` and a `write_seq` `written` that are no valid pair
// (section 9.2).
bool out_of_range(std::uint64_t read, std::uint64_t written,
                  std::uint32_t capacity) noexcept {
    return read > written || written - read > capacity;
}

}  // namespace

int Queue::open(Attachment& attachment, std::string_view name,
                std::unique_ptr<Queue>& out) {
    out.reset();
    if (attachment.observer()) {
        // An observer has no side in any queue (section 11.4, item 4), and
        // no queue table: refused before anything is looked up or mapped.
        return SCADA_E_NOT_OWNER;
    }
    if (!is_valid_name(name, MAX_SHORT_NAME_LENGTH)) {
        return SCADA_E_INVALID;
    }
    const std::optional<std::size_t> queue_id = attachment.find_queue(name);
    if (!queue_id.has_value()) {
        return SCADA_E_NOT_FOUND;
    }
    const QueueAccess& access = attachment.queue(*queue_id);
    if (access.produced && access.consumed) {
        // One process at both ends: which side is meant cannot be told.
        // The tag-set builder refuses such a queue.
        return SCADA_E_INVALID;
    }
    if (!access.produced && !access.consumed) {
        return SCADA_E_NOT_OWNER;
    }
    auto queue =
        std::make_unique<Queue>(attachment, *queue_id, access.produced);
    if (!attachment.claim_queue(*queue_id, access.produced)) {
        return SCADA_E_BUSY;
    }
    // From here the handle owns the side: if what follows fails, its
    // destructor closes it.
    queue->claimed_ = true;
    // Mapped and brought into memory here (section 11.2), if attach did not.
    attachment.want_queue(*queue_id);
    if (access.consumed) {
        // The mapping the queue has from here on (decision D-40).
        static_cast<void>(attachment.queue_dead(
            *queue_id, attachment.queue_bytes(*queue_id)));
    }
    out = std::move(queue);
    return SCADA_OK;
}

Queue::Queue(Attachment& attachment, std::size_t queue_id,
             bool producer) noexcept
    : attachment_(attachment), id_(queue_id), producer_(producer) {}

Queue::~Queue() {
    if (wait_set_ != nullptr) {
        wait_set_->remove(*this);
    }
    // A queue stays bound to its thread while any side that posts to it is
    // open: its producer side, and for an answer queue the side that
    // consumes its `cmd` queue. The close of the last of them clears the
    // binding, whichever thread closes (review of PR #56).
    if (claimed_) {
        attachment_.close_queue(id_, producer_);
    }
}

scada_queue_stats_t Queue::stats() noexcept {
    const QueueCounters& counters = attachment_.counters(id_);
    const QueueAccess& access = attachment_.queue(id_);
    // Relaxed: each count is a value on its own.
    constexpr std::memory_order RELAXED = std::memory_order_relaxed;
    scada_queue_stats_t stats{};
    stats.posted = counters.posted.load(RELAXED);
    stats.delivered = counters.delivered.load(RELAXED);
    stats.expired = counters.expired.load(RELAXED);
    stats.aborted = counters.aborted.load(RELAXED);
    stats.skipped = counters.skipped.load(RELAXED);
    stats.answers_dropped = counters.answers_dropped.load(RELAXED);
    stats.invalid = counters.invalid.load(RELAXED);
    stats.out_of_range = counters.out_of_range.load(RELAXED);
    stats.replaced = counters.replaced.load(RELAXED);
    // Below MAX_QUEUES, which fits 16 bits.
    stats.queue_id = static_cast<std::uint16_t>(id_);
    stats.kind = to_raw(access.kind);
    stats.producer = producer_ ? 1 : 0;
    return stats;
}

std::size_t Queue::record_size() const noexcept {
    return attachment_.queue(id_).record_size;
}

int Queue::post(std::span<const std::byte> record) noexcept {
    if (!producer_ || record.size() != record_size() ||
        record.size() > MAX_RECORD_SIZE) {
        return SCADA_E_INVALID;
    }
    RecordBytes bytes{};
    std::ranges::copy(record, bytes.begin());
    RecordWords words{
        std::bit_cast<std::array<std::uint64_t, MAX_RECORD_WORDS>>(bytes)};
    // Every build (decision D-40): a second posting thread is refused.
    if (!attachment_.check_queue_thread(id_)) {
        return SCADA_E_INVALID;
    }
    return attachment_.post_record(id_, words);
}

Queue::Look Queue::poll(std::span<std::byte> out) noexcept {
    const QueueAccess& access = attachment_.queue(id_);
    const std::span<std::byte> bytes = attachment_.queue_bytes(id_);
    if (attachment_.queue_dead(id_, bytes)) {
        pending_ = false;
        count_one(attachment_.counters(id_).out_of_range);
        return Look::BAD_RANGE;  // replaced inside the generation (D-40)
    }
    if (bytes.empty()) {
        pending_ = false;
        return Look::NOTHING;  // the producer has not created it yet
    }
    std::uint64_t& cursor = attachment_.own_cursor(id_);
    // Relaxed: this thread is the only writer of the cursor.
    const std::uint64_t read = load_shared(cursor, std::memory_order_relaxed);
    const std::uint64_t written = queue_write_seq(bytes);
    if (read == written) {
        pending_ = false;
        return Look::NOTHING;
    }
    if (out_of_range(read, written, access.capacity)) {
        pending_ = false;
        count_one(attachment_.counters(id_).out_of_range);
        return Look::BAD_RANGE;  // reported, not followed (section 9.2)
    }
    RecordWords words;
    load_record(bytes, access.capacity, access.record_size, read, words);
    bool valid = false;
    switch (access.kind) {
        case QueueKind::EVENTS: {
            const EventRecord event = event_of(words);
            valid = event.seq == read && event_valid(event);
            break;
        }
        case QueueKind::CMD: {
            const WriteRequest request = request_of(words);
            valid = request.request_id == request_id_of(id_, read) &&
                    request_valid(request);
            break;
        }
        case QueueKind::RESULT:
            valid = result_valid(result_of(words));
            break;
    }
    if (!valid || access.kind == QueueKind::CMD) {
        // A record that is not valid is passed over, so that one damaged
        // record cannot stop the queue. A request is taken before it is
        // executed (section 9.2: at most once). Release: the producer loads
        // the cursor with acquire and may then reuse the slot.
        store_shared(cursor, read + 1, std::memory_order_release);
        pending_ = false;
        if (!valid) {
            count_one(attachment_.counters(id_).invalid);
            return Look::INVALID;
        }
        // The copy is judged, never the mapped record, on a clock read
        // after `write_seq` was loaded.
        const WriteRequest request = request_of(words);
        if (request_expired(request, monotonic_ns())) {
            // Never executed late (section 9.2). The cursor has moved on
            // first: a crash before the answer loses the answer, which the
            // requester sees as the request expiring, and never gives the
            // request twice. The answer is dropped and counted if it cannot
            // be posted. No wake: scada_tag_notify or the slice of 100 ms
            // of the requester's wait delivers it.
            count_one(attachment_.counters(id_).expired);
            if (answer_bound_) {
                static_cast<void>(attachment_.answer(id_, request.request_id,
                                                     WriteStatus::EXPIRED));
            } else {
                count_one(attachment_.counters(id_).answers_dropped);
            }
            return Look::ANSWERED;
        }
    } else {
        pending_ = true;
        pending_position_ = read;
    }
    const auto record = std::bit_cast<RecordBytes>(words.words);
    std::ranges::copy(std::span(record).first(out.size()), out.begin());
    count_one(attachment_.counters(id_).delivered);
    return Look::DELIVERED;
}

int Queue::next(std::span<std::byte> out, int timeout_ms) {
    if (producer_ || timeout_ms < 0 || out.size() != record_size() ||
        out.size() > MAX_RECORD_SIZE) {
        return SCADA_E_INVALID;
    }
    const QueueAccess& access = attachment_.queue(id_);
    if (access.answer_queue.has_value()) {
        // The thread that takes the requests is the one that posts to the
        // answer queue (decision D-40). Bound at the first call; a thread
        // that may not post there gets no answer posted, and the drops are
        // counted, in every build: this is the library's own post, not a
        // misuse by the caller.
        answer_bound_ = attachment_.bind_queue_thread(*access.answer_queue);
    }
    const std::int64_t deadline = deadline_after(monotonic_ns(), timeout_ms);
    // Expired requests passed over without a look at the clock: at most a
    // queue's worth, then the deadline is looked at.
    std::uint32_t answered = 0;
    bool stopped = false;
    for (;;) {
        switch (poll(out)) {
            case Look::DELIVERED:
                return SCADA_OK;
            case Look::INVALID:
                return SCADA_E_LAYOUT;  // one record passed over: progress
            case Look::BAD_RANGE:
                // A stopped queue (section 9.2, decision D-40) does not end
                // the wait early: a caller that loops on next must not spin.
                // It is reported when the timeout has passed.
                stopped = true;
                break;
            case Look::ANSWERED:
                if (++answered < access.capacity) {
                    continue;  // look at the next request at once
                }
                answered = 0;
                if (monotonic_ns() >= deadline) {
                    return SCADA_E_TIMEOUT;
                }
                continue;
            case Look::NOTHING:
                break;
        }
        const std::int64_t now = monotonic_ns();
        if (now >= deadline) {
            return stopped ? SCADA_E_LAYOUT : SCADA_E_TIMEOUT;
        }
        // Section 8.4: no wait is longer than the slice.
        const std::int64_t slice_end =
            deadline - now < WAIT_SLICE_NS ? deadline : now + WAIT_SLICE_NS;
        const std::optional<FutexWaiter> word = waiter();
        if (!word.has_value()) {
            // The producer has not created the queue yet, or the queue is
            // stopped: look for its region again (at most once a second),
            // and meanwhile wait a slice.
            attachment_.refresh_peers();
            sleep_until(slice_end);
            continue;
        }
        if (futex_wait(word->word, word->expected, slice_end) ==
            FutexWait::FAILED) {
            return SCADA_E_IO;
        }
    }
}

int Queue::ack() noexcept {
    if (producer_ || attachment_.queue(id_).kind == QueueKind::CMD ||
        !pending_) {
        return SCADA_E_INVALID;
    }
    pending_ = false;
    // Release: the producer may reuse the slot once it sees the cursor.
    store_shared(attachment_.own_cursor(id_), pending_position_ + 1,
                 std::memory_order_release);
    return SCADA_OK;
}

bool Queue::ready() const noexcept {
    if (producer_ || attachment_.queue_is_dead(id_)) {
        return false;
    }
    const std::span<std::byte> bytes = attachment_.queue_bytes(id_);
    if (bytes.empty()) {
        return false;
    }
    // Relaxed: this thread is the only writer of the cursor.
    const std::uint64_t read =
        load_shared(attachment_.own_cursor(id_), std::memory_order_relaxed);
    const std::uint64_t written = queue_write_seq(bytes);
    return read != written &&
           !out_of_range(read, written, attachment_.queue(id_).capacity);
}

std::optional<FutexWaiter> Queue::waiter() const noexcept {
    const std::span<std::byte> bytes = attachment_.queue_bytes(id_);
    if (producer_ || bytes.empty() || attachment_.queue_is_dead(id_)) {
        return std::nullopt;
    }
    // Relaxed: this thread is the only writer of the cursor.
    const std::uint64_t read =
        load_shared(attachment_.own_cursor(id_), std::memory_order_relaxed);
    // A stopped queue gives no word: a wait on it would end at once, since
    // `write_seq` never equals the cursor, and its waiter would spin. The
    // caller sleeps a slice instead.
    if (out_of_range(read, queue_write_seq(bytes),
                     attachment_.queue(id_).capacity)) {
        return std::nullopt;
    }
    return FutexWaiter{.word = &queue_header(bytes).write_seq,
                       .expected = futex_word_of(read),
                       .process_private = false};
}

}  // namespace scada::tag
