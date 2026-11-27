#ifndef SCADA_DRIVER_IDENTITY_H
#define SCADA_DRIVER_IDENTITY_H

// A device's identity checks and restart indications (driver interface
// contract 3.6, 7.6; configuration schemas contract 4.8, 4.9; decision
// D-145), as the compile step gives them to the driver: each names one point
// of the device, by index. A check on a protocol `address` reaches the
// driver as a point of its own, without a tag.

#include "scada/common/realtime.h"
#include "scada/driver/tag_value.h"
#include "scada/tag/types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace scada::driver {

// A value of a check or a rule, of the point's own type (the compile step
// converts the configured value to it). A STRING's text is owned.
struct ExpectedValue {
    tag::TagType type = tag::TagType::INT64;
    bool b = false;
    std::int64_t i = 0;
    std::uint64_t u = 0;
    double f = 0.0;
    // NOLINTNEXTLINE(readability-redundant-member-init): GCC's -Wmissing-field-initializers needs it where a designated initializer leaves the member out
    std::string text{};
};

// True when `read` is `expected`: the same type and value. A FLOAT64 NaN is
// equal to nothing.
[[nodiscard]] bool same_value(const ExpectedValue& expected,
                              const TagValue& read) noexcept;

// Below (negative), equal (0) or above (positive) `bound`, for two values of
// one numeric type or BOOL; `comparable` is false for any other pair, a
// STRING or a NaN included.
[[nodiscard]] int compare_value(const TagValue& read,
                                const ExpectedValue& bound,
                                bool& comparable) noexcept;

// One entry of `identify` (CS 4.8).
struct IdentityCheck {
    enum class Kind : std::uint8_t {
        EQUALS = 1,  // `values[0]`
        IN,          // any of `values`
        RANGE,       // `values[0]` to `values[1]`, both included
        PATTERN,     // `pattern`, a text pattern (text_pattern.h), STRING
    };
    std::uint32_t point = 0;  // an index into the device's points
    Kind kind = Kind::EQUALS;
    std::vector<ExpectedValue> values;
    std::string pattern;
};

// True when `read`, the value of the check's point, passes `check`.
[[nodiscard]] bool identity_matches(const IdentityCheck& check,
                                    const TagValue& read) noexcept;

// One entry of `restart_indication` (CS 4.9).
struct RestartRule {
    enum class Kind : std::uint8_t {
        DECREASES = 1,  // the value goes below the one before
        CHANGES,        // the value differs from the one before
        EQUALS,         // the value becomes `value` (a power-up flag)
    };
    std::uint32_t point = 0;  // an index into the device's points
    Kind kind = Kind::DECREASES;
    // For EQUALS.
    // NOLINTNEXTLINE(readability-redundant-member-init): GCC's -Wmissing-field-initializers needs it where a designated initializer leaves the member out
    ExpectedValue value{};
};

// The restart rules of one device, over the valid samples of their points.
// The first sample after `reset` is the baseline of DECREASES and CHANGES;
// EQUALS holds on its rising edge only, so that a flag the device leaves set
// is one restart, not one per poll (D-145).
class RestartWatch {
public:
    // `rules` is borrowed and outlives the watch. Allocates.
    explicit RestartWatch(std::span<const RestartRule> rules);

    // A new connection: every rule starts again from its next sample. A
    // power-up flag found set then is not a restart: the identity check of
    // the connect covers the device as a restart would.
    SCADA_RT void reset() noexcept;

    // A valid sample `value` of point `point`; true when a rule on that
    // point holds, which makes the device restarted (7.6).
    SCADA_RT bool sample(std::uint32_t point, const TagValue& value) noexcept;

private:
    struct Last {
        bool seen = false;
        TagValue value;  // a STRING's text is in `text`, not in `value`
        std::array<char, TAG_STRING_MAX_BYTES> text{};
        std::size_t text_length = 0;
        bool flag_set = false;
    };

    std::span<const RestartRule> rules_;
    std::vector<Last> last_;  // one per rule
};

}  // namespace scada::driver

#endif  // SCADA_DRIVER_IDENTITY_H
