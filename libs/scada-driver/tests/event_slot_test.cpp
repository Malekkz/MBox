// The event slot between a worker and the main thread (decision D-147):
// counts add up, the details are the last event's, and a take never reads
// a torn text while the worker writes.

#include "../src/event_slot.h"
#include "scada/driver/channel_engine.h"
#include "scada/driver/tag_value.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

namespace scada::driver {
namespace {

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers): the counts and texts are the test data

using detail::EventSlot;

EventRecord record_of(std::uint64_t count, std::string_view text,
                      std::uint32_t check = 0) {
    EventRecord record;
    record.count = count;
    record.time_ns = 1234;
    record.reason = 2;
    record.check = check;
    record.length = std::min(text.size(), record.text.size());
    std::copy_n(text.data(), record.length, record.text.data());
    return record;
}

std::string text_of(const EventRecord& record) {
    return {record.text.data(), record.length};
}

TEST(EventSlot, CountsAddUpAndTheDetailsAreTheLastOnes) {
    EventSlot slot;
    EventRecord out;
    EXPECT_FALSE(slot.take(DeviceEvent::MISMATCH, out));
    slot.add(DeviceEvent::MISMATCH, record_of(1, "first", 3));
    slot.add(DeviceEvent::MISMATCH, record_of(2, "second", 4));
    ASSERT_TRUE(slot.take(DeviceEvent::MISMATCH, out));
    EXPECT_EQ(out.count, 3U);
    EXPECT_EQ(text_of(out), "second");
    EXPECT_EQ(out.check, 4U);
    EXPECT_EQ(out.reason, 2U);
    EXPECT_EQ(out.time_ns, 1234);
    // Taken: none until the next.
    EXPECT_FALSE(slot.take(DeviceEvent::MISMATCH, out));
    // The kinds are apart.
    slot.add(DeviceEvent::RESTART, record_of(1, ""));
    EXPECT_FALSE(slot.take(DeviceEvent::MATCH, out));
    EXPECT_TRUE(slot.take(DeviceEvent::RESTART, out));
}

TEST(EventSlot, AFullTextGoesThroughWhole) {
    EventSlot slot;
    const std::string full(TAG_STRING_MAX_BYTES, 'x');
    slot.add(DeviceEvent::MATCH, record_of(1, full));
    EventRecord out;
    ASSERT_TRUE(slot.take(DeviceEvent::MATCH, out));
    EXPECT_EQ(text_of(out), full);
}

TEST(EventSlot, ATakeWhileTheWorkerWritesReadsNoTornText) {
    // The worker writes texts of one repeated letter; a take sees one
    // letter only, or no details, and every count arrives.
    EventSlot slot;
    std::atomic<bool> done{false};
    constexpr std::uint64_t ADDS = 20'000;
    std::thread worker([&] {
        for (std::uint64_t index = 0; index < ADDS; ++index) {
            const std::string text(1 + (index % 200),
                                   static_cast<char>('a' + (index % 26)));
            slot.add(DeviceEvent::MISMATCH, record_of(1, text));
        }
        done.store(true, std::memory_order_release);
    });
    std::uint64_t seen = 0;
    EventRecord out;
    bool torn = false;
    while (!done.load(std::memory_order_acquire) || seen < ADDS) {
        if (!slot.take(DeviceEvent::MISMATCH, out)) {
            if (done.load(std::memory_order_acquire) && seen >= ADDS) {
                break;
            }
            continue;
        }
        seen += out.count;
        const std::string text = text_of(out);
        torn = torn || std::any_of(text.begin(), text.end(), [&](char byte) {
                   return byte != text.front();
               });
    }
    worker.join();
    EXPECT_EQ(seen, ADDS);
    EXPECT_FALSE(torn);
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

}  // namespace
}  // namespace scada::driver
