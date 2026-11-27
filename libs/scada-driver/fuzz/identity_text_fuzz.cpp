// Fuzz target for the text of `sys.dev.<device>.identity` (driver interface
// contract 3.6, 11.1; decision D-145): the values come from the device. The
// input is split at each NUL into fields; a field's first byte chooses its
// type (STRING, INT64, UINT64, FLOAT64, BOOL, or not read) and the rest is
// its value. The joined text must fit the STRING maximum and hold only
// printable bytes, whatever the fields.

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

scada::driver::IdentityField field_of(std::string_view part) {
    if (part.empty()) {
        return {};
    }
    const std::string_view rest = part.substr(1);
    switch (static_cast<std::uint8_t>(part.front()) % KINDS) {
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
    while (!rest.empty() && fields.size() < MAX_FIELDS) {
        const std::size_t end = rest.find('\0');
        fields.push_back(field_of(rest.substr(0, end)));
        rest = end == std::string_view::npos ? std::string_view{}
                                             : rest.substr(end + 1);
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
    return 0;
}
