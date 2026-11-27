// Fuzz target for the processing after decode and the write conversion.
// Raw values come from devices through a protocol's decode, so every field
// of a raw sample is outside input; the rule fields are taken from the input
// too, so that rules the compile step would refuse cannot cause undefined
// behaviour either. Each input is a header, a sequence of samples and a
// string. Invariants that abort, so the fuzzer reports them:
// - every write has a quality and flags of the tag database (tag database
//   contract 6), never OWNER_DOWN or RESTORED (contract 5.2);
// - a STRING value is at most 250 bytes of well-formed UTF-8;
// - a GOOD or UNCERTAIN FLOAT64 value is finite;
// - a write carries SUBSTITUTED exactly when its value is a configured
//   substitute, given now or kept (review R1 of #157), and a STRING write
//   never does (#169). A second state runs
//   the same samples with another substitute value: the values written
//   differ exactly when the value is a substitute;
// - a write conversion that succeeds gives a pattern within its width, and
//   on the integer path reads back as the request.
// The features byte of the header turns each companion rule on or off and
// the companions take raw values of their own, so that GOOD, UNCERTAIN and
// BAD writes, substitutes and holds all occur (#165). With SCADA_FUZZ_STATS
// set, a run prints how many writes of each quality and each path it saw;
// with SCADA_FUZZ_REACH set, it also fails unless it saw each of them.

#include "../src/utf8.h"
#include "../tests/tzif_builder.h"
#include "scada/driver/point_rules.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/sample_processing.h"
#include "scada/driver/tag_value.h"
#include "scada/driver/write_conversion.h"
#include "scada/driver/zone_table.h"
#include "scada/tag/types.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
using scada::tag::QualityFlag;
using scada::tag::TagType;

constexpr std::size_t HEADER_BYTES = 10;
constexpr std::size_t SAMPLE_BYTES = 28;
constexpr std::size_t MAX_SAMPLES = 16;
constexpr unsigned OUTCOMES = 5;
constexpr unsigned KINDS = 6;
constexpr unsigned TAG_TYPES = 5;  // BOOL to STRING (#169)
constexpr std::array<std::uint8_t, 6> WIDTHS{0, 1, 8, 16, 32, 64};
constexpr std::int64_t SECOND = 1'000'000'000;
constexpr std::uint64_t COUNTER_LIMIT = 65535;
constexpr std::int64_t ENUM_CODE_TWO = 2;
constexpr unsigned BYTE_BITS = 8;

// A zone file of an invented zone: +1 h, with daylight saving from the last
// Sunday of March to the last Sunday of October, by its footer alone.
std::vector<std::byte> zone_file() {
    constexpr std::int32_t PLUS_ONE_HOUR = 3600;
    return scada::driver::test::build_tzif(
        scada::driver::test::TzifSpec{.transitions = {},
                                      .type_of = {},
                                      .offsets = {PLUS_ONE_HOUR},
                                      .footer = "CET-1CEST,M3.5.0,M10.5.0/3"});
}

const scada::driver::ZoneTable& zone() {
    static const scada::driver::ZoneTable TABLE = [] {
        const auto built = scada::driver::ZoneTable::from_tzif(zone_file());
        if (!built.ok()) {
            std::abort();
        }
        return built.value();
    }();
    return TABLE;
}

std::uint64_t word_at(std::span<const std::uint8_t> data, std::size_t offset) {
    std::array<std::uint8_t, BYTE_BITS> bytes{};
    for (std::size_t index = 0; index < BYTE_BITS; ++index) {
        bytes.at(index) = data[offset + index];
    }
    return std::bit_cast<std::uint64_t>(bytes);
}

