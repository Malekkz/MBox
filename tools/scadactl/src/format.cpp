#include "scada/ctl/format.h"

#include "scada/tag/scada_tag.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace scada::ctl {

namespace {

// The names of the quality flags, bit 0 first (contract, section 6).
constexpr std::array<std::string_view, 12> FLAG_NAMES = {
    "STALE",    "COMM_FAIL",  "OUT_OF_RANGE",   "SUBSTITUTED",
    "NOT_INIT", "OWNER_DOWN", "TIME_UNCERTAIN", "BLOCKED",
    "TEST",     "OVERFLOW",   "RESTORED",       "LOCAL_TIME"};

constexpr std::string_view HEX_DIGITS = "0123456789abcdef";

constexpr std::int64_t NS_PER_MS = 1'000'000;
constexpr std::int64_t MS_PER_S = 1'000;

// A code point decoded from well-formed UTF-8 and its length in bytes.
struct CodePoint {
    std::uint32_t value = 0;
    std::size_t length = 0;
};

std::uint32_t byte_at(std::string_view bytes, std::size_t index) noexcept {
    return static_cast<unsigned char>(bytes[index]);
}

constexpr std::uint32_t ASCII_END = 0x80;

// The lead bytes of a sequence of `length` bytes, the bits of the lead
// byte that belong to the code point, and the smallest code point the
// sequence may hold, so that an overlong form is refused (RFC 3629).
struct SequenceForm {
    std::uint32_t first_lead;
    std::uint32_t last_lead;
    std::size_t length;
    std::uint32_t lead_mask;
    std::uint32_t minimum;
};
constexpr std::array<SequenceForm, 3> SEQUENCE_FORMS = {{
    {.first_lead = 0xC2,
     .last_lead = 0xDF,
     .length = 2,
     .lead_mask = 0x1F,
     .minimum = 0x80},
    {.first_lead = 0xE0,
     .last_lead = 0xEF,
     .length = 3,
     .lead_mask = 0x0F,
     .minimum = 0x800},
    {.first_lead = 0xF0,
     .last_lead = 0xF4,
     .length = 4,
     .lead_mask = 0x07,
     .minimum = 0x10000},
}};

// The code point at the start of `bytes`, or nothing if `bytes` does not
// start with a well-formed UTF-8 sequence (no overlong forms, no
// surrogates, nothing above U+10FFFF).
std::optional<CodePoint> decode_utf8(std::string_view bytes) noexcept {
    constexpr std::uint32_t CONTINUATION_MASK = 0xC0;
    constexpr std::uint32_t CONTINUATION = 0x80;
    constexpr std::uint32_t PAYLOAD = 0x3F;
    constexpr unsigned SHIFT = 6;
    if (bytes.empty()) {
        return std::nullopt;
    }
    const std::uint32_t lead = byte_at(bytes, 0);
    if (lead < ASCII_END) {
        return CodePoint{.value = lead, .length = 1};
    }
    const auto* const form = std::find_if(
        SEQUENCE_FORMS.begin(), SEQUENCE_FORMS.end(),
        [lead](const SequenceForm& candidate) {
            return lead >= candidate.first_lead && lead <= candidate.last_lead;
        });
    if (form == SEQUENCE_FORMS.end()) {
        return std::nullopt;
    }
    const std::size_t length = form->length;
    std::uint32_t value = lead & form->lead_mask;
    const std::uint32_t minimum = form->minimum;
    if (bytes.size() < length) {
        return std::nullopt;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const std::uint32_t next = byte_at(bytes, i);
        if ((next & CONTINUATION_MASK) != CONTINUATION) {
            return std::nullopt;
        }
        value = (value << SHIFT) | (next & PAYLOAD);
    }
    constexpr std::uint32_t SURROGATE_FIRST = 0xD800;
    constexpr std::uint32_t SURROGATE_LAST = 0xDFFF;
    constexpr std::uint32_t LAST_CODE_POINT = 0x10FFFF;
    if (value < minimum || value > LAST_CODE_POINT ||
        (value >= SURROGATE_FIRST && value <= SURROGATE_LAST)) {
        return std::nullopt;
    }
    return CodePoint{.value = value, .length = length};
}

void append_hex(std::string& out, std::uint32_t value, std::size_t digits) {
    constexpr unsigned BITS_PER_DIGIT = 4;
    constexpr std::uint32_t DIGIT_MASK = 0xF;
    for (std::size_t i = digits; i > 0; --i) {
        const auto shift = static_cast<unsigned>((i - 1) * BITS_PER_DIGIT);
        out.push_back(HEX_DIGITS[(value >> shift) & DIGIT_MASK]);
    }
}

// True for the characters that a terminal treats as controls or that
// reorder the text around them: C1 controls, and the bidirectional
// embeddings, overrides and isolates.
bool is_unsafe_code_point(std::uint32_t value) noexcept {
    constexpr std::uint32_t C1_FIRST = 0x80;
    constexpr std::uint32_t C1_LAST = 0x9F;
    constexpr std::uint32_t MARK_LRM = 0x200E;
    constexpr std::uint32_t MARK_RLM = 0x200F;
    constexpr std::uint32_t EMBEDDING_FIRST = 0x202A;
    constexpr std::uint32_t EMBEDDING_LAST = 0x202E;
    constexpr std::uint32_t ISOLATE_FIRST = 0x2066;
    constexpr std::uint32_t ISOLATE_LAST = 0x2069;
    return (value >= C1_FIRST && value <= C1_LAST) || value == MARK_LRM ||
           value == MARK_RLM ||
           (value >= EMBEDDING_FIRST && value <= EMBEDDING_LAST) ||
           (value >= ISOLATE_FIRST && value <= ISOLATE_LAST);
}

template <typename T>
void append_number(std::string& out, T value) {
    // Holds any 64-bit integer and the shortest form of any double.
    constexpr std::size_t NUMBER_CHARS = 32;
    std::array<char, NUMBER_CHARS> buffer{};
    const auto result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    // The buffer holds any 64-bit integer and the shortest form of any
    // double, so to_chars cannot fail here.
    out.append(buffer.data(), result.ptr);
}

void append_double(std::string& out, double value, bool json) {
    if (std::isnan(value)) {
        out.append(json ? "\"nan\"" : "nan");
    } else if (std::isinf(value)) {
        if (value < 0) {
            out.append(json ? "\"-inf\"" : "-inf");
        } else {
            out.append(json ? "\"inf\"" : "inf");
        }
    } else {
        append_number(out, value);
    }
}

void append_padded(std::string& out, std::int64_t value, std::size_t width) {
    std::string digits;
    append_number(digits, value);
    if (digits.size() < width) {
        out.append(width - digits.size(), '0');
    }
    out.append(digits);
}

}  // namespace

