#include "scada/driver/point_rules.h"

#include "scada/common/static_string.h"
#include "scada/common/status.h"
#include "scada/driver/raw_value.h"
#include "scada/tag/types.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace scada::driver {

namespace {

Status refused(StaticString what) noexcept {
    return {StatusCode::INVALID_ARGUMENT, what};
}

bool slot_ok(const PointRules& rules, std::uint8_t slot) noexcept {
    return slot < rules.companions.size();
}

bool field_ok(bool has_field, std::uint8_t low, std::uint8_t high) noexcept {
    return !has_field || (low <= high && high <= MAX_BIT_NUMBER);
}

template <typename T, typename Key>
bool strictly_sorted(const std::vector<T>& items, Key key) noexcept {
    return std::adjacent_find(items.begin(), items.end(),
                              [&](const T& left, const T& right) {
                                  return key(left) >= key(right);
                              }) == items.end();
}

bool is_integer_tag(tag::TagType type) noexcept {
    return type == tag::TagType::INT64 || type == tag::TagType::UINT64;
}

}  // namespace

bool is_integer_scaling(const ScaleRule& scale) noexcept {
    if (!scale.integer) {
        return false;
    }
    return scale.form == ScaleRule::Form::GAIN || scale.divide_by == 1;
}

namespace {

// The companion references and the bit and field numbers.
Status validate_references(const PointRules& rules) noexcept {
    if (rules.exponent.has_value() &&
        !slot_ok(rules, rules.exponent->companion)) {
        return refused("exponent companion slot out of range");
    }
    if (rules.time_companion.has_value() &&
        !slot_ok(rules, *rules.time_companion)) {
        return refused("time companion slot out of range");
    }
    for (const QualityRule& rule : rules.quality_rules) {
        if ((!rule.self && !slot_ok(rules, rule.companion)) ||
            !field_ok(rule.has_field, rule.lo, rule.hi) ||
            !strictly_sorted(rule.entries, [](const QualityRule::Entry& entry) {
                return entry.key;
            })) {
            return refused("quality rule not valid");
        }
    }
    for (const HealthRule& rule : rules.health) {
        if (!slot_ok(rules, rule.companion) ||
            !field_ok(rule.has_field, rule.lo, rule.hi) ||
            !strictly_sorted(rule.good,
                             [](std::int64_t value) { return value; })) {
            return refused("health rule not valid");
        }
    }
    if (rules.bits.size() > MAX_POINT_BITS ||
        std::any_of(
            rules.bits.begin(), rules.bits.end(),
            [](const BitRule& bit) { return bit.bit > MAX_BIT_NUMBER; })) {
        return refused("bit number out of range");
    }
    return {};
}

}  // namespace

Status validate_point_rules(const PointRules& rules) noexcept {
    if (const Status status = validate_references(rules); !status.ok()) {
        return status;
    }
    // CF-504 as the compile step gives it (`check_value_type` in
    // libscada-conf, decision D-115): a string raw is held by a STRING tag
    // only, and a STRING tag holds nothing else; a BOOL tag holds a bool raw
    // only. The two must change together (#169).
    if ((rules.tag_type == tag::TagType::STRING) !=
        (rules.raw_kind == RawKind::STRING)) {
        return refused(
            "a STRING tag needs a string raw type, and only it (CF-504)");
    }
    if (rules.tag_type == tag::TagType::BOOL &&
        rules.raw_kind != RawKind::BOOL) {
        return refused("a BOOL tag needs a bool raw type (CF-504)");
    }
    if (rules.double_point.has_value() &&
        !strictly_sorted(
            rules.double_point->states,
            [](const DoublePointRule::Entry& entry) { return entry.raw; })) {
        return refused("double point states not sorted");
    }
    if (rules.has_enum &&
        (rules.tag_type != tag::TagType::INT64 || rules.scale.has_value() ||
         !strictly_sorted(rules.enum_codes,
                          [](std::int64_t value) { return value; }))) {
        return refused("enumeration not valid");
    }
    if (rules.scale.has_value() &&
        rules.scale->form == ScaleRule::Form::DIVIDE_BY &&
        rules.scale->divide_by == 0) {
        return refused("divide_by must be positive");
    }
    if (is_integer_tag(rules.tag_type) &&
        ((rules.scale.has_value() && !is_integer_scaling(*rules.scale)) ||
         rules.exponent.has_value())) {
        return refused("integer value type needs an integer scaling (CF-517)");
    }
    if (is_integer_tag(rules.tag_type) && rules.raw_kind == RawKind::FLOAT &&
        !rules.has_enum && !rules.double_point.has_value()) {
        return refused("integer value type on a floating raw type (CF-504)");
    }
    if ((rules.device_time.local && rules.device_time.zone == nullptr) ||
        (rules.companion_time.local && rules.companion_time.zone == nullptr)) {
        return refused("local device time without a zone");
    }
    return {};
}

}  // namespace scada::driver
