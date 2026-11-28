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

}  // namespace

void EventSlot::add(DeviceEvent kind, const EventRecord& record) noexcept {
    Kind& target = kinds_.at(static_cast<std::size_t>(kind));
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
            (static_cast<std::uint64_t>(length) << LENGTH_SHIFT),
        std::memory_order_release);
    for (std::size_t word = 0; word < TEXT_WORDS; ++word) {
        std::array<char, sizeof(std::uint64_t)> bytes{};
        const std::size_t from = word * bytes.size();
        if (from < length) {
            std::copy_n(record.text.begin() + static_cast<std::ptrdiff_t>(from),
                        std::min(bytes.size(), length - from), bytes.begin());
        }
        target.words.at(2 + word).store(std::bit_cast<std::uint64_t>(bytes),
                                        std::memory_order_release);
    }
    target.sequence.store(sequence + 2, std::memory_order_release);
    target.count.fetch_add(record.count, std::memory_order_release);
}

bool EventSlot::take(DeviceEvent kind, EventRecord& out) noexcept {
    Kind& source = kinds_.at(static_cast<std::size_t>(kind));
    out = EventRecord{};
    out.count = source.count.exchange(0, std::memory_order_acquire);
    if (out.count == 0) {
        return false;
    }
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
            words.at(word) =
                source.words.at(word).load(std::memory_order_acquire);
        }
        if (source.sequence.load(std::memory_order_relaxed) != before) {
            continue;  // written meanwhile
        }
        out.time_ns = std::bit_cast<std::int64_t>(words[0]);
        out.reason = static_cast<std::uint8_t>(words[1] & BYTE);
        out.check =
            static_cast<std::uint32_t>((words[1] >> CHECK_SHIFT) & CHECK_MASK);
        out.length =
            std::min(static_cast<std::size_t>(words[1] >> LENGTH_SHIFT),
                     out.text.size());
        for (std::size_t word = 0; word < TEXT_WORDS; ++word) {
            const auto bytes =
                std::bit_cast<std::array<char, sizeof(std::uint64_t)>>(
                    words.at(2 + word));
            const std::size_t from = word * bytes.size();
            if (from < out.length) {
                std::copy_n(
                    bytes.begin(), std::min(bytes.size(), out.length - from),
                    out.text.begin() + static_cast<std::ptrdiff_t>(from));
            }
        }
        return true;
    }
    return true;  // the count, without details
}

}  // namespace scada::driver::detail
