#ifndef SCADA_DRIVER_WRITE_CONVERSION_H
#define SCADA_DRIVER_WRITE_CONVERSION_H

// Engineering value to raw value for a write (driver interface contract
// 8.4; configuration schemas contract 7.2, "From engineering value to raw
// value"). Review by a person: this is command handling.
//
// A conversion that cannot be made exactly as the contract states is
// refused; the caller answers the request REJECTED with INVALID_VALUE
// (contract 8.2, check 4). Floating-point contraction is off for this code
// (decision D-98).
//
// These functions give the raw value of a `setpoint` and of a `single`
// written as its value. A `double`, a `step`, a `pulse`, a `trigger` and
// every `on`/`off` encoding are written by their encodings (contract 8.3,
// 8.5), not through to_raw_value. A protocol's encode sends the `pattern` of
// the RawValue, never its number: a NaN payload or a release pattern does
// not survive a conversion through a number.

#include "scada/common/realtime.h"
#include "scada/driver/point_rules.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/tag_value.h"
#include "scada/tag/types.h"

#include <cstdint>
#include <optional>
#include <span>

namespace scada::driver {

// What a write of one point converts to: the point's tag type, its raw
// type, and the scaling it is read with.
struct WriteRules {
    tag::TagType tag_type = tag::TagType::INT64;
    // BOOL (width 1), SIGNED or UNSIGNED (8, 16, 32 or 64), FLOAT (32 or
    // 64); any other kind or width is refused.
    RawKind raw_kind = RawKind::UNSIGNED;
    std::uint8_t raw_width = 0;
    bool invert = false;               // BOOL points
    const ScaleRule* scale = nullptr;  // borrowed; null: no `scale`
    // An `enum` point: the defined codes, sorted (borrowed). An enumeration
    // is written as its code and has no scale (decision D-131).
    // NOLINTBEGIN(readability-redundant-member-init): GCC's
    // -Wmissing-field-initializers needs it where a designated initializer
    // leaves the member out.
    std::span<const std::int64_t> enum_codes{};
    // NOLINTEND(readability-redundant-member-init)
    bool has_enum = false;
};

// The result of a conversion: the raw value to send, and the engineering
// value that raw value reads back as. On the binary64 path with an integer
// raw type the request is rounded to the raw step (contract 8.4), so
// `written` may differ from the request by up to half a step; the caller
// reports and compares feedback against `written`.
struct WriteValue {
    RawValue raw{};
    TagValue written{};
};

// Converts `value` to the raw value of a point with `rules`. Returns nothing
// when the value is refused: not of the tag's type, not finite, not exact on
// the integer path, not finite after conversion, outside the raw type, not
// a defined code of an enumeration, or rules that are not valid (contract
// 8.4; CF-517 for an integer tag with any other scaling, decision D-133).
SCADA_RT [[nodiscard]] std::optional<WriteValue> to_raw_value(
    const WriteRules& rules, const TagValue& value) noexcept;

// True when a write of a point with `rules` takes the exact integer path
// (CS 7.2 as ruled with CF-517): an INT64 or UINT64 tag with no scale, or
// with an integer scaling (is_integer_scaling).
SCADA_RT [[nodiscard]] bool is_integer_path(const WriteRules& rules) noexcept;

// The raw value of a configured raw pattern (`safe_state: { raw }`,
// `limits.release: { raw }`; contract 8.4: "written as the pattern, without
// conversion") for a raw type of `kind` and `width`. The pattern is the
// value's bits in that width (CS 2.3 has already turned a negative decimal
// into its two's complement); a pattern with a bit above the width is
// refused, as is a kind or width to_raw_value refuses.
SCADA_RT [[nodiscard]] std::optional<RawValue> raw_from_pattern(
    RawKind kind, std::uint8_t width, std::uint64_t pattern) noexcept;

}  // namespace scada::driver

#endif  // SCADA_DRIVER_WRITE_CONVERSION_H
