#ifndef SCADA_DRIVER_RAW_VALUE_H
#define SCADA_DRIVER_RAW_VALUE_H

// What a protocol's decode delivers for one point, before the framework's
// processing (driver interface contract 5.1, 5.2).

#include "scada/common/realtime.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::driver {

// The outcome of one point in one answer (contract 5.2).
enum class Outcome : std::uint8_t {
    VALUE = 1,           // the device answered with a value
    NO_VALUE = 2,        // the device answered with its "no value" marker
    ABSENT = 3,          // the point is not on this device
    DEVICE_INVALID = 4,  // the device reported the value invalid
    COMM_FAIL = 5,       // no usable answer
};

// How the raw number of a sample is to be read (contract 5.1: "the raw
// number the protocol read from it with the binding's raw type").
enum class RawKind : std::uint8_t {
    BOOL = 1,      // `number_u` is 0 or 1
    SIGNED = 2,    // `number_i`
    UNSIGNED = 3,  // `number_u`
    FLOAT = 4,     // `number_f`; `width` 32 for a float32, 64 for a float64
    STRING = 5,    // `bytes`, UTF-8 as the protocol decoded it
    TIME = 6,      // `number_i`: nanoseconds since 1970-01-01 on the
                   // device's clock, converted by the point's device time
};

// The largest width of a raw pattern, in bits (contract 5.1).
inline constexpr unsigned RAW_MAX_WIDTH = 64;

// A raw value. `pattern` holds the value's bits after the binding's byte
// order, word order and parts, before any numeric interpretation, in its low
// `width` bits; the bits above are 0. Invalid markers compare against it
// (contract 5.1, step 1). The number field named by `kind` holds the number
// the protocol read from the pattern; for kinds the protocol computes (a
// modulo-10,000 value, a time) it need not be a plain reading of `pattern`.
//
// `bytes` is borrowed: it points into the frame being decoded and is valid
// only during the delivery that carries it.
struct RawValue {
    RawKind kind = RawKind::UNSIGNED;
    std::uint8_t width = 0;
    std::uint64_t pattern = 0;
    std::int64_t number_i = 0;
    std::uint64_t number_u = 0;
    double number_f = 0.0;
    // NOLINTBEGIN(readability-redundant-member-init): GCC\'s
    // -Wmissing-field-initializers needs it where a designated initializer
    // leaves the member out.
    std::span<const std::byte> bytes{};
    // NOLINTEND(readability-redundant-member-init)
};

// Each returns the raw value of a number of the stated raw type, with its
// pattern. They are the forms a protocol's decode uses; a protocol may fill
// a RawValue itself when its pattern and number differ.
SCADA_RT [[nodiscard]] RawValue raw_bool(bool value) noexcept;
// `width` is 8, 16, 32 or 64; `value` must fit it.
SCADA_RT [[nodiscard]] RawValue raw_signed(std::int64_t value,
                                           unsigned width) noexcept;
SCADA_RT [[nodiscard]] RawValue raw_unsigned(std::uint64_t value,
                                             unsigned width) noexcept;
SCADA_RT [[nodiscard]] RawValue raw_float32(float value) noexcept;
SCADA_RT [[nodiscard]] RawValue raw_float64(double value) noexcept;
// `text` is borrowed, as `RawValue::bytes`.
SCADA_RT [[nodiscard]] RawValue raw_string(
    std::span<const std::byte> text) noexcept;
// `device_ns`: nanoseconds since 1970-01-01 as the device's clock reads.
SCADA_RT [[nodiscard]] RawValue raw_time(std::int64_t device_ns) noexcept;

// Returns the mask of the low `width` bits (all bits for 64 or more).
SCADA_RT [[nodiscard]] constexpr std::uint64_t width_mask(
    unsigned width) noexcept {
    return width >= RAW_MAX_WIDTH ? ~std::uint64_t{0}
                                  : (std::uint64_t{1} << width) - 1U;
}

}  // namespace scada::driver

#endif  // SCADA_DRIVER_RAW_VALUE_H
