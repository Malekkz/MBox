// Fuzz target for the check of text that goes into a string slot. The text
// of a string tag comes from outside the process that writes it, for
// example from a device. The result of the check is compared with a
// decoder written another way: one that computes every code point and
// judges its value.

#include "utf8.h"

#include "scada/common/panic.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace {

constexpr std::uint32_t LARGEST_CODE_POINT = 0x10FFFF;
constexpr std::uint32_t FIRST_SURROGATE = 0xD800;
constexpr std::uint32_t LAST_SURROGATE = 0xDFFF;
constexpr std::uint32_t CONTINUATION_PAYLOAD_BITS = 6;

// How a sequence starts: the bits of its first byte that tell its length,
// the bits of it that belong to the code point, and the smallest code
// point that needs a sequence of that length.
struct Start {
    std::uint8_t mask;
    std::uint8_t pattern;
    std::size_t length;
    std::uint32_t smallest;
};
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): the bit patterns of UTF-8
constexpr std::array<Start, 4> STARTS{{
    {0x80, 0x00, 1, 0x0},
    {0xE0, 0xC0, 2, 0x80},
    {0xF0, 0xE0, 3, 0x800},
    {0xF8, 0xF0, 4, 0x10000},
}};
constexpr std::uint8_t CONTINUATION_MASK = 0xC0;
constexpr std::uint8_t CONTINUATION_PATTERN = 0x80;
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

// The length of the well-formed sequence at the start of `bytes`, or 0 if
// there is none.
std::size_t sequence_length(std::span<const std::uint8_t> bytes) {
    for (const Start& start : STARTS) {
        if ((bytes.front() & start.mask) != start.pattern) {
            continue;
        }
        if (bytes.size() < start.length) {
            return 0;
        }
        // The payload of the first byte, then 6 bits of each further byte.
        std::uint32_t code_point =
            bytes.front() & static_cast<std::uint8_t>(~start.mask);
        for (const std::uint8_t byte : bytes.subspan(1, start.length - 1)) {
            if ((byte & CONTINUATION_MASK) != CONTINUATION_PATTERN) {
                return 0;
            }
            code_point = (code_point << CONTINUATION_PAYLOAD_BITS) |
                         (byte & static_cast<std::uint8_t>(~CONTINUATION_MASK));
        }
        const bool valid =
            code_point >= start.smallest && code_point <= LARGEST_CODE_POINT &&
            (code_point < FIRST_SURROGATE || code_point > LAST_SURROGATE);
        return valid ? start.length : 0;
    }
    return 0;
}

// True if `bytes` are well-formed UTF-8, by decoding.
bool decodes(std::span<const std::uint8_t> bytes) {
    while (!bytes.empty()) {
        const std::size_t length = sequence_length(bytes);
        if (length == 0) {
            return false;
        }
        bytes = bytes.subspan(length);
    }
    return true;
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::span<const std::uint8_t> input(data, size);
    // libFuzzer hands over bytes; the check takes the same bytes as text.
    std::string text;
    text.reserve(input.size());
    for (const std::uint8_t byte : input) {
        text.push_back(static_cast<char>(byte));
    }
    if (scada::tag::is_utf8(text) != decodes(input)) {
        scada::panic("the two judgements of a text differ");
    }
    return 0;
}
