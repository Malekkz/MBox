#include "scada/driver/command_check.h"

#include "scada/driver/compiled.h"
#include "scada/driver/failure.h"
#include "scada/driver/tag_value.h"
#include "scada/driver/write_conversion.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/types.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>

namespace scada::driver {

namespace {

constexpr std::uint32_t RAW_MAX = 0xFFFF;
constexpr int REASON_SHIFT = 16;
constexpr std::int64_t DAY_NS = 86'400'000'000'000;
// 2^63 and 2^64 as doubles: the first values outside INT64 and UINT64.
constexpr double TWO_63 = 9223372036854775808.0;
constexpr double TWO_64 = 18446744073709551616.0;

constexpr Answer rejected(Reason reason) noexcept {
    return Answer{.status = SCADA_RESULT_REJECTED, .reason = reason};
}

CheckOutcome answer(Answer given) noexcept {
    return CheckOutcome{.order = false, .answer = given};
}

// The request's value as a value of `type`; nothing when its value type is
// another (check 4: "of the tag's type").
std::optional<TagValue> request_value(const scada_write_request& request,
                                      tag::TagType type) noexcept {
    if (request.value_type != static_cast<std::uint8_t>(type)) {
        return std::nullopt;
    }
    switch (type) {
        case tag::TagType::BOOL:
            return bool_value(request.value != 0);
        case tag::TagType::INT64:
            return int64_value(std::bit_cast<std::int64_t>(request.value));
        case tag::TagType::UINT64:
            return uint64_value(request.value);
        case tag::TagType::FLOAT64:
            return float64_value(std::bit_cast<double>(request.value));
        case tag::TagType::STRING:
            break;
    }
    return std::nullopt;
}

// `value` >= `bound` and `value` <= `bound`, exactly, for every type: an
// INT64 or UINT64 value is never rounded to a double to be compared.
bool at_least(const TagValue& value, double bound) noexcept {
    switch (value.type) {
        case tag::TagType::INT64:
            if (bound <= -TWO_63) {
                return true;
            }
            if (bound >= TWO_63) {
                return false;
            }
            return value.i >= static_cast<std::int64_t>(std::ceil(bound));
        case tag::TagType::UINT64:
            if (bound <= 0.0) {
                return true;
            }
            if (bound >= TWO_64) {
                return false;
            }
            return value.u >= static_cast<std::uint64_t>(std::ceil(bound));
        case tag::TagType::FLOAT64:
            return value.f >= bound;
        case tag::TagType::BOOL:
            return (value.b ? 1.0 : 0.0) >= bound;
        case tag::TagType::STRING:
            break;
    }
    return false;
}

bool at_most(const TagValue& value, double bound) noexcept {
    switch (value.type) {
        case tag::TagType::INT64:
            if (bound >= TWO_63) {
                return true;
            }
            if (bound < -TWO_63) {
                return false;
            }
            return value.i <= static_cast<std::int64_t>(std::floor(bound));
        case tag::TagType::UINT64:
            if (bound >= TWO_64) {
                return true;
            }
            if (bound < 0.0) {
                return false;
            }
            return value.u <= static_cast<std::uint64_t>(std::floor(bound));
        case tag::TagType::FLOAT64:
            return value.f <= bound;
        case tag::TagType::BOOL:
            return (value.b ? 1.0 : 0.0) <= bound;
        case tag::TagType::STRING:
            break;
    }
    return false;
}

bool within_limits(const TagValue& value, const OutputLimits& limits) noexcept {
    if ((limits.min && !at_least(value, *limits.min)) ||
        (limits.max && !at_most(value, *limits.max))) {
        return false;
    }
    if (limits.ranges.empty()) {
        return true;
    }
    return std::any_of(limits.ranges.begin(), limits.ranges.end(),
                       [&](const ValueRange& range) {
                           return at_least(value, range.lo) &&
                                  at_most(value, range.hi);
                       });
}

// One of the kind's values (8.3), "off" refused for `on_only`.
bool kind_allows(const TagValue& value, const CompiledOutput& output) noexcept {
    switch (output.kind) {
        case OutputKind::SINGLE:
            return value.type == tag::TagType::BOOL &&
                   (value.b || !output.on_only);
        case OutputKind::DOUBLE:
            // 1 open (off), 2 closed (on); 0 and 3 refused.
            return value.type == tag::TagType::INT64 &&
                   (value.i == 2 || (value.i == 1 && !output.on_only));
        case OutputKind::SETPOINT:
            return true;
        case OutputKind::STEP:
            return value.type == tag::TagType::INT64 &&
                   (value.i == 1 || value.i == -1);
        case OutputKind::PULSE:
        case OutputKind::TRIGGER:
            return value.type == tag::TagType::BOOL && value.b;
    }
    return false;
}

WriteRules write_rules(const CompiledPoint& point,
                       const CompiledOutput& output) noexcept {
    return WriteRules{
        .tag_type = point.rules.tag_type,
        .raw_kind = point.rules.raw_kind,
        .raw_width = output.raw_width,
        .invert = point.rules.invert,
        .scale = point.rules.scale ? &*point.rules.scale : nullptr,
        .enum_codes = point.rules.enum_codes,
        .has_enum = point.rules.has_enum,
    };
}

// Check 4: of the tag's type, finite, one of the kind's values, within the
// limits, convertible to raw (a setpoint; the other kinds are sent by their
// encodings, 8.3, 8.5).
bool valid_value(const scada_write_request& request, const CompiledPoint& point,
                 const CompiledOutput& output, WriteValue& converted) noexcept {
    const std::optional<TagValue> value =
        request_value(request, point.rules.tag_type);
    if (!value) {
        return false;
    }
    if (value->type == tag::TagType::FLOAT64 && !std::isfinite(value->f)) {
        return false;  // before any limit is compared (8.2)
    }
    if (!kind_allows(*value, output) || !within_limits(*value, output.limits)) {
        return false;
    }
    if (output.kind != OutputKind::SETPOINT) {
        return true;
    }
    const std::optional<WriteValue> raw =
        to_raw_value(write_rules(point, output), *value);
    if (!raw) {
        return false;
    }
    converted = *raw;
    return true;
}

// Check 3: the operation against the output's control model.
bool allowed_operation(const scada_write_request& request,
                       const CompiledOutput& output) noexcept {
    switch (request.operation) {
        case SCADA_OP_SELECT:
        case SCADA_OP_OPERATE:
        case SCADA_OP_CANCEL:
            return output.model == ControlModel::SBO;
        case SCADA_OP_DIRECT:
            return request.source != SCADA_SOURCE_OPERATOR ||
                   output.model == ControlModel::DIRECT;
        default:
            return false;
    }
}

// Check 5 (D-92; ruling O-2): the mode tag read at the check. Any read that
// is not SCADA_OK, a value that is not 1, 2 or 3, NOT_INIT, or any other
// flag fails it for every source (fail closed).
bool mode_allows(std::uint8_t source, const TagReading& mode) noexcept {
    if (mode.code != SCADA_OK || mode.value.type != SCADA_TYPE_INT64) {
        return false;
    }
    const auto stored = std::bit_cast<std::int64_t>(mode.value.value);
    const bool good = mode.value.quality == SCADA_QUALITY_GOOD &&
                      mode.value.quality_flags == 0;
    switch (source) {
        case SCADA_SOURCE_OPERATOR:
            return good &&
                   stored == static_cast<std::int64_t>(OutputMode::MANUAL);
        case SCADA_SOURCE_REMOTE:
            return good &&
                   stored == static_cast<std::int64_t>(OutputMode::REMOTE);
        case SCADA_SOURCE_AUTO: {
            // While `scada-cmd` is down its stored mode cannot change: BAD
            // with OWNER_DOWN as the only flag and a stored AUTO is AUTO.
            const bool owner_down =
                mode.value.quality == SCADA_QUALITY_BAD &&
                mode.value.quality_flags == SCADA_FLAG_OWNER_DOWN;
            return (good || owner_down) &&
                   stored == static_cast<std::int64_t>(OutputMode::AUTO);
        }
        default:
            return false;
    }
}

// Check 6 (ruling R-16): the panel input GOOD and not active; anything else
// blocks AUTO.
bool panel_allows(const TagReading& panel) noexcept {
    return panel.code == SCADA_OK && panel.value.type == SCADA_TYPE_BOOL &&
           panel.value.quality == SCADA_QUALITY_GOOD && panel.value.value == 0;
}

// Check 8 (3.5; R-12): the device's state. Until the owner answers the open
// questions of D-147, INITIALISING refuses every write (the strict interim).
// CONNECTING passes: the worker answers it at check 11.
std::optional<Answer> refused_by_state(DeviceStatus state,
                                       bool read_now) noexcept {
    switch (state) {
        case DeviceStatus::DISABLED:
        case DeviceStatus::MISMATCH:
            return rejected(Reason::CONFIG_MISMATCH);
        case DeviceStatus::DEMOTED:
            if (read_now) {
                return std::nullopt;  // a probe instead (8.14)
            }
            return rejected(Reason::DEVICE_UNAVAILABLE);
        case DeviceStatus::REFUSING:
        case DeviceStatus::INITIALISING:
            return rejected(Reason::DEVICE_UNAVAILABLE);
        case DeviceStatus::CONNECTING:
        case DeviceStatus::RUNNING:
            return std::nullopt;
    }
    return rejected(Reason::DEVICE_UNAVAILABLE);
}

CheckOutcome check_read_now(const CheckTarget& target) noexcept {
    // Check 2: a readable point of this driver.
    if (target.device == nullptr || target.point == nullptr ||
        target.point->access == Access::WO || !target.point->poll_group) {
        return answer(rejected(Reason::UNKNOWN_TARGET));
    }
    if (const std::optional<Answer> refused =
            refused_by_state(target.state, true)) {
        return answer(*refused);
    }
    return CheckOutcome{.order = true};
}

}  // namespace

std::int32_t protocol_error(Reason reason, std::uint32_t raw) noexcept {
    const std::uint32_t low = std::min(raw, RAW_MAX);
    return static_cast<std::int32_t>(
        (static_cast<std::uint32_t>(reason) << REASON_SHIFT) | low);
}

WriteLimiter::WriteLimiter(WriteLimits limits)
    : limits_(limits), times_(limits.max_per_day) {}

bool WriteLimiter::allows(std::int64_t now_ns) const noexcept {
    if (any_ && limits_.min_interval_ns > 0 &&
        now_ns - last_ns_ < limits_.min_interval_ns) {
        return false;
    }
    if (limits_.max_per_day == 0 || counted_ < times_.size()) {
        return true;
    }
    // Full: the oldest of the last `max_per_day` writes must be a day old.
    return now_ns - times_[next_] >= DAY_NS;
}

void WriteLimiter::count(std::int64_t now_ns) noexcept {
    any_ = true;
    last_ns_ = now_ns;
    if (times_.empty()) {
        return;
    }
    times_[next_] = now_ns;
    next_ = (next_ + 1) % times_.size();
    counted_ = std::min(counted_ + 1, times_.size());
}

CheckOutcome check_request(const scada_write_request& request,
                           const CheckTarget& target, CommandReads& reads,
                           std::int64_t now_ns) noexcept {
    if (request.operation == SCADA_OP_READ_NOW) {
        return check_read_now(target);
    }
    // 2: a point of this driver with an output.
    if (target.device == nullptr || target.point == nullptr ||
        target.output == nullptr) {
        return answer(rejected(Reason::UNKNOWN_TARGET));
    }
    const CompiledOutput& output = *target.output;
    // 3: the operation against the kind and control model.
    if (!allowed_operation(request, output)) {
        return answer(rejected(Reason::WRONG_CONTROL_MODEL));
    }
    // 4: the value (a CANCEL carries none).
    WriteValue converted{};
    if (request.operation != SCADA_OP_CANCEL &&
        !valid_value(request, *target.point, output, converted)) {
        return answer(rejected(Reason::INVALID_VALUE));
    }
    // 5: source against mode.
    if (!mode_allows(request.source, reads.mode(output))) {
        return answer(Answer{.status = SCADA_RESULT_NOT_AUTHORISED});
    }
    // 6: AUTO against the area's local panel.
    if (request.source == SCADA_SOURCE_AUTO && output.local_blocks_auto &&
        !panel_allows(reads.panel(output))) {
        return answer(Answer{.status = SCADA_RESULT_NOT_AUTHORISED,
                             .reason = Reason::LOCAL_AUTHORITY});
    }
    // 7: selections. None is live until slice 7 (D-150).
    switch (request.operation) {
        case SCADA_OP_SELECT:
            return answer(rejected(Reason::NOT_SUPPORTED));
        case SCADA_OP_OPERATE:
            return answer(rejected(Reason::NOT_SELECTED));
        case SCADA_OP_CANCEL:
            // Nothing is sent (8.6, rule 6).
            return answer(Answer{.status = SCADA_RESULT_OK,
                                 .reason = Reason::NOTHING_TO_CANCEL});
        default:
            break;  // a DIRECT: no selection is live, so no SELECTED
    }
    // 8: the device's state.
    if (const std::optional<Answer> refused =
            refused_by_state(target.state, false)) {
        return answer(*refused);
    }
    // 9: the point's write limits.
    if (target.limiter != nullptr && !target.limiter->allows(now_ns)) {
        return answer(rejected(Reason::RATE_LIMITED));
    }
    return CheckOutcome{.order = true, .value = converted};
}

}  // namespace scada::driver
