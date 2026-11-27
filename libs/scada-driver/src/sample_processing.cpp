#include "scada/driver/sample_processing.h"

#include "checked.h"
#include "scada/driver/point_rules.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/tag_value.h"
#include "scada/driver/zone_table.h"
#include "scada/tag/types.h"
#include "utf8.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace scada::driver {

namespace {

using tag::Quality;
using tag::QualityFlag;
using tag::TagType;

constexpr std::uint16_t flag(QualityFlag value) noexcept {
    return tag::to_raw(value);
}

constexpr QualityMark GOOD_MARK{};
constexpr QualityMark UNCERTAIN_MARK{.quality = Quality::UNCERTAIN};

// The worse of two marks: the higher quality value, the union of flags
// (CS 4.7: "The worst result of all rules ... is the tag's quality").
QualityMark worse(QualityMark first, QualityMark second) noexcept {
    return QualityMark{
        .quality = tag::to_raw(first.quality) >= tag::to_raw(second.quality)
                       ? first.quality
                       : second.quality,
        .flags = static_cast<std::uint16_t>(first.flags | second.flags)};
}

QualityMark bad(std::uint16_t flags) noexcept {
    return QualityMark{.quality = Quality::BAD, .flags = flags};
}

// `mark` with a quality of the tag database and only the flags in `mask`.
QualityMark masked_mark(QualityMark mark, std::uint16_t mask) noexcept {
    const Quality quality =
        tag::quality_from_raw(tag::to_raw(mark.quality)).value_or(Quality::BAD);
    return QualityMark{.quality = quality,
                       .flags = static_cast<std::uint16_t>(mark.flags & mask)};
}

// A mark a protocol gave a sample, with only the flags a protocol may give
// (DEVICE_FLAGS_MASK).
QualityMark device_mark(QualityMark mark) noexcept {
    return masked_mark(mark, DEVICE_FLAGS_MASK);
}

// A mark the configuration gave (a `quality_from` entry), with only the
// flags the configuration may give (CONFIG_FLAGS_MASK; review R2 of #157).
QualityMark config_mark(QualityMark mark) noexcept {
    return masked_mark(mark, CONFIG_FLAGS_MASK);
}

// The flags a failed companion passes to the point that depends on it
// (contract 5.5: "that outcome's flags").
std::uint16_t companion_flags(const CompanionInput& companion) noexcept {
    const std::uint16_t own = companion.outcome == Outcome::COMM_FAIL
                                  ? flag(QualityFlag::COMM_FAIL)
                                  : 0;
    // The companion's own processing gave these. SUBSTITUTED describes the
    // companion's value, not the point's, and does not pass (review R1 of
    // #157): neither mask holds it.
    const auto carried = static_cast<std::uint16_t>(
        companion.mark.flags &
        (CONFIG_FLAGS_MASK | DEVICE_FLAGS_MASK | flag(QualityFlag::COMM_FAIL)));
    return static_cast<std::uint16_t>(own | carried);
}

// Powers of ten that binary64 holds exactly, up to the exponent limit of
// CS 4.7 (exact up to 10^22).
constexpr std::array<double, 21> POWERS_OF_TEN{
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9, 1e10,
    1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20};

// 2^63 as a binary64: every integral binary64 of smaller magnitude converts
// to int64 exactly.
constexpr double TWO_TO_THE_63 = 9223372036854775808.0;

// How far a floating raw value may be from an integer and still be read as
// one (CS 5.5; decision D-131).
constexpr double INTEGER_TOLERANCE = 0.001;

constexpr unsigned FLOAT32_WIDTH = 32;
constexpr unsigned FLOAT64_WIDTH = 64;

// The raw value as a signed integer, when it is one exactly.
std::optional<std::int64_t> integer_of(const RawValue& raw) noexcept {
    switch (raw.kind) {
        case RawKind::BOOL:
        case RawKind::UNSIGNED:
            if (raw.number_u > static_cast<std::uint64_t>(
                                   std::numeric_limits<std::int64_t>::max())) {
                return std::nullopt;
            }
            return static_cast<std::int64_t>(raw.number_u);
        case RawKind::SIGNED:
            return raw.number_i;
        case RawKind::FLOAT: {
            const double value = raw.number_f;
            if (!std::isfinite(value) || std::trunc(value) != value ||
                value >= TWO_TO_THE_63 || value < -TWO_TO_THE_63) {
                return std::nullopt;
            }
            return static_cast<std::int64_t>(value);
        }
        case RawKind::STRING:
        case RawKind::TIME:
            return std::nullopt;
    }
    return std::nullopt;
}

// A binary64 number as the nearest integer, when it is within the tolerance
// of one and inside int64 (CS 5.5).
std::optional<std::int64_t> near_integer(double number) noexcept {
    if (!std::isfinite(number)) {
        return std::nullopt;
    }
    const double rounded = std::round(number);
    if (std::fabs(number - rounded) > INTEGER_TOLERANCE ||
        rounded >= TWO_TO_THE_63 || rounded < -TWO_TO_THE_63) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(rounded);
}

// The raw value as a code: exact for an integer, within the tolerance for a
// floating value (enumerations and double points alike, decision D-131).
std::optional<std::int64_t> code_of(const RawValue& raw) noexcept {
    return raw.kind == RawKind::FLOAT ? near_integer(raw.number_f)
                                      : integer_of(raw);
}

// The raw value as a binary64 number.
double double_of(const RawValue& raw) noexcept {
    switch (raw.kind) {
        case RawKind::BOOL:
        case RawKind::UNSIGNED:
            return static_cast<double>(raw.number_u);
        case RawKind::SIGNED:
        case RawKind::TIME:
            return static_cast<double>(raw.number_i);
        case RawKind::FLOAT:
            return raw.number_f;
        case RawKind::STRING:
            return 0.0;
    }
    return 0.0;
}

bool is_integer_kind(RawKind kind) noexcept {
    return kind == RawKind::BOOL || kind == RawKind::SIGNED ||
           kind == RawKind::UNSIGNED;
}

bool is_integer_tag(TagType type) noexcept {
    return type == TagType::INT64 || type == TagType::UINT64;
}

// A raw value whose width a protocol left inconsistent with its kind (a
// width of 0 would make every marker miss) is not used (decision D-131).
bool width_ok(const RawValue& raw) noexcept {
    switch (raw.kind) {
        case RawKind::BOOL:
        case RawKind::SIGNED:
        case RawKind::UNSIGNED:
            return raw.width >= 1 && raw.width <= RAW_MAX_WIDTH;
        case RawKind::FLOAT:
            return raw.width == FLOAT32_WIDTH || raw.width == FLOAT64_WIDTH;
        case RawKind::STRING:
        case RawKind::TIME:
            return true;
    }
    return false;
}

detail::Wide wide_of(const RawValue& raw) noexcept {
    return raw.kind == RawKind::SIGNED
               ? detail::wide_from_signed(raw.number_i)
               : detail::wide_from_unsigned(raw.number_u);
}

// Bits `low` to `high` of `pattern`, shifted down; nothing for a field that
// is not within 0 to 63 with low <= high.
std::optional<std::uint64_t> field_of(std::uint64_t pattern, std::uint8_t low,
                                      std::uint8_t high) noexcept {
    if (low > high || high > MAX_BIT_NUMBER) {
        return std::nullopt;
    }
    const unsigned width = static_cast<unsigned>(high - low) + 1U;
    return (pattern >> low) & width_mask(width);
}

// The key a rule looks up: a field of the pattern, or the whole value.
std::optional<std::int64_t> key_of(const RawValue& raw, bool has_field,
                                   std::uint8_t low,
                                   std::uint8_t high) noexcept {
    if (!has_field) {
        return code_of(raw);
    }
    const std::optional<std::uint64_t> field = field_of(raw.pattern, low, high);
    // A field of 64 bits with its top bit set does not fit int64 and
    // matches no key.
    if (!field.has_value() ||
        *field > static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*field);
}

bool marker_matches(const InvalidMarker& marker, const RawValue& raw) noexcept {
    if (marker.any_nan) {
        return raw.kind == RawKind::FLOAT && std::isnan(raw.number_f);
    }
    if (raw.kind == RawKind::STRING) {
        return false;
    }
    return (raw.pattern & width_mask(raw.width)) == marker.pattern;
}

CompanionInput companion_at(std::span<const CompanionInput> companions,
                            std::uint8_t slot) noexcept {
    return slot < companions.size()
               ? companions[slot]
               : CompanionInput{.outcome = Outcome::COMM_FAIL};
}

// The work of one call, kept together so that the steps can be functions.
struct Work {
    const PointRules* rules = nullptr;
    PointState* state = nullptr;
    const RawSample* sample = nullptr;
    std::span<const CompanionInput> companions;
    const SampleTimes* times = nullptr;
    ProcessedSample* out = nullptr;
    QualityMark mark{};
    // A value to write was computed or chosen; otherwise the last value is
    // written again.
    bool new_value = false;
    // The new value came from the device (or a substitute), so the point is
    // no longer NOT_INIT.
    bool from_device = false;
    // The new value is the configured substitute (`on_invalid: substitute`).
    bool substituted = false;
    TagValue value{};
    // The read confirmed the value (contract 7.5).
    bool confirmed = false;
    // Set for a STRING value taken from the sample.
    std::span<const std::byte> text;
    std::int64_t source_time_ns = 0;
    std::uint16_t time_flags = 0;
};

// An UNCERTAIN companion makes what is read from it at most UNCERTAIN, with
// its flags; any other companion that is read adds nothing (decision D-131).
QualityMark uncertain_companion(const CompanionInput& companion) noexcept {
    return companion.mark.quality == Quality::UNCERTAIN
               ? QualityMark{.quality = Quality::UNCERTAIN,
                             .flags = companion_flags(companion)}
               : GOOD_MARK;
}

// A companion the value depends on (exponent, time): usable when it was
// read with a value that is not BAD; an UNCERTAIN one makes the value at
// most UNCERTAIN (contract 5.5; decision D-131).
std::optional<CompanionInput> usable_companion(Work& work,
                                               std::uint8_t slot) noexcept {
    const CompanionInput companion = companion_at(work.companions, slot);
    if (companion.outcome != Outcome::VALUE ||
        companion.mark.quality == Quality::BAD) {
        work.mark = worse(work.mark, bad(companion_flags(companion)));
        return std::nullopt;
    }
    work.mark = worse(work.mark, uncertain_companion(companion));
    return companion;
}

// Step 6: the `quality_from` rules (CS 4.7). A rule on the point's own word
// applies only to an answer with a value.
QualityMark quality_from_rules(const Work& work) noexcept {
    QualityMark mark = GOOD_MARK;
    const bool has_raw = work.sample->outcome == Outcome::VALUE;
    for (const QualityRule& rule : work.rules->quality_rules) {
        std::optional<RawValue> raw;
        if (rule.self) {
            if (!has_raw) {
                continue;
            }
            raw = work.sample->raw;
        } else {
            const CompanionInput companion =
                companion_at(work.companions, rule.companion);
            if (companion.outcome != Outcome::VALUE ||
                companion.mark.quality == Quality::BAD) {
                mark = worse(mark, bad(companion_flags(companion)));
                continue;
            }
            mark = worse(mark, uncertain_companion(companion));
            raw = companion.raw;
        }
        const std::optional<std::int64_t> key =
            key_of(*raw, rule.has_field, rule.lo, rule.hi);
        QualityMark found = config_mark(rule.otherwise);
        if (key.has_value()) {
            const auto entry = std::lower_bound(
                rule.entries.begin(), rule.entries.end(), *key,
                [](const QualityRule::Entry& candidate, std::int64_t wanted) {
                    return candidate.key < wanted;
                });
            if (entry != rule.entries.end() && entry->key == *key) {
                found = config_mark(entry->mark);
            }
        }
        mark = worse(mark, found);
    }
    return mark;
}

// Step 6: the device's `health` rules that apply to the point (CS 4.9).
QualityMark health_mark(const Work& work) noexcept {
    QualityMark mark = GOOD_MARK;
    for (const HealthRule& rule : work.rules->health) {
        const CompanionInput companion =
            companion_at(work.companions, rule.companion);
        bool good = false;
        if (companion.outcome == Outcome::VALUE &&
            companion.mark.quality != Quality::BAD && !companion.stale) {
            const std::optional<std::int64_t> key =
                key_of(companion.raw, rule.has_field, rule.lo, rule.hi);
            good = key.has_value() &&
                   std::binary_search(rule.good.begin(), rule.good.end(), *key);
            mark = worse(mark, uncertain_companion(companion));
        }
        if (!good) {
            mark = worse(
                mark, bad(rule.comm_fail ? flag(QualityFlag::COMM_FAIL) : 0));
        }
    }
    return mark;
}

// Step 3, the exponent (CS 4.7); nothing when the value is BAD.
std::optional<double> apply_exponent(Work& work,
                                     const ExponentRule& rule) noexcept {
    const std::optional<CompanionInput> companion =
        usable_companion(work, rule.companion);
    if (!companion.has_value()) {
        return std::nullopt;
    }
    const std::optional<std::int64_t> exponent = integer_of(companion->raw);
    if (!exponent.has_value() || *exponent < EXPONENT_MIN ||
        *exponent > EXPONENT_MAX) {
        work.mark = worse(work.mark, bad(flag(QualityFlag::OUT_OF_RANGE)));
        return std::nullopt;
    }
    const std::int64_t power = rule.divide ? -*exponent : *exponent;
    const double factor =
        POWERS_OF_TEN.at(static_cast<std::size_t>(power >= 0 ? power : -power));
    // A negative power divides by the exact 10^|e|, never multiplies by an
    // inexact 10^-|e|.
    const double raw = double_of(work.sample->raw);
    return power >= 0 ? raw * factor : raw / factor;
}

// Step 4 on the integer path: exact, checked; false when the value is BAD.
bool integer_scale(Work& work) noexcept {
    const PointRules& rules = *work.rules;
    std::optional<detail::Wide> result = wide_of(work.sample->raw);
    if (rules.scale.has_value()) {
        const ScaleRule& scale = *rules.scale;
        // `divide_by: 1` is a gain of 1 (decision D-133).
        const std::int64_t gain =
            scale.form == ScaleRule::Form::GAIN ? scale.gain_i : 1;
        result = detail::wide_multiply(*result, detail::wide_from_signed(gain));
        if (result.has_value()) {
            result = detail::wide_add(*result,
                                      detail::wide_from_signed(scale.offset_i));
        }
    }
    if (result.has_value()) {
        if (rules.tag_type == TagType::INT64) {
            if (const auto fit = detail::wide_to_signed(*result)) {
                work.value = int64_value(*fit);
                return true;
            }
        } else if (const auto fit = detail::wide_to_unsigned(*result)) {
            work.value = uint64_value(*fit);
            return true;
        }
    }
    work.mark = worse(work.mark, bad(flag(QualityFlag::OVERFLOW)));
    return false;
}

// Step 4 in binary64: two operations, never fused (decision D-98).
double binary_scale(const PointRules& rules, double number) noexcept {
    if (!rules.scale.has_value()) {
        return number;
    }
    const ScaleRule& scale = *rules.scale;
    if (scale.form == ScaleRule::Form::GAIN) {
        const double product = number * scale.gain;
        return product + scale.offset;
    }
    const double quotient = number / static_cast<double>(scale.divide_by);
    return quotient + scale.offset;
}

// Steps 3 and 4 on a numeric raw value; false when the value is BAD.
bool scale_value(Work& work) noexcept {
    const PointRules& rules = *work.rules;
    const RawValue& raw = work.sample->raw;
    const bool integer_tag = is_integer_tag(rules.tag_type);
    if (integer_tag &&
        (rules.exponent.has_value() ||
         (rules.scale.has_value() && !is_integer_scaling(*rules.scale)))) {
        // CF-517 refuses it (decision D-133); a value is never computed.
        work.mark = worse(work.mark, bad(flag(QualityFlag::OUT_OF_RANGE)));
        return false;
    }
    if (integer_tag && is_integer_kind(raw.kind)) {
        return integer_scale(work);
    }
    if (integer_tag) {
        // A floating raw value on an integer value type is CF-504 (decision
        // D-134): it is never read as an integer, scaled or not.
        work.mark = worse(work.mark, bad(flag(QualityFlag::OUT_OF_RANGE)));
        return false;
    }
    std::optional<double> number;
    if (rules.exponent.has_value()) {
        number = apply_exponent(work, *rules.exponent);
        if (!number.has_value()) {
            return false;
        }
    }
    const double scaled =
        binary_scale(rules, number.has_value() ? *number : double_of(raw));
    if (!std::isfinite(scaled)) {
        work.mark = worse(work.mark, bad(flag(QualityFlag::OVERFLOW)));
        return false;
    }
    work.value = float64_value(scaled);
    return true;
}

// Step 5 for counters (contract 5.4, CS 5.3); false when the value is BAD.
bool apply_counter(Work& work, const CounterRule& rule) noexcept {
    PointState& state = *work.state;
    const RawValue& raw = work.sample->raw;
    if (!is_integer_kind(raw.kind) ||
        (raw.kind == RawKind::SIGNED && raw.number_i < 0)) {
        work.mark = worse(work.mark, bad(flag(QualityFlag::OUT_OF_RANGE)));
        return false;
    }
    const std::uint64_t current = raw.kind == RawKind::SIGNED
                                      ? static_cast<std::uint64_t>(raw.number_i)
                                      : raw.number_u;
    if (rule.saturate && current == rule.limit) {
        work.mark.flags |= flag(QualityFlag::OVERFLOW);
    }
    if (state.counter_has_previous && current < state.counter_previous) {
        // A wrap is a decrease from at least limit/2 to below it (contract
        // 5.4): a 32-bit counter stepping back across half its limit is
        // read as a wrap, by that rule.
        const std::uint64_t half = rule.limit / 2U;
        const bool wrap =
            !rule.saturate && state.counter_previous >= half && current < half;
        if (!wrap) {
            switch (rule.on_decrease) {
                case CounterRule::OnDecrease::ACCEPT:
                    break;
                case CounterRule::OnDecrease::HOLD:
                    // Keep the counter's own last value, not whatever the
                    // tag held last, until the counter passes it.
                    state.counter_holding = true;
                    work.value = state.counter_held;
                    work.mark = worse(work.mark, UNCERTAIN_MARK);
                    return true;
                case CounterRule::OnDecrease::RESET:
                    work.out->counts.counter_reset = true;
                    break;
            }
        }
    }
    state.counter_holding = false;
    state.counter_has_previous = true;
    state.counter_previous = current;
    state.counter_held = work.value;
    return true;
}

// Step 5 for double points (contract 5.4); the value is always written.
void apply_double_point(Work& work, const DoublePointRule& rule) noexcept {
    const std::optional<std::int64_t> raw = code_of(work.sample->raw);
    if (raw.has_value()) {
        const std::int64_t code = *raw;
        const auto entry = std::lower_bound(
            rule.states.begin(), rule.states.end(), code,
            [](const DoublePointRule::Entry& candidate, std::int64_t wanted) {
                return candidate.raw < wanted;
            });
        if (entry != rule.states.end() && entry->raw == code) {
            work.value = int64_value(tag::to_raw(entry->code));
            if (entry->code == DoublePoint::BAD_STATE) {
                // Written as 3 with BAD, so that a screen shows "faulty".
                work.mark = worse(work.mark, bad(0));
            }
            return;
        }
    }
    // A raw code the map does not name is a bad state (decision D-132).
    work.value = int64_value(tag::to_raw(DoublePoint::BAD_STATE));
    work.mark = worse(work.mark, bad(flag(QualityFlag::OUT_OF_RANGE)));
}

// Step 5 for enumerations (CS 5.5): an INT64 tag of the code, no scale;
// false when the value is BAD.
bool apply_enum(Work& work) noexcept {
    const PointRules& rules = *work.rules;
    if (rules.tag_type != TagType::INT64 || rules.scale.has_value() ||
        rules.exponent.has_value()) {
        // Refused by validate_point_rules (decision D-131): an enumeration
        // is read and written as the device's code.
        work.mark = worse(work.mark, bad(flag(QualityFlag::OUT_OF_RANGE)));
        return false;
    }
    const std::optional<std::int64_t> code = code_of(work.sample->raw);
    if (!code.has_value()) {
        work.mark = worse(work.mark, bad(flag(QualityFlag::OUT_OF_RANGE)));
        return false;
    }
    work.value = int64_value(*code);
    if (!std::binary_search(rules.enum_codes.begin(), rules.enum_codes.end(),
                            *code)) {
        work.mark = worse(
            work.mark, QualityMark{.quality = Quality::UNCERTAIN,
                                   .flags = flag(QualityFlag::OUT_OF_RANGE)});
    }
    return true;
}

// The value of a STRING point (contract 5.2, 5.4); false when it is BAD.
bool take_string(Work& work) noexcept {
    const std::span<const std::byte> bytes = work.sample->raw.bytes;
    if (work.sample->raw.kind != RawKind::STRING || !detail::is_utf8(bytes)) {
        work.mark = worse(work.mark, bad(flag(QualityFlag::OUT_OF_RANGE)));
        return false;
    }
    const std::size_t length = detail::utf8_prefix(bytes, TAG_STRING_MAX_BYTES);
    if (length < bytes.size()) {
        work.mark.flags |= flag(QualityFlag::OVERFLOW);
    }
    work.text = bytes.first(length);
    work.value = string_value({});
    return true;
}

// A device time converted by `rule`; uncertain when the conversion is, or
// when the clock is not declared synchronised (CS 4.6).
UtcTime device_to_utc(const DeviceTimeRule& rule,
                      std::int64_t device_ns) noexcept {
    if (!rule.local) {
        return UtcTime{.utc_ns = device_ns, .uncertain = !rule.synced};
    }
    if (rule.zone == nullptr) {
        // Refused by validate_point_rules; never shown as certain.
        return UtcTime{.utc_ns = device_ns, .uncertain = true};
    }
    UtcTime utc = rule.zone->to_utc(device_ns);
    utc.uncertain = utc.uncertain || !rule.synced;
    return utc;
}

// Step 7, the time of the sample (contract 5.8).
void sample_time(Work& work) noexcept {
    const PointRules& rules = *work.rules;
    work.source_time_ns = work.times->receive_time_ns;
    work.time_flags = flag(QualityFlag::LOCAL_TIME);
    if (!rules.time_companion.has_value()) {
        return;
    }
    const std::optional<CompanionInput> companion =
        usable_companion(work, *rules.time_companion);
    if (!companion.has_value()) {
        return;
    }
    if (companion->raw.kind != RawKind::TIME) {
        work.mark = worse(work.mark, bad(flag(QualityFlag::OUT_OF_RANGE)));
        return;
    }
    const UtcTime utc =
        device_to_utc(rules.companion_time, companion->raw.number_i);
    work.source_time_ns = utc.utc_ns;
    work.time_flags = utc.uncertain ? flag(QualityFlag::TIME_UNCERTAIN) : 0;
}

// The last valid value, held UNCERTAIN with STALE (and `extra`), or BAD
// with STALE once `max_age` has passed since it was read (CS 4.7, for
// `keep_last`; decision D-131, for DEVICE_INVALID). A point without
// `max_age` cannot hold (CF-640 refuses `keep_last` without it).
void hold_last(Work& work, QualityMark extra) noexcept {
    PointState& state = *work.state;
    const PointRules& rules = *work.rules;
    const bool young =
        state.has_valid && rules.max_age_ns > 0 &&
        work.times->receive_mono_ns - state.valid_mono_ns <= rules.max_age_ns;
    const QualityMark stale{.quality = Quality::UNCERTAIN,
                            .flags = flag(QualityFlag::STALE)};
    if (extra.quality == Quality::BAD) {
        state.holding = false;
        work.mark = extra;
        return;
    }
    if (young) {
        state.holding = true;
        work.new_value = true;
        work.value = state.valid_value;
        work.mark = worse(stale, extra);
        return;
    }
    state.holding = false;
    work.mark = bad(static_cast<std::uint16_t>(
        extra.flags | (state.has_valid ? flag(QualityFlag::STALE) : 0)));
}

// Step 1, invalid markers on the raw pattern. Returns true when one matched
// and decided the value.
bool apply_markers(Work& work) noexcept {
    // A marker never applies to a STRING tag: a string has no raw pattern,
    // and a sample of another kind is no string (take_string gives it BAD).
    // marker_matches looks at the sample's kind, which a protocol fault can
    // make other than the configured one (#169).
    if (work.rules->tag_type == TagType::STRING) {
        return false;
    }
    for (const InvalidMarker& marker : work.rules->markers) {
        if (!marker_matches(marker, work.sample->raw)) {
            continue;
        }
        work.out->counts.sentinel = true;
        work.state->holding = false;
        switch (marker.action) {
            case InvalidAction::BAD:
                work.mark = bad(marker.flags & CONFIG_FLAGS_MASK);
                break;
            case InvalidAction::KEEP_LAST:
                hold_last(work, GOOD_MARK);
                break;
            case InvalidAction::SUBSTITUTE:
                // UNCERTAIN with SUBSTITUTED: a value substituted by
                // configuration (contract 5.2, R-6; tag database contract
                // 1.1, section 6).
                work.new_value = true;
                work.from_device = true;
                work.substituted = true;
                work.value = marker.substitute;
                work.mark = UNCERTAIN_MARK;
                break;
        }
        return true;
    }
    return false;
}

// `raw_range` on the raw number (CS 4.7); it only adds to the mark.
void apply_raw_range(Work& work) noexcept {
    const RawValue& raw = work.sample->raw;
    if (!work.rules->raw_range.has_value() || raw.kind == RawKind::STRING ||
        raw.kind == RawKind::TIME) {
        return;
    }
    const RawRange& range = *work.rules->raw_range;
    const double number = double_of(raw);
    if (number < range.min || number > range.max) {
        work.mark = worse(
            work.mark, QualityMark{.quality = Quality::UNCERTAIN,
                                   .flags = flag(QualityFlag::OUT_OF_RANGE)});
    }
}

// Steps 3 to 5 by the tag's type; false when the value is BAD.
bool compute_value(Work& work) noexcept {
    const PointRules& rules = *work.rules;
    const RawValue& raw = work.sample->raw;
    switch (rules.tag_type) {
        case TagType::BOOL:
            if (raw.kind == RawKind::STRING || raw.kind == RawKind::TIME) {
                work.mark =
                    worse(work.mark, bad(flag(QualityFlag::OUT_OF_RANGE)));
                return false;
            }
            work.value = bool_value((double_of(raw) != 0.0) != rules.invert);
            return true;
        case TagType::STRING:
            return take_string(work);
        case TagType::INT64:
        case TagType::UINT64:
        case TagType::FLOAT64:
            break;
    }
    if (raw.kind == RawKind::TIME) {
        // A device time: INT64 nanoseconds UTC (contract 5.4), with
        // TIME_UNCERTAIN as for a source time (CS 4.6).
        if (rules.tag_type != TagType::INT64) {
            work.mark = worse(work.mark, bad(flag(QualityFlag::OUT_OF_RANGE)));
            return false;
        }
        const UtcTime utc = device_to_utc(rules.device_time, raw.number_i);
        work.value = int64_value(utc.utc_ns);
        if (utc.uncertain) {
            work.mark.flags |= flag(QualityFlag::TIME_UNCERTAIN);
        }
        return true;
    }
    if (raw.kind == RawKind::STRING) {
        work.mark = worse(work.mark, bad(flag(QualityFlag::OUT_OF_RANGE)));
        return false;
    }
    if (rules.double_point.has_value()) {
        apply_double_point(work, *rules.double_point);
        return true;
    }
    if (rules.has_enum) {
        return apply_enum(work);
    }
    if (!scale_value(work)) {
        return false;
    }
    return !rules.counter.has_value() || apply_counter(work, *rules.counter);
}

// The bits of the word (CS 5.4), from the one read.
bool take_bits(Work& work) noexcept {
    const PointRules& rules = *work.rules;
    std::uint64_t values = 0;
    for (std::size_t index = 0;
         index < rules.bits.size() && index < MAX_POINT_BITS; ++index) {
        const BitRule& rule = rules.bits[index];
        if (rule.bit > MAX_BIT_NUMBER) {
            work.mark = worse(work.mark, bad(flag(QualityFlag::OUT_OF_RANGE)));
            return false;
        }
        const bool set =
            (((work.sample->raw.pattern >> rule.bit) & 1U) != 0) != rule.invert;
        values |= (set ? std::uint64_t{1} : 0U) << index;
    }
    work.state->bit_values = values;
    return true;
}

// Steps 1 to 5 for an answer with a value.
void process_value(Work& work) noexcept {
    const RawValue& raw = work.sample->raw;
    if (!width_ok(raw)) {
        work.mark = bad(flag(QualityFlag::OUT_OF_RANGE));
        return;
    }
    if (apply_markers(work)) {
        return;
    }
    work.state->holding = false;
    // Step 2, non-finite values without a marker.
    if (raw.kind == RawKind::FLOAT && !std::isfinite(raw.number_f)) {
        work.out->counts.invalid = true;
        work.mark = bad(flag(QualityFlag::OUT_OF_RANGE));
        return;
    }
    work.confirmed = true;
    apply_raw_range(work);
    if (!compute_value(work) || !take_bits(work)) {
        return;
    }
    work.new_value = true;
    work.from_device = true;
}

// A DEVICE_INVALID answer: never GOOD, never a value nobody read; the last
// valid value is held as `keep_last` holds it (decision D-131).
void process_device_invalid(Work& work) noexcept {
    work.out->counts.invalid = true;
    const QualityMark mapping =
        worse(UNCERTAIN_MARK, device_mark(work.sample->mark));
    hold_last(work, mapping);
}

// Steps 1 to 6 for rules validate_point_rules accepts: the outcome, then
// the worst of all `quality_from` and `health` rules (CS 4.7, 4.9).
void process_outcome(Work& work) noexcept {
    PointState& state = *work.state;
    const RawSample& sample = *work.sample;
    switch (sample.outcome) {
        case Outcome::VALUE:
            process_value(work);
            break;
        case Outcome::NO_VALUE:
            state.holding = false;
            work.mark = bad(device_mark(sample.mark).flags);
            break;
        case Outcome::ABSENT:
            state.holding = false;
            work.mark = bad(0);
            break;
        case Outcome::DEVICE_INVALID:
            process_device_invalid(work);
            break;
        case Outcome::COMM_FAIL:
            state.holding = false;
            work.mark = bad(flag(QualityFlag::COMM_FAIL));
            break;
    }
    work.mark = worse(work.mark, quality_from_rules(work));
    work.mark = worse(work.mark, health_mark(work));
}

// Fills `out` from the state's last value and mark.
void fill_writes(const PointRules& rules, const PointState& state,
                 ProcessedSample& out) noexcept {
    out.write_tag = rules.has_tag;
    out.tag.value = state.value;
    if (state.value.type == TagType::STRING) {
        out.tag.value.text =
            std::string_view(state.text.data(), state.text.size());
    }
    out.tag.quality = state.quality;
    out.tag.flags = state.flags;
    out.tag.source_time_ns = state.source_time_ns;
    out.bit_count = std::min(rules.bits.size(), MAX_POINT_BITS);
    out.bit_values = state.bit_values;
}

void store_text(PointState& state, std::span<const std::byte> text) noexcept {
    // The capacity was reserved by init_point_state: this never allocates
    // for a prepared state, and never writes beyond what was reserved.
    const std::size_t length =
        std::min({text.size(), TAG_STRING_MAX_BYTES, state.text.capacity()});
    state.text.clear();
    for (std::size_t index = 0; index < length; ++index) {
        state.text.push_back(static_cast<char>(text[index]));
    }
}

}  // namespace

