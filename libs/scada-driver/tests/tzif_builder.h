#ifndef SCADA_DRIVER_TESTS_TZIF_BUILDER_H
#define SCADA_DRIVER_TESTS_TZIF_BUILDER_H

// Builds zone files (TZif version 2, RFC 9636) for tests: a version 1
// header with an empty data block, then the version 2 header, data and
// footer. Fields can be broken on purpose.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scada::driver::test {

struct TzifSpec {
    std::vector<std::int64_t> transitions;
    std::vector<std::uint8_t> type_of;  // one per transition
    std::vector<std::int32_t> offsets;  // one per type
    std::string footer = "UTC0";
    char version = '2';
    std::uint32_t leap_count = 0;
    std::uint32_t chars = 4;  // designation bytes ("UTC\0")
};

inline void put_be(std::vector<std::byte>& out, std::uint64_t value,
                   std::size_t width) {
    constexpr unsigned BYTE_BITS = 8;
    constexpr std::uint64_t BYTE_MASK = 0xFFU;
    for (std::size_t index = width; index > 0; --index) {
        out.push_back(static_cast<std::byte>(
            (value >> ((index - 1) * BYTE_BITS)) & BYTE_MASK));
    }
}

inline void put_header(std::vector<std::byte>& out, char version,
                       std::uint32_t leap, std::uint32_t times,
                       std::uint32_t types, std::uint32_t chars) {
    for (const char character : std::string_view("TZif")) {
        out.push_back(static_cast<std::byte>(character));
    }
    out.push_back(static_cast<std::byte>(version));
    constexpr std::size_t UNUSED = 15;
    out.insert(out.end(), UNUSED, std::byte{0});
    constexpr std::size_t COUNT_BYTES = 4;
    put_be(out, 0, COUNT_BYTES);  // isutcnt
    put_be(out, 0, COUNT_BYTES);  // isstdcnt
    put_be(out, leap, COUNT_BYTES);
    put_be(out, times, COUNT_BYTES);
    put_be(out, types, COUNT_BYTES);
    put_be(out, chars, COUNT_BYTES);
}

inline std::vector<std::byte> build_tzif(const TzifSpec& spec) {
    std::vector<std::byte> out;
    // Version 1: one type, no transitions, one designation byte.
    put_header(out, spec.version, 0, 0, 1, 1);
    constexpr std::size_t TYPE_BYTES = 6;
    out.insert(out.end(), TYPE_BYTES + 1, std::byte{0});
    put_header(out, spec.version, spec.leap_count,
               static_cast<std::uint32_t>(spec.transitions.size()),
               static_cast<std::uint32_t>(spec.offsets.size()), spec.chars);
    constexpr std::size_t TIME_BYTES = 8;
    for (const std::int64_t when : spec.transitions) {
        put_be(out, static_cast<std::uint64_t>(when), TIME_BYTES);
    }
    for (const std::uint8_t type : spec.type_of) {
        out.push_back(static_cast<std::byte>(type));
    }
    constexpr std::size_t OFFSET_BYTES = 4;
    for (const std::int32_t offset : spec.offsets) {
        put_be(out, static_cast<std::uint32_t>(offset), OFFSET_BYTES);
        out.push_back(std::byte{0});  // isdst
        out.push_back(std::byte{0});  // designation index
    }
    constexpr std::size_t LEAP_BYTES = 12;
    out.insert(out.end(),
               static_cast<std::size_t>(spec.leap_count) * LEAP_BYTES,
               std::byte{0});
    out.insert(out.end(), spec.chars, std::byte{0});
    out.push_back(std::byte{'\n'});
    for (const char character : spec.footer) {
        out.push_back(static_cast<std::byte>(character));
    }
    out.push_back(std::byte{'\n'});
    return out;
}

}  // namespace scada::driver::test

#endif  // SCADA_DRIVER_TESTS_TZIF_BUILDER_H