// Where the rule fields are in the header, and the options of its second
// byte.
constexpr std::size_t TYPE_BYTE = 0;
constexpr std::size_t CHOICE_BYTE = 1;
constexpr std::size_t GAIN_BYTE = 2;
constexpr std::size_t OFFSET_BYTE = 3;
constexpr std::size_t EXPONENT_BYTE = 4;
constexpr std::size_t MARKER_BYTE = 5;
constexpr std::size_t LOW_BYTE = 6;
constexpr std::size_t HIGH_BYTE = 7;
constexpr std::size_t FEATURES_BYTE = 8;
constexpr std::size_t STATUS_BYTE = 9;
constexpr unsigned HAS_SCALE = 0;
constexpr unsigned DIVIDE = 1;
constexpr unsigned INTEGER = 2;
constexpr unsigned HAS_EXPONENT = 3;
constexpr unsigned HAS_MARKER = 4;
constexpr unsigned HAS_COUNTER = 5;
constexpr unsigned HAS_ENUM = 6;
constexpr unsigned LOCAL_COMPANION = 7;
// The options of the features byte (#165).
constexpr unsigned TIME_COMPANION = 0;  // `time_from` a companion
constexpr unsigned HEALTH = 1;          // a `health` rule on companion 1
constexpr unsigned QUALITY_RULE = 2;    // a `quality_from` rule
constexpr unsigned HEALTH_FRESH = 3;    // companion 1 is never stale
constexpr unsigned TIME_RAW = 4;        // companion 0 holds a device time
constexpr unsigned DOUBLE_POINT = 5;    // `kind: double_point` (#169)
constexpr unsigned RAW_RANGE = 6;       // `raw_range` (#169)
constexpr unsigned STATUS_GOOD = 0;     // of the status byte: always good
constexpr unsigned SLOTS = 3;           // two companions and one past them
constexpr unsigned ACTIONS = 3;
constexpr unsigned MAX_AGE_STEPS = 4;
constexpr double GAIN_DIVISOR = 7.0;
constexpr std::uint16_t ALL_FLAGS = 0xFFFFU;

bool option(std::uint8_t choice, unsigned bit) noexcept {
    return ((static_cast<unsigned>(choice) >> bit) & 1U) != 0;
}

