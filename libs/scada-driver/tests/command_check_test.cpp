// The checks of a write request on the command thread (driver interface
// contract 8.2, checks 2 to 9; conformance test 21: each check fails a
// request with its status and reason; decision D-150).

#include "scada/driver/command_check.h"

#include "scada/driver/compiled.h"
#include "scada/driver/failure.h"
#include "scada/driver/point_rules.h"
#include "scada/driver/raw_value.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <limits>
#include <optional>

namespace scada::driver {
namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

constexpr std::int64_t NOW = 1'000'000'000;
constexpr std::int64_t SECOND = 1'000'000'000;
constexpr std::int64_t DAY = 86'400 * SECOND;

// The mode tag and panel input as the test sets them; counts its reads.
class Reads final : public CommandReads {
public:
    TagReading mode_reading =
        good_int(static_cast<std::int64_t>(OutputMode::MANUAL));
    TagReading panel_reading = good_bool(false);
    int mode_reads = 0;
    int panel_reads = 0;

    static TagReading good_int(std::int64_t value) {
        TagReading reading{.code = SCADA_OK};
        reading.value.type = SCADA_TYPE_INT64;
        reading.value.value = std::bit_cast<std::uint64_t>(value);
        reading.value.quality = SCADA_QUALITY_GOOD;
        return reading;
    }
    static TagReading good_bool(bool value) {
        TagReading reading{.code = SCADA_OK};
        reading.value.type = SCADA_TYPE_BOOL;
        reading.value.value = value ? 1 : 0;
        reading.value.quality = SCADA_QUALITY_GOOD;
        return reading;
    }

    TagReading mode(const CompiledOutput& /*output*/) noexcept override {
        ++mode_reads;
        return mode_reading;
    }
    TagReading panel(const CompiledOutput& /*output*/) noexcept override {
        ++panel_reads;
        return panel_reading;
    }
};

CompiledPoint make_point(const char* name, Access access) {
    CompiledPoint point;
    point.name = name;
    point.access = access;
    return point;
}

// A device with one INT64 setpoint point (raw int16, gain 0.1), one BOOL
// point, one INT64 double point and one read-only point.
struct Rig {
    Rig() {
        CompiledPoint speed = make_point("speed", Access::RW);
        speed.rules.tag_type = tag::TagType::FLOAT64;
        speed.rules.raw_kind = RawKind::SIGNED;
        speed.rules.scale = ScaleRule{.gain = 0.1};
        speed.poll_group = 0;
        CompiledPoint run = make_point("run", Access::RW);
        run.rules.tag_type = tag::TagType::BOOL;
        run.rules.raw_kind = RawKind::BOOL;
        CompiledPoint breaker = make_point("cb", Access::RW);
        breaker.rules.tag_type = tag::TagType::INT64;
        breaker.rules.raw_kind = RawKind::UNSIGNED;
        CompiledPoint level = make_point("level", Access::RO);
        level.poll_group = 0;
        device.points.push_back(std::move(speed));
        device.points.push_back(std::move(run));
        device.points.push_back(std::move(breaker));
        device.points.push_back(std::move(level));
        setpoint =
            CompiledOutput{.name = "pump.speed_cmd",
                           .point = 0,
                           .kind = OutputKind::SETPOINT,
                           .limits = OutputLimits{.min = 0.0, .max = 100.0},
                           .raw_width = 16};
        single = CompiledOutput{.name = "pump.run_cmd",
                                .point = 1,
                                .kind = OutputKind::SINGLE,
                                .raw_width = 1};
        breaker_cmd = CompiledOutput{.name = "cb.cmd",
                                     .point = 2,
                                     .kind = OutputKind::DOUBLE,
                                     .model = ControlModel::SBO,
                                     .raw_width = 16};
    }

