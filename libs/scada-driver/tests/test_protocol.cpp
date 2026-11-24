#include "test_protocol.h"

#include "scada/common/status.h"
#include "scada/driver/compiled.h"
#include "scada/driver/exchange.h"
#include "scada/driver/plan.h"
#include "scada/driver/protocol.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/sample_processing.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

namespace scada::driver::test {

namespace {

constexpr unsigned BYTE_BITS = 8;
constexpr std::uint64_t BYTE_MASK = 0xFFU;
constexpr unsigned UNIT_BITS = 16;
constexpr std::uint8_t READ = 'R';
constexpr std::uint32_t TID_MASK = 0xFFFFU;
constexpr std::size_t TID_AT = 0;
constexpr std::size_t UNIT_AT = 2;
constexpr std::size_t KIND_AT = 3;
constexpr std::size_t SPACE_AT = 4;
constexpr std::size_t FIRST_AT = 5;
constexpr std::size_t COUNT_AT = 9;
constexpr std::size_t STATUS_AT = 3;
constexpr std::size_t ANSWER_COUNT_AT = 4;

void put(std::span<std::byte> out, std::size_t offset, std::uint64_t value,
         std::size_t width) noexcept {
    for (std::size_t index = 0; index < width; ++index) {
        out[offset + index] = static_cast<std::byte>(
            (value >> (BYTE_BITS * (width - 1 - index))) & BYTE_MASK);
    }
}

std::uint64_t get(std::span<const std::byte> bytes, std::size_t offset,
                  std::size_t width) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
        value = (value << BYTE_BITS) |
                std::to_integer<std::uint64_t>(bytes[offset + index]);
    }
    return value;
}

}  // namespace

Status TestProtocol::configure(const DriverConfig& /*config*/,
                               CompiledConfig& /*out*/,
                               ConfigErrors& /*errors*/) const noexcept {
    return {StatusCode::UNSUPPORTED, "the test protocol is compiled in code"};
}

DeviceCaps TestProtocol::device_caps(
    const CompiledDevice& /*device*/) const noexcept {
    return DeviceCaps{.max_read_units = MAX_UNITS,
                      .max_write_units = 1,
                      .max_frame_bytes = ANSWER_HEADER_BYTES + (2 * MAX_UNITS)};
}

PointCaps TestProtocol::point_caps(
    const CompiledPoint& /*point*/) const noexcept {
    return PointCaps{};
}

Status TestProtocol::plan(const CompiledDevice& device,
                          const ReadRules& read_rules,
                          ReadPlan& out) const noexcept {
    const auto& unit = protocol_data<TestDevice>(*device.protocol);
    // Clusters: a point and every companion it names share one, the lowest
    // index of the group (union of the companion relation).
    std::vector<std::uint32_t> cluster(device.points.size());
    std::iota(cluster.begin(), cluster.end(), 0U);
    auto root = [&](std::uint32_t index) {
        while (cluster[index] != index) {
            index = cluster[index];
        }
        return index;
    };
    for (std::uint32_t index = 0; index < device.points.size(); ++index) {
        for (const std::uint32_t companion :
             device.points[index].rules.companions) {
            if (companion >= device.points.size()) {
                return {StatusCode::INVALID_ARGUMENT, "companion out of range"};
            }
            const std::uint32_t one = root(index);
            const std::uint32_t other = root(companion);
            cluster[std::max(one, other)] = std::min(one, other);
        }
    }
    // The framework sizes `out.by_group` for the channel's poll groups.
    const auto groups = static_cast<std::uint32_t>(out.by_group.size());
    std::vector<PlanPoint> points;
    for (std::uint32_t index = 0; index < device.points.size(); ++index) {
        const CompiledPoint& point = device.points[index];
        if (!point.poll_group.has_value() || point.access == Access::WO) {
            continue;
        }
        const auto& binding = protocol_data<TestPoint>(*point.protocol);
        points.push_back(PlanPoint{.point = index,
                                   .poll_group = *point.poll_group,
                                   .cluster = root(index),
                                   .space = binding.space,
                                   .unit_id = unit.unit_id,
                                   .first = binding.first,
                                   .count = binding.count});
    }
    return plan_blocks(points, read_rules, groups, out);
}