// Rules from the header bytes: tag type, scale, markers, exponent, bits,
// fields, counters and times all reachable, valid or not.
PointRules rules_from(std::span<const std::uint8_t> header) {
    PointRules rules;
    rules.tag_type = static_cast<TagType>(1U + (header[TYPE_BYTE] % TAG_TYPES));
    // The configured raw type, from the byte's high bits: valid and refused
    // pairs of tag and raw type both occur (CF-504, #169).
    constexpr unsigned RAW_KIND_SHIFT = 3;
    rules.raw_kind = static_cast<RawKind>(
        1U + ((header[TYPE_BYTE] >> RAW_KIND_SHIFT) % KINDS));
    const std::uint8_t choice = header[CHOICE_BYTE];
    const auto gain = static_cast<std::int8_t>(header[GAIN_BYTE]);
    const auto offset = static_cast<std::int8_t>(header[OFFSET_BYTE]);
    rules.companions = {1, 2};
    if (option(choice, HAS_SCALE)) {
        rules.scale = scada::driver::ScaleRule{
            .form = option(choice, DIVIDE)
                        ? scada::driver::ScaleRule::Form::DIVIDE_BY
                        : scada::driver::ScaleRule::Form::GAIN,
            .integer = option(choice, INTEGER),
            .gain_i = gain,
            .offset_i = offset,
            .divide_by = header[GAIN_BYTE],
            .gain = static_cast<double>(gain) / GAIN_DIVISOR,
            .offset = static_cast<double>(offset)};
    }
    if (option(choice, HAS_EXPONENT)) {
        rules.exponent = scada::driver::ExponentRule{
            .companion =
                static_cast<std::uint8_t>(header[EXPONENT_BYTE] % SLOTS),
            .divide = option(header[EXPONENT_BYTE], LOCAL_COMPANION)};
    }
    if (option(choice, HAS_MARKER)) {
        rules.markers.push_back(scada::driver::InvalidMarker{
            .pattern = header[MARKER_BYTE],
            .action = static_cast<scada::driver::InvalidAction>(
                1U + (header[MARKER_BYTE] % ACTIONS)),
            .flags = static_cast<std::uint16_t>(
                header[LOW_BYTE] | (header[HIGH_BYTE] << BYTE_BITS)),
            .substitute = scada::driver::zero_value(rules.tag_type)});
        rules.markers.push_back(scada::driver::InvalidMarker{.any_nan = true});
    }
    if (option(choice, HAS_COUNTER)) {
        rules.counter = scada::driver::CounterRule{
            .limit = COUNTER_LIMIT,
            .saturate = option(header[EXPONENT_BYTE], 0),
            .on_decrease = static_cast<scada::driver::CounterRule::OnDecrease>(
                1U + (header[LOW_BYTE] % ACTIONS))};
    } else if (option(choice, HAS_ENUM)) {
        rules.has_enum = true;
        rules.enum_codes = {0, 1, ENUM_CODE_TWO};
    }
    rules.bits = {
        scada::driver::BitRule{.bit = header[LOW_BYTE]},
        scada::driver::BitRule{.bit = header[HIGH_BYTE], .invert = true}};
    const std::uint8_t features = header[FEATURES_BYTE];
    if (option(features, QUALITY_RULE)) {
        rules.quality_rules.push_back(scada::driver::QualityRule{
            .self = option(header[GAIN_BYTE], 0),
            .has_field = true,
            .lo = header[OFFSET_BYTE],
            .hi = header[EXPONENT_BYTE],
            .entries = {{.key = 0,
                         .mark = {.quality = Quality::UNCERTAIN,
                                  .flags = ALL_FLAGS}}}});
    }
    if (option(features, HEALTH)) {
        rules.health.push_back(scada::driver::HealthRule{
            .companion = 1, .good = {1}, .comm_fail = true});
    }
    if (option(features, DOUBLE_POINT)) {
        using scada::driver::DoublePoint;
        rules.double_point = scada::driver::DoublePointRule{
            .states = {{.raw = 0, .code = DoublePoint::INTERMEDIATE},
                       {.raw = 1, .code = DoublePoint::OPEN},
                       {.raw = 2, .code = DoublePoint::CLOSED},
                       {.raw = 3, .code = DoublePoint::BAD_STATE}}};
    }
    if (option(features, RAW_RANGE)) {
        const auto low = static_cast<double>(gain);
        rules.raw_range = scada::driver::RawRange{
            .min = low, .max = low + static_cast<double>(header[OFFSET_BYTE])};
    }
    if (option(features, TIME_COMPANION)) {
        rules.time_companion =
            static_cast<std::uint8_t>(header[MARKER_BYTE] % SLOTS);
    }
    rules.device_time =
        scada::driver::DeviceTimeRule{.local = true, .zone = &zone()};
    rules.companion_time =
        scada::driver::DeviceTimeRule{.local = option(choice, LOCAL_COMPANION),
                                      .zone = &zone(),
                                      .synced = true};
    rules.max_age_ns = SECOND * (1 + (choice % MAX_AGE_STEPS));
    return rules;
}

// Where the fields of a sample are in its bytes.
constexpr std::size_t PATTERN_OFFSET = 4;
constexpr std::size_t NUMBER_OFFSET = 12;
constexpr std::size_t FLOAT_OFFSET = 20;

RawValue raw_from(std::span<const std::uint8_t> bytes,
                  std::span<const std::byte> text) {
    RawValue raw;
    raw.kind = static_cast<RawKind>(1U + (bytes[0] % KINDS));
    raw.width = WIDTHS.at(bytes[1] % WIDTHS.size());
    raw.pattern =
        word_at(bytes, PATTERN_OFFSET) & scada::driver::width_mask(raw.width);
    raw.number_i = static_cast<std::int64_t>(word_at(bytes, NUMBER_OFFSET));
    raw.number_u = word_at(bytes, NUMBER_OFFSET);
    raw.number_f = std::bit_cast<double>(word_at(bytes, FLOAT_OFFSET));
    raw.bytes = text;
    return raw;
}

