#include "log_internal.h"

#include "scada/common/log.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <sys/stat.h>

namespace scada {

namespace {

constexpr std::array<std::string_view, 4> RESERVED_KEYS{
    "MESSAGE", "PRIORITY", "SYSLOG_IDENTIFIER", LOG_TIME_FIELD};

constexpr bool is_upper(char character) noexcept {
    return character >= 'A' && character <= 'Z';
}

constexpr bool is_digit(char character) noexcept {
    return character >= '0' && character <= '9';
}

constexpr std::uint64_t DECIMAL_BASE = 10;
constexpr unsigned BITS_PER_BYTE = 8;
constexpr std::uint64_t BYTE_MASK = 0xFF;

// A UTF-8 continuation byte has the form 10xxxxxx.
constexpr unsigned UTF8_CONTINUATION_MASK = 0xC0;
constexpr unsigned UTF8_CONTINUATION_BITS = 0x80;
constexpr int UTF8_MAX_CONTINUATION_BYTES = 3;

}  // namespace

bool is_valid_log_key(std::string_view key) noexcept {
    if (key.empty() || key.size() > LOG_MAX_KEY_BYTES ||
        !is_upper(key.front())) {
        return false;
    }
    const bool characters_ok =
        std::all_of(key.begin(), key.end(), [](char character) {
            return is_upper(character) || is_digit(character) ||
                   character == '_';
        });
    return characters_ok &&
           std::find(RESERVED_KEYS.begin(), RESERVED_KEYS.end(), key) ==
               RESERVED_KEYS.end();
}

namespace log_internal {

std::string_view truncate_utf8(std::string_view text,
                               std::size_t max_bytes) noexcept {
    if (text.size() <= max_bytes) {
        return text;
    }
    // A UTF-8 character has at most 3 continuation bytes (10xxxxxx). If the
    // first byte that would be cut off is one of them, the cut is inside a
    // character: move it back to the start of that character.
    std::size_t cut = max_bytes;
    for (int step = 0; step < UTF8_MAX_CONTINUATION_BYTES && cut > 0; ++step) {
        const auto next = static_cast<unsigned char>(text[cut]);
        if ((next & UTF8_CONTINUATION_MASK) != UTF8_CONTINUATION_BITS) {
            break;
        }
        --cut;
    }
    return text.substr(0, cut);
}

std::optional<std::uint64_t> parse_decimal(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    constexpr std::uint64_t MAX = UINT64_MAX;
    std::uint64_t value = 0;
    for (const char character : text) {
        if (!is_digit(character)) {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (MAX - digit) / DECIMAL_BASE) {
            return std::nullopt;
        }
        value = value * DECIMAL_BASE + digit;
    }
    return value;
}

bool is_journal_stream(std::string_view journal_stream,
                       int descriptor) noexcept {
    const std::size_t colon = journal_stream.find(':');
    if (colon == std::string_view::npos) {
        return false;
    }
    const auto device = parse_decimal(journal_stream.substr(0, colon));
    const auto inode = parse_decimal(journal_stream.substr(colon + 1));
    if (!device || !inode) {
        return false;
    }
    struct stat info {};
    if (::fstat(descriptor, &info) != 0) {
        return false;
    }
    return static_cast<std::uint64_t>(info.st_dev) == *device &&
           static_cast<std::uint64_t>(info.st_ino) == *inode;
}

void BoundedText::append(std::string_view text) noexcept {
    const std::size_t room = storage_.size() - size_;
    const std::size_t count = std::min(room, text.size());
    std::copy_n(text.begin(), count, storage_.subspan(size_).begin());
    size_ += count;
    if (count < text.size()) {
        truncated_ = true;
    }
}

void BoundedText::append(char character) noexcept {
    append(std::string_view(&character, 1));
}

void BoundedText::append_number(std::uint64_t value,
                                std::size_t min_width) noexcept {
    // 20 digits hold any 64-bit value. Filled from the last digit backwards.
    std::array<char, UINT64_DIGITS> digits{};
    std::size_t first = digits.size();
    const std::size_t width =
        std::clamp<std::size_t>(min_width, 1, UINT64_DIGITS);
    while (value != 0 || digits.size() - first < width) {
        --first;
        digits.at(first) = static_cast<char>('0' + value % DECIMAL_BASE);
        value /= DECIMAL_BASE;
    }
    append(std::string_view(digits.data(), digits.size()).substr(first));
}

void BoundedText::append_le64(std::uint64_t value) noexcept {
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        append(static_cast<char>(value & BYTE_MASK));
        value >>= BITS_PER_BYTE;
    }
}

}  // namespace log_internal

}  // namespace scada
