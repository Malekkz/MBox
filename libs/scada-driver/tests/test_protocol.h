#ifndef SCADA_DRIVER_TESTS_TEST_PROTOCOL_H
#define SCADA_DRIVER_TESTS_TEST_PROTOCOL_H

// The test protocol of the conformance tests (driver interface contract 15:
// "a scripted frame protocol over a loopback transport"). Its frames are
// invented for the tests; every number is big-endian.
//
//   read request:  tid(2) unit(1) 'R' space(1) first(4) count(2)
//   answer:        tid(2) unit(1) status(1) count(2) data(2 * count)
//
// A unit is a 16-bit register. A point reads 1 to 4 adjacent units of one
// space as one unsigned raw value. `status` is one of AnswerStatus; any
// status but OK carries no data.

#include "scada/common/status.h"
#include "scada/driver/compiled.h"
#include "scada/driver/exchange.h"
#include "scada/driver/plan.h"
#include "scada/driver/protocol.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::driver::test {

inline constexpr std::size_t REQUEST_BYTES = 11;
inline constexpr std::size_t ANSWER_HEADER_BYTES = 6;
inline constexpr std::uint64_t MAX_UNITS = 64;
inline constexpr std::uint64_t MAX_POINT_UNITS = 4;

enum class AnswerStatus : std::uint8_t {
    OK = 0,
    ILLEGAL_ADDRESS = 1,  // DecodeResult::SPLIT
    BUSY = 2,
    DEVICE_INVALID = 3,
    ABSENT = 4,
    UNREACHABLE = 5,
};

// The binding of a device: its unit id.
struct TestDevice final : ProtocolData {
    std::uint8_t unit_id = 1;
};

// The binding of a point: the units it reads.
struct TestPoint final : ProtocolData {
    std::uint8_t space = 0;
    std::uint32_t first = 0;
    std::uint8_t count = 1;  // 1 to MAX_POINT_UNITS
};

class TestProtocol final : public FrameProtocol {
public:
    // The test protocol is compiled in code by the tests; its `configure`
    // from a configuration tree arrives with the framework's own compile
    // step (slice 8). Returns UNSUPPORTED.
    [[nodiscard]] Status configure(
        const DriverConfig& config, CompiledConfig& out,
        ConfigErrors& errors) const noexcept override;

    [[nodiscard]] DeviceCaps device_caps(
        const CompiledDevice& device) const noexcept override;
    [[nodiscard]] PointCaps point_caps(
        const CompiledPoint& point) const noexcept override;

    // Plans with `plan_blocks`: a point and its companions form one cluster.
    [[nodiscard]] Status plan(const CompiledDevice& device,
                              const ReadRules& read_rules,
                              ReadPlan& out) const noexcept override;

    SCADA_RT [[nodiscard]] EncodeResult encode(
        const CompiledDevice& device, ChannelState& channel,
        const Request& request, Attempt& attempt,
        TxBuffer& out) const noexcept override;

    SCADA_RT [[nodiscard]] FrameState frame_state(
        const Request& request, const Attempt& attempt,
        std::span<const std::byte> received) const noexcept override;

    SCADA_RT [[nodiscard]] DecodeResult decode(
        const CompiledDevice& device, DeviceState& state,
        const Request& request, const Attempt& attempt,
        std::span<const std::byte> frame,
        SampleSink& out) const noexcept override;
};

}  // namespace scada::driver::test

#endif  // SCADA_DRIVER_TESTS_TEST_PROTOCOL_H
