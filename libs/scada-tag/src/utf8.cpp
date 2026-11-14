#include "utf8.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace scada::tag {

namespace {

constexpr std::uint8_t CONTINUATION_MASK = 0xC0;
constexpr std::uint8_t CONTINUATION_BITS = 0x80;

}  // namespace

bool is_utf8(std::string_view text) noexcept {
    // The first byte of a sequence gives its length and, for three and
    // four bytes, the range of the second byte (Unicode, table 3-7).
    struct Lead {
        std::uint8_t first;
        std::uint8_t last;
        std::size_t length;
        std::uint8_t second_first;
        std::uint8_t second_last;
    };
    // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): the byte ranges of well-formed UTF-8
    constexpr std::array<Lead, 9> LEADS{{
        {0x00, 0x7F, 1, 0x80, 0xBF},
        {0xC2, 0xDF, 2, 0x80, 0xBF},
        {0xE0, 0xE0, 3, 0xA0, 0xBF},
        {0xE1, 0xEC, 3, 0x80, 0xBF},
        {0xED, 0xED, 3, 0x80, 0x9F},
        {0xEE, 0xEF, 3, 0x80, 0xBF},
        {0xF0, 0xF0, 4, 0x90, 0xBF},
        {0xF1, 0xF3, 4, 0x80, 0xBF},
        {0xF4, 0xF4, 4, 0x80, 0x8F},
    }};
    // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    std::size_t position = 0;
    while (position < text.size()) {
        const auto byte = [&](std::size_t index) {
            return static_cast<std::uint8_t>(text[index]);
        };
        const std::uint8_t lead = byte(position);
        const auto* const found =
            std::ranges::find_if(LEADS, [lead](const Lead& range) {
                return lead >= range.first && lead <= range.last;
            });
        if (found == LEADS.end() || found->length > text.size() - position) {
            return false;
        }
        for (std::size_t i = 1; i < found->length; ++i) {
            const std::uint8_t next = byte(position + i);
            const bool valid =
                i == 1
                    ? next >= found->second_first && next <= found->second_last
                    : (next & CONTINUATION_MASK) == CONTINUATION_BITS;
            if (!valid) {
                return false;
            }
        }
        position += found->length;
    }
    return true;
}

}  // namespace scada::tag
