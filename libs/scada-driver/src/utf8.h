#ifndef SCADA_DRIVER_UTF8_H
#define SCADA_DRIVER_UTF8_H

#include "scada/common/realtime.h"

#include <cstddef>
#include <span>

namespace scada::driver::detail {

// True when `text` is well-formed UTF-8 (RFC 3629: no overlong forms, no
// surrogates, nothing above U+10FFFF).
SCADA_RT [[nodiscard]] bool is_utf8(std::span<const std::byte> text) noexcept;

// The longest prefix of the well-formed UTF-8 `text` that has at most
// `limit` bytes and ends at a character boundary.
SCADA_RT [[nodiscard]] std::size_t utf8_prefix(std::span<const std::byte> text,
                                               std::size_t limit) noexcept;

}  // namespace scada::driver::detail

#endif  // SCADA_DRIVER_UTF8_H
