// Deadlines of an exchange (driver interface contract 7.1, 7.2), against
// the contract's own example: a read of 125 registers at 1,200 bit/s in 8E1
// is 8 bytes out (73 ms) and 255 back (2.34 s).

#include "scada/driver/timing.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace scada::driver {
namespace {

constexpr std::int64_t MS = 1'000'000;
constexpr std::uint32_t BAUD_1200 = 1200;
constexpr std::uint32_t BAUD_19200 = 19200;
constexpr std::uint32_t BAUD_115200 = 115200;
constexpr std::uint8_t DATA_BITS = 8;
constexpr std::size_t REQUEST_BYTES = 8;
constexpr std::size_t ANSWER_BYTES = 255;
constexpr std::int64_t TIMEOUT = 300 * MS;

constexpr LineTiming LINE_1200_8E1{
    .bit_rate = BAUD_1200, .bits_per_char = bits_per_char(DATA_BITS, true, 1)};

TEST(Timing, CharacterBits) {
    EXPECT_EQ(bits_per_char(DATA_BITS, true, 1), 11);   // 8E1
    EXPECT_EQ(bits_per_char(DATA_BITS, false, 1), 10);  // 8N1
    EXPECT_EQ(bits_per_char(DATA_BITS, false, 2), 11);  // 8N2
}

TEST(Timing, TheContractsExample) {
    // 8 x 11 bits at 1,200 bit/s = 73.33 ms; 255 x 11 = 2.3375 s.
    EXPECT_EQ(frame_time_ns(LINE_1200_8E1, REQUEST_BYTES), 73'333'334);
    EXPECT_EQ(frame_time_ns(LINE_1200_8E1, ANSWER_BYTES), 2'337'500'000);
    // A silent device times out 73 ms plus the timeout after the send.
    EXPECT_EQ(first_byte_deadline_ns(LINE_1200_8E1, 0, REQUEST_BYTES, TIMEOUT),
              73'333'334 + TIMEOUT);
    // One that answers gets 2.34 s plus the margin after its first byte;
    // at 1,200 bit/s 3.5 characters (32.08 ms) beat the 20 ms floor.
    EXPECT_EQ(margin_ns(LINE_1200_8E1), 32'083'334);
    EXPECT_EQ(answer_deadline_ns(LINE_1200_8E1, 0, ANSWER_BYTES),
              2'337'500'000 + 32'083'334);
}

TEST(Timing, TheMarginIsAtLeastTwentyMilliseconds) {
    // 3.5 x 11 bits at 19,200 bit/s is 2 ms; at 115,200 it is 0.33 ms.
    const LineTiming fast{.bit_rate = BAUD_19200,
                          .bits_per_char = bits_per_char(DATA_BITS, true, 1)};
    EXPECT_EQ(margin_ns(fast), 20 * MS);
    const LineTiming faster{.bit_rate = BAUD_115200, .bits_per_char = 10};
    EXPECT_EQ(margin_ns(faster), 20 * MS);
}

TEST(Timing, AStreamHasNoLineTimeButKeepsTheMargin) {
    const LineTiming tcp{};
    EXPECT_EQ(frame_time_ns(tcp, ANSWER_BYTES), 0);
    EXPECT_EQ(first_byte_deadline_ns(tcp, 5 * MS, REQUEST_BYTES, TIMEOUT),
              5 * MS + TIMEOUT);
    EXPECT_EQ(answer_deadline_ns(tcp, 7 * MS, ANSWER_BYTES), 7 * MS + 20 * MS);
}

TEST(Timing, DeadlinesSaturate) {
    constexpr std::int64_t MAX = std::numeric_limits<std::int64_t>::max();
    EXPECT_EQ(first_byte_deadline_ns(LINE_1200_8E1, MAX - 1, REQUEST_BYTES,
                                     TIMEOUT),
              MAX);
    EXPECT_EQ(answer_deadline_ns(LINE_1200_8E1, MAX, ANSWER_BYTES), MAX);
    EXPECT_EQ(frame_time_ns(LINE_1200_8E1,
                            std::numeric_limits<std::size_t>::max()),
              MAX);
}

}  // namespace
}  // namespace scada::driver
