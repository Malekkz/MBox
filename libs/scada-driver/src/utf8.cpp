#include "utf8.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::driver::detail {

namespace {

constexpr std::uint8_t CONTINUATION_MASK = 0xC0U;
constexpr std::uint8_t CONTINUATION = 0x80U;
constexpr std::uint8_t CONTINUATION_MAX = 0xBFU;

std::uint8_t byte_at(std::span<const std::byte> text, std::size_t index) {
    return static_cast<std::uint8_t>(text[index]);
}

// The length of the character that starts with `lead`, and the range its
// second byte must be in (RFC 3629, section 4); 0 for a byte that starts
// none.
struct Lead {
    std::size_t length = 0;
    std::uint8_t second_min = CONTINUATION;
    std::uint8_t second_max = CONTINUATION_MAX;
};

Lead classify(std::uint8_t lead) noexcept {
    constexpr std::uint8_t ASCII_END = 0x80U;
    constexpr std::uint8_t TWO_FIRST = 0xC2U;
    constexpr std::uint8_t THREE_FIRST = 0xE0U;
    constexpr std::uint8_t THREE_SURROGATES = 0xEDU;
    constexpr std::uint8_t FOUR_FIRST = 0xF0U;
    constexpr std::uint8_t FOUR_LAST = 0xF4U;
    constexpr std::uint8_t THREE_MIN_SECOND = 0xA0U;
    constexpr std::uint8_t SURROGATE_MAX_SECOND = 0x9FU;
    constexpr std::uint8_t FOUR_MIN_SECOND = 0x90U;
    constexpr std::uint8_t FOUR_LAST_MAX_SECOND = 0x8FU;
    constexpr std::size_t ONE = 1;
    constexpr std::size_t TWO = 2;
    constexpr std::size_t THREE = 3;
    constexpr std::size_t FOUR = 4;
    if (lead < ASCII_END) {
        return Lead{.length = ONE};
    }
    if (lead < TWO_FIRST) {
        return Lead{};
    }
    if (lead < THREE_FIRST) {
        return Lead{.length = TWO};
    }
    if (lead == THREE_FIRST) {
        return Lead{.length = THREE, .second_min = THREE_MIN_SECOND};
    }
    if (lead == THREE_SURROGATES) {
        return Lead{.length = THREE, .second_max = SURROGATE_MAX_SECOND};
    }
    if (lead < FOUR_FIRST) {
        return Lead{.length = THREE};
    }
    if (lead == FOUR_FIRST) {
        return Lead{.length = FOUR, .second_min = FOUR_MIN_SECOND};
    }
    if (lead < FOUR_LAST) {
        return Lead{.length = FOUR};
    }
    if (lead == FOUR_LAST) {
        return Lead{.length = FOUR, .second_max = FOUR_LAST_MAX_SECOND};
    }
    return Lead{};
}

}  // namespace

bool is_utf8(std::span<const std::byte> text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const Lead lead = classify(byte_at(text, index));
        if (lead.length == 0 || lead.length > text.size() - index) {
            return false;
        }
        if (lead.length > 1) {
            const std::uint8_t second = byte_at(text, index + 1);
            if (second < lead.second_min || second > lead.second_max) {
                return false;
            }
            for (std::size_t next = 2; next < lead.length; ++next) {
                if ((byte_at(text, index + next) & CONTINUATION_MASK) !=
                    CONTINUATION) {
                    return false;
                }
            }
        }
        index += lead.length;
    }
    return true;
}

std::size_t utf8_prefix(std::span<const std::byte> text,
                        std::size_t limit) noexcept {
    if (text.size() <= limit) {
        return text.size();
    }
    std::size_t end = limit;
    // Step back over continuation bytes to the start of the character that
    // the limit cuts; that start is the boundary.
    while (end > 0 &&
           (byte_at(text, end) & CONTINUATION_MASK) == CONTINUATION) {
        --end;
    }
    return end;
}

}  // namespace scada::driver::detail
