#include "scada/ctl/pattern.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace scada::ctl {

namespace {

bool is_name_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == '-';
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
    for (const char c : text) {
        if (!is_name_char(c) && c != '*' && c != '?') {
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
    std::size_t p = 0;
    std::size_t n = 0;
    std::optional<std::size_t> star;
    std::size_t star_n = 0;
    while (n < name.size()) {
        if (p < pattern.size() &&
            (pattern[p] == '?' || pattern[p] == name[n])) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p;
            star_n = n;
            ++p;
        } else if (star) {
            p = *star + 1;
            ++star_n;
            n = star_n;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

}  // namespace scada::ctl
