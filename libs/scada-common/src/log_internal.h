#ifndef SCADA_COMMON_SRC_LOG_INTERNAL_H
#define SCADA_COMMON_SRC_LOG_INTERNAL_H

// Helpers shared by the logger and its sinks. Not part of the public
// interface; the unit tests include this header directly.

#include "scada/common/log.h"
#include "scada/common/realtime.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace scada::log_internal {

// Decimal digits of the largest 64-bit unsigned value.
inline constexpr std::size_t UINT64_DIGITS = 20;

// Returns the longest prefix of `text` that is at most `max_bytes` long and
// does not end inside a UTF-8 character. Text that is not valid UTF-8 is cut
// within 3 bytes of the limit.
[[nodiscard]] std::string_view truncate_utf8(std::string_view text,
                                             std::size_t max_bytes) noexcept;

// Parses an unsigned decimal number that fills all of `text`. Returns nothing
// for an empty string, any other character, or a value beyond 64 bits.
[[nodiscard]] std::optional<std::uint64_t> parse_decimal(
    std::string_view text) noexcept;

// Returns true if `journal_stream` (the value of the JOURNAL_STREAM
// environment variable, "<device>:<inode>" in decimal) names the file that
// `descriptor` refers to. Malformed text gives false.
[[nodiscard]] bool is_journal_stream(std::string_view journal_stream,
                                     int descriptor) noexcept;

// Appends text to caller-provided storage and never writes beyond it; what
// does not fit is discarded and remembered as truncation. The storage must
// outlive the BoundedText, which keeps a span of it.
class BoundedText {
public:
    explicit BoundedText(std::span<char> storage) noexcept
        : storage_(storage) {}

    void append(std::string_view text) noexcept;
    void append(char character) noexcept;

    // Appends `value` in decimal, padded with zeros to `min_width` digits.
    void append_number(std::uint64_t value, std::size_t min_width = 1) noexcept;

    // Appends `value` as 8 bytes, least significant first.
    void append_le64(std::uint64_t value) noexcept;

    // True if anything was discarded.
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }

    // The text appended so far; valid while the storage is.
    [[nodiscard]] std::string_view view() const noexcept {
        return {storage_.data(), size_};
    }

private:
    std::span<char> storage_;
    std::size_t size_ = 0;
    bool truncated_ = false;
};

// Where a field's key and value end in a record's text.
struct FieldSize {
    std::uint16_t key = 0;
    std::uint16_t value = 0;
};

inline constexpr std::size_t SLOT_TEXT_BYTES =
    LOG_MAX_MESSAGE_BYTES +
    (LOG_MAX_FIELDS * (LOG_MAX_KEY_BYTES + LOG_MAX_VALUE_BYTES));

// One queued record. `text` holds the message, then key and value of each
// field in turn, without separators; the sizes say where each part ends.
// Fixed size, so that queueing a record never allocates.
struct Slot {
    std::array<char, SLOT_TEXT_BYTES> text{};
    std::array<FieldSize, LOG_MAX_FIELDS> fields{};
    std::int64_t time_ns = 0;
    std::size_t message_size = 0;
    std::size_t field_count = 0;
    LogLevel level = LogLevel::INFO;
};

// Copies a record into `slot`, cut to the size limits, fields with an
// invalid key omitted. Allocates nothing, takes no lock, makes no system
// call.
SCADA_RT void fill_slot(Slot& slot, LogLevel level, std::int64_t time_ns,
                        std::string_view message,
                        std::span<const LogField> fields) noexcept;

// The message of `slot`, and its fields into `fields`; the views point into
// `slot`, valid while it is.
std::string_view read_slot(const Slot& slot,
                           std::array<LogField, LOG_MAX_FIELDS>& fields,
                           std::size_t& field_count) noexcept;

// A single-producer, single-consumer ring of records, the core of a
// LogQueue (D-148). `tail_` is written by the producer only and `head_` by
// the consumer only; each publishes the slots before it with release, and
// the other side reads it with acquire, so a slot is never read and written
// at once, with no lock between the two. The indexes count records since
// creation: at 2^64 records they would wrap, which no process lives to see.
class RecordRing {
public:
    // `capacity` records, at least 1. May throw std::bad_alloc.
    explicit RecordRing(std::size_t capacity) : slots_(capacity) {}

    // Producer: copies the record in and returns true, or false when full.
    SCADA_RT bool push(LogLevel level, std::int64_t time_ns,
                       std::string_view message,
                       std::span<const LogField> fields) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail - head == slots_.size()) {
            return false;
        }
        fill_slot(slots_[tail % slots_.size()], level, time_ns, message,
                  fields);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer: moves the oldest record into `out`; false when empty.
    bool pop(Slot& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head == tail) {
            return false;
        }
        out = slots_[head % slots_.size()];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Any thread: true if no record waits. Exact for the consumer; another
    // thread may see a record that is being taken.
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    // Producer: true if a push now would fail. The consumer only makes
    // room, so a false answer holds until the producer's next push.
    SCADA_RT [[nodiscard]] bool full() const noexcept {
        return tail_.load(std::memory_order_relaxed) -
                   head_.load(std::memory_order_acquire) ==
               slots_.size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return slots_.size();
    }

private:
    std::vector<Slot> slots_;  // fixed size; never grows
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
};

}  // namespace scada::log_internal

#endif  // SCADA_COMMON_SRC_LOG_INTERNAL_H
