#include "scada/driver/identity_text.h"

#include "scada/driver/tag_value.h"
#include "scada/tag/types.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <span>
#include <string_view>
#include <system_error>

namespace scada::driver {
namespace {

constexpr char FIRST_SHOWN = 0x20;
constexpr char LAST_SHOWN = 0x7E;

// Room for any value as identity_field writes it before it is cut: a
// STRING of the tag database maximum, and a little more, so that a longer
// one is seen to be longer.
constexpr std::size_t SCRATCH_BYTES = TAG_STRING_MAX_BYTES + 6;

// Copies `text` into `out`, at most its size.
std::size_t copy_into(std::string_view text, std::span<char> out) noexcept {
    const std::size_t length = std::min(text.size(), out.size());
    std::copy_n(text.data(), length, out.data());
    return length;
}

// A number by to_chars into `out`; nothing if it does not fit.
template <class Number>
std::size_t number_into(Number number, std::span<char> out) noexcept {
    const auto [end, error] =
        std::to_chars(out.data(), out.data() + out.size(), number);
    if (error != std::errc{}) {
        return 0;
    }
    return static_cast<std::size_t>(end - out.data());
}

}  // namespace

std::size_t clean_identity_text(std::string_view text,
                                std::span<char> out) noexcept {
    const std::size_t length = std::min(text.size(), out.size());
    for (std::size_t index = 0; index < length; ++index) {
        const char byte = text[index];
        out[index] = byte >= FIRST_SHOWN && byte <= LAST_SHOWN ? byte : '?';
    }
    return length;
}

std::size_t identity_field(const TagValue& value,
                           std::span<char> out) noexcept {
    switch (value.type) {
        case tag::TagType::BOOL:
            return copy_into(value.b ? "true" : "false", out);
        case tag::TagType::INT64:
            return number_into(value.i, out);
        case tag::TagType::UINT64:
            return number_into(value.u, out);
        case tag::TagType::FLOAT64:
            return number_into(value.f, out);
        case tag::TagType::STRING:
            return clean_identity_text(value.text, out);
        default:
            return 0;
    }
}

std::size_t join_identity(std::span<const IdentityField> fields,
                          std::span<char, TAG_STRING_MAX_BYTES> out) noexcept {
    if (fields.empty()) {
        return 0;
    }
    const std::size_t separators =
        (fields.size() - 1) * IDENTITY_SEPARATOR.size();
    const std::size_t share = separators >= out.size()
                                  ? 0
                                  : (out.size() - separators) / fields.size();
    std::array<char, SCRATCH_BYTES> scratch{};
    std::size_t length = 0;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index > 0) {
            length += copy_into(IDENTITY_SEPARATOR,
                                std::span<char>(out).subspan(length));
        }
        const IdentityField& field = fields[index];
        if (!field.read || share == 0) {
            continue;  // an empty field
        }
        const std::size_t full = identity_field(field.value, scratch);
        const std::span<char> place = std::span<char>(out).subspan(
            length, std::min(share, out.size() - length));
        if (full <= place.size()) {
            length += copy_into(std::string_view(scratch.data(), full), place);
        } else if (!place.empty()) {
            // Cut to the share, the last byte the mark.
            length += copy_into(
                std::string_view(scratch.data(), place.size() - 1), place);
            place[place.size() - 1] = IDENTITY_CUT;
            ++length;
        }
    }
    return length;
}

}  // namespace scada::driver