void check(const TagWrite& write) {
    constexpr std::uint16_t NEVER =
        scada::tag::to_raw(QualityFlag::OWNER_DOWN) |
        scada::tag::to_raw(QualityFlag::RESTORED);
    if (!scada::tag::quality_from_raw(scada::tag::to_raw(write.quality))
             .has_value() ||
        !scada::tag::quality_flags_valid(write.flags) ||
        (write.flags & NEVER) != 0) {
        std::abort();
    }
    if (write.value.type == TagType::STRING &&
        (write.value.text.size() > scada::driver::TAG_STRING_MAX_BYTES ||
         !scada::driver::detail::is_utf8(std::as_bytes(
             std::span(write.value.text.data(), write.value.text.size()))))) {
        std::abort();
    }
    if (write.value.type == TagType::FLOAT64 && write.quality != Quality::BAD &&
        !std::isfinite(write.value.f)) {
        std::abort();
    }
}

// The companions of one sample: companion 0 (a status, an exponent or a
// time source) with the sample's raw value or a device time, and companion
// 1, the health point, whose status is 1 (good) or 0.
std::array<CompanionInput, 2> companions_for(
    std::span<const std::uint8_t> header, std::span<const std::uint8_t> bytes,
    const RawValue& raw) {
    const std::uint8_t features = header[FEATURES_BYTE];
    RawValue first = raw;
    if (option(features, TIME_RAW)) {
        first = RawValue{};
        first.kind = RawKind::TIME;
        first.number_i = raw.number_i;
    }
    constexpr unsigned STATUS_WIDTH = 16;
    // The status bit is the low bit of the sample's last byte, not of the
    // width byte, so that health and width vary apart (#169).
    constexpr std::size_t STATUS_OF_SAMPLE = SAMPLE_BYTES - 1;
    const bool healthy = option(header[STATUS_BYTE], STATUS_GOOD) ||
                         (bytes[STATUS_OF_SAMPLE] & 1U) != 0;
    return {
        CompanionInput{
            .outcome = static_cast<Outcome>(1U + (bytes[3] % OUTCOMES)),
            .raw = first,
            .mark = {.quality = static_cast<Quality>(bytes[2] % 3U),
                     .flags = static_cast<std::uint16_t>(raw.pattern)}},
        CompanionInput{
            .outcome = Outcome::VALUE,
            .raw = scada::driver::raw_unsigned(healthy ? 1 : 0, STATUS_WIDTH),
            .stale = !option(features, HEALTH_FRESH) && (bytes[0] & 1U) != 0}};
}

// Equal values, a FLOAT64 by its bits.
bool same_value(const scada::driver::TagValue& got,
                const scada::driver::TagValue& want) {
    if (got.type != want.type) {
        return false;
    }
    switch (want.type) {
        case TagType::BOOL:
            return got.b == want.b;
        case TagType::INT64:
            return got.i == want.i;
        case TagType::UINT64:
            return got.u == want.u;
        case TagType::FLOAT64:
            return std::bit_cast<std::uint64_t>(got.f) ==
                   std::bit_cast<std::uint64_t>(want.f);
        case TagType::STRING:
            return got.text == want.text;
    }
    return false;
}

// A value of `type` that is not zero_value(type), for the second state.
scada::driver::TagValue other_substitute(TagType type) {
    switch (type) {
        case TagType::BOOL:
            return scada::driver::bool_value(true);
        case TagType::INT64:
            return scada::driver::int64_value(1);
        case TagType::UINT64:
            return scada::driver::uint64_value(1);
        case TagType::FLOAT64:
            return scada::driver::float64_value(1.0);
        case TagType::STRING:
            // Static text: a substitute borrows its text (#169).
            return scada::driver::string_value("other");
    }
    return scada::driver::zero_value(type);
}

// SUBSTITUTED both ways: `write` and `other` are the writes of the two
// states. A value that differs is a substitute and has the flag; a value
// that does not is none and has not.
void check_substituted(const TagWrite& write, const TagWrite& other) {
    const bool substitute = !same_value(write.value, other.value);
    const auto marked = [](const TagWrite& one) {
        return (one.flags & scada::tag::to_raw(QualityFlag::SUBSTITUTED)) != 0;
    };
    if (marked(write) != substitute || marked(other) != substitute) {
        std::abort();
    }
    // A STRING tag has no substitute: a marker never matches a string raw,
    // and a STRING tag on any other raw is refused (CF-504, #169).
    if (write.value.type == TagType::STRING && marked(write)) {
        std::abort();
    }
}

