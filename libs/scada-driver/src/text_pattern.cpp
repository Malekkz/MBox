#include "scada/driver/text_pattern.h"

#include <cstddef>
#include <string_view>

namespace scada::driver {
namespace {

// One part of the pattern between two unescaped stars: `begin` to `end` in
// the pattern, escapes still in it.
struct Part {
    std::size_t begin = 0;
    std::size_t end = 0;
};

// The part that starts at `from`: up to the next unescaped `*` or the end.
Part part_at(std::string_view pattern, std::size_t from) noexcept {
    std::size_t index = from;
    while (index < pattern.size() && pattern[index] != '*') {
        // An escape takes the next character with it, a star too; one at
        // the end stands for itself.
        const bool escape =
            pattern[index] == '\\' && index + 1 < pattern.size();
        index += escape ? std::size_t{2} : std::size_t{1};
    }
    return Part{.begin = from, .end = index};
}

// True when `part` matches `text` from `start` on; `used` is then the number of
// text bytes it took. A part has no star, so it takes exactly one byte per
// element (a literal, an escaped character or `?`).
bool fits(std::string_view pattern, Part part, std::string_view text,
          std::size_t start, std::size_t& used) noexcept {
    std::size_t position = start;
    for (std::size_t index = part.begin; index < part.end; ++index) {
        if (position >= text.size()) {
            return false;
        }
        char wanted = pattern[index];
        bool any = false;
        if (wanted == '\\' && index + 1 < part.end) {
            wanted = pattern[++index];
        } else if (wanted == '?') {
            any = true;
        }
        if (!any && text[position] != wanted) {
            return false;
        }
        ++position;
    }
    used = position - start;
    return true;
}

// The text bytes `part` takes: one per element.
std::size_t length_of(std::string_view pattern, Part part) noexcept {
    std::size_t length = 0;
    for (std::size_t index = part.begin; index < part.end; ++index) {
        if (pattern[index] == '\\' && index + 1 < part.end) {
            ++index;
        }
        ++length;
    }
    return length;
}

}  // namespace

bool text_matches(std::string_view pattern, std::string_view text) noexcept {
    // The first part is anchored at the start of the text.
    Part part = part_at(pattern, 0);
    std::size_t used = 0;
    if (!fits(pattern, part, text, 0, used)) {
        return false;
    }
    if (part.end == pattern.size()) {
        return used == text.size();  // no star: the whole text, exactly
    }
    std::size_t position = used;
    std::size_t from = part.end + 1;  // past the star
    for (;;) {
        part = part_at(pattern, from);
        if (part.end == pattern.size()) {
            // The last part is anchored at the end of the text, and must not
            // overlap what the parts before took.
            const std::size_t length = length_of(pattern, part);
            if (length > text.size() - position) {
                return false;
            }
            return fits(pattern, part, text, text.size() - length, used);
        }
        // A middle part at its first fit: a later fit leaves less text for
        // the parts after it and gains nothing, since a star is between.
        bool placed = false;
        for (std::size_t start = position; start <= text.size(); ++start) {
            if (fits(pattern, part, text, start, used)) {
                position = start + used;
                placed = true;
                break;
            }
        }
        if (!placed) {
            return false;
        }
        from = part.end + 1;
    }
}

}  // namespace scada::driver
