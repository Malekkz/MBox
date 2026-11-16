#ifndef SCADA_TAG_QUEUE_H
#define SCADA_TAG_QUEUE_H

// One side of a reliable queue (docs/contracts/tag-database.md, section 9):
// the producer posts, the consumer takes records and acknowledges them. The
// C interface `scada_queue_*` is a thin layer over this class. Private to
// libscada-tag.

#include "attachment.h"
#include "futex.h"

#include "scada/common/realtime.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace scada::tag {

class WaitSet;

// One thread uses a queue handle (section 13.2). The attachment must
// outlive it.
class Queue {
public:
    // Opens the side of the queue `name` that the process of `attachment`
    // has: producer or consumer. A return code of scada/tag/scada_tag.h, as
    // scada_queue_open states.
    [[nodiscard]] static int open(Attachment& attachment, std::string_view name,
                                  std::unique_ptr<Queue>& out);

    Queue(Attachment& attachment, std::size_t queue_id, bool producer) noexcept;
    // Leaves the wait set the queue is in and gives its side free.
    ~Queue();
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;
    Queue(Queue&&) = delete;
    Queue& operator=(Queue&&) = delete;

    // As scada_queue_post, with `record` of the record size.
    SCADA_RT [[nodiscard]] int post(std::span<const std::byte> record) noexcept;

    // As scada_queue_next, with `out` of the record size.
    [[nodiscard]] int next(std::span<std::byte> out, int timeout_ms);

    // As scada_queue_ack.
    [[nodiscard]] int ack() noexcept;

    // As scada_queue_stats.
    [[nodiscard]] scada_queue_stats_t stats() noexcept;

    // True if `next` would return at once with something other than
    // SCADA_E_TIMEOUT, except for a cursor out of range, which `next` has
    // reported. Changes nothing. Consumer only.
    [[nodiscard]] bool ready() const noexcept;

    // The word to wait on for the next record, if the queue region is
    // mapped and the queue is not stopped. Consumer only.
    [[nodiscard]] std::optional<FutexWaiter> waiter() const noexcept;

    [[nodiscard]] Attachment& attachment() const noexcept {
        return attachment_;
    }
    [[nodiscard]] std::size_t id() const noexcept { return id_; }
    [[nodiscard]] bool producer() const noexcept { return producer_; }
    [[nodiscard]] std::size_t record_size() const noexcept;

    // The wait set the queue is in, or null. Set by WaitSet only.
    [[nodiscard]] WaitSet* wait_set() const noexcept { return wait_set_; }
    void set_wait_set(WaitSet* wait_set) noexcept { wait_set_ = wait_set; }

private:
    // What one look at the queue found.
    enum class Look : std::uint8_t {
        DELIVERED,  // `out` holds a record
        NOTHING,    // no record
        INVALID,    // a record that is not valid was passed over
        BAD_RANGE,  // cursor and `write_seq` are no valid pair
        ANSWERED,   // an expired request was answered and passed over
    };

    // `next` in a RETIRING generation (decision D-54): a record that is
    // left, or SCADA_E_RETIRING once the queue is drained; a `cmd` queue
    // answered ABORTED. Never waits.
    [[nodiscard]] int drain_retiring(std::span<std::byte> out) noexcept;
    // One look at the queue without waiting; `write_seq` is loaded with
    // `order` (decision D-54).
    [[nodiscard]] Look poll(
        std::span<std::byte> out,
        std::memory_order order = std::memory_order_acquire) noexcept;

    Attachment& attachment_;
    std::size_t id_ = 0;
    bool producer_ = false;
    // True once `open` has claimed the side (`Attachment::claim_queue`).
    bool claimed_ = false;
    // EVENTS and RESULT: a record was delivered and not yet acknowledged;
    // its position.
    bool pending_ = false;
    std::uint64_t pending_position_ = 0;
    // CMD: the calling thread may post to the answer queue (`next`).
    bool answer_bound_ = false;
    WaitSet* wait_set_ = nullptr;
};

}  // namespace scada::tag

#endif  // SCADA_TAG_QUEUE_H
