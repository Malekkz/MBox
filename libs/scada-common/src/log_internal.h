#ifndef SCADA_COMMON_SRC_LOG_INTERNAL_H
#define SCADA_COMMON_SRC_LOG_INTERNAL_H

// Helpers shared by the logger and its sinks. Not part of the public
// interface; the unit tests include this header directly.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace scada::log_internal {

// Returns the longest prefix of `text` that is at most `max_bytes` long and
// does not end inside a UTF-8 character. Text that is not valid UTF-8 is cut
// within 3 bytes of the limit.
[[nodiscard]] std::string_view truncate_utf8(std::string_view text,
                                             std::size_t max_bytes) noexcept;

// Parses an unsigned decimal number that fills all of `text`. Returns nothing
// for an empty string, any other character, or a value beyond 64 bits.
[[nodiscard]] std::optional<std::uint64_t> parse_decimal(
    std::string_view text) noexcept;

// Returns true if `journal_stream` (the value of the JOURNAL_STREAM
// environment variable, "<device>:<inode>" in decimal) names the file that
// `fd` refers to. Malformed text gives false.
[[nodiscard]] bool is_journal_stream(std::string_view journal_stream,
                                     int fd) noexcept;

// Appends text to caller-provided storage and never writes beyond it; what
// does not fit is discarded and remembered as truncation. The storage must
// outlive the BoundedText, which keeps a span of it.
class BoundedText {
public:
    explicit BoundedText(std::span<char> storage) noexcept
        : storage_(storage) {}

    void append(std::string_view text) noexcept;
    void append(char character) noexcept;

    // Appends `value` in decimal, padded with zeros to `min_width` digits.
    void append_number(std::uint64_t value, std::size_t min_width = 1) noexcept;

    // Appends `value` as 8 bytes, least significant first.
    void append_le64(std::uint64_t value) noexcept;

    // True if anything was discarded.
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }

    // The text appended so far; valid while the storage is.
    [[nodiscard]] std::string_view view() const noexcept {
        return {storage_.data(), size_};
    }

private:
    std::span<char> storage_;
    std::size_t size_ = 0;
    bool truncated_ = false;
};

}  // namespace scada::log_internal

#endif  // SCADA_COMMON_SRC_LOG_INTERNAL_H
