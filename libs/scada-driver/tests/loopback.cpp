#include "loopback.h"

#include "scada/driver/compiled.h"
#include "scada/driver/exchange.h"
#include "scada/driver/plan.h"
#include "scada/driver/protocol.h"
#include "scada/driver/scheduler.h"
#include "test_protocol.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace scada::driver::test {

namespace {

constexpr unsigned BYTE_BITS = 8;
constexpr std::uint64_t BYTE_MASK = 0xFFU;
constexpr std::size_t TX_BYTES = 256;
constexpr std::size_t SINK_SAMPLES = 256;

std::uint64_t get(std::span<const std::byte> bytes, std::size_t offset,
                  std::size_t width) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
        value = (value << BYTE_BITS) |
                std::to_integer<std::uint64_t>(bytes[offset + index]);
    }
    return value;
}

void append(std::vector<std::byte>& out, std::uint64_t value,
            std::size_t width) {
    for (std::size_t index = 0; index < width; ++index) {
        out.push_back(static_cast<std::byte>(
            (value >> (BYTE_BITS * (width - 1 - index))) & BYTE_MASK));
    }
}

std::uint32_t key_of(std::uint8_t unit_id, std::uint8_t space) {
    return (std::uint32_t{unit_id} << BYTE_BITS) | space;
}

}  // namespace

void SimBus::map(std::uint8_t unit_id, std::uint8_t space, std::uint32_t first,
                 std::uint32_t last) {
    for (std::uint32_t address = first; address <= last; ++address) {
        registers_.try_emplace(Key{key_of(unit_id, space), address}, 0);
    }
}

void SimBus::set(std::uint8_t unit_id, std::uint8_t space,
                 std::uint32_t address, std::uint16_t value) {
    registers_[Key{key_of(unit_id, space), address}] = value;
}

void SimBus::side_effect(std::uint8_t unit_id, std::uint8_t space,
                         std::uint32_t first, std::uint32_t last) {
    for (std::uint32_t address = first; address <= last; ++address) {
        clears_[Key{key_of(unit_id, space), address}] = true;
    }
}

std::vector<std::byte> SimBus::answer(std::span<const std::byte> request,
                                      std::int64_t now_ns) {
    const auto tid = get(request, 0, 2);
    const auto unit_id = static_cast<std::uint8_t>(get(request, 2, 1));
    const auto space = static_cast<std::uint8_t>(get(request, 4, 1));
    const auto first = static_cast<std::uint32_t>(get(request, 5, 4));
    const auto count = static_cast<std::uint16_t>(get(request, 9, 2));
    log_.push_back(LoggedRequest{.time_ns = now_ns,
                                 .unit_id = unit_id,
                                 .space = space,
                                 .first = first,
                                 .count = count});
    std::vector<std::byte> out;
    const auto fault = faults_.find(unit_id);
    const Fault now_fault =
        fault == faults_.end() ? Fault::NONE : fault->second;
    if (now_fault == Fault::SILENT) {
        return out;
    }
    append(out, tid, 2);
    if (now_fault == Fault::GARBAGE) {
        append(out, unit_id + 1, 1);  // another unit: frame_state says BROKEN
        append(out, 0, 3);
        return out;
    }
    append(out, unit_id, 1);
    if (now_fault == Fault::BUSY || now_fault == Fault::UNREACHABLE) {
        append(out,
               static_cast<std::uint8_t>(now_fault == Fault::BUSY
                                             ? AnswerStatus::BUSY
                                             : AnswerStatus::UNREACHABLE),
               1);
        append(out, 0, 2);
        return out;
    }
    bool mapped = true;
    bool clears = false;
    for (std::uint32_t address = first; address < first + count; ++address) {
        const Key key{key_of(unit_id, space), address};
        mapped = mapped && registers_.contains(key);
        clears = clears || clears_.contains(key);
    }
    if (!mapped) {
        append(out, static_cast<std::uint8_t>(AnswerStatus::ILLEGAL_ADDRESS),
               1);
        append(out, 0, 2);
        return out;
    }
    if (clears) {
        ++side_effect_reads_;
    }
    append(out, static_cast<std::uint8_t>(AnswerStatus::OK), 1);
    append(out, count, 2);
    for (std::uint32_t address = first; address < first + count; ++address) {
        const Key key{key_of(unit_id, space), address};
        append(out, registers_[key], 2);
        if (clears_.contains(key)) {
            registers_[key] = 0;
        }
    }
    return out;
}

Loop::Loop(const CompiledChannel& channel, const FrameProtocol& protocol,
           SimBus& bus, std::int64_t start_ns, std::int64_t exchange_ns,
           std::size_t max_pending_orders)
    : channel_(&channel),
      protocol_(&protocol),
      bus_(&bus),
      scheduler_(channel, max_pending_orders, start_ns),
      device_states_(channel.devices.size()),
      tx_(TX_BYTES),
      sink_storage_(SINK_SAMPLES),
      now_ns_(start_ns),
      exchange_ns_(exchange_ns) {}

void Loop::exchange(const Choice& choice) {
    LoopEvent event{.time_ns = now_ns_,
                    .choice = choice,
                    .result = DecodeResult::Kind::BROKEN,
                    .samples = {}};
    if (choice.kind == Choice::Kind::ORDER) {
        // The content of an order arrives with the command slices; here the
        // step is only scheduled and logged.
        events_.push_back(std::move(event));
        now_ns_ += exchange_ns_;
        return;
    }
    const CompiledDevice& device = channel_->devices[choice.device];
    const Request& request = device.plan.requests[choice.request];
    Attempt attempt;
    TxBuffer buffer{.bytes = tx_};
    const EncodeResult encoded =
        protocol_->encode(device, channel_state_, request, attempt, buffer);
    EXPECT_EQ(encoded.kind, EncodeResult::Kind::OK);
    const std::vector<std::byte> answer =
        bus_->answer(std::span(tx_).first(encoded.length), now_ns_);
    const FrameState state = protocol_->frame_state(request, attempt, answer);
    EXPECT_EQ(state.kind, FrameState::Kind::COMPLETE);
    SampleSink sink(sink_storage_);
    const DecodeResult result =
        protocol_->decode(device, device_states_[choice.device], request,
                          attempt, std::span(answer).first(state.length), sink);
    event.result = result.kind;
    event.samples.assign(sink.samples().begin(), sink.samples().end());
    if (result.kind == DecodeResult::Kind::SPLIT) {
        (void)scheduler_.refused(choice.device, choice.request);
    }
    events_.push_back(std::move(event));
    now_ns_ += exchange_ns_;
}

void Loop::run_until(std::int64_t end_ns) {
    while (now_ns_ < end_ns) {
        const Choice choice = scheduler_.take(now_ns_);
        if (choice.kind == Choice::Kind::IDLE) {
            return;
        }
        if (choice.kind == Choice::Kind::WAIT) {
            now_ns_ = choice.wake_ns;
            continue;
        }
        exchange(choice);
    }
}

}  // namespace scada::driver::test
