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

// The flags a failed companion passes to the point that depends on it
// (contract 5.5: "that outcome's flags").
std::uint16_t companion_flags(Outcome outcome) noexcept {
    return outcome == Outcome::COMM_FAIL ? flag(QualityFlag::COMM_FAIL) : 0;
}

// Powers of ten that binary64 holds exactly, up to the exponent limit of
// CS 4.7 (exact up to 10^22).
constexpr std::array<double, 21> POWERS_OF_TEN{
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9, 1e10,
    1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20};

// 2^63 as a binary64: every integral binary64 of smaller magnitude converts
// to int64 exactly.
constexpr double TWO_TO_THE_63 = 9223372036854775808.0;

// The raw value as a signed integer, when it is one.
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
        case RawKind::TIME:
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
            return std::nullopt;
    }
    return std::nullopt;
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

detail::Wide wide_of(const RawValue& raw) noexcept {
    return raw.kind == RawKind::SIGNED
               ? detail::wide_from_signed(raw.number_i)
               : detail::wide_from_unsigned(raw.number_u);
}

// Bits `low` to `high` of `pattern`, shifted down.
std::uint64_t field_of(std::uint64_t pattern, std::uint8_t low,
                       std::uint8_t high) noexcept {
    const unsigned width = static_cast<unsigned>(high - low) + 1U;
    return (pattern >> low) & width_mask(width);
}

