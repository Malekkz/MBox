#include "utf8.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace scada::conf::utf8 {

namespace {

constexpr unsigned char CONTINUATION_LOW = 0x80;
constexpr unsigned char CONTINUATION_HIGH = 0xBF;
constexpr unsigned char LEAD_TWO = 0xC0;
constexpr unsigned char LEAD_TWO_FIRST_VALID = 0xC2;
constexpr unsigned char LEAD_TWO_LAST = 0xDF;
constexpr unsigned char LEAD_THREE = 0xE0;
constexpr unsigned char LEAD_THREE_LAST = 0xEF;
constexpr unsigned char LEAD_THREE_SURROGATES = 0xED;
constexpr unsigned char LEAD_FOUR = 0xF0;
constexpr unsigned char LEAD_FOUR_LAST_VALID = 0xF4;
// The second byte's range where it rules out overlong forms, surrogates and
// code points above U+10FFFF.
constexpr unsigned char AFTER_E0_LOW = 0xA0;
constexpr unsigned char AFTER_ED_HIGH = 0x9F;
constexpr unsigned char AFTER_F0_LOW = 0x90;
constexpr unsigned char AFTER_F4_HIGH = 0x8F;
constexpr std::uint32_t TWO_BYTE_BITS = 0x1F;
constexpr std::uint32_t THREE_BYTE_BITS = 0x0F;
constexpr std::uint32_t FOUR_BYTE_BITS = 0x07;
constexpr std::uint32_t CONTINUATION_BITS = 0x3F;
constexpr unsigned BITS_PER_CONTINUATION = 6;
constexpr std::uint32_t LIMIT_ONE_BYTE = 0x80;
constexpr std::uint32_t LIMIT_TWO_BYTES = 0x800;
constexpr std::uint32_t LIMIT_THREE_BYTES = 0x10000;
constexpr std::size_t TWO = 2;
constexpr std::size_t THREE = 3;
constexpr std::size_t FOUR = 4;

void append_byte(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(static_cast<unsigned char>(value)));
}

std::uint32_t continuation_byte(std::uint32_t code_point, unsigned shift) {
    return CONTINUATION | ((code_point >> shift) & CONTINUATION_BITS);
}

// How a UTF-8 sequence starts: its length, the bits of its lead byte, and
// the allowed range of its second byte.
struct Lead {
    std::size_t length = 0;
    std::uint32_t bits = 0;
    unsigned char low = CONTINUATION_LOW;
    unsigned char high = CONTINUATION_HIGH;
};

std::optional<Lead> lead_of(unsigned char byte) noexcept {
    if (byte >= LEAD_TWO_FIRST_VALID && byte <= LEAD_TWO_LAST) {
        return Lead{.length = TWO,
                    .bits = byte & TWO_BYTE_BITS,
                    .low = CONTINUATION_LOW,
                    .high = CONTINUATION_HIGH};
    }
    if (byte >= LEAD_THREE && byte <= LEAD_THREE_LAST) {
        return Lead{.length = THREE,
                    .bits = byte & THREE_BYTE_BITS,
                    .low = byte == LEAD_THREE ? AFTER_E0_LOW : CONTINUATION_LOW,
                    .high = byte == LEAD_THREE_SURROGATES ? AFTER_ED_HIGH
                                                          : CONTINUATION_HIGH};
    }
    if (byte >= LEAD_FOUR && byte <= LEAD_FOUR_LAST_VALID) {
        return Lead{.length = FOUR,
                    .bits = byte & FOUR_BYTE_BITS,
                    .low = byte == LEAD_FOUR ? AFTER_F0_LOW : CONTINUATION_LOW,
                    .high = byte == LEAD_FOUR_LAST_VALID ? AFTER_F4_HIGH
                                                         : CONTINUATION_HIGH};
    }
    return std::nullopt;
}

}  // namespace

void append(std::string& out, std::uint32_t code_point) {
    constexpr unsigned SHIFT_1 = BITS_PER_CONTINUATION;
    constexpr unsigned SHIFT_2 = 2 * BITS_PER_CONTINUATION;
    constexpr unsigned SHIFT_3 = 3 * BITS_PER_CONTINUATION;
    if (code_point < LIMIT_ONE_BYTE) {
        append_byte(out, code_point);
    } else if (code_point < LIMIT_TWO_BYTES) {
        append_byte(out, LEAD_TWO | (code_point >> SHIFT_1));
        append_byte(out, continuation_byte(code_point, 0));
    } else if (code_point < LIMIT_THREE_BYTES) {
        append_byte(out, LEAD_THREE | (code_point >> SHIFT_2));
        append_byte(out, continuation_byte(code_point, SHIFT_1));
        append_byte(out, continuation_byte(code_point, 0));
    } else {
        append_byte(out, LEAD_FOUR | (code_point >> SHIFT_3));
        append_byte(out, continuation_byte(code_point, SHIFT_2));
        append_byte(out, continuation_byte(code_point, SHIFT_1));
        append_byte(out, continuation_byte(code_point, 0));
    }
}

std::optional<std::pair<std::size_t, std::uint32_t>> decode(
    std::string_view text, std::size_t where) noexcept {
    if (where >= text.size()) {
        return std::nullopt;
    }
    const std::optional<Lead> lead =
        lead_of(static_cast<unsigned char>(text[where]));
    if (!lead || text.size() - where < lead->length) {
        return std::nullopt;
    }
    std::uint32_t code_point = lead->bits;
    for (std::size_t i = 1; i < lead->length; ++i) {
        const auto next = static_cast<unsigned char>(text[where + i]);
        const unsigned char min = i == 1 ? lead->low : CONTINUATION_LOW;
        const unsigned char max = i == 1 ? lead->high : CONTINUATION_HIGH;
        if (next < min || next > max) {
            return std::nullopt;
        }
        code_point =
            (code_point << BITS_PER_CONTINUATION) | (next & CONTINUATION_BITS);
    }
    return std::make_pair(lead->length, code_point);
}

std::size_t sequence_length(unsigned char byte) noexcept {
    if (byte >= LEAD_FOUR) {
        return FOUR;
    }
    if (byte >= LEAD_THREE) {
        return THREE;
    }
    if (byte >= LEAD_TWO) {
        return TWO;
    }
    return 1;
}

}  // namespace scada::conf::utf8
