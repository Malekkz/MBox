#include "name_pattern.h"

#include <cstddef>
#include <string_view>

namespace scada::conf {

bool matches_name_pattern(std::string_view pattern, std::string_view name) {
    std::size_t cursor = 0;
    std::size_t pos = 0;
    std::size_t star = std::string_view::npos;
    std::size_t resume = 0;
    while (pos < name.size()) {
        if (cursor < pattern.size() &&
            (pattern[cursor] == name[pos] ||
             (pattern[cursor] == '?' && name[pos] != '.'))) {
            ++cursor;
            ++pos;
        } else if (cursor < pattern.size() && pattern[cursor] == '*') {
            star = cursor++;
            resume = pos;
        } else if (star != std::string_view::npos && name[resume] != '.') {
            cursor = star + 1;
            pos = ++resume;
        } else {
            return false;
        }
    }
    while (cursor < pattern.size() && pattern[cursor] == '*') {
        ++cursor;
    }
    return cursor == pattern.size();
}

bool is_name_pattern(std::string_view text) noexcept {
    return text.find_first_of("*?") != std::string_view::npos;
}

}  // namespace scada::conf