// What a run saw, by write (#165): each quality, and the paths that hold
// or substitute a value and what follows them. Printed at exit when
// SCADA_FUZZ_STATS is set, so that a replay of the corpus shows them.
struct Stats {
    std::array<std::uint64_t, 4> by_quality{};  // by the quality's raw value
    std::uint64_t substitutes = 0;    // a substitute given now or kept
    std::uint64_t holds = 0;          // a value held UNCERTAIN with STALE
    std::uint64_t expired_holds = 0;  // a hold that went BAD with STALE
    std::uint64_t good_after_substitute = 0;
    std::uint64_t good_after_hold = 0;  // after a hold or its expiry
    std::uint64_t good_strings = 0;     // a STRING tag written GOOD (#169)
    std::uint64_t good_bools = 0;       // a BOOL tag written GOOD
    std::uint64_t good_double_points = 0;
    std::uint64_t outside_raw_range = 0;  // UNCERTAIN by `raw_range`
};

// The process-wide counts of the run.
Stats& run_stats() {
    static Stats stats;
    return stats;
}

void print_stats() {
    const Stats& stats = run_stats();
    const auto quality = [&stats](Quality value) {
        return std::to_string(stats.by_quality.at(scada::tag::to_raw(value)));
    };
    const std::string text =
        "writes: GOOD " + quality(Quality::GOOD) + ", UNCERTAIN " +
        quality(Quality::UNCERTAIN) + ", BAD " + quality(Quality::BAD) +
        "\npaths: substitute " + std::to_string(stats.substitutes) +
        ", good after substitute " +
        std::to_string(stats.good_after_substitute) + ", hold " +
        std::to_string(stats.holds) + ", expired hold " +
        std::to_string(stats.expired_holds) + ", good after hold " +
        std::to_string(stats.good_after_hold) + "\nkinds: good string " +
        std::to_string(stats.good_strings) + ", good bool " +
        std::to_string(stats.good_bools) + ", good double point " +
        std::to_string(stats.good_double_points) + ", outside raw_range " +
        std::to_string(stats.outside_raw_range) + "\n";
    (void)std::fputs(text.c_str(), stderr);
}

// With SCADA_FUZZ_REACH set: the run fails unless it wrote GOOD and
// UNCERTAIN values and hit every path of Stats, so that a corpus that no
// longer reaches them fails its replay (#165).
void check_reach() {
    print_stats();
    const Stats& stats = run_stats();
    const std::array<std::uint64_t, 11> needed{
        stats.by_quality.at(scada::tag::to_raw(Quality::GOOD)),
        stats.by_quality.at(scada::tag::to_raw(Quality::UNCERTAIN)),
        stats.substitutes,
        stats.good_after_substitute,
        stats.holds,
        stats.expired_holds,
        stats.good_after_hold,
        stats.good_strings,
        stats.good_bools,
        stats.good_double_points,
        stats.outside_raw_range};
    if (std::ranges::find(needed, std::uint64_t{0}) != needed.end()) {
        (void)std::fputs("not every quality and path was reached\n", stderr);
        std::_Exit(EXIT_FAILURE);
    }
}

// Registers what the environment asks for at exit, once. Called from the
// first input, so that libFuzzer and the replay driver of the other builds
// both do it.
bool register_at_exit() {
    // NOLINTBEGIN(concurrency-mt-unsafe): read once, by the one thread that
    // runs the inputs
    if (std::getenv("SCADA_FUZZ_REACH") != nullptr) {
        (void)std::atexit(check_reach);
    } else if (std::getenv("SCADA_FUZZ_STATS") != nullptr) {
        (void)std::atexit(print_stats);
    }
    // NOLINTEND(concurrency-mt-unsafe)
    return true;
}

