#include "scada/driver/identity.h"

#include "scada/common/static_string.h"
#include "scada/common/status.h"
#include "scada/driver/tag_value.h"
#include "scada/driver/text_pattern.h"
#include "scada/tag/types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace scada::driver {
namespace {

using tag::TagType;

// 2^63 and 2^64, exactly representable as doubles.
constexpr double TWO_63 = 9'223'372'036'854'775'808.0;
constexpr double TWO_64 = 18'446'744'073'709'551'616.0;

template <class Number>
ValueOrder order(Number left, Number right) noexcept {
    if (left < right) {
        return ValueOrder::LESS;
    }
    return right < left ? ValueOrder::GREATER : ValueOrder::EQUAL;
}

// The order of a number whose whole part is `whole`, against `fraction`, the
// fraction of the other number it equalled in whole parts: above, below or
// the same.
ValueOrder by_fraction(double fraction) noexcept {
    if (fraction > 0.0) {
        return ValueOrder::LESS;  // the integer is below the double
    }
    return fraction < 0.0 ? ValueOrder::GREATER : ValueOrder::EQUAL;
}

// `value` against `real`, exactly: the double is never rounded to the
// integer's type, nor the integer to a double.
ValueOrder signed_against_double(std::int64_t value, double real) noexcept {
    if (std::isnan(real)) {
        return ValueOrder::UNORDERED;
    }
    if (real >= TWO_63) {
        return ValueOrder::LESS;
    }
    if (real < -TWO_63) {
        return ValueOrder::GREATER;
    }
    const double whole = std::trunc(real);  // exact: |real| < 2^63
    const auto truncated = static_cast<std::int64_t>(whole);
    const ValueOrder by_whole = order(value, truncated);
    return by_whole != ValueOrder::EQUAL ? by_whole : by_fraction(real - whole);
}

ValueOrder unsigned_against_double(std::uint64_t value, double real) noexcept {
    if (std::isnan(real)) {
        return ValueOrder::UNORDERED;
    }
    if (real < 0.0) {
        return ValueOrder::GREATER;
    }
    if (real >= TWO_64) {
        return ValueOrder::LESS;
    }
    const double whole = std::trunc(real);  // exact: 0 <= real < 2^64
    const auto truncated = static_cast<std::uint64_t>(whole);
    const ValueOrder by_whole = order(value, truncated);
    return by_whole != ValueOrder::EQUAL ? by_whole : by_fraction(real - whole);
}

ValueOrder reversed(ValueOrder order) noexcept {
    switch (order) {
        case ValueOrder::LESS:
            return ValueOrder::GREATER;
        case ValueOrder::GREATER:
            return ValueOrder::LESS;
        case ValueOrder::EQUAL:
        case ValueOrder::UNORDERED:
        default:
            return order;
    }
}

bool is_number(TagType type) noexcept {
    return type == TagType::INT64 || type == TagType::UINT64 ||
           type == TagType::FLOAT64;
}

// A signed integer against an unsigned one, without conversion.
ValueOrder signed_against_unsigned(std::int64_t value,
                                   std::uint64_t other) noexcept {
    if (std::cmp_less(value, other)) {
        return ValueOrder::LESS;
    }
    return std::cmp_equal(value, other) ? ValueOrder::EQUAL
                                        : ValueOrder::GREATER;
}

ValueOrder doubles(double left, double right) noexcept {
    if (std::isnan(left) || std::isnan(right)) {
        return ValueOrder::UNORDERED;
    }
    return order(left, right);
}

// Two numbers of any of the three number types: each pair by its own rule,
// the mirrored pairs by reversing it.
ValueOrder compare_numbers(const TagValue& left,
                           const TagValue& right) noexcept {
    if (left.type == TagType::INT64) {
        switch (right.type) {
            case TagType::INT64:
                return order(left.i, right.i);
            case TagType::UINT64:
                return signed_against_unsigned(left.i, right.u);
            default:
                return signed_against_double(left.i, right.f);
        }
    }
    if (left.type == TagType::UINT64) {
        switch (right.type) {
            case TagType::INT64:
                return reversed(signed_against_unsigned(right.i, left.u));
            case TagType::UINT64:
                return order(left.u, right.u);
            default:
                return unsigned_against_double(left.u, right.f);
        }
    }
    switch (right.type) {  // `left` is FLOAT64
        case TagType::INT64:
            return reversed(signed_against_double(right.i, left.f));
        case TagType::UINT64:
            return reversed(unsigned_against_double(right.u, left.f));
        default:
            return doubles(left.f, right.f);
    }
}

}  // namespace