std::string_view type_name(std::uint8_t type) noexcept {
    switch (type) {
        case SCADA_TYPE_BOOL:
            return "bool";
        case SCADA_TYPE_INT64:
            return "int64";
        case SCADA_TYPE_UINT64:
            return "uint64";
        case SCADA_TYPE_FLOAT64:
            return "float64";
        case SCADA_TYPE_STRING:
            return "string";
        default:
            return "unknown";
    }
}

std::string_view quality_name(std::uint8_t quality) noexcept {
    switch (quality) {
        case SCADA_QUALITY_GOOD:
            return "GOOD";
        case SCADA_QUALITY_UNCERTAIN:
            return "UNCERTAIN";
        case SCADA_QUALITY_BAD:
            return "BAD";
        default:
            return "UNKNOWN";
    }
}

void append_flag_names(std::string& out, std::uint16_t flags,
                       std::string_view separator) {
    constexpr std::size_t BITS = 16;
    constexpr std::size_t HEX_WIDTH = 4;
    bool first = true;
    for (std::size_t bit = 0; bit < BITS; ++bit) {
        const auto mask = static_cast<std::uint32_t>(1U << bit);
        if ((flags & mask) == 0) {
            continue;
        }
        if (!first) {
            out.append(separator);
        }
        first = false;
        if (bit < FLAG_NAMES.size()) {
            out.append(FLAG_NAMES.at(bit));
        } else {
            out.append("0x");
            append_hex(out, mask, HEX_WIDTH);
        }
    }
}

Mark mark_of(std::uint8_t quality, std::uint16_t flags) noexcept {
    if (quality == SCADA_QUALITY_GOOD) {
        return (flags & SCADA_FLAG_STALE) != 0 ? Mark::ATTENTION : Mark::NONE;
    }
    if (quality == SCADA_QUALITY_UNCERTAIN) {
        return Mark::ATTENTION;
    }
    return Mark::BAD;
}

char mark_char(Mark mark) noexcept {
    switch (mark) {
        case Mark::NONE:
            return ' ';
        case Mark::ATTENTION:
            return '~';
        case Mark::BAD:
            return '!';
    }
    return '!';
}

bool has_value(const scada_value& value) noexcept {
    return value.update_mono_ns != 0;
}

void append_value_text(std::string& out, const scada_value& value,
                       std::string_view text) {
    if (!has_value(value)) {
        out.push_back('-');
        return;
    }
    switch (value.type) {
        case SCADA_TYPE_BOOL:
            out.append(value.value != 0 ? "true" : "false");
            break;
        case SCADA_TYPE_INT64:
            append_number(out, std::bit_cast<std::int64_t>(value.value));
            break;
        case SCADA_TYPE_UINT64:
            append_number(out, value.value);
            break;
        case SCADA_TYPE_FLOAT64:
            append_double(out, std::bit_cast<double>(value.value), false);
            break;
        case SCADA_TYPE_STRING:
            out.push_back('"');
            append_terminal_safe(out, text);
            out.push_back('"');
            break;
        default:
            out.push_back('-');
            break;
    }
}

