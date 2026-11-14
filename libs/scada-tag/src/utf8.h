#ifndef SCADA_TAG_UTF8_H
#define SCADA_TAG_UTF8_H

// Validation of text that goes into a string slot
// (docs/contracts/tag-database.md, section 7: the data of a slot is UTF-8).
// Private to libscada-tag.

#include <string_view>

namespace scada::tag {

// True if `text` is well-formed UTF-8 (Unicode, table 3-7): no truncated or
// overlong sequence, no surrogate, nothing above U+10FFFF. An empty text is
// well-formed. It accepts any bytes and reads none outside `text`.
[[nodiscard]] bool is_utf8(std::string_view text) noexcept;

}  // namespace scada::tag

#endif  // SCADA_TAG_UTF8_H
