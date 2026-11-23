#include "scada/driver/raw_value.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::driver {

namespace {

constexpr std::uint8_t FLOAT32_WIDTH = 32;
constexpr std::uint8_t FLOAT64_WIDTH = 64;
constexpr std::uint8_t BOOL_WIDTH = 1;

}  // namespace

RawValue raw_bool(bool value) noexcept {
    const std::uint64_t bit = value ? 1U : 0U;
    return RawValue{.kind = RawKind::BOOL,
                    .width = BOOL_WIDTH,
                    .pattern = bit,
                    .number_u = bit};
}

RawValue raw_signed(std::int64_t value, unsigned width) noexcept {
    // Two's complement in `width` bits: the conversion to unsigned is
    // defined modulo 2^64, and the mask keeps the low bits.
    return RawValue{
        .kind = RawKind::SIGNED,
        .width = static_cast<std::uint8_t>(width),
        .pattern = static_cast<std::uint64_t>(value) & width_mask(width),
        .number_i = value};
}

RawValue raw_unsigned(std::uint64_t value, unsigned width) noexcept {
    return RawValue{.kind = RawKind::UNSIGNED,
                    .width = static_cast<std::uint8_t>(width),
                    .pattern = value & width_mask(width),
                    .number_u = value};
}

RawValue raw_float32(float value) noexcept {
    return RawValue{.kind = RawKind::FLOAT,
                    .width = FLOAT32_WIDTH,
                    .pattern = std::bit_cast<std::uint32_t>(value),
                    .number_f = static_cast<double>(value)};
}

RawValue raw_float64(double value) noexcept {
    return RawValue{.kind = RawKind::FLOAT,
                    .width = FLOAT64_WIDTH,
                    .pattern = std::bit_cast<std::uint64_t>(value),
                    .number_f = value};
}

RawValue raw_string(std::span<const std::byte> text) noexcept {
    return RawValue{.kind = RawKind::STRING, .bytes = text};
}

RawValue raw_time(std::int64_t device_ns) noexcept {
    return RawValue{.kind = RawKind::TIME,
                    .width = FLOAT64_WIDTH,
                    .pattern = static_cast<std::uint64_t>(device_ns),
                    .number_i = device_ns};
}

}  // namespace scada::driver
