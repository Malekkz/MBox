// Unit tests of the suspend detection of scada-tagd (contract 3.4,
// decision D-45), with the clocks given as samples.

#include "scada/tagd/suspend.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>

namespace {

using scada::tagd::ClockSample;
using scada::tagd::read_sleep_states;
using scada::tagd::SuspendFinding;
using scada::tagd::SuspendWatch;

constexpr std::int64_t BOOT_NS = 5'000'000'000;
constexpr std::int64_t READ_NS = 2'000;  // between the two reads
constexpr std::int64_t SUSPEND_NS = 7'000'000'000;
constexpr std::int64_t STEP_NS = 250'000'000;  // one check
constexpr std::int64_t SAMPLES = 100;

// A sample at monotonic time `mono_ns` with `suspended_ns` spent suspended
// since boot, read with CLOCK_BOOTTIME in the middle of a read of
// `READ_NS`.
ClockSample at(std::int64_t mono_ns, std::int64_t suspended_ns) {
    return ClockSample{.monotonic_before_ns = mono_ns,
                       .boottime_ns = mono_ns + (READ_NS / 2) + suspended_ns,
                       .monotonic_after_ns = mono_ns + READ_NS};
}

TEST(SuspendWatch, NoSuspendIsNotReportedWhateverTheReadTime) {
    SuspendWatch watch;
    for (std::int64_t i = 0; i < SAMPLES; ++i) {
        EXPECT_FALSE(watch.observe(at(BOOT_NS + (i * STEP_NS), 0)).has_value());
    }
}

TEST(SuspendWatch, ASuspendBeforeTheStartIsReportedOnceAtTheStart) {
    SuspendWatch watch;
    // A lower bound: the read time does not count.
    EXPECT_EQ(
        watch.observe(at(BOOT_NS, SUSPEND_NS)),
        std::optional(SuspendFinding{
            .at_start = true, .suspended_ns = SUSPEND_NS - (READ_NS / 2)}));
    EXPECT_FALSE(watch.observe(at(BOOT_NS + STEP_NS, SUSPEND_NS)).has_value());
}

TEST(SuspendWatch, ASuspendWhileRunningIsReportedWithItsLength) {
    SuspendWatch watch;
    EXPECT_FALSE(watch.observe(at(BOOT_NS, 0)).has_value());
    EXPECT_EQ(watch.observe(at(BOOT_NS + STEP_NS, SUSPEND_NS)),
              std::optional(SuspendFinding{
                  .at_start = false, .suspended_ns = SUSPEND_NS - READ_NS}));
    EXPECT_FALSE(
        watch.observe(at(BOOT_NS + (2 * STEP_NS), SUSPEND_NS)).has_value());
    // A second suspend is reported again.
    EXPECT_TRUE(
        watch.observe(at(BOOT_NS + (3 * STEP_NS), 2 * SUSPEND_NS)).has_value());
}

TEST(SuspendWatch, ASampleWhoseMonotonicClockGoesBackIsIgnored) {
    SuspendWatch watch;
    ClockSample broken = at(BOOT_NS, SUSPEND_NS);
    broken.monotonic_after_ns = broken.monotonic_before_ns - 1;
    EXPECT_FALSE(watch.observe(broken).has_value());
}

TEST(SuspendWatch, TheClocksOfThisMachineGiveASample) {
    const ClockSample sample = scada::tagd::sample_clocks();
    EXPECT_LE(sample.monotonic_before_ns, sample.monotonic_after_ns);
    // CLOCK_BOOTTIME is never behind CLOCK_MONOTONIC.
    EXPECT_GE(sample.boottime_ns, sample.monotonic_before_ns);
}

class SleepStates : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("scada-suspend-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    [[nodiscard]] const std::filesystem::path& dir() const { return dir_; }

    [[nodiscard]] std::string file_with(const std::string& text) const {
        const std::filesystem::path path = dir_ / "state";
        std::ofstream(path, std::ios::binary) << text;
        return path.string();
    }

private:
    std::filesystem::path dir_;
};

TEST_F(SleepStates, AreReadTrimmed) {
    EXPECT_EQ(read_sleep_states(file_with("freeze mem disk\n")),
              "freeze mem disk");
}

TEST_F(SleepStates, AnEmptyOrMissingFileOffersNone) {
    EXPECT_EQ(read_sleep_states(file_with("\n")), "");
    EXPECT_EQ(read_sleep_states((dir() / "none").string()), "");
}

TEST_F(SleepStates, AreOutsideInput) {
    const std::string text = std::string("mem\x01\xff") + std::string(300, 'x');
    const std::string states = read_sleep_states(file_with(text));
    EXPECT_EQ(states.substr(0, 5), "mem??");
    EXPECT_EQ(states.size(), 256U);
}

}  // namespace
