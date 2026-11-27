// Fuzz target for the text pattern of identity checks (configuration schemas
// contract 2.5, 4.8; decision D-147): the text comes from the device. The
// input is a pattern, a NUL, and the text; the match must agree with the
// plain definition of the pattern, computed by the textbook table.

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
    const std::size_t split = input.find('\0');
    if (split == std::string::npos) {
        return 0;
    }
    const std::string_view pattern = std::string_view(input).substr(0, split);
    const std::string_view text = std::string_view(input).substr(split + 1);
    if (pattern.size() > MAX_BYTES || text.size() > MAX_BYTES) {
        return 0;
    }
    if (scada::driver::text_matches(pattern, text) !=
        reference(pattern, text)) {
        std::abort();
    }
    return 0;
}