    [[nodiscard]] CheckTarget target(
        const CompiledOutput& output,
        DeviceStatus state = DeviceStatus::RUNNING) const {
        return CheckTarget{.device = &device,
                           .point = &device.points[output.point],
                           .output = &output,
                           .state = state,
                           .limiter = &limiter};
    }

    CompiledDevice device;
    CompiledOutput setpoint;
    CompiledOutput single;
    CompiledOutput breaker_cmd;
    WriteLimiter limiter{WriteLimits{}};
    Reads reads;
};

scada_write_request direct_float(double value,
                                 std::uint8_t source = SCADA_SOURCE_OPERATOR) {
    scada_write_request request{};
    request.request_id = 7;
    request.operation = SCADA_OP_DIRECT;
    request.source = source;
    request.value_type = SCADA_TYPE_FLOAT64;
    request.value = std::bit_cast<std::uint64_t>(value);
    request.user_id = source == SCADA_SOURCE_OPERATOR ? 1 : 0;
    return request;
}

scada_write_request with(std::uint8_t operation, std::uint8_t type,
                         std::uint64_t value,
                         std::uint8_t source = SCADA_SOURCE_OPERATOR) {
    scada_write_request request = direct_float(0.0, source);
    request.operation = operation;
    request.value_type = type;
    request.value = value;
    return request;
}

constexpr Answer rejected(Reason reason) {
    return Answer{.status = SCADA_RESULT_REJECTED, .reason = reason};
}

TEST(CommandCheck, ReasonsPackWithTheirRawCode) {
    EXPECT_EQ(protocol_error(Reason::NONE), 0);
    EXPECT_EQ(protocol_error(Reason::UNKNOWN_TARGET), 10 << 16);
    EXPECT_EQ(protocol_error(Reason::DEVICE_FAULT, 0x1234),
              (31 << 16) | 0x1234);
    // A raw code wider than 16 bits is 0xFFFF (D-85).
    EXPECT_EQ(protocol_error(Reason::DEVICE_FAULT, 0x12345),
              (31 << 16) | 0xFFFF);
    EXPECT_EQ(protocol_error(Reason::LOCAL_AUTHORITY), 70 << 16);
}

TEST(CommandCheck, AValidSetpointIsAnOrderWithItsRawValue) {
    Rig rig;
    const CheckOutcome outcome = check_request(
        direct_float(12.5), rig.target(rig.setpoint), rig.reads, NOW);
    ASSERT_TRUE(outcome.order);
    EXPECT_EQ(outcome.value.raw.kind, RawKind::SIGNED);
    EXPECT_EQ(outcome.value.raw.pattern, 125U);  // 12.5 with gain 0.1 (8.4)
}

// --- Check 2: the target --------------------------------------------------

TEST(CommandCheck, Check2AnUnknownTargetIsRejected) {
    Rig rig;
    CheckTarget none = rig.target(rig.setpoint);
    none.output = nullptr;
    EXPECT_EQ(check_request(direct_float(1.0), none, rig.reads, NOW).answer,
              rejected(Reason::UNKNOWN_TARGET));
    none.device = nullptr;
    EXPECT_EQ(check_request(direct_float(1.0), none, rig.reads, NOW).answer,
              rejected(Reason::UNKNOWN_TARGET));
    // An output whose point is not resolved is no target either.
    CheckTarget pointless = rig.target(rig.setpoint);
    pointless.point = nullptr;
    EXPECT_EQ(
        check_request(direct_float(1.0), pointless, rig.reads, NOW).answer,
        rejected(Reason::UNKNOWN_TARGET));
    EXPECT_EQ(rig.reads.mode_reads, 0);  // the first failing check stops it
}

TEST(CommandCheck, Check2AReadNowNeedsAReadablePoint) {
    Rig rig;
    const scada_write_request read_now = with(SCADA_OP_READ_NOW, 0, 0);
    CheckTarget level = rig.target(rig.setpoint);
    level.output = nullptr;
    level.point = &rig.device.points[3];
    EXPECT_TRUE(check_request(read_now, level, rig.reads, NOW).order);
    // The BOOL point is never polled: nothing to read.
    level.point = &rig.device.points[1];
    EXPECT_EQ(check_request(read_now, level, rig.reads, NOW).answer,
              rejected(Reason::UNKNOWN_TARGET));
}

// --- Check 3: the operation and the control model ---------------------------

TEST(CommandCheck, Check3SelectOperateAndCancelOnlyOnAnSboOutput) {
    Rig rig;
    for (const std::uint8_t operation :
         {SCADA_OP_SELECT, SCADA_OP_OPERATE, SCADA_OP_CANCEL}) {
        scada_write_request request = direct_float(1.0);
        request.operation = operation;
        EXPECT_EQ(
            check_request(request, rig.target(rig.setpoint), rig.reads, NOW)
                .answer,
            rejected(Reason::WRONG_CONTROL_MODEL))
            << int{operation};
    }
}

TEST(CommandCheck, Check3AnOperatorsDirectNeedsADirectOutput) {
    Rig rig;
    const scada_write_request close =
        with(SCADA_OP_DIRECT, SCADA_TYPE_INT64, 2);
    EXPECT_EQ(check_request(close, rig.target(rig.breaker_cmd), rig.reads, NOW)
                  .answer,
              rejected(Reason::WRONG_CONTROL_MODEL));
    // AUTO and REMOTE may write either.
    rig.reads.mode_reading = Reads::good_int(1);
    EXPECT_TRUE(check_request(with(SCADA_OP_DIRECT, SCADA_TYPE_INT64, 2,
                                   SCADA_SOURCE_AUTO),
                              rig.target(rig.breaker_cmd), rig.reads, NOW)
                    .order);
}

// --- Check 4: the value -----------------------------------------------------

TEST(CommandCheck, Check4TheValueMustBeOfTheTagsType) {
    Rig rig;
    EXPECT_EQ(check_request(with(SCADA_OP_DIRECT, SCADA_TYPE_INT64, 5),
                            rig.target(rig.setpoint), rig.reads, NOW)
                  .answer,
              rejected(Reason::INVALID_VALUE));
}

TEST(CommandCheck, Check4NaNAndInfinityAreRefusedBeforeTheLimits) {
    Rig rig;
    rig.setpoint.limits = OutputLimits{};  // no limit to stop it otherwise
    for (const double value : {std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity()}) {
        EXPECT_EQ(check_request(direct_float(value), rig.target(rig.setpoint),
                                rig.reads, NOW)
                      .answer,
                  rejected(Reason::INVALID_VALUE));
    }
}

TEST(CommandCheck, Check4TheLimitsAndTheirRanges) {
    Rig rig;
    const auto check = [&](double value) {
        return check_request(direct_float(value), rig.target(rig.setpoint),
                             rig.reads, NOW);
    };
    EXPECT_TRUE(check(0.0).order);
    EXPECT_TRUE(check(100.0).order);
    EXPECT_EQ(check(-0.1).answer, rejected(Reason::INVALID_VALUE));
    EXPECT_EQ(check(100.1).answer, rejected(Reason::INVALID_VALUE));
    rig.setpoint.limits.ranges = {ValueRange{.lo = 0.0, .hi = 10.0},
                                  ValueRange{.lo = 50.0, .hi = 60.0}};
    EXPECT_TRUE(check(55.0).order);
    EXPECT_EQ(check(20.0).answer, rejected(Reason::INVALID_VALUE));
}

TEST(CommandCheck, Check4AnIntegerLimitIsComparedExactly) {
    // A UINT64 above 2^53 is not rounded to a double: 2^53 + 1 is above a
    // maximum of 2^53.
    Rig rig;
    CompiledPoint& counter = rig.device.points[2];
    counter.rules.tag_type = tag::TagType::UINT64;
    CompiledOutput output{
        .name = "count.cmd",
        .point = 2,
        .kind = OutputKind::SETPOINT,
        .limits = OutputLimits{.min = std::nullopt, .max = 9007199254740992.0},
        .raw_width = 64};
    const std::uint64_t at = 9007199254740992U;
    EXPECT_TRUE(check_request(with(SCADA_OP_DIRECT, SCADA_TYPE_UINT64, at),
                              rig.target(output), rig.reads, NOW)
                    .order);
    EXPECT_EQ(check_request(with(SCADA_OP_DIRECT, SCADA_TYPE_UINT64, at + 1),
                            rig.target(output), rig.reads, NOW)
                  .answer,
              rejected(Reason::INVALID_VALUE));
}

TEST(CommandCheck, Check4AValueOutsideTheRawTypeIsRefused) {
    // 4000.0 with gain 0.1 is 40000: outside int16 (8.4).
    Rig rig;
    rig.setpoint.limits = OutputLimits{};
    EXPECT_EQ(check_request(direct_float(4000.0), rig.target(rig.setpoint),
                            rig.reads, NOW)
                  .answer,
              rejected(Reason::INVALID_VALUE));
}

TEST(CommandCheck, Check4TheValuesOfEachKind) {
    Rig rig;
    rig.reads.mode_reading = Reads::good_int(1);  // AUTO, for the sbo double
    const auto direct = [&](const CompiledOutput& output, std::uint8_t type,
                            std::uint64_t value) {
        return check_request(
            with(SCADA_OP_DIRECT, type, value, SCADA_SOURCE_AUTO),
            rig.target(output), rig.reads, NOW);
    };
    // double: 1 open, 2 closed; 0 and 3 refused.
    EXPECT_TRUE(direct(rig.breaker_cmd, SCADA_TYPE_INT64, 1).order);
    EXPECT_TRUE(direct(rig.breaker_cmd, SCADA_TYPE_INT64, 2).order);
    EXPECT_FALSE(direct(rig.breaker_cmd, SCADA_TYPE_INT64, 0).order);
    EXPECT_FALSE(direct(rig.breaker_cmd, SCADA_TYPE_INT64, 3).order);
    // single: 0 or 1, but on_only refuses 0.
    EXPECT_TRUE(direct(rig.single, SCADA_TYPE_BOOL, 0).order);
    rig.single.on_only = true;
    EXPECT_EQ(direct(rig.single, SCADA_TYPE_BOOL, 0).answer,
              rejected(Reason::INVALID_VALUE));
    EXPECT_TRUE(direct(rig.single, SCADA_TYPE_BOOL, 1).order);
    // pulse and trigger: 1 only.
    rig.single.kind = OutputKind::PULSE;
    rig.single.on_only = false;
    EXPECT_FALSE(direct(rig.single, SCADA_TYPE_BOOL, 0).order);
    EXPECT_TRUE(direct(rig.single, SCADA_TYPE_BOOL, 1).order);
    rig.single.kind = OutputKind::TRIGGER;
    EXPECT_FALSE(direct(rig.single, SCADA_TYPE_BOOL, 0).order);
    // step: +1 or -1.
    CompiledOutput step = rig.breaker_cmd;
    step.kind = OutputKind::STEP;
    EXPECT_TRUE(direct(step, SCADA_TYPE_INT64, 1).order);
    EXPECT_TRUE(direct(step, SCADA_TYPE_INT64,
                       std::bit_cast<std::uint64_t>(std::int64_t{-1}))
                    .order);
    EXPECT_FALSE(direct(step, SCADA_TYPE_INT64, 2).order);
}

// --- Check 5: source against mode (D-92, O-2) --------------------------------

TEST(CommandCheck, Check5EachSourceNeedsItsMode) {
    Rig rig;
    const auto from = [&](std::uint8_t source, std::int64_t mode) {
        rig.reads.mode_reading = Reads::good_int(mode);
        return check_request(direct_float(1.0, source),
                             rig.target(rig.setpoint), rig.reads, NOW);
    };
    EXPECT_TRUE(from(SCADA_SOURCE_OPERATOR, 2).order);
    EXPECT_TRUE(from(SCADA_SOURCE_REMOTE, 3).order);
    EXPECT_TRUE(from(SCADA_SOURCE_AUTO, 1).order);
    const Answer not_authorised{.status = SCADA_RESULT_NOT_AUTHORISED};
    EXPECT_EQ(from(SCADA_SOURCE_OPERATOR, 1).answer, not_authorised);
    EXPECT_EQ(from(SCADA_SOURCE_REMOTE, 2).answer, not_authorised);
    EXPECT_EQ(from(SCADA_SOURCE_AUTO, 3).answer, not_authorised);
    EXPECT_EQ(from(SCADA_SOURCE_AUTO, 0).answer, not_authorised);
    EXPECT_EQ(from(SCADA_SOURCE_AUTO, 4).answer, not_authorised);
}

TEST(CommandCheck, Check5AutoGoesOnWhileTheModesOwnerIsDown) {
    // Ruling O-2: BAD with OWNER_DOWN as its only flag and a stored AUTO.
    Rig rig;
    rig.reads.mode_reading = Reads::good_int(1);
    rig.reads.mode_reading.value.quality = SCADA_QUALITY_BAD;
    rig.reads.mode_reading.value.quality_flags = SCADA_FLAG_OWNER_DOWN;
    EXPECT_TRUE(check_request(direct_float(1.0, SCADA_SOURCE_AUTO),
                              rig.target(rig.setpoint), rig.reads, NOW)
                    .order);
    // Not for the operator, and not with any other flag.
    rig.reads.mode_reading.value.value = 2;
    EXPECT_FALSE(check_request(direct_float(1.0), rig.target(rig.setpoint),
                               rig.reads, NOW)
                     .order);
    rig.reads.mode_reading.value.value = 1;
    rig.reads.mode_reading.value.quality_flags =
        SCADA_FLAG_OWNER_DOWN | SCADA_FLAG_NOT_INIT;
    EXPECT_FALSE(check_request(direct_float(1.0, SCADA_SOURCE_AUTO),
                               rig.target(rig.setpoint), rig.reads, NOW)
                     .order);
}

TEST(CommandCheck, Check5FailsClosed) {
    Rig rig;
    const auto auto_with = [&](TagReading mode) {
        rig.reads.mode_reading = mode;
        return check_request(direct_float(1.0, SCADA_SOURCE_AUTO),
                             rig.target(rig.setpoint), rig.reads, NOW)
            .answer;
    };
    const Answer not_authorised{.status = SCADA_RESULT_NOT_AUTHORISED};
    TagReading mode = Reads::good_int(1);
    mode.code = SCADA_E_NOT_FOUND;
    EXPECT_EQ(auto_with(mode), not_authorised);
    mode = Reads::good_int(1);
    mode.value.quality_flags = SCADA_FLAG_NOT_INIT;
    EXPECT_EQ(auto_with(mode), not_authorised);
    mode = Reads::good_int(1);
    mode.value.quality = SCADA_QUALITY_UNCERTAIN;
    EXPECT_EQ(auto_with(mode), not_authorised);
    mode = Reads::good_bool(true);  // not an INT64 tag
    EXPECT_EQ(auto_with(mode), not_authorised);
}

// --- Check 6: the local panel (R-16) ---------------------------------------

TEST(CommandCheck, Check6TheLocalPanelBlocksAutoAndFailsClosed) {
    Rig rig;
    rig.setpoint.local_blocks_auto = true;
    rig.reads.mode_reading = Reads::good_int(1);
    const auto auto_with = [&](TagReading panel) {
        rig.reads.panel_reading = panel;
        return check_request(direct_float(1.0, SCADA_SOURCE_AUTO),
                             rig.target(rig.setpoint), rig.reads, NOW);
    };
    const Answer local{.status = SCADA_RESULT_NOT_AUTHORISED,
                       .reason = Reason::LOCAL_AUTHORITY};
    EXPECT_TRUE(auto_with(Reads::good_bool(false)).order);
    EXPECT_EQ(auto_with(Reads::good_bool(true)).answer, local);
    TagReading bad = Reads::good_bool(false);
    bad.value.quality = SCADA_QUALITY_BAD;
    EXPECT_EQ(auto_with(bad).answer, local);
    TagReading missing = Reads::good_bool(false);
    missing.code = SCADA_E_NOT_FOUND;
    EXPECT_EQ(auto_with(missing).answer, local);
}

TEST(CommandCheck, Check6TheOperatorIsNotBlockedByThePanel) {
    Rig rig;
    rig.setpoint.local_blocks_auto = true;
    rig.reads.panel_reading = Reads::good_bool(true);
    EXPECT_TRUE(check_request(direct_float(1.0), rig.target(rig.setpoint),
                              rig.reads, NOW)
                    .order);
    EXPECT_EQ(rig.reads.panel_reads, 0);
}

// --- Check 7: selections, none live until slice 7 ---------------------------

TEST(CommandCheck, Check7WithNoSelectionLive) {
    Rig rig;
    const auto on_breaker = [&](std::uint8_t operation, std::uint8_t type,
                                std::uint64_t value) {
        return check_request(with(operation, type, value),
                             rig.target(rig.breaker_cmd), rig.reads, NOW)
            .answer;
    };
    EXPECT_EQ(on_breaker(SCADA_OP_SELECT, SCADA_TYPE_INT64, 2),
              rejected(Reason::NOT_SUPPORTED));
    EXPECT_EQ(on_breaker(SCADA_OP_OPERATE, SCADA_TYPE_INT64, 2),
              rejected(Reason::NOT_SELECTED));
    EXPECT_EQ(on_breaker(SCADA_OP_CANCEL, 0, 0),
              (Answer{.status = SCADA_RESULT_OK,
                      .reason = Reason::NOTHING_TO_CANCEL}));
}

// --- Check 8: the device's state --------------------------------------------

TEST(CommandCheck, Check8TheDevicesState) {
    Rig rig;
    const auto in = [&](DeviceStatus state) {
        return check_request(direct_float(1.0), rig.target(rig.setpoint, state),
                             rig.reads, NOW);
    };
    EXPECT_EQ(in(DeviceStatus::DISABLED).answer,
              rejected(Reason::CONFIG_MISMATCH));
    EXPECT_EQ(in(DeviceStatus::MISMATCH).answer,
              rejected(Reason::CONFIG_MISMATCH));
    EXPECT_EQ(in(DeviceStatus::DEMOTED).answer,
              rejected(Reason::DEVICE_UNAVAILABLE));
    EXPECT_EQ(in(DeviceStatus::REFUSING).answer,
              rejected(Reason::DEVICE_UNAVAILABLE));
    // The strict interim of D-147: no write while INITIALISING.
    EXPECT_EQ(in(DeviceStatus::INITIALISING).answer,
              rejected(Reason::DEVICE_UNAVAILABLE));
    // CONNECTING is the worker's check 11.
    EXPECT_TRUE(in(DeviceStatus::CONNECTING).order);
    EXPECT_TRUE(in(DeviceStatus::RUNNING).order);
}

TEST(CommandCheck, Check8AReadNowOnADemotedDeviceIsAProbe) {
    Rig rig;
    CheckTarget level = rig.target(rig.setpoint, DeviceStatus::DEMOTED);
    level.output = nullptr;
    level.point = &rig.device.points[3];
    EXPECT_TRUE(
        check_request(with(SCADA_OP_READ_NOW, 0, 0), level, rig.reads, NOW)
            .order);
    level.state = DeviceStatus::MISMATCH;
    EXPECT_EQ(
        check_request(with(SCADA_OP_READ_NOW, 0, 0), level, rig.reads, NOW)
            .answer,
        rejected(Reason::CONFIG_MISMATCH));
}

// --- Check 9: write limits (8.9) --------------------------------------------

TEST(CommandCheck, Check9AWriteBeyondItsLimitsIsRateLimited) {
    Rig rig;
    rig.limiter = WriteLimiter(WriteLimits{.min_interval_ns = SECOND});
    rig.limiter.count(NOW);
    EXPECT_EQ(check_request(direct_float(1.0), rig.target(rig.setpoint),
                            rig.reads, NOW + SECOND - 1)
                  .answer,
              rejected(Reason::RATE_LIMITED));
    EXPECT_TRUE(check_request(direct_float(1.0), rig.target(rig.setpoint),
                              rig.reads, NOW + SECOND)
                    .order);
}

TEST(WriteLimiter, NoLimitAllowsEveryWrite) {
    WriteLimiter limiter(WriteLimits{});
    limiter.count(NOW);
    EXPECT_TRUE(limiter.allows(NOW));
}

TEST(WriteLimiter, TheMinimumIntervalCountsFromTheLastWrite) {
    WriteLimiter limiter(WriteLimits{.min_interval_ns = SECOND});
    EXPECT_TRUE(limiter.allows(NOW));  // nothing counted yet
    limiter.count(NOW);
    EXPECT_FALSE(limiter.allows(NOW + SECOND - 1));
    EXPECT_TRUE(limiter.allows(NOW + SECOND));
}

TEST(WriteLimiter, AtMostMaxPerDayInAnyDay) {
    WriteLimiter limiter(WriteLimits{.max_per_day = 3});
    limiter.count(NOW);
    limiter.count(NOW + SECOND);
    EXPECT_TRUE(limiter.allows(NOW + 2 * SECOND));
    limiter.count(NOW + 2 * SECOND);
    EXPECT_FALSE(limiter.allows(NOW + 3 * SECOND));
    // A day after the oldest of the three, one more is allowed, not two.
    EXPECT_FALSE(limiter.allows(NOW + DAY - 1));
    EXPECT_TRUE(limiter.allows(NOW + DAY));
    limiter.count(NOW + DAY);
    EXPECT_FALSE(limiter.allows(NOW + DAY + SECOND - 1));
    EXPECT_TRUE(limiter.allows(NOW + DAY + SECOND));
}

// --- The order of the checks ------------------------------------------------

TEST(CommandCheck, TheFirstFailingCheckAnswers) {
    // Every check from 3 on fails at once; each fix lets the next answer.
    Rig rig;
    rig.reads.mode_reading = Reads::good_int(1);  // AUTO, not MANUAL
    rig.limiter = WriteLimiter(WriteLimits{.min_interval_ns = SECOND});
    rig.limiter.count(NOW);
    scada_write_request request = direct_float(200.0);  // over the limit
    request.operation = SCADA_OP_SELECT;  // not on a direct output
    const auto now = [&] {
        return check_request(request,
                             rig.target(rig.setpoint, DeviceStatus::DEMOTED),
                             rig.reads, NOW)
            .answer;
    };
    EXPECT_EQ(now(), rejected(Reason::WRONG_CONTROL_MODEL));
    request.operation = SCADA_OP_DIRECT;
    EXPECT_EQ(now(), rejected(Reason::INVALID_VALUE));
    request.value = std::bit_cast<std::uint64_t>(50.0);
    EXPECT_EQ(now(), (Answer{.status = SCADA_RESULT_NOT_AUTHORISED}));
    rig.reads.mode_reading = Reads::good_int(2);
    EXPECT_EQ(now(), rejected(Reason::DEVICE_UNAVAILABLE));
    EXPECT_EQ(
        check_request(request, rig.target(rig.setpoint), rig.reads, NOW).answer,
        rejected(Reason::RATE_LIMITED));
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

}  // namespace
}  // namespace scada::driver