// The key a rule looks up: a field of the pattern, or the whole value.
std::optional<std::int64_t> key_of(const RawValue& raw, bool has_field,
                                   std::uint8_t low,
                                   std::uint8_t high) noexcept {
    if (has_field) {
        // A field has at most 64 bits; one of 64 bits does not fit int64
        // when its top bit is set, and then matches no key.
        const std::uint64_t field = field_of(raw.pattern, low, high);
        if (field > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(field);
    }
    return integer_of(raw);
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

// The mark of the `quality_from` rules (CS 4.7).
QualityMark quality_from_rules(const PointRules& rules, const RawSample& sample,
                               std::span<const CompanionInput> companions) {
    QualityMark mark = GOOD_MARK;
    for (const QualityRule& rule : rules.quality_rules) {
        const RawValue* raw = &sample.raw;
        if (!rule.self) {
            if (rule.companion >= companions.size()) {
                mark = worse(mark, bad(flag(QualityFlag::COMM_FAIL)));
                continue;
            }
            const CompanionInput& companion = companions[rule.companion];
            if (companion.outcome != Outcome::VALUE) {
                mark = worse(mark, bad(companion_flags(companion.outcome)));
                continue;
            }
            raw = &companion.raw;
        }
        const std::optional<std::int64_t> key =
            key_of(*raw, rule.has_field, rule.lo, rule.hi);
        QualityMark found = rule.otherwise;
        if (key.has_value()) {
            const auto entry = std::lower_bound(
                rule.entries.begin(), rule.entries.end(), *key,
                [](const QualityRule::Entry& candidate, std::int64_t wanted) {
                    return candidate.key < wanted;
                });
            if (entry != rule.entries.end() && entry->key == *key) {
                found = entry->mark;
            }
        }
        mark = worse(mark, found);
    }
    return mark;
}

// The mark of the device's `health` rules that apply to the point (CS 4.9).
QualityMark health_mark(const PointRules& rules,
                        std::span<const CompanionInput> companions) {
    QualityMark mark = GOOD_MARK;
    for (const HealthRule& rule : rules.health) {
        const std::uint16_t fail_flags =
            rule.comm_fail ? flag(QualityFlag::COMM_FAIL) : 0;
        if (rule.companion >= companions.size()) {
            mark = worse(mark, bad(fail_flags));
            continue;
        }
        const CompanionInput& companion = companions[rule.companion];
        bool good = false;
        if (companion.outcome == Outcome::VALUE && !companion.stale) {
            const std::optional<std::int64_t> key =
                key_of(companion.raw, rule.has_field, rule.lo, rule.hi);
            good = key.has_value() &&
                   std::binary_search(rule.good.begin(), rule.good.end(), *key);
        }
        if (!good) {
            mark = worse(mark, bad(fail_flags));
        }
    }
    return mark;
}

// The work of one call, kept together so that the steps can be functions.
struct Work {
    const PointRules* rules = nullptr;
    PointState* state = nullptr;
    const RawSample* sample = nullptr;
    std::span<const CompanionInput> companions;
    const SampleTimes* times = nullptr;
    ProcessedSample out{};
    QualityMark mark{};
    // A new value was computed from the device; otherwise the last value is
    // written again.
    bool new_value = false;
    TagValue value{};
    // The read confirmed the value (contract 7.5).
    bool confirmed = false;
    // Set for a STRING value taken from the sample.
    std::span<const std::byte> text;
    std::int64_t source_time_ns = 0;
    std::uint16_t time_flags = 0;
};

// The input of a companion slot; a slot that is missing counts as lost.
CompanionInput companion_at(std::span<const CompanionInput> companions,
                            std::uint8_t slot) noexcept {
    return slot < companions.size()
               ? companions[slot]
               : CompanionInput{.outcome = Outcome::COMM_FAIL};
}

// Step 3, the exponent (CS 4.7); nothing when the value is BAD.
std::optional<double> apply_exponent(Work& work,
                                     const ExponentRule& rule) noexcept {
    const CompanionInput companion =
        companion_at(work.companions, rule.companion);
    if (companion.outcome != Outcome::VALUE) {
        work.mark = worse(work.mark, bad(companion_flags(companion.outcome)));
        return std::nullopt;
    }
    const std::optional<std::int64_t> exponent = integer_of(companion.raw);
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
        if (!scale.integer || scale.form != ScaleRule::Form::GAIN) {
            // CF-517 refuses it (decision D-133); stated here so that a
            // value is never invented.
            work.mark = worse(work.mark, bad(flag(QualityFlag::OUT_OF_RANGE)));
            return false;
        }
        result = detail::wide_multiply(*result,
                                       detail::wide_from_signed(scale.gain_i));
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

// A binary64 value on an INT64 or UINT64 tag: an enumeration or a double
// point reads a floating raw value as its nearest integer (CS 5.5); false
// when it is not within the tolerance or the type.
bool to_integer_tag(Work& work, double number) noexcept {
    constexpr double TOLERANCE = 0.001;
    const double rounded = std::round(number);
    const bool is_unsigned = work.rules->tag_type == TagType::UINT64;
    if (std::fabs(number - rounded) > TOLERANCE || rounded >= TWO_TO_THE_63 ||
        rounded < -TWO_TO_THE_63 || (is_unsigned && rounded < 0.0)) {
        work.mark = worse(work.mark, bad(flag(QualityFlag::OUT_OF_RANGE)));
        return false;
    }
    work.value = is_unsigned ? uint64_value(static_cast<std::uint64_t>(rounded))
                             : int64_value(static_cast<std::int64_t>(rounded));
    return true;
}

// Steps 3 and 4 on a numeric raw value; false when the value is BAD.
bool scale_value(Work& work) noexcept {
    const PointRules& rules = *work.rules;
    const RawValue& raw = work.sample->raw;
    const bool integer_tag =
        rules.tag_type == TagType::INT64 || rules.tag_type == TagType::UINT64;
    std::optional<double> number;
    if (integer_tag && rules.exponent.has_value()) {
        // CF-517 refuses an exponent on an integer tag (decision D-133).
        work.mark = worse(work.mark, bad(flag(QualityFlag::OUT_OF_RANGE)));
        return false;
    }
    if (rules.exponent.has_value()) {
        number = apply_exponent(work, *rules.exponent);
        if (!number.has_value()) {
            return false;
        }
    } else if (integer_tag && is_integer_kind(raw.kind)) {
        return integer_scale(work);
    }
    const double scaled =
        binary_scale(rules, number.has_value() ? *number : double_of(raw));
    if (!std::isfinite(scaled)) {
        work.mark = worse(work.mark, bad(flag(QualityFlag::OVERFLOW)));
        return false;
    }
    if (integer_tag) {
        return to_integer_tag(work, scaled);
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
        const std::uint64_t half = rule.limit / 2U;
        const bool wrap =
            !rule.saturate && state.counter_previous >= half && current < half;
        if (!wrap) {
            switch (rule.on_decrease) {
                case CounterRule::OnDecrease::ACCEPT:
                    break;
                case CounterRule::OnDecrease::HOLD:
                    // Keep the previous value until the counter passes it.
                    state.counter_holding = true;
                    work.value = state.value;
                    work.mark = worse(
                        work.mark, QualityMark{.quality = Quality::UNCERTAIN});
                    return true;
                case CounterRule::OnDecrease::RESET:
                    work.out.counts.counter_reset = true;
                    break;
            }
        }
    }
    state.counter_holding = false;
    state.counter_has_previous = true;
    state.counter_previous = current;
    return true;
}

// Step 5 for double points (contract 5.4); the value is always written.
void apply_double_point(Work& work, const DoublePointRule& rule) noexcept {
    const std::optional<std::int64_t> raw = integer_of(work.sample->raw);
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

// Step 5 for enumerations (CS 5.5).
void apply_enum(Work& work) noexcept {
    const std::vector<std::int64_t>& codes = work.rules->enum_codes;
    if (!std::binary_search(codes.begin(), codes.end(), work.value.i)) {
        work.mark = worse(
            work.mark, QualityMark{.quality = Quality::UNCERTAIN,
                                   .flags = flag(QualityFlag::OUT_OF_RANGE)});
    }
}

// The value of a STRING point (contract 5.2, 5.4); false when it is BAD.
bool take_string(Work& work) noexcept {
    const std::span<const std::byte> bytes = work.sample->raw.bytes;
    if (!detail::is_utf8(bytes)) {
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

// Step 7, the time of the sample (contract 5.8).
void sample_time(Work& work) noexcept {
    const PointRules& rules = *work.rules;
    work.source_time_ns = work.times->receive_time_ns;
    work.time_flags = flag(QualityFlag::LOCAL_TIME);
    if (!rules.time_companion.has_value()) {
        return;
    }
    const CompanionInput companion =
        companion_at(work.companions, *rules.time_companion);
    if (companion.outcome != Outcome::VALUE ||
        companion.raw.kind != RawKind::TIME) {
        work.mark = worse(work.mark, bad(companion_flags(companion.outcome)));
        return;
    }
    const std::int64_t device_ns = companion.raw.number_i;
    const DeviceTimeRule& rule = rules.companion_time;
    const UtcTime utc =
        rule.local ? rule.zone.to_utc(device_ns) : UtcTime{.utc_ns = device_ns};
    work.source_time_ns = utc.utc_ns;
    work.time_flags =
        (utc.uncertain || !rule.synced) ? flag(QualityFlag::TIME_UNCERTAIN) : 0;
}

// Step 1, invalid markers on the raw pattern. Returns true when one matched
// and decided the write.
bool apply_markers(Work& work) noexcept {
    const PointRules& rules = *work.rules;
    PointState& state = *work.state;
    for (const InvalidMarker& marker : rules.markers) {
        if (!marker_matches(marker, work.sample->raw)) {
            continue;
        }
        work.out.counts.sentinel = true;
        state.keeping_last = marker.action == InvalidAction::KEEP_LAST;
        switch (marker.action) {
            case InvalidAction::BAD:
                work.mark = bad(marker.flags);
                break;
            case InvalidAction::KEEP_LAST:
                if (state.has_valid && rules.max_age_ns > 0 &&
                    work.times->receive_mono_ns - state.valid_mono_ns <=
                        rules.max_age_ns) {
                    work.new_value = true;
                    work.value = state.valid_value;
                    work.mark = QualityMark{.quality = Quality::UNCERTAIN,
                                            .flags = flag(QualityFlag::STALE)};
                } else {
                    // No valid value, or older than `max_age` (and a point
                    // without `max_age`, which CF-640 refuses): BAD.
                    work.mark = bad(flag(QualityFlag::STALE));
                }
                break;
            case InvalidAction::SUBSTITUTE:
                // UNCERTAIN; SUBSTITUTED only from tag database 1.1
                // (contract 5.2, R-6 depends on #89).
                work.new_value = true;
                work.value = marker.substitute;
                work.mark = QualityMark{.quality = Quality::UNCERTAIN};
                break;
        }
        return true;
    }
    state.keeping_last = false;
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
        // A device time: INT64 nanoseconds UTC (contract 5.4).
        const DeviceTimeRule& rule = rules.device_time;
        work.value = int64_value(
            rule.local ? rule.zone.to_utc(raw.number_i).utc_ns : raw.number_i);
        return true;
    }
    if (rules.double_point.has_value()) {
        apply_double_point(work, *rules.double_point);
        return true;
    }
    if (!scale_value(work)) {
        return false;
    }
    if (rules.counter.has_value() && !apply_counter(work, *rules.counter)) {
        return false;
    }
    if (rules.has_enum) {
        apply_enum(work);
    }
    return true;
}

// Steps 1 to 6 for an answer with a value.
void process_value(Work& work) noexcept {
    const PointRules& rules = *work.rules;
    PointState& state = *work.state;
    const RawValue& raw = work.sample->raw;

    if (apply_markers(work)) {
        return;
    }
    // Step 2, non-finite values without a marker.
    if (raw.kind == RawKind::FLOAT && !std::isfinite(raw.number_f)) {
        work.out.counts.invalid = true;
        work.mark = bad(flag(QualityFlag::OUT_OF_RANGE));
        return;
    }
    work.confirmed = true;
    apply_raw_range(work);
    if (!compute_value(work)) {
        return;
    }
    work.new_value = true;

    // Bits from the one read of the word (CS 5.4).
    for (std::size_t index = 0;
         index < rules.bits.size() && index < state.bit_values.size();
         ++index) {
        const BitRule& rule = rules.bits[index];
        state.bit_values.at(index) =
            (((raw.pattern >> rule.bit) & 1U) != 0) != rule.invert;
    }

    // Step 6, quality from companions and health rules.
    work.mark = worse(work.mark,
                      quality_from_rules(rules, *work.sample, work.companions));
    work.mark = worse(work.mark, health_mark(rules, work.companions));
}

// Fills the writes of `out` from the state's last value and mark.
void fill_writes(const PointRules& rules, const PointState& state,
                 ProcessedSample& out) noexcept {
    out.write_tag = rules.has_tag;
    out.tag.value = state.value;
    if (state.value.type == TagType::STRING) {
        out.tag.value.text =
            std::string_view(state.text.data(), state.text_length);
    }
    out.tag.quality = state.quality;
    out.tag.flags = state.flags;
    out.tag.source_time_ns = state.source_time_ns;
    out.bit_count = std::min(rules.bits.size(), out.bits.size());
    for (std::size_t index = 0; index < out.bit_count; ++index) {
        out.bits.at(index) =
            TagWrite{.value = bool_value(state.bit_values.at(index)),
                     .quality = state.quality,
                     .flags = state.flags,
                     .source_time_ns = state.source_time_ns};
    }
}

}  // namespace

void reset_point_state(PointState& state, TagType type,
                       std::int64_t now_mono_ns) noexcept {
    state = PointState{};
    state.value = zero_value(type);
    state.valid_value = zero_value(type);
    state.quality = Quality::BAD;
    state.flags = flag(QualityFlag::NOT_INIT);
    state.confirmed_mono_ns = now_mono_ns;
    state.valid_mono_ns = now_mono_ns;
}

ProcessedSample process_sample(const PointRules& rules, PointState& state,
                               const RawSample& sample,
                               std::span<const CompanionInput> companions,
                               const SampleTimes& times) noexcept {
    Work work;
    work.rules = &rules;
    work.state = &state;
    work.sample = &sample;
    work.companions = companions;
    work.times = &times;

    switch (sample.outcome) {
        case Outcome::VALUE:
            process_value(work);
            break;
        case Outcome::NO_VALUE:
            work.out.counts.sentinel = true;
            work.mark = bad(sample.mark.flags);
            break;
        case Outcome::ABSENT:
            work.mark = bad(0);
            break;
        case Outcome::DEVICE_INVALID:
            work.out.counts.invalid = true;
            // Never GOOD: the device said the value is invalid.
            work.mark =
                worse(QualityMark{.quality = Quality::UNCERTAIN}, sample.mark);
            break;
        case Outcome::COMM_FAIL:
            work.mark = bad(flag(QualityFlag::COMM_FAIL));
            break;
    }

    // ABSENT is counted while it lasts; a lost answer says nothing either
    // way.
    if (sample.outcome == Outcome::ABSENT && !state.absent) {
        state.absent = true;
        work.out.counts.became_absent = true;
    } else if (sample.outcome != Outcome::ABSENT &&
               sample.outcome != Outcome::COMM_FAIL && state.absent) {
        state.absent = false;
        work.out.counts.left_absent = true;
    }

    if (sample.outcome == Outcome::VALUE) {
        sample_time(work);
    } else {
        // The time is the time of this observation, stamped by the driver.
        work.source_time_ns = times.receive_time_ns;
        work.time_flags = flag(QualityFlag::LOCAL_TIME);
    }

    // The new state of the point.
    if (work.new_value) {
        state.value = work.value;
        if (rules.tag_type == TagType::STRING && !work.text.empty()) {
            std::transform(
                work.text.begin(), work.text.end(), state.text.begin(),
                [](std::byte byte) { return static_cast<char>(byte); });
            state.text_length = work.text.size();
        } else if (rules.tag_type == TagType::STRING) {
            state.text_length = 0;
        }
        if (!state.keeping_last) {
            state.has_value = true;
        }
    }
    auto flags = static_cast<std::uint16_t>(work.mark.flags | work.time_flags);
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

    fill_writes(rules, state, work.out);
    return work.out;
}

bool check_stale(const PointRules& rules, PointState& state,
                 std::int64_t now_mono_ns, ProcessedSample& out) noexcept {
    if (rules.max_age_ns <= 0) {
        return false;
    }
    const std::uint16_t stale = flag(QualityFlag::STALE);
    if (state.keeping_last && state.quality != Quality::BAD &&
        now_mono_ns - state.valid_mono_ns > rules.max_age_ns) {
        // `keep_last` past `max_age`: BAD with STALE (CS 4.7).
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