void init_point_state(PointState& state, TagType type) {
    if (type == TagType::STRING) {
        state.text.reserve(TAG_STRING_MAX_BYTES);
    }
}

void reset_point_state(PointState& state, TagType type,
                       std::int64_t now_mono_ns) noexcept {
    state.value = zero_value(type);
    state.quality = Quality::BAD;
    state.flags = flag(QualityFlag::NOT_INIT);
    state.source_time_ns = 0;
    state.has_value = false;
    state.confirmed_mono_ns = now_mono_ns;
    state.has_valid = false;
    state.valid_value = zero_value(type);
    state.valid_mono_ns = now_mono_ns;
    state.holding = false;
    state.absent = false;
    state.counter_has_previous = false;
    state.counter_previous = 0;
    state.counter_holding = false;
    state.counter_held = zero_value(type);
    state.bit_values = 0;
    state.substituted = false;
    state.text.clear();
}

void seed_point_state(PointState& state, const TagValue& stored,
                      bool substituted) noexcept {
    if (stored.type != state.value.type) {
        return;
    }
    state.substituted = substituted;
    if (stored.type == TagType::STRING) {
        store_text(state, std::as_bytes(std::span(stored.text)));
        return;
    }
    state.value = stored;
}

void process_sample(const PointRules& rules, PointState& state,
                    const RawSample& sample,
                    std::span<const CompanionInput> companions,
                    const SampleTimes& times, ProcessedSample& out) noexcept {
    out = ProcessedSample{};
    Work work;
    work.rules = &rules;
    work.state = &state;
    work.sample = &sample;
    work.companions = companions;
    work.times = &times;
    work.out = &out;

    // Rules that validate_point_rules refuses give BAD with OUT_OF_RANGE on
    // every path, the value kept: no marker, hold or companion applies to
    // them (#169; decision D-131). The compile step refuses them first.
    const bool refused = !validate_point_rules(rules).ok();
    if (refused) {
        state.holding = false;
        work.mark = bad(flag(QualityFlag::OUT_OF_RANGE));
    } else {
        process_outcome(work);
    }
    if (sample.outcome == Outcome::NO_VALUE) {
        out.counts.sentinel = true;
    }

    // ABSENT is counted while it lasts; a lost answer says nothing either
    // way.
    if (sample.outcome == Outcome::ABSENT && !state.absent) {
        state.absent = true;
        out.counts.became_absent = true;
    } else if (sample.outcome != Outcome::ABSENT &&
               sample.outcome != Outcome::COMM_FAIL && state.absent) {
        state.absent = false;
        out.counts.left_absent = true;
    }

    if (sample.outcome == Outcome::VALUE && !refused) {
        sample_time(work);
    } else {
        // The time is the time of this observation, stamped by the driver;
        // refused rules apply no time companion either (#169).
        work.source_time_ns = times.receive_time_ns;
        work.time_flags = flag(QualityFlag::LOCAL_TIME);
    }

    // The new state of the point. SUBSTITUTED describes the value (tag
    // database contract 6, 12): it is set by the substitute action alone,
    // and stays with a kept substitute value through COMM_FAIL, ABSENT,
    // NO_VALUE and a refused value (review R1 of #157).
    if (work.new_value) {
        state.substituted = work.substituted;
        state.value = work.value;
        if (rules.tag_type == TagType::STRING && !state.holding) {
            store_text(state, work.text);
        }
        if (work.from_device) {
            state.has_value = true;
        }
    }
    auto flags = static_cast<std::uint16_t>(
        (work.mark.flags & ~flag(QualityFlag::SUBSTITUTED)) | work.time_flags);
    if (state.substituted) {
        flags |= flag(QualityFlag::SUBSTITUTED);
    }
    if (work.confirmed) {
        state.confirmed_mono_ns = times.receive_mono_ns;
    } else {
        // Not confirmed: a STALE mark already given stays (contract 7.5).
        flags |=
            static_cast<std::uint16_t>(state.flags & flag(QualityFlag::STALE));
    }
    if (work.new_value && work.confirmed && work.mark.quality != Quality::BAD) {
        state.has_valid = true;
        state.valid_value = state.value;
        state.valid_mono_ns = times.receive_mono_ns;
    }
    if (!state.has_value) {
        flags |= flag(QualityFlag::NOT_INIT);
    }
    state.quality = work.mark.quality;
    state.flags = flags;
    state.source_time_ns = work.source_time_ns;

    fill_writes(rules, state, out);
}

bool check_stale(const PointRules& rules, PointState& state,
                 std::int64_t now_mono_ns, ProcessedSample& out) noexcept {
    if (rules.max_age_ns <= 0) {
        return false;
    }
    const std::uint16_t stale = flag(QualityFlag::STALE);
    if (state.holding && state.quality != Quality::BAD &&
        now_mono_ns - state.valid_mono_ns > rules.max_age_ns) {
        // A held value past `max_age`: BAD with STALE (CS 4.7).
        state.holding = false;
        state.quality = Quality::BAD;
        state.flags |= stale;
    } else if ((state.flags & stale) == 0 &&
               now_mono_ns - state.confirmed_mono_ns > rules.max_age_ns) {
        state.flags |= stale;
    } else {
        return false;
    }
    out = ProcessedSample{};
    fill_writes(rules, state, out);
    return true;
}

}  // namespace scada::driver
