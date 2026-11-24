#ifndef SCADA_CONF_SRC_UTF8_H
#define SCADA_CONF_SRC_UTF8_H

// UTF-8 (RFC 3629) for the parser and the diagnostics of libscada-conf. Not
// installed.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace scada::conf::utf8 {

constexpr unsigned char CONTINUATION = 0x80;
constexpr unsigned char CONTINUATION_MASK = 0xC0;
constexpr std::uint32_t LAST_CODE_POINT = 0x10FFFF;
constexpr std::uint32_t FIRST_SURROGATE = 0xD800;
constexpr std::uint32_t LAST_SURROGATE = 0xDFFF;

// The length of the UTF-8 sequence that starts at `text[where]` and its code
// point, or nothing if it is not valid UTF-8 (overlong forms, surrogates and
// values above U+10FFFF included), or for an ASCII byte or `where` past the
// end.
[[nodiscard]] std::optional<std::pair<std::size_t, std::uint32_t>> decode(
    std::string_view text, std::size_t where) noexcept;

// The number of bytes of the UTF-8 sequence whose lead byte is `byte`, in
// input already checked to be valid UTF-8.
[[nodiscard]] std::size_t sequence_length(unsigned char byte) noexcept;

// Appends the UTF-8 form of `code_point`, which must be a scalar value.
void append(std::string& out, std::uint32_t code_point);

}  // namespace scada::conf::utf8

#endif  // SCADA_CONF_SRC_UTF8_H
