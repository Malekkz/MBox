#include "scada/conf/names.h"

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace scada::conf {

namespace {

constexpr std::size_t MAX_LABEL = 63;
constexpr std::size_t MAX_ZONE_COMPONENT = 14;
constexpr std::size_t IPV4_PARTS = 4;
constexpr unsigned IPV4_PART_MAX = 255;
constexpr std::size_t IPV6_GROUPS = 8;
constexpr std::size_t IPV6_GROUP_DIGITS = 4;
constexpr unsigned IPV4_PREFIX_MAX = 32;
constexpr unsigned IPV6_PREFIX_MAX = 128;
constexpr unsigned BASE_TEN = 10;
constexpr unsigned HOURS_PER_DAY = 24;
constexpr unsigned MINUTES_PER_HOUR = 60;
constexpr unsigned char FIRST_PRINTABLE = 0x20;
constexpr unsigned char ASCII_DEL = 0x7F;
constexpr unsigned char C1_LEAD = 0xC2;
constexpr unsigned char C1_SECOND_LOW = 0x80;
constexpr unsigned char C1_SECOND_HIGH = 0x9F;

bool is_lower(char chr) noexcept {
    return chr >= 'a' && chr <= 'z';
}
bool is_digit(char chr) noexcept {
    return chr >= '0' && chr <= '9';
}
bool is_letter(char chr) noexcept {
    return is_lower(chr) || (chr >= 'A' && chr <= 'Z');
}
bool is_hex(char chr) noexcept {
    return is_digit(chr) || (chr >= 'a' && chr <= 'f') ||
           (chr >= 'A' && chr <= 'F');
}

// Splits `text` at `separator` and calls `part` on each piece; false as
// soon as one is refused.
template <typename Part>
bool every_part(std::string_view text, char separator, Part part) {
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find(separator, start);
        const std::string_view piece = text.substr(
            start, end == std::string_view::npos ? std::string_view::npos
                                                 : end - start);
        if (!part(piece)) {
            return false;
        }
        if (end == std::string_view::npos) {
            return true;
        }
        start = end + 1;
    }
}

// A whole number of at most `max` without leading zeros.
bool small_number(std::string_view text, unsigned max) noexcept {
    if (text.empty() || text.size() > 3 ||
        (text.size() > 1 && text[0] == '0')) {
        return false;
    }
    unsigned value = 0;
    for (const char chr : text) {
        if (!is_digit(chr)) {
            return false;
        }
        value = (value * BASE_TEN) + static_cast<unsigned>(chr - '0');
    }
    return value <= max;
}

bool ipv6_group(std::string_view group) noexcept {
    return !group.empty() && group.size() <= IPV6_GROUP_DIGITS &&
           std::ranges::all_of(group, is_hex);
}

// The groups of one side of `::` (or of a whole address without it); an
// IPv4 tail counts as two groups and may only be last.
bool ipv6_groups(std::string_view side, bool may_end_in_ipv4,
                 std::size_t& count) {
    count = 0;
    if (side.empty()) {
        return true;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t end = side.find(':', start);
        const std::string_view group = side.substr(
            start, end == std::string_view::npos ? std::string_view::npos
                                                 : end - start);
        if (end == std::string_view::npos && may_end_in_ipv4 &&
            group.find('.') != std::string_view::npos) {
            if (!is_ipv4_address(group)) {
                return false;
            }
            count += 2;
            return true;
        }
        if (!ipv6_group(group)) {
            return false;
        }
        ++count;
        if (end == std::string_view::npos) {
            return true;
        }
        start = end + 1;
    }
}

}  // namespace

bool is_identifier(std::string_view text) noexcept {
    return !text.empty() && text.size() <= MAX_IDENTIFIER &&
           is_lower(text.front()) && std::ranges::all_of(text, [](char chr) {
               return is_lower(chr) || is_digit(chr) || chr == '_';
           });
}

bool is_tag_name(std::string_view text, std::size_t max_length) noexcept {
    if (text.empty() || text.size() > max_length) {
        return false;
    }
    return every_part(text, '.', [](std::string_view segment) {
        return !segment.empty() &&
               (is_lower(segment.front()) || is_digit(segment.front())) &&
               std::ranges::all_of(segment, [](char chr) {
                   return is_lower(chr) || is_digit(chr) || chr == '_' ||
                          chr == '-';
               });
    });
}

bool is_poll_group_name(std::string_view text) noexcept {
    return !text.empty() && text.size() <= MAX_POLL_GROUP_NAME &&
           (is_lower(text.front()) || is_digit(text.front())) &&
           std::ranges::all_of(text,
                               [](char chr) {
                                   return is_lower(chr) || is_digit(chr) ||
                                          chr == '_';
                               }) &&
           !std::ranges::all_of(text, is_digit);
}

bool is_protocol_name(std::string_view text) noexcept {
    return !text.empty() && text.size() <= MAX_PROTOCOL_NAME &&
           std::ranges::all_of(
               text, [](char chr) { return is_lower(chr) || is_digit(chr); });
}

