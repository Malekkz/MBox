#include "scada/driver/text_pattern.h"

#include <cstddef>
#include <string_view>

namespace scada::driver {

bool text_matches_counted(std::string_view pattern, std::string_view text,
                          std::size_t& comparisons) noexcept {
    comparisons = 0;
    constexpr auto NONE = static_cast<std::size_t>(-1);
    std::size_t at_pattern = 0;
    std::size_t at_text = 0;
    // The pattern position just past the last `*` met, and the text
    // position it was tried from: where to resume when a literal fails.
    std::size_t star = NONE;
    std::size_t star_text = 0;
    while (at_text < text.size()) {
        if (at_pattern < pattern.size() && pattern[at_pattern] == '*') {
            star = ++at_pattern;
            star_text = at_text;
            continue;
        }
        if (at_pattern < pattern.size()) {
            // One element: an escaped byte, `?`, or a literal byte.
            const bool escaped =
                pattern[at_pattern] == '\\' && at_pattern + 1 < pattern.size();
            const char wanted =
                escaped ? pattern[at_pattern + 1] : pattern[at_pattern];
            const bool any = !escaped && wanted == '?';
            ++comparisons;
            if (any || text[at_text] == wanted) {
                at_pattern += escaped ? std::size_t{2} : std::size_t{1};
                ++at_text;
                continue;
            }
        }
        if (star == NONE) {
            return false;
        }
        // The last star takes one byte more; the rest is tried again.
        at_pattern = star;
        at_text = ++star_text;
    }
    while (at_pattern < pattern.size() && pattern[at_pattern] == '*') {
        ++at_pattern;
    }
    return at_pattern == pattern.size();
}

bool text_matches(std::string_view pattern, std::string_view text) noexcept {
    std::size_t comparisons = 0;
    return text_matches_counted(pattern, text, comparisons);
}

}  // namespace scada::driver