// What the last write of one input was, for the paths that follow it.
enum class Last : std::uint8_t { OTHER, SUBSTITUTE, HOLD, EXPIRED };

// Counts `write` (with `other`, the second state's write) after `last`;
// `held` says that the write can be a hold (processing, not the stale
// check). Returns what this write was.
Last count(const PointRules& rules, const TagWrite& write,
           const TagWrite& other, Last last, bool held) {
    Stats& stats = run_stats();
    ++stats.by_quality.at(
        static_cast<std::size_t>(scada::tag::to_raw(write.quality)) %
        stats.by_quality.size());
    if (write.quality == Quality::GOOD) {
        stats.good_strings += write.value.type == TagType::STRING ? 1U : 0U;
        stats.good_bools += write.value.type == TagType::BOOL ? 1U : 0U;
        stats.good_double_points += rules.double_point.has_value() ? 1U : 0U;
    }
    if (rules.raw_range.has_value() && write.quality == Quality::UNCERTAIN &&
        (write.flags & scada::tag::to_raw(QualityFlag::OUT_OF_RANGE)) != 0) {
        ++stats.outside_raw_range;
    }
    const bool stale =
        (write.flags & scada::tag::to_raw(QualityFlag::STALE)) != 0;
    if (write.quality == Quality::GOOD) {
        if (last == Last::SUBSTITUTE) {
            ++stats.good_after_substitute;
        } else if (last == Last::HOLD || last == Last::EXPIRED) {
            ++stats.good_after_hold;
        }
    }
    if (!same_value(write.value, other.value)) {
        ++stats.substitutes;
        return Last::SUBSTITUTE;
    }
    if (held && write.quality == Quality::UNCERTAIN && stale) {
        ++stats.holds;
        return Last::HOLD;
    }
    if (last == Last::HOLD && write.quality == Quality::BAD && stale) {
        ++stats.expired_holds;
        return Last::EXPIRED;
    }
    return last == Last::EXPIRED && write.quality == Quality::BAD
               ? Last::EXPIRED
               : Last::OTHER;
}

// The raw value of a write, read back by the processing with the same
// rules, gives the value the write reports as written (contract 8.4; CS 4.7:
// read and write of an integer value type are exact inverses).
void check_read_back(const scada::driver::WriteRules& write,
                     const scada::driver::WriteValue& converted) {
    PointRules read;
    read.tag_type = write.tag_type;
    read.raw_kind = write.raw_kind;
    read.invert = write.invert;
    if (write.scale != nullptr) {
        read.scale = *write.scale;
    }
    read.has_enum = write.has_enum;
    read.enum_codes.assign(write.enum_codes.begin(), write.enum_codes.end());
    if (!scada::driver::validate_point_rules(read).ok()) {
        return;  // the compile step refuses these rules
    }
    PointState state;
    scada::driver::init_point_state(state, read.tag_type);
    scada::driver::reset_point_state(state, read.tag_type, 0);
    ProcessedSample out;
    scada::driver::process_sample(
        read, state, RawSample{.outcome = Outcome::VALUE, .raw = converted.raw},
        {}, scada::driver::SampleTimes{}, out);
    if (out.tag.quality != Quality::GOOD ||
        !same_value(out.tag.value, converted.written)) {
        std::abort();
    }
}