void append_value_json(std::string& out, const scada_value& value,
                       std::string_view text) {
    if (!has_value(value)) {
        out.append("null");
        return;
    }
    switch (value.type) {
        case SCADA_TYPE_BOOL:
            out.append(value.value != 0 ? "true" : "false");
            break;
        case SCADA_TYPE_INT64:
            append_number(out, std::bit_cast<std::int64_t>(value.value));
            break;
        case SCADA_TYPE_UINT64:
            append_number(out, value.value);
            break;
        case SCADA_TYPE_FLOAT64:
            append_double(out, std::bit_cast<double>(value.value), true);
            break;
        case SCADA_TYPE_STRING:
            append_json_string(out, text);
            break;
        default:
            out.append("null");
            break;
    }
}

void append_terminal_safe(std::string& out, std::string_view bytes) {
    constexpr std::uint32_t FIRST_PRINTABLE = 0x20;
    constexpr std::uint32_t LAST_PRINTABLE = 0x7E;
    constexpr std::size_t BYTE_DIGITS = 2;
    constexpr std::size_t CODE_POINT_DIGITS = 4;
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        const auto point = decode_utf8(bytes.substr(pos));
        if (!point) {
            out.append("\\x");
            append_hex(out, byte_at(bytes, pos), BYTE_DIGITS);
            ++pos;
            continue;
        }
        const std::uint32_t value = point->value;
        if (value == '"' || value == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(value));
        } else if (value == '\n') {
            out.append("\\n");
        } else if (value == '\r') {
            out.append("\\r");
        } else if (value == '\t') {
            out.append("\\t");
        } else if (value < FIRST_PRINTABLE || value == LAST_PRINTABLE + 1) {
            out.append("\\x");
            append_hex(out, value, BYTE_DIGITS);
        } else if (is_unsafe_code_point(value)) {
            out.append("\\u");
            append_hex(out, value, CODE_POINT_DIGITS);
        } else {
            out.append(bytes.substr(pos, point->length));
        }
        pos += point->length;
    }
}

void append_json_string(std::string& out, std::string_view bytes) {
    constexpr std::uint32_t FIRST_PRINTABLE = 0x20;
    constexpr std::uint32_t DELETE = 0x7F;
    constexpr std::size_t CODE_POINT_DIGITS = 4;
    out.push_back('"');
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        const auto point = decode_utf8(bytes.substr(pos));
        if (!point) {
            out.append("\\ufffd");
            ++pos;
            continue;
        }
        const std::uint32_t value = point->value;
        if (value == '"' || value == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(value));
        } else if (value == '\n') {
            out.append("\\n");
        } else if (value == '\r') {
            out.append("\\r");
        } else if (value == '\t') {
            out.append("\\t");
        } else if (value < FIRST_PRINTABLE || value == DELETE ||
                   is_unsafe_code_point(value)) {
            out.append("\\u");
            append_hex(out, value, CODE_POINT_DIGITS);
        } else {
            out.append(bytes.substr(pos, point->length));
        }
        pos += point->length;
    }
    out.push_back('"');
}

void append_age(std::string& out, std::int64_t age_ns) {
    constexpr std::size_t MS_DIGITS = 3;
    const std::int64_t millis = age_ns > 0 ? age_ns / NS_PER_MS : 0;
    append_number(out, millis / MS_PER_S);
    out.push_back('.');
    append_padded(out, millis % MS_PER_S, MS_DIGITS);
    out.append(" s");
}

void append_utc_time(std::string& out, std::int64_t time_ns) {
    if (time_ns == 0) {
        out.push_back('-');
        return;
    }
    using std::chrono::days;
    using std::chrono::floor;
    using std::chrono::milliseconds;
    using std::chrono::nanoseconds;
    using std::chrono::sys_days;
    using std::chrono::system_clock;
    using std::chrono::time_point;
    using std::chrono::year_month_day;
    constexpr std::size_t TWO = 2;
    constexpr std::size_t THREE = 3;
    constexpr std::size_t FOUR = 4;
    constexpr std::int64_t S_PER_MIN = 60;
    constexpr std::int64_t S_PER_HOUR = 3600;

    const time_point<system_clock, milliseconds> when{
        floor<milliseconds>(nanoseconds{time_ns})};
    const auto day = floor<days>(when);
    const year_month_day date{sys_days{day}};
    const std::int64_t ms_of_day = (when - day).count();
    const std::int64_t s_of_day = ms_of_day / MS_PER_S;

    append_padded(out, static_cast<int>(date.year()), FOUR);
    out.push_back('-');
    append_padded(out, static_cast<unsigned>(date.month()), TWO);
    out.push_back('-');
    append_padded(out, static_cast<unsigned>(date.day()), TWO);
    out.push_back('T');
    append_padded(out, s_of_day / S_PER_HOUR, TWO);
    out.push_back(':');
    append_padded(out, s_of_day % S_PER_HOUR / S_PER_MIN, TWO);
    out.push_back(':');
    append_padded(out, s_of_day % S_PER_MIN, TWO);
    out.push_back('.');
    append_padded(out, ms_of_day % MS_PER_S, THREE);
    out.push_back('Z');
}

}  // namespace scada::ctl
