#include "crc32.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::tag {

namespace {

constexpr std::uint32_t POLYNOMIAL = 0xEDB8'8320U;
constexpr std::uint32_t ALL_ONES = 0xFFFF'FFFFU;
constexpr std::size_t TABLE_SIZE = 256;
constexpr int BITS_PER_BYTE = 8;
constexpr std::uint32_t LOW_BYTE = 0xFFU;

constexpr std::array<std::uint32_t, TABLE_SIZE> make_table() noexcept {
    std::array<std::uint32_t, TABLE_SIZE> table{};
    for (std::uint32_t i = 0; i < TABLE_SIZE; ++i) {
        std::uint32_t value = i;
        for (int bit = 0; bit < BITS_PER_BYTE; ++bit) {
            value =
                (value & 1U) != 0 ? POLYNOMIAL ^ (value >> 1U) : value >> 1U;
        }
        table.at(i) = value;
    }
    return table;
}

constexpr std::array<std::uint32_t, TABLE_SIZE> TABLE = make_table();

}  // namespace

std::uint32_t crc32(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = ALL_ONES;
    for (const std::byte byte : bytes) {
        const std::uint32_t index =
            (crc ^ std::to_integer<std::uint32_t>(byte)) & LOW_BYTE;
        // In range: masked to the low byte.
        crc = TABLE.at(index) ^ (crc >> static_cast<unsigned>(BITS_PER_BYTE));
    }
    return crc ^ ALL_ONES;
}

}  // namespace scada::tag