bool is_unit_id(std::string_view text) noexcept {
    return !text.empty() && text.size() <= MAX_UNIT_ID &&
           (is_lower(text.front()) || is_digit(text.front())) &&
           std::ranges::all_of(text, [](char chr) {
               return is_lower(chr) || is_digit(chr) || chr == '-';
           });
}

bool is_process_name(std::string_view text) noexcept {
    for (const std::string_view fixed :
         {"tagd", "control", "cmd", "alarm", "hist", "api", "audit", "io"}) {
        if (text == fixed) {
            return true;
        }
    }
    return text.starts_with("drv-") && is_protocol_name(text.substr(4));
}

bool is_free_text(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte < FIRST_PRINTABLE || byte == ASCII_DEL) {
            return false;
        }
        if (byte == C1_LEAD && i + 1 < text.size()) {
            const auto next = static_cast<unsigned char>(text[i + 1]);
            if (next >= C1_SECOND_LOW && next <= C1_SECOND_HIGH) {
                return false;
            }
        }
    }
    return true;
}

bool is_ipv4_address(std::string_view text) noexcept {
    std::size_t parts = 0;
    const bool each = every_part(text, '.', [&parts](std::string_view part) {
        ++parts;
        return small_number(part, IPV4_PART_MAX);
    });
    return each && parts == IPV4_PARTS;
}

bool is_ipv6_address(std::string_view text) noexcept {
    const std::size_t gap = text.find("::");
    std::size_t left = 0;
    std::size_t right = 0;
    if (gap == std::string_view::npos) {
        return ipv6_groups(text, true, left) && left == IPV6_GROUPS;
    }
    if (text.find("::", gap + 1) != std::string_view::npos) {
        return false;
    }
    const std::string_view before = text.substr(0, gap);
    const std::string_view after = text.substr(gap + 2);
    return ipv6_groups(before, false, left) &&
           ipv6_groups(after, true, right) && left + right < IPV6_GROUPS;
}

bool is_ip_address(std::string_view text) noexcept {
    return is_ipv4_address(text) || is_ipv6_address(text);
}

bool is_host_name(std::string_view text) noexcept {
    if (text.empty() || text.size() > MAX_HOST_NAME) {
        return false;
    }
    const bool labels_ok = every_part(text, '.', [](std::string_view label) {
        return !label.empty() && label.size() <= MAX_LABEL &&
               label.front() != '-' && label.back() != '-' &&
               std::ranges::all_of(label, [](char chr) {
                   return is_letter(chr) || is_digit(chr) || chr == '-';
               });
    });
    const bool numeric = std::ranges::all_of(
        text, [](char chr) { return is_digit(chr) || chr == '.'; });
    return labels_ok && !numeric;
}

bool is_cidr(std::string_view text) noexcept {
    const std::size_t slash = text.find('/');
    if (slash == std::string_view::npos) {
        return false;
    }
    const std::string_view address = text.substr(0, slash);
    const std::string_view prefix = text.substr(slash + 1);
    if (is_ipv4_address(address)) {
        return small_number(prefix, IPV4_PREFIX_MAX);
    }
    return is_ipv6_address(address) && small_number(prefix, IPV6_PREFIX_MAX);
}

bool is_time_zone_name(std::string_view text) noexcept {
    return !text.empty() &&
           every_part(text, '/', [](std::string_view component) {
               return !component.empty() &&
                      component.size() <= MAX_ZONE_COMPONENT &&
                      is_letter(component.front()) &&
                      std::ranges::all_of(component, [](char chr) {
                          return is_letter(chr) || is_digit(chr) ||
                                 chr == '_' || chr == '-' || chr == '+';
                      });
           });
}

bool is_time_of_day(std::string_view text) noexcept {
    constexpr std::size_t LENGTH = 5;
    if (text.size() != LENGTH || text[2] != ':' || !is_digit(text[0]) ||
        !is_digit(text[1]) || !is_digit(text[3]) || !is_digit(text[4])) {
        return false;
    }
    const auto digit = [text](std::size_t index) {
        return static_cast<unsigned>(text[index] - '0');
    };
    const unsigned hours = (digit(0) * BASE_TEN) + digit(1);
    const unsigned minutes = (digit(3) * BASE_TEN) + digit(4);
    return hours < HOURS_PER_DAY && minutes < MINUTES_PER_HOUR;
}

bool is_https_url(std::string_view text) noexcept {
    constexpr std::string_view SCHEME = "https://";
    if (!text.starts_with(SCHEME)) {
        return false;
    }
    const std::string_view rest = text.substr(SCHEME.size());
    if (rest.empty() || rest.front() == '/' || rest.front() == ':') {
        return false;
    }
    return std::ranges::none_of(rest, [](char chr) {
        return static_cast<unsigned char>(chr) <= ' ' || chr == '\x7F';
    });
}

}  // namespace scada::conf
