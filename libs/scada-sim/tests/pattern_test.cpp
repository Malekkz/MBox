// Unit tests for the value patterns of the simulator: the self-checking
// pattern tells a torn value from a whole one, and the waveforms have the
// shape and range they promise.

#include "scada/sim/pattern.h"

#include "scada/tag/scada_tag.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace {

using namespace scada::sim;

constexpr double AMPLITUDE = 10.0;
constexpr std::uint64_t PERIOD = 8;
constexpr std::uint64_t WALK_STEPS = 10'000;
constexpr std::uint64_t SOME_COUNT = 12'345;
// Every combination of the 12 flag bits once.
constexpr std::uint64_t FLAG_COMBINATIONS = 1U << 12U;
constexpr std::uint64_t FIRST_COUNTS = 100;
constexpr std::uint64_t SEED = 7;

// A value as a read of write number `count` returns it.
scada_value as_read(std::uint64_t count) {
    scada_value value = check_value(count);
    value.change_count = static_cast<std::uint32_t>(count);
    return value;
}

TEST(Pattern, NamesRoundTrip) {
    for (const Pattern pattern : {Pattern::CHECK, Pattern::RAMP, Pattern::SINE,
                                  Pattern::STEP, Pattern::WALK}) {
        EXPECT_EQ(pattern_from_name(to_string(pattern)), pattern);
    }
    EXPECT_EQ(pattern_from_name("square"), std::nullopt);
    EXPECT_EQ(pattern_from_name(""), std::nullopt);
    EXPECT_EQ(pattern_from_name("CHECK"), std::nullopt);
}

TEST(Pattern, CheckIsUnsignedAndTheWavesAreFloatingPoint) {
    EXPECT_EQ(tag_type(Pattern::CHECK), scada::tag::TagType::UINT64);
    EXPECT_EQ(tag_type(Pattern::RAMP), scada::tag::TagType::FLOAT64);
    EXPECT_EQ(tag_type(Pattern::WALK), scada::tag::TagType::FLOAT64);
}

TEST(Pattern, CheckValueFieldsAreDerivedFromTheCount) {
    const scada_value value = check_value(SOME_COUNT);
    EXPECT_EQ(value.type, SCADA_TYPE_UINT64);
    EXPECT_EQ(value.value, SOME_COUNT);
    EXPECT_EQ(value.quality, SOME_COUNT % CHECK_QUALITIES);
    EXPECT_EQ(value.quality_flags, SOME_COUNT & CHECK_FLAGS);
    EXPECT_EQ(value.source_time_ns,
              CHECK_BASE_SOURCE_TIME + static_cast<std::int64_t>(SOME_COUNT));
}

TEST(Pattern, CheckNeverWritesTheFlagOnlyAReadAdds) {
    std::uint64_t with_owner_down = 0;
    for (std::uint64_t count = 1; count <= FLAG_COMBINATIONS; ++count) {
        if ((check_value(count).quality_flags & SCADA_FLAG_OWNER_DOWN) != 0) {
            ++with_owner_down;
        }
    }
    EXPECT_EQ(with_owner_down, 0U);
}

TEST(Pattern, AWholeCheckValueIsRecognised) {
    for (std::uint64_t count = 1; count < FIRST_COUNTS; ++count) {
        EXPECT_TRUE(is_check_value(as_read(count))) << count;
    }
    EXPECT_TRUE(is_check_value(as_read(MAX_CHECK_COUNT)));
    // The change count of the record wraps at 32 bits.
    EXPECT_TRUE(is_check_value(as_read((std::uint64_t{1} << 32U) + 5)));
}

TEST(Pattern, ANeverWrittenRecordIsWhole) {
    scada_value value{};
    value.type = SCADA_TYPE_UINT64;
    value.quality = SCADA_QUALITY_BAD;
    value.quality_flags = SCADA_FLAG_NOT_INIT;
    EXPECT_TRUE(is_check_value(value));
    value.quality_flags = SCADA_FLAG_NOT_INIT | SCADA_FLAG_OWNER_DOWN;
    EXPECT_FALSE(is_check_value(value));
}

// A read across the first write: value 0 and the flags of the record before
// it, with a field of the write.
TEST(Pattern, ANeverWrittenRecordWithAFieldOfTheFirstWriteIsTorn) {
    scada_value never{};
    never.type = SCADA_TYPE_UINT64;
    never.quality = SCADA_QUALITY_BAD;
    never.quality_flags = SCADA_FLAG_NOT_INIT;
    const scada_value first = as_read(1);
    scada_value torn = never;
    torn.source_time_ns = first.source_time_ns;
    EXPECT_FALSE(is_check_value(torn));
    torn = never;
    torn.change_count = first.change_count;
    EXPECT_FALSE(is_check_value(torn));
    torn = never;
    torn.update_time_ns = first.source_time_ns;
    EXPECT_FALSE(is_check_value(torn));
    torn = never;
    torn.update_mono_ns = 1;
    EXPECT_FALSE(is_check_value(torn));
}

