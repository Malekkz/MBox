#include "event_slot.h"

#include "scada/driver/channel_engine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace scada::driver::detail {

namespace {

// A take reads the details again at most this often while the worker
// writes them, then gives the count without them.
constexpr int TRIES = 4;
constexpr unsigned CHECK_SHIFT = 8;
constexpr unsigned LENGTH_SHIFT = 40;
constexpr std::uint64_t BYTE = 0xFF;
constexpr std::uint64_t CHECK_MASK = 0xFFFF'FFFF;
constexpr unsigned DETAILS_SHIFT = 63;  // has_details
constexpr std::uint64_t LENGTH_MASK = 0xFF;

}  // namespace

void EventSlot::add(DeviceEvent kind, const EventRecord& record) noexcept {
    Kind& target = kinds_[event_index(kind)];
    // The details first, under an odd sequence number; then the count, with
    // release, so that a take that sees the count sees these details. Each
    // word is stored with release, so that a take that reads it sees the
    // odd number before it (no fence: the thread sanitizer takes none).
    const std::uint64_t sequence =
        target.sequence.load(std::memory_order_relaxed);
    target.sequence.store(sequence + 1, std::memory_order_relaxed);
    target.words[0].store(std::bit_cast<std::uint64_t>(record.time_ns),
                          std::memory_order_release);
    const std::size_t length = std::min(record.length, record.text.size());
    target.words[1].store(
        static_cast<std::uint64_t>(record.reason) |
            (static_cast<std::uint64_t>(record.check) << CHECK_SHIFT) |
            (static_cast<std::uint64_t>(length) << LENGTH_SHIFT) |
            (static_cast<std::uint64_t>(record.has_details ? 1 : 0)
             << DETAILS_SHIFT),
        std::memory_order_release);
    for (std::size_t word = 0; word < TEXT_WORDS; ++word) {
        std::array<char, sizeof(std::uint64_t)> bytes{};
        const std::size_t from = word * bytes.size();
        if (from < length) {
            std::copy_n(record.text.begin() + static_cast<std::ptrdiff_t>(from),
                        std::min(bytes.size(), length - from), bytes.begin());
        }
        target.words[2 + word].store(std::bit_cast<std::uint64_t>(bytes),
                                     std::memory_order_release);
    }
    target.sequence.store(sequence + 2, std::memory_order_release);
    target.count.fetch_add(record.count, std::memory_order_release);
}

bool EventSlot::take(DeviceEvent kind, EventRecord& out) noexcept {
    const std::size_t index = event_index(kind);
    Kind& source = kinds_[index];
    EventRecord& last = last_[index];
    const std::uint64_t count =
        source.count.exchange(0, std::memory_order_acquire);
    if (count == 0) {
        return false;
    }
    // When the worker wrote the details through every read, `last` keeps
    // the earlier ones, so that a record never shows empty details in place
    // of known ones (PR 177 S4).
    (void)read_details(source, last);
    out = last;
    out.count = count;
    return true;
}

bool EventSlot::read_details(const Kind& source, EventRecord& into) noexcept {
    for (int attempt = 0; attempt < TRIES; ++attempt) {
        const std::uint64_t before =
            source.sequence.load(std::memory_order_acquire);
        if (before % 2 != 0) {
            continue;  // being written
        }
        std::array<std::uint64_t, WORDS> words{};
        // Acquire: a word of a later add brings its odd number with it, so
        // the number read after them differs.
        for (std::size_t word = 0; word < WORDS; ++word) {
            words[word] = source.words[word].load(std::memory_order_acquire);
        }
        if (source.sequence.load(std::memory_order_relaxed) != before) {
            continue;  // written meanwhile
        }
        into = EventRecord{};
        into.time_ns = std::bit_cast<std::int64_t>(words[0]);
        into.reason = static_cast<std::uint8_t>(words[1] & BYTE);
        into.check =
            static_cast<std::uint32_t>((words[1] >> CHECK_SHIFT) & CHECK_MASK);
        into.length = std::min(
            static_cast<std::size_t>((words[1] >> LENGTH_SHIFT) & LENGTH_MASK),
            into.text.size());
        into.has_details = ((words[1] >> DETAILS_SHIFT) & 1U) != 0;
        for (std::size_t word = 0; word < TEXT_WORDS; ++word) {
            const auto bytes =
                std::bit_cast<std::array<char, sizeof(std::uint64_t)>>(
                    words[2 + word]);
            const std::size_t from = word * bytes.size();
            if (from < into.length) {
                std::copy_n(
                    bytes.begin(), std::min(bytes.size(), into.length - from),
                    into.text.begin() + static_cast<std::ptrdiff_t>(from));
            }
        }
        return true;
    }
    return false;
}

}  // namespace scada::driver::detail