void check_conversions(const RawValue& raw,
                       std::span<const std::uint8_t> header) {
    const PointRules rules = rules_from(header);
    const scada::driver::WriteRules write{
        .tag_type = rules.tag_type,
        // The point's raw type is the sample's, so that the width fits it;
        // a string or time raw value takes one from the header.
        .raw_kind = raw.kind == RawKind::STRING || raw.kind == RawKind::TIME
                        ? static_cast<RawKind>(1U + (header[0] % 4U))
                        : raw.kind,
        .raw_width = raw.width,
        .invert = (header[1] & 1U) != 0,
        .scale = rules.scale.has_value() ? &*rules.scale : nullptr,
        .enum_codes = rules.enum_codes,
        .has_enum = rules.has_enum};
    const std::array<scada::driver::TagValue, 4> values{
        scada::driver::float64_value(raw.number_f),
        scada::driver::int64_value(raw.number_i),
        scada::driver::uint64_value(raw.number_u),
        scada::driver::bool_value((raw.number_u & 1U) != 0)};
    for (const scada::driver::TagValue& value : values) {
        const auto converted = scada::driver::to_raw_value(write, value);
        if (!converted.has_value()) {
            continue;
        }
        if ((converted->raw.pattern &
             ~scada::driver::width_mask(write.raw_width)) != 0) {
            std::abort();
        }
        if (scada::driver::is_integer_path(write) &&
            value.type != TagType::BOOL &&
            (converted->written.i != value.i ||
             converted->written.u != value.u)) {
            std::abort();
        }
        check_read_back(write, *converted);
    }
    (void)scada::driver::raw_from_pattern(raw.kind, raw.width, raw.pattern);
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    static const bool REGISTERED = register_at_exit();
    (void)REGISTERED;
    if (size < HEADER_BYTES + SAMPLE_BYTES) {
        return 0;
    }
    const std::span<const std::uint8_t> input(data, size);
    const std::span<const std::uint8_t> header = input.first(HEADER_BYTES);
    const std::size_t samples =
        std::min(MAX_SAMPLES, (size - HEADER_BYTES) / SAMPLE_BYTES);
    const std::span<const std::byte> text =
        std::as_bytes(input.subspan(HEADER_BYTES + (samples * SAMPLE_BYTES)));

    const PointRules rules = rules_from(header);
    PointRules other_rules = rules;
    for (scada::driver::InvalidMarker& marker : other_rules.markers) {
        if (marker.action == scada::driver::InvalidAction::SUBSTITUTE) {
            marker.substitute = other_substitute(rules.tag_type);
        }
    }
    PointState state;
    scada::driver::init_point_state(state, rules.tag_type);
    scada::driver::reset_point_state(state, rules.tag_type, 0);
    PointState other_state;
    scada::driver::init_point_state(other_state, rules.tag_type);
    scada::driver::reset_point_state(other_state, rules.tag_type, 0);
    ProcessedSample out;
    ProcessedSample other;
    Last last = Last::OTHER;
    for (std::size_t index = 0; index < samples; ++index) {
        const std::span<const std::uint8_t> bytes =
            input.subspan(HEADER_BYTES + (index * SAMPLE_BYTES), SAMPLE_BYTES);
        const RawValue raw = raw_from(bytes, text);
        const RawSample sample{
            .outcome = static_cast<Outcome>(1U + (bytes[2] % OUTCOMES)),
            .raw = raw,
            .mark = {.quality = static_cast<Quality>(bytes[3] % 4U),
                     .flags = static_cast<std::uint16_t>(
                         word_at(bytes, FLOAT_OFFSET))}};
        const std::array<CompanionInput, 2> companions =
            companions_for(header, bytes, raw);
        const auto step = static_cast<std::int64_t>(index + 1);
        const SampleTimes times{.receive_time_ns = step * SECOND,
                                .receive_mono_ns = step * SECOND};
        scada::driver::process_sample(rules, state, sample, companions, times,
                                      out);
        scada::driver::process_sample(other_rules, other_state, sample,
                                      companions, times, other);
        check(out.tag);
        check_substituted(out.tag, other.tag);
        last = count(rules, out.tag, other.tag, last, true);
        const std::int64_t later = (step * SECOND) + (SECOND / 2);
        const bool stale = scada::driver::check_stale(rules, state, later, out);
        if (scada::driver::check_stale(other_rules, other_state, later,
                                       other) != stale) {
            std::abort();
        }
        if (stale) {
            check(out.tag);
            check_substituted(out.tag, other.tag);
            last = count(rules, out.tag, other.tag, last, false);
        }
        // The zone table on any device time.
        (void)zone().to_utc(raw.number_i);
        check_conversions(raw, header);
    }
    return 0;
}
