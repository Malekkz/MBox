// Fuzz target for the processing after decode and the write conversion.
// The raw values come from devices, through a protocol's decode, so every
// field of a raw sample is outside input: the input bytes give an outcome,
// a raw kind and width, the pattern, the numbers and a string, which run
// through a fixed set of point rules. A result that breaks an invariant of
// the tag database (a quality or flag that is none, a string over 250 bytes
// or not UTF-8, a GOOD value that is not finite) aborts, so the fuzzer
// reports it.

#include "scada/driver/point_rules.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/sample_processing.h"
#include "scada/driver/tag_value.h"
#include "scada/driver/write_conversion.h"
#include "scada/tag/types.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <vector>

namespace {

using scada::driver::CompanionInput;
using scada::driver::Outcome;
using scada::driver::PointRules;
using scada::driver::PointState;
using scada::driver::ProcessedSample;
using scada::driver::RawKind;
using scada::driver::RawSample;
using scada::driver::RawValue;
using scada::driver::SampleTimes;
using scada::driver::TagWrite;
using scada::tag::Quality;
using scada::tag::TagType;

constexpr std::size_t HEADER_BYTES = 32;
constexpr std::size_t WORD_BYTES = 8;
constexpr unsigned OUTCOMES = 5;
constexpr unsigned KINDS = 6;
constexpr std::array<std::uint8_t, 5> WIDTHS{1, 8, 16, 32, 64};
constexpr std::uint8_t STATUS_FIELD_HIGH = 3;
constexpr std::uint8_t TOP_BIT = 63;
constexpr std::uint64_t COUNTER_LIMIT = 65535;
constexpr std::int64_t SECOND = 1'000'000'000;
constexpr std::uint64_t ALL_ONES_16 = 0xFFFFU;
constexpr double GAIN = 0.1;
constexpr std::int64_t GAIN_I = 3;
constexpr std::int64_t OFFSET_I = -7;
constexpr std::int64_t ENUM_CODE_TWO = 2;

std::uint64_t word_at(std::span<const std::uint8_t> data, std::size_t offset) {
    std::array<std::uint8_t, WORD_BYTES> bytes{};
    for (std::size_t index = 0; index < WORD_BYTES; ++index) {
        bytes.at(index) = data[offset + index];
    }
    return std::bit_cast<std::uint64_t>(bytes);
}

// The point rules the samples run through: one per path of the processing.
std::vector<PointRules> make_rules() {
    std::vector<PointRules> all;
    PointRules plain;
    plain.tag_type = TagType::INT64;
    all.push_back(plain);

    PointRules scaled;
    scaled.tag_type = TagType::FLOAT64;
    scaled.scale = scada::driver::ScaleRule{.gain = GAIN};
    scaled.markers.push_back(scada::driver::InvalidMarker{.any_nan = true});
    scaled.raw_range = scada::driver::RawRange{.min = -1.0, .max = 1.0};
    all.push_back(scaled);

    PointRules integer;
    integer.tag_type = TagType::INT64;
    integer.scale = scada::driver::ScaleRule{
        .integer = true, .gain_i = GAIN_I, .offset_i = OFFSET_I};
    integer.markers.push_back(scada::driver::InvalidMarker{
        .pattern = ALL_ONES_16,
        .action = scada::driver::InvalidAction::KEEP_LAST});
    integer.max_age_ns = SECOND;
    all.push_back(integer);

    PointRules counter;
    counter.tag_type = TagType::UINT64;
    counter.counter = scada::driver::CounterRule{
        .limit = COUNTER_LIMIT,
        .on_decrease = scada::driver::CounterRule::OnDecrease::HOLD};
    all.push_back(counter);

    PointRules position;
    position.tag_type = TagType::INT64;
    position.double_point = scada::driver::DoublePointRule{
        .states = {{0, scada::driver::DoublePoint::INTERMEDIATE},
                   {1, scada::driver::DoublePoint::OPEN}}};
    all.push_back(position);

    PointRules enumeration;
    enumeration.tag_type = TagType::INT64;
    enumeration.has_enum = true;
    enumeration.enum_codes = {0, 1, ENUM_CODE_TWO};
    enumeration.bits = {scada::driver::BitRule{.bit = 0},
                        scada::driver::BitRule{.bit = TOP_BIT}};
    enumeration.companions = {1};
    enumeration.quality_rules.push_back(
        scada::driver::QualityRule{.self = true,
                                   .has_field = true,
                                   .lo = 0,
                                   .hi = STATUS_FIELD_HIGH,
                                   .entries = {}});
    all.push_back(enumeration);

    PointRules text;
    text.tag_type = TagType::STRING;
    all.push_back(text);

    PointRules flag;
    flag.tag_type = TagType::BOOL;
    flag.invert = true;
    all.push_back(flag);
    return all;
}

void check(const TagWrite& write) {
    if (!scada::tag::quality_from_raw(scada::tag::to_raw(write.quality))
             .has_value() ||
        !scada::tag::quality_flags_valid(write.flags)) {
        std::abort();
    }
    if (write.value.type == TagType::STRING &&
        write.value.text.size() > scada::driver::TAG_STRING_MAX_BYTES) {
        std::abort();
    }
    if (write.value.type == TagType::FLOAT64 &&
        write.quality == Quality::GOOD && !std::isfinite(write.value.f)) {
        std::abort();
    }
}

void check(const ProcessedSample& out) {
    check(out.tag);
    for (std::size_t index = 0; index < out.bit_count; ++index) {
        check(out.bits.at(index));
    }
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    if (size < HEADER_BYTES) {
        return 0;
    }
    const std::span<const std::uint8_t> input(data, size);
    RawValue raw;
    raw.kind = static_cast<RawKind>(1U + (input[1] % KINDS));
    raw.width = WIDTHS.at(input[2] % WIDTHS.size());
    raw.pattern =
        word_at(input, WORD_BYTES) & scada::driver::width_mask(raw.width);
    raw.number_i = static_cast<std::int64_t>(word_at(input, 2 * WORD_BYTES));
    raw.number_u = word_at(input, 2 * WORD_BYTES);
    raw.number_f = std::bit_cast<double>(word_at(input, 3 * WORD_BYTES));
    raw.bytes = std::as_bytes(input.subspan(HEADER_BYTES));
    const RawSample sample{
        .outcome = static_cast<Outcome>(1U + (input[0] % OUTCOMES)),
        .raw = raw};
    const CompanionInput companion{.outcome = Outcome::VALUE, .raw = raw};

    static const std::vector<PointRules> RULES = make_rules();
    for (const PointRules& rules : RULES) {
        PointState state;
        scada::driver::reset_point_state(state, rules.tag_type, 0);
        // Twice, so that the second sees the state the first left.
        for (std::int64_t step = 1; step <= 2; ++step) {
            check(scada::driver::process_sample(
                rules, state, sample, {&companion, 1},
                SampleTimes{.receive_time_ns = step * SECOND,
                            .receive_mono_ns = step * SECOND}));
        }
        ProcessedSample stale;
        if (scada::driver::check_stale(rules, state, 3 * SECOND, stale)) {
            check(stale);
        }
    }

    // The write conversion of the same numbers, on the paths of 8.4.
    const scada::driver::ScaleRule gain{.gain = GAIN};
    const scada::driver::ScaleRule exact{
        .integer = true, .gain_i = GAIN_I, .offset_i = OFFSET_I};
    const scada::driver::WriteRules float_rules{.tag_type = TagType::FLOAT64,
                                                .raw_kind = RawKind::SIGNED,
                                                .raw_width = raw.width,
                                                .scale = &gain};
    const scada::driver::WriteRules integer_rules{.tag_type = TagType::INT64,
                                                  .raw_kind = RawKind::UNSIGNED,
                                                  .raw_width = raw.width,
                                                  .scale = &exact};
    if (raw.width >= WIDTHS.at(1)) {
        const std::optional<RawValue> from_float = scada::driver::to_raw_value(
            float_rules, scada::driver::float64_value(raw.number_f));
        const std::optional<RawValue> from_integer =
            scada::driver::to_raw_value(
                integer_rules, scada::driver::int64_value(raw.number_i));
        for (const std::optional<RawValue>& converted :
             {from_float, from_integer}) {
            if (converted.has_value() &&
                converted->pattern != (converted->pattern &
                                       scada::driver::width_mask(raw.width))) {
                std::abort();
            }
        }
    }
    return 0;
}
