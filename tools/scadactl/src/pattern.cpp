#include "scada/ctl/pattern.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace scada::ctl {

namespace {

bool is_name_char(char chr) noexcept {
    return (chr >= 'a' && chr <= 'z') || (chr >= '0' && chr <= '9') ||
           chr == '_' || chr == '.' || chr == '-';
}

}  // namespace

std::optional<TagPattern> TagPattern::parse(std::string_view text,
                                            std::string& detail) {
    if (text.empty()) {
        detail = "the pattern is empty";
        return std::nullopt;
    }
    if (text.size() > PATTERN_MAX) {
        detail = "the pattern is longer than 127 bytes";
        return std::nullopt;
    }
    for (const char chr : text) {
        if (!is_name_char(chr) && chr != '*' && chr != '?') {
            detail =
                "the pattern holds a character other than a-z, 0-9, '_', "
                "'.', '-', '*' and '?'";
            return std::nullopt;
        }
    }
    return TagPattern(std::string(text));
}

bool TagPattern::matches(std::string_view name) const noexcept {
    // The usual greedy glob match with one backtracking point: the last
    // `*` seen and the position in `name` it was tried at. Each step moves
    // forward in `name` or retries the last `*` one character further, so
    // the work is bounded by the product of the lengths at worst and is
    // linear in practice.
    const std::string_view pattern = text_;
    std::size_t pat = 0;
    std::size_t pos = 0;
    std::optional<std::size_t> star;
    std::size_t star_pos = 0;
    while (pos < name.size()) {
        if (pat < pattern.size() &&
            (pattern[pat] == '?' || pattern[pat] == name[pos])) {
            ++pat;
            ++pos;
        } else if (pat < pattern.size() && pattern[pat] == '*') {
            star = pat;
            star_pos = pos;
            ++pat;
        } else if (star) {
            pat = *star + 1;
            ++star_pos;
            pos = star_pos;
        } else {
            return false;
        }
    }
    while (pat < pattern.size() && pattern[pat] == '*') {
        ++pat;
    }
    return pat == pattern.size();
}

}  // namespace scada::ctl
