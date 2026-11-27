#ifndef SCADA_DRIVER_IDENTITY_H
#define SCADA_DRIVER_IDENTITY_H

// A device's identity checks and restart indications (driver interface
// contract 3.6, 7.6; configuration schemas contract 4.8, 4.9; decision
// D-147), as the compile step gives them to the driver: each names one point
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

// How two values order (D-147).
enum class ValueOrder : std::uint8_t { LESS = 1, EQUAL, GREATER, UNORDERED };

// `left` against `right`, exactly: INT64, UINT64 and FLOAT64 against each
// other by their mathematical values (a signed and an unsigned integer
// without conversion, an integer and a FLOAT64 without rounding either), a
// BOOL against a BOOL (false below true), a STRING equal to a STRING of the
// same bytes and else UNORDERED, a NaN UNORDERED, and any other pair
// UNORDERED. The one comparison of identity checks and restart rules.
SCADA_RT [[nodiscard]] ValueOrder compare_values(
    const TagValue& left, const TagValue& right) noexcept;

// `value` as a TagValue; a STRING's text points into `value`.
SCADA_RT [[nodiscard]] TagValue as_value(const ExpectedValue& value) noexcept;

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

// True when `read`, the value of the check's point, passes `check`. A
// STRING is the cleaned text of identity_text.h, and the pattern is matched
// against it.
SCADA_RT [[nodiscard]] bool identity_matches(const IdentityCheck& check,
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

// The restart rules of one device, over the valid samples of their points
// (ProcessedSample::rule_valid and rule_value: before a counter's hold). A
// sample that is not valid is not given: it neither fires a rule nor moves
// its baseline, and an `equals` edge is taken across it. The baseline is
// the first valid sample after the watch was made or `reset`, which the
// engine does at an attach and a reload of the device only: it survives a
// lost connection, so that a device that restarts and drops its connection
// is counted (D-147). EQUALS holds on its rising edge only, so that a flag
// the device leaves set is one restart, not one per poll.
class RestartWatch {
public:
    // `rules` is borrowed and outlives the watch. Allocates.
    explicit RestartWatch(std::span<const RestartRule> rules);

    // An attach or a reload of the device: every rule starts again from
    // its next valid sample.
    SCADA_RT void reset() noexcept;

    // A valid sample `value` of point `point`; `counter_wrapped` when the
    // point is a counter whose decrease processing read as a wrap (contract
    // 5.4), which DECREASES does not take for a restart. True when a rule on
    // that point holds: the device restarted (7.6).
    SCADA_RT bool sample(std::uint32_t point, const TagValue& value,
                         bool counter_wrapped) noexcept;

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