ValueOrder compare_values(const TagValue& left,
                          const TagValue& right) noexcept {
    if (is_number(left.type) && is_number(right.type)) {
        return compare_numbers(left, right);
    }
    if (left.type != right.type) {
        return ValueOrder::UNORDERED;
    }
    if (left.type == TagType::BOOL) {
        return order(left.b, right.b);
    }
    if (left.type == TagType::STRING) {
        return left.text == right.text ? ValueOrder::EQUAL
                                       : ValueOrder::UNORDERED;
    }
    return ValueOrder::UNORDERED;
}

TagValue as_value(const ExpectedValue& value) noexcept {
    return TagValue{.type = value.type,
                    .b = value.b,
                    .i = value.i,
                    .u = value.u,
                    .f = value.f,
                    .text = value.text};
}

bool identity_matches(const IdentityCheck& check,
                      const TagValue& read) noexcept {
    const auto equals = [&](const ExpectedValue& value) {
        return compare_values(read, as_value(value)) == ValueOrder::EQUAL;
    };
    switch (check.kind) {
        case IdentityCheck::Kind::EQUALS:
            return !check.values.empty() && equals(check.values.front());
        case IdentityCheck::Kind::IN:
            return std::any_of(check.values.begin(), check.values.end(),
                               equals);
        case IdentityCheck::Kind::RANGE: {
            if (check.values.size() != 2) {
                return false;
            }
            const ValueOrder low =
                compare_values(read, as_value(check.values[0]));
            const ValueOrder high =
                compare_values(read, as_value(check.values[1]));
            return (low == ValueOrder::GREATER || low == ValueOrder::EQUAL) &&
                   (high == ValueOrder::LESS || high == ValueOrder::EQUAL);
        }
        case IdentityCheck::Kind::PATTERN:
            return read.type == TagType::STRING &&
                   text_matches(check.pattern, read.text);
        default:
            return false;
    }
}

namespace {

Status refused(StaticString what) noexcept {
    return {StatusCode::INVALID_ARGUMENT, what};
}

// An expected value fit for a point of `type`: its type, no NaN, a text
// that the cleaned text of the device could match (review S3).
Status check_value(const ExpectedValue& value, TagType type) noexcept {
    if (value.type != type) {
        return refused("an expected value of another type than its point");
    }
    if (type == TagType::FLOAT64 && std::isnan(value.f)) {
        return refused("an expected value that is NaN");
    }
    if (type == TagType::STRING) {
        if (value.text.size() > TAG_STRING_MAX_BYTES) {
            return refused("an expected text over 250 bytes");
        }
        if (!std::all_of(value.text.begin(), value.text.end(), [](char byte) {
                return byte >= ' ' && byte <= '~';
            })) {
            return refused(
                "an expected text with a byte outside 0x20 to 0x7E, which "
                "the cleaned text never holds");
        }
    }
    return {};
}

// A pattern the matcher takes as written (ruling item 5).
Status check_pattern(std::string_view pattern) noexcept {
    if (pattern.size() > TEXT_PATTERN_MAX_BYTES) {
        return refused("a pattern over 250 bytes");
    }
    // A single `\` at the end escapes nothing: an even run of them is
    // literal backslashes, an odd one ends in a lone one.
    std::size_t backslashes = 0;
    for (auto at = pattern.rbegin(); at != pattern.rend() && *at == '\\';
         ++at) {
        ++backslashes;
    }
    if (backslashes % 2 != 0) {
        return refused("a pattern that ends in a single backslash");
    }
    return {};
}

}  // namespace

