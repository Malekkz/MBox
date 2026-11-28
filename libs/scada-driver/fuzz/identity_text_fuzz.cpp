// Fuzz target for the text of `sys.dev.<device>.identity` (driver interface
// contract 3.6, 11.1; decision D-147): the values come from the device. The
// input is a list of fields, each a kind byte, a length byte and that many
// bytes, so that a value may hold a NUL (PR 171 S6). The kind chooses the
// type (STRING, INT64, UINT64, FLOAT64, BOOL, or not read). The joined text
// must fit the STRING maximum, hold only printable bytes, and be each
// field's shown value in turn, separated, each in its equal share, a value
// longer than its share cut to it with the mark as its last byte.

#include "scada/driver/identity_text.h"
#include "scada/driver/tag_value.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t MAX_FIELDS = 64;
constexpr std::uint8_t KINDS = 6;
constexpr char FIRST_SHOWN = 0x20;
constexpr char LAST_SHOWN = 0x7E;

// The 8 bytes after the kind as a number (zero-filled).
std::uint64_t bits_of(std::string_view bytes) {
    std::array<char, sizeof(std::uint64_t)> word{};
    std::copy_n(bytes.data(), std::min(bytes.size(), word.size()), word.data());
    return std::bit_cast<std::uint64_t>(word);
}

scada::driver::IdentityField field_of(std::uint8_t kind,
                                      std::string_view rest) {
    switch (kind % KINDS) {
        case 0:
            return {.read = true, .value = scada::driver::string_value(rest)};
        case 1:
            return {.read = true,
                    .value = scada::driver::int64_value(
                        std::bit_cast<std::int64_t>(bits_of(rest)))};
        case 2:
            return {.read = true,
                    .value = scada::driver::uint64_value(bits_of(rest))};
        case 3:
            return {.read = true,
                    .value = scada::driver::float64_value(
                        std::bit_cast<double>(bits_of(rest)))};
        case 4:
            return {.read = true,
                    .value = scada::driver::bool_value(!rest.empty())};
        default:
            return {};  // not read
    }
}

// The joined text by its definition: separators first, then the share of
// each field.
std::string expected_text(
    std::span<const scada::driver::IdentityField> fields) {
    if (fields.empty()) {
        return {};
    }
    constexpr std::size_t MAX = scada::driver::TAG_STRING_MAX_BYTES;
    const std::size_t separators =
        (fields.size() - 1) * scada::driver::IDENTITY_SEPARATOR.size();
    const std::size_t share =
        separators >= MAX ? 0 : (MAX - separators) / fields.size();
    std::string text;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index > 0) {
            text += scada::driver::IDENTITY_SEPARATOR;
        }
        if (!fields[index].read || share == 0) {
            continue;
        }
        // Larger than any shown value: a STRING is at most 255 bytes here.
        std::array<char, 2 * MAX> whole{};
        const std::size_t full =
            scada::driver::identity_field(fields[index].value, whole);
        if (full <= share) {
            text.append(whole.data(), full);
        } else {
            text.append(whole.data(), share - 1);
            text += scada::driver::IDENTITY_CUT;
        }
    }
    return text.substr(0, MAX);
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    std::string input;
    input.reserve(size);
    for (const std::byte byte : std::as_bytes(std::span(data, size))) {
        input.push_back(static_cast<char>(byte));
    }
    std::vector<scada::driver::IdentityField> fields;
    std::string_view rest = input;
    constexpr std::size_t HEADER = 2;
    while (rest.size() >= HEADER && fields.size() < MAX_FIELDS) {
        const auto kind = static_cast<std::uint8_t>(rest[0]);
        const std::size_t length = std::min<std::size_t>(
            static_cast<unsigned char>(rest[1]), rest.size() - HEADER);
        fields.push_back(field_of(kind, rest.substr(HEADER, length)));
        rest = rest.substr(HEADER + length);
    }
    std::array<char, scada::driver::TAG_STRING_MAX_BYTES> out{};
    const std::size_t length = scada::driver::join_identity(fields, out);
    if (length > out.size()) {
        std::abort();
    }
    for (const char byte : std::span(out).first(length)) {
        if (byte < FIRST_SHOWN || byte > LAST_SHOWN) {
            std::abort();
        }
    }
    if (std::string_view(out.data(), length) != expected_text(fields)) {
        std::abort();
    }
    return 0;
}