TEST(Pattern, EveryFieldOfAnotherWriteIsTorn) {
    const scada_value whole = as_read(SOME_COUNT);
    const scada_value other = as_read(SOME_COUNT + 1);
    scada_value torn = whole;
    torn.quality = other.quality;
    EXPECT_FALSE(is_check_value(torn));
    torn = whole;
    torn.quality_flags = other.quality_flags;
    EXPECT_FALSE(is_check_value(torn));
    torn = whole;
    torn.source_time_ns = other.source_time_ns;
    EXPECT_FALSE(is_check_value(torn));
    torn = whole;
    torn.change_count = other.change_count;
    EXPECT_FALSE(is_check_value(torn));
    torn = whole;
    torn.value = other.value;
    EXPECT_FALSE(is_check_value(torn));
    torn = whole;
    torn.type = SCADA_TYPE_INT64;
    EXPECT_FALSE(is_check_value(torn));
    torn = whole;
    torn.value = MAX_CHECK_COUNT + 1;
    EXPECT_FALSE(is_check_value(torn));
}

TEST(Pattern, ChangesAreCheckedTheSameWay) {
    const scada_value written = check_value(SOME_COUNT);
    scada_change change{};
    change.type = written.type;
    change.quality = written.quality;
    change.quality_flags = written.quality_flags;
    change.value = written.value;
    change.source_time_ns = written.source_time_ns;
    EXPECT_TRUE(is_check_change(change));
    change.source_time_ns += 1;
    EXPECT_FALSE(is_check_change(change));
    // A change is always a write.
    EXPECT_FALSE(is_check_change(scada_change{}));
}

TEST(Pattern, RampRisesAndStartsAgain) {
    const Waveform waveform{
        .amplitude = AMPLITUDE, .period = PERIOD, .seed = 1};
    for (std::uint64_t count = 1; count <= 2 * PERIOD; ++count) {
        const double expected = AMPLITUDE *
                                static_cast<double>(count % PERIOD) /
                                static_cast<double>(PERIOD);
        EXPECT_DOUBLE_EQ(wave_value(Pattern::RAMP, waveform, 0, count, 0.0),
                         expected);
    }
    // The tag shifts the phase.
    EXPECT_DOUBLE_EQ(wave_value(Pattern::RAMP, waveform, 1, PERIOD, 0.0),
                     AMPLITUDE / static_cast<double>(PERIOD));
}

TEST(Pattern, SineStaysWithinTheAmplitudeAndRepeats) {
    const Waveform waveform{
        .amplitude = AMPLITUDE, .period = PERIOD, .seed = 1};
    EXPECT_NEAR(wave_value(Pattern::SINE, waveform, 0, PERIOD / 4, 0.0),
                AMPLITUDE, 1e-9);
    for (std::uint64_t count = 1; count <= PERIOD; ++count) {
        const double value = wave_value(Pattern::SINE, waveform, 3, count, 0.0);
        EXPECT_LE(std::abs(value), AMPLITUDE);
        EXPECT_DOUBLE_EQ(
            value, wave_value(Pattern::SINE, waveform, 3, count + PERIOD, 0.0));
    }
}

TEST(Pattern, StepIsLowForHalfAPeriodThenHigh) {
    const Waveform waveform{
        .amplitude = AMPLITUDE, .period = PERIOD, .seed = 1};
    for (std::uint64_t count = PERIOD; count < 2 * PERIOD; ++count) {
        const double expected = count - PERIOD < PERIOD / 2 ? 0.0 : AMPLITUDE;
        EXPECT_DOUBLE_EQ(wave_value(Pattern::STEP, waveform, 0, count, 0.0),
                         expected)
            << count;
    }
}

TEST(Pattern, WalkTakesSmallStepsWithinTheAmplitude) {
    const Waveform waveform{
        .amplitude = AMPLITUDE, .period = PERIOD, .seed = SEED};
    const double largest_step = 2.0 * AMPLITUDE / static_cast<double>(PERIOD);
    double value = 0.0;
    bool moved = false;
    for (std::uint64_t count = 1; count <= WALK_STEPS; ++count) {
        const double next =
            wave_value(Pattern::WALK, waveform, 2, count, value);
        EXPECT_LE(std::abs(next - value), largest_step);
        EXPECT_LE(std::abs(next), AMPLITUDE);
        moved = moved || next != value;
        value = next;
    }
    EXPECT_TRUE(moved);
}

TEST(Pattern, WalkIsAFunctionOfSeedTagAndCount) {
    const Waveform waveform{
        .amplitude = AMPLITUDE, .period = PERIOD, .seed = SEED};
    Waveform other_seed = waveform;
    other_seed.seed = SEED + 1;
    const double value = wave_value(Pattern::WALK, waveform, 2, 5, 1.0);
    EXPECT_DOUBLE_EQ(value, wave_value(Pattern::WALK, waveform, 2, 5, 1.0));
    EXPECT_NE(value, wave_value(Pattern::WALK, other_seed, 2, 5, 1.0));
    EXPECT_NE(value, wave_value(Pattern::WALK, waveform, 3, 5, 1.0));
}

}  // namespace