Status validate_identity_check(const IdentityCheck& check,
                               TagType point_type) noexcept {
    for (const ExpectedValue& value : check.values) {
        if (Status status = check_value(value, point_type); !status.ok()) {
            return status;
        }
    }
    switch (check.kind) {
        case IdentityCheck::Kind::EQUALS:
            return check.values.size() == 1
                       ? Status{}
                       : refused("an `equals` check without one value");
        case IdentityCheck::Kind::IN:
            return !check.values.empty()
                       ? Status{}
                       : refused("an `in` check without a value");
        case IdentityCheck::Kind::RANGE: {
            if (check.values.size() != 2) {
                return refused("a `range` check without two values");
            }
            const ValueOrder order = compare_values(as_value(check.values[0]),
                                                    as_value(check.values[1]));
            return order == ValueOrder::LESS || order == ValueOrder::EQUAL
                       ? Status{}
                       : refused(
                             "a `range` check whose low end is above "
                             "its high end");
        }
        case IdentityCheck::Kind::PATTERN:
            if (point_type != TagType::STRING) {
                return refused(
                    "a `pattern` check on a point that is no "
                    "STRING");
            }
            return check_pattern(check.pattern);
    }
    return refused("an identity check of no known kind");
}

Status validate_restart_rule(const RestartRule& rule, TagType point_type,
                             bool time_point) noexcept {
    switch (rule.kind) {
        case RestartRule::Kind::DECREASES:
            if (point_type == TagType::BOOL || point_type == TagType::STRING ||
                time_point) {
                return refused(
                    "`decreases` on a BOOL, STRING or time point (no order "
                    "that a restart resets)");
            }
            return {};
        case RestartRule::Kind::CHANGES:
            return {};
        case RestartRule::Kind::EQUALS:
            return check_value(rule.value, point_type);
    }
    return refused("a restart rule of no known kind");
}

RestartWatch::RestartWatch(std::span<const RestartRule> rules)
    : rules_(rules), last_(rules.size()) {}

void RestartWatch::reset() noexcept {
    for (Last& last : last_) {
        last.seen = false;
        last.flag_set = false;
    }
}

bool RestartWatch::sample(std::uint32_t point, const TagValue& value) noexcept {
    // A value that orders against nothing, itself included (a NaN), or a
    // STRING longer than the tag database holds, is not valid: it neither
    // fires a rule nor moves its baseline (review S5).
    if (compare_values(value, value) != ValueOrder::EQUAL ||
        (value.type == TagType::STRING &&
         value.text.size() > TAG_STRING_MAX_BYTES)) {
        return false;
    }
    bool restarted = false;
    for (std::size_t index = 0; index < rules_.size(); ++index) {
        const RestartRule& rule = rules_[index];
        if (rule.point != point) {
            continue;
        }
        Last& last = last_[index];
        TagValue before = last.value;
        before.text = std::string_view(last.text.data(), last.text_length);
        const ValueOrder against_before = compare_values(value, before);
        switch (rule.kind) {
            case RestartRule::Kind::DECREASES:
                // Every decrease, a counter's wrap included: a false restart
                // once per wrap period costs a re-check; a missed one would
                // leave a swapped device unchecked (ruling change, review M6).
                restarted = restarted ||
                            (last.seen && against_before == ValueOrder::LESS);
                break;
            case RestartRule::Kind::CHANGES:
                restarted = restarted ||
                            (last.seen && against_before != ValueOrder::EQUAL);
                break;
            case RestartRule::Kind::EQUALS: {
                const bool set = compare_values(value, as_value(rule.value)) ==
                                 ValueOrder::EQUAL;
                // The first sample is the baseline: a flag found set then is
                // no rise.
                restarted = restarted || (last.seen && set && !last.flag_set);
                last.flag_set = set;
                break;
            }
            default:
                break;
        }
        last.seen = true;
        last.value = value;
        last.value.text = {};
        last.text_length = std::min(value.text.size(), last.text.size());
        std::copy_n(value.text.data(), last.text_length, last.text.data());
    }
    return restarted;
}

}  // namespace scada::driver
