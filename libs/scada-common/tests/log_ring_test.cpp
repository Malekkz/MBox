// The ring of a LogQueue (log_internal::RecordRing; D-148, PR 182 M3): one
// producer and one consumer at once, with no lock between them, so that
// only the ring's own release and acquire order the slots. Every field of
// record i is derived from i, so a torn or stale slot shows; the thread
// sanitizer reports the race if the order is weakened.

#include "log_internal.h"
#include "scada/common/log.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace scada::log_internal {
namespace {

constexpr std::array<LogLevel, 6> LEVELS{LogLevel::CRITICAL, LogLevel::ERROR,
                                         LogLevel::WARNING,  LogLevel::NOTICE,
                                         LogLevel::INFO,     LogLevel::DEBUG};

// Record i: message "record <i>", time i, a level by i, fields N=<i> and
// HALF=<i/2>, and a third field only for odd i.
bool push_record(RecordRing& ring, std::uint64_t index) {
    const std::string message = "record " + std::to_string(index);
    const std::string number = std::to_string(index);
    const std::string half = std::to_string(index / 2);
    const std::array<LogField, 3> fields{
        LogField{"N", number}, LogField{"HALF", half}, LogField{"ODD", "yes"}};
    return ring.push(
        LEVELS.at(index % LEVELS.size()), static_cast<std::int64_t>(index),
        message,
        std::span<const LogField>(fields).first(index % 2 == 0 ? 2 : 3));
}

// Whether `slot` is record `index` in every field.
::testing::AssertionResult is_record(const Slot& slot, std::uint64_t index) {
    std::array<LogField, LOG_MAX_FIELDS> fields{};
    std::size_t count = 0;
    const std::string_view message = read_slot(slot, fields, count);
    const bool odd = index % 2 != 0;
    if (message != "record " + std::to_string(index) ||
        slot.time_ns != static_cast<std::int64_t>(index) ||
        slot.level != LEVELS.at(index % LEVELS.size()) ||
        count != (odd ? 3U : 2U) || fields[0].key != "N" ||
        fields[0].value != std::to_string(index) || fields[1].key != "HALF" ||
        fields[1].value != std::to_string(index / 2) ||
        (odd && (fields[2].key != "ODD" || fields[2].value != "yes"))) {
        return ::testing::AssertionFailure()
               << "record " << index << " read as '" << message << "' at "
               << slot.time_ns;
    }
    return ::testing::AssertionSuccess();
}

// The producer pushes RECORDS in order, retrying when full; the consumer
// pops at once, with no lock and no wait between the two.
::testing::AssertionResult overlap(std::size_t capacity,
                                   std::uint64_t records) {
    RecordRing ring(capacity);
    std::atomic<bool> failed{false};
    std::thread producer([&] {
        for (std::uint64_t index = 0; index < records; ++index) {
            while (!push_record(ring, index)) {
                if (failed.load(std::memory_order_relaxed)) {
                    return;
                }
                std::this_thread::yield();
            }
        }
    });
    Slot slot;
    std::uint64_t next = 0;
    ::testing::AssertionResult result = ::testing::AssertionSuccess();
    while (next < records) {
        if (!ring.pop(slot)) {
            std::this_thread::yield();
            continue;
        }
        result = is_record(slot, next);
        if (!result) {
            failed.store(true, std::memory_order_relaxed);
            break;
        }
        ++next;
    }
    producer.join();
    if (result && !ring.empty()) {
        return ::testing::AssertionFailure() << "records left over";
    }
    return result << " (capacity " << capacity << ")";
}

TEST(RecordRing, AProducerAndAConsumerOverlapWithNoLock) {
    constexpr std::uint64_t RECORDS = 20'000;
    for (const std::size_t capacity : {1U, 2U, 3U, 7U, 64U, 256U}) {
        EXPECT_TRUE(overlap(capacity, RECORDS));
    }
}

TEST(RecordRing, AFullRingRefusesAndAnEmptyOneGivesNothing) {
    RecordRing ring(2);
    Slot slot;
    EXPECT_TRUE(ring.empty());
    EXPECT_FALSE(ring.pop(slot));
    EXPECT_TRUE(push_record(ring, 0));
    EXPECT_FALSE(ring.full());
    EXPECT_TRUE(push_record(ring, 1));
    EXPECT_TRUE(ring.full());
    EXPECT_FALSE(push_record(ring, 2));
    EXPECT_EQ(ring.capacity(), 2U);
    ASSERT_TRUE(ring.pop(slot));
    EXPECT_TRUE(is_record(slot, 0));
    EXPECT_FALSE(ring.full());
    EXPECT_TRUE(push_record(ring, 2));
    ASSERT_TRUE(ring.pop(slot));
    EXPECT_TRUE(is_record(slot, 1));
    ASSERT_TRUE(ring.pop(slot));
    EXPECT_TRUE(is_record(slot, 2));
    EXPECT_TRUE(ring.empty());
}

}  // namespace
}  // namespace scada::log_internal
