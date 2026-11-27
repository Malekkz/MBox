// Fuzz target for the text pattern of identity checks (configuration schemas
// contract 2.5, 4.8; decision D-147): the text comes from the device. The
// input is the pattern's length in two bytes (little-endian), the pattern,
// and the text, so that either may hold a NUL. The match must agree with
// the plain definition of the pattern, computed by the textbook table, and
// take at most (n + 1)(m + 1) byte comparisons (PR 171 S6).

#include "scada/driver/text_pattern.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// The longest pattern or text the table is built for: a device's STRING is
// at most 250 bytes (TD 7), and the table grows with both.
constexpr std::size_t MAX_BYTES = 512;

bool reference(std::string_view pattern, std::string_view text) {
    struct Element {
        bool star = false;
        bool any = false;
        char literal = 0;
    };
    std::vector<Element> elements;
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        if (pattern[index] == '\\' && index + 1 < pattern.size()) {
            elements.push_back(Element{.literal = pattern[++index]});
        } else if (pattern[index] == '*') {
            elements.push_back(Element{.star = true});
        } else if (pattern[index] == '?') {
            elements.push_back(Element{.any = true});
        } else {
            elements.push_back(Element{.literal = pattern[index]});
        }
    }
    std::vector<bool> matched(text.size() + 1, false);
    matched[0] = true;
    for (const Element& element : elements) {
        std::vector<bool> next(text.size() + 1, false);
        for (std::size_t length = 0; length <= text.size(); ++length) {
            if (element.star) {
                next[length] =
                    matched[length] || (length > 0 && next[length - 1]);
            } else if (length > 0 && matched[length - 1]) {
                next[length] =
                    element.any || text[length - 1] == element.literal;
            }
        }
        matched = std::move(next);
    }
    return matched[text.size()];
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    std::string input;
    input.reserve(size);
    for (const std::byte byte : std::as_bytes(std::span(data, size))) {
        input.push_back(static_cast<char>(byte));
    }
    constexpr std::size_t PREFIX = 2;
    if (input.size() < PREFIX) {
        return 0;
    }
    const std::size_t pattern_size =
        static_cast<std::size_t>(static_cast<unsigned char>(input[0])) |
        (static_cast<std::size_t>(static_cast<unsigned char>(input[1])) << 8U);
    const std::string_view body = std::string_view(input).substr(PREFIX);
    if (pattern_size > body.size()) {
        return 0;
    }
    const std::string_view pattern = body.substr(0, pattern_size);
    const std::string_view text = body.substr(pattern_size);
    if (pattern.size() > MAX_BYTES || text.size() > MAX_BYTES) {
        return 0;
    }
    std::size_t comparisons = 0;
    const bool matched =
        scada::driver::text_matches_counted(pattern, text, comparisons);
    if (matched != reference(pattern, text) ||
        matched != scada::driver::text_matches(pattern, text) ||
        comparisons > (text.size() + 1) * (pattern.size() + 1)) {
        std::abort();
    }
    return 0;
}