EncodeResult TestProtocol::encode(const CompiledDevice& device,
                                  ChannelState& channel, const Request& request,
                                  Attempt& attempt,
                                  TxBuffer& out) const noexcept {
    // The test protocol reads only: a write cannot be encoded.
    if (request.kind != Request::Kind::READ ||
        out.bytes.size() < REQUEST_BYTES || request.count == 0 ||
        request.count > MAX_UNITS) {
        return EncodeResult{};
    }
    const auto& unit = protocol_data<TestDevice>(*device.protocol);
    attempt.transaction_id = channel.take_transaction_id() & TID_MASK;
    put(out.bytes, TID_AT, attempt.transaction_id, 2);
    put(out.bytes, UNIT_AT, unit.unit_id, 1);
    put(out.bytes, KIND_AT, READ, 1);
    put(out.bytes, SPACE_AT, request.space, 1);
    put(out.bytes, FIRST_AT, request.first, 4);
    put(out.bytes, COUNT_AT, request.count, 2);
    return EncodeResult{
        .kind = EncodeResult::Kind::OK,
        .length = REQUEST_BYTES,
        .expected_max = ANSWER_HEADER_BYTES + (2 * request.count)};
}

FrameState TestProtocol::frame_state(
    const Request& request, const Attempt& attempt,
    std::span<const std::byte> received) const noexcept {
    if (received.size() < ANSWER_HEADER_BYTES) {
        return FrameState{.kind = FrameState::Kind::NEED_MORE,
                          .need = ANSWER_HEADER_BYTES - received.size()};
    }
    const std::uint64_t status = get(received, STATUS_AT, 1);
    const std::uint64_t count = get(received, ANSWER_COUNT_AT, 2);
    const bool answered = status == static_cast<std::uint8_t>(AnswerStatus::OK);
    if ((answered && count != request.count) || (!answered && count != 0) ||
        get(received, UNIT_AT, 1) != request.unit_id) {
        return FrameState{.kind = FrameState::Kind::BROKEN};
    }
    const std::size_t length = ANSWER_HEADER_BYTES + (2 * count);
    if (received.size() < length) {
        return FrameState{.kind = FrameState::Kind::NEED_MORE,
                          .need = length - received.size()};
    }
    const std::uint64_t tid = get(received, TID_AT, 2);
    if (tid != (attempt.transaction_id & TID_MASK)) {
        return FrameState{.kind = FrameState::Kind::STALE, .length = length};
    }
    return FrameState{.kind = FrameState::Kind::COMPLETE, .length = length};
}

DecodeResult TestProtocol::decode(const CompiledDevice& device,
                                  DeviceState& /*state*/,
                                  const Request& request,
                                  const Attempt& /*attempt*/,
                                  std::span<const std::byte> frame,
                                  SampleSink& out) const noexcept {
    using Kind = DecodeResult::Kind;
    if (frame.size() < ANSWER_HEADER_BYTES) {
        return DecodeResult{.kind = Kind::BROKEN};
    }
    switch (static_cast<AnswerStatus>(get(frame, STATUS_AT, 1))) {
        case AnswerStatus::OK:
            break;
        case AnswerStatus::ILLEGAL_ADDRESS:
            return DecodeResult{.kind = Kind::SPLIT};
        case AnswerStatus::BUSY:
            return DecodeResult{.kind = Kind::BUSY};
        case AnswerStatus::DEVICE_INVALID:
            return DecodeResult{.kind = Kind::DEVICE_INVALID};
        case AnswerStatus::ABSENT:
            return DecodeResult{.kind = Kind::ABSENT};
        case AnswerStatus::UNREACHABLE:
            return DecodeResult{.kind = Kind::UNREACHABLE};
        default:
            return DecodeResult{.kind = Kind::BROKEN};
    }
    if (frame.size() != ANSWER_HEADER_BYTES + (2 * request.count)) {
        return DecodeResult{.kind = Kind::BROKEN};
    }
    for (const std::uint32_t index : request.points) {
        const auto& binding =
            protocol_data<TestPoint>(*device.points[index].protocol);
        if (binding.first < request.first ||
            binding.first + binding.count > request.first + request.count) {
            return DecodeResult{.kind = Kind::BROKEN};
        }
        std::uint64_t value = 0;
        for (std::uint64_t unit = 0; unit < binding.count; ++unit) {
            const std::size_t offset =
                ANSWER_HEADER_BYTES +
                (2 * (binding.first - request.first + unit));
            value = (value << UNIT_BITS) | get(frame, offset, 2);
        }
        (void)out.add(index,
                      RawSample{.outcome = Outcome::VALUE,
                                .raw = raw_unsigned(
                                    value, static_cast<std::uint8_t>(
                                               UNIT_BITS * binding.count))});
    }
    return DecodeResult{.kind = Kind::SAMPLES};
}

}  // namespace scada::driver::test
