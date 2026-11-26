// scada-drv-test: a driver process of the test protocol (driver interface
// contract 15: "a scripted frame protocol over a loopback transport"), for
// the integration tests of the driver process (decision D-142). The
// configuration is given on the command line until the framework's compile
// step (slice 8):
//
//   scada-drv-test --base-dir DIR [--tagd-uid N] [--heartbeat-ms N]
//       ( --channel NAME (tcp HOST PORT | serial PATH BAUD)
//             [--connect-timeout-ms N] [--reconnect-ms MIN MAX] [--hang]
//         ( --device NAME UNIT [--timeout-ms N] [--retries N] [--disabled]
//           ( --point NAME FIRST GROUP [--max-age-ms N] )... )... )...
//
// A point reads one unit of space 0 into an INT64 tag `<device>.<point>`;
// GROUP is 0 (200 ms), 1 (1 s) or 2 (10 s). `--hang` makes the channel's
// first receive block for good, for the channel watchdog test (2.6).
//
// Every call of the protocol is checked to run on its thread (conformance
// test 1): `configure`, `plan` and the capabilities on the main thread,
// `encode`, `frame_state` and `decode` on one worker thread per device and
// never the main one. A call on another thread ends the process at once
// with exit status 3 and "thread check failed" on standard error.

#include "scada/common/log.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/driver/compiled.h"
#include "scada/driver/connections.h"
#include "scada/driver/driver.h"
#include "scada/driver/exchange.h"
#include "scada/driver/plan.h"
#include "scada/driver/protocol.h"
#include "scada/driver/timing.h"
#include "scada/driver/transport.h"
#include "test_protocol.h"

#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <poll.h>
#include <unistd.h>

namespace {

using scada::driver::CompiledChannel;
using scada::driver::CompiledConfig;
using scada::driver::CompiledDevice;
using scada::driver::CompiledPoint;
using scada::driver::IoStatus;
using scada::driver::Transport;
using scada::driver::test::TestDevice;
using scada::driver::test::TestPoint;
using scada::driver::test::TestProtocol;

constexpr int EXIT_THREAD_CHECK = 3;
constexpr std::int64_t NS_PER_MS = 1'000'000;
constexpr std::int64_t MAX_MS = 3'600'000;
constexpr std::uint16_t SERIAL_BITS_8N1 = 10;
// The limits of the command line: those of CS 4.6 and A.1 where it has them.
constexpr std::int64_t MAX_UID = std::int64_t{1} << 31;
constexpr std::uint8_t MAX_UNIT_ID = 247;
constexpr std::uint8_t MAX_RETRIES = 10;
constexpr std::uint32_t MAX_REGISTER = 1'000'000;
constexpr std::uint32_t MAX_GROUP = 2;
// The CS 4.6 default poll groups, in ms.
constexpr std::int64_t FAST_MS = 200;
constexpr std::int64_t NORMAL_MS = 1'000;
constexpr std::int64_t SLOW_MS = 10'000;

[[noreturn]] void thread_check_failed(std::string_view call) noexcept {
    // Not through the logger: the check stops the process at once.
    const std::string line = "thread check failed: " + std::string(call) + "\n";
    // The status says it if the line cannot be written.
    if (::write(STDERR_FILENO, line.data(), line.size()) < 0) {
        std::_Exit(EXIT_THREAD_CHECK);
    }
    std::_Exit(EXIT_THREAD_CHECK);
}

// The test protocol, with the thread of every call checked (test 1).
class ThreadCheckedProtocol final : public scada::driver::FrameProtocol {
public:
    explicit ThreadCheckedProtocol(const CompiledConfig& config)
        : main_(std::this_thread::get_id()) {
        std::size_t slot = 0;
        for (const CompiledChannel& channel : config.channels) {
            for (const CompiledDevice& device : channel.devices) {
                slots_.emplace(&device, slot++);
            }
        }
        workers_ = std::vector<std::atomic<std::thread::id>>(slot);
    }

    [[nodiscard]] scada::Status configure(
        const scada::driver::DriverConfig& config, CompiledConfig& out,
        scada::driver::ConfigErrors& errors) const noexcept override {
        on_main("configure");
        return inner_.configure(config, out, errors);
    }
    [[nodiscard]] scada::driver::DeviceCaps device_caps(
        const CompiledDevice& device) const noexcept override {
        // Asked on the main thread at start, and by the engine on its
        // worker when it is built there (D-142).
        return inner_.device_caps(device);
    }
    [[nodiscard]] scada::driver::PointCaps point_caps(
        const CompiledPoint& point) const noexcept override {
        return inner_.point_caps(point);
    }
    [[nodiscard]] scada::Status plan(
        const CompiledDevice& device, const scada::driver::ReadRules& rules,
        scada::driver::ReadPlan& out) const noexcept override {
        on_main("plan");
        return inner_.plan(device, rules, out);
    }
    SCADA_RT [[nodiscard]] scada::driver::EncodeResult encode(
        const CompiledDevice& device, scada::driver::ChannelState& channel,
        const scada::driver::Request& request, scada::driver::Attempt& attempt,
        scada::driver::TxBuffer& out) const noexcept override {
        on_worker(device, "encode");
        return inner_.encode(device, channel, request, attempt, out);
    }
    SCADA_RT [[nodiscard]] scada::driver::FrameState frame_state(
        const scada::driver::Request& request,
        const scada::driver::Attempt& attempt,
        std::span<const std::byte> received) const noexcept override {
        if (std::this_thread::get_id() == main_) {
            thread_check_failed("frame_state on the main thread");
        }
        return inner_.frame_state(request, attempt, received);
    }
    SCADA_RT [[nodiscard]] scada::driver::DecodeResult decode(
        const CompiledDevice& device, scada::driver::DeviceState& state,
        const scada::driver::Request& request,
        const scada::driver::Attempt& attempt, std::span<const std::byte> frame,
        scada::driver::SampleSink& sink) const noexcept override {
        on_worker(device, "decode");
        return inner_.decode(device, state, request, attempt, frame, sink);
    }

private:
    void on_main(std::string_view call) const noexcept {
        if (std::this_thread::get_id() != main_) {
            thread_check_failed(call);
        }
    }
    void on_worker(const CompiledDevice& device,
                   std::string_view call) const noexcept {
        const std::thread::id self = std::this_thread::get_id();
        const auto found = slots_.find(&device);
        if (self == main_ || found == slots_.end()) {
            thread_check_failed(call);
        }
        std::thread::id expected{};
        std::atomic<std::thread::id>& slot = workers_[found->second];
        if (!slot.compare_exchange_strong(expected, self) && expected != self) {
            thread_check_failed(call);
        }
    }

    TestProtocol inner_;
    std::thread::id main_;
    std::unordered_map<const CompiledDevice*, std::size_t> slots_;
    mutable std::vector<std::atomic<std::thread::id>> workers_;
};

// A transport whose first receive never returns: a worker that stops
// moving its progress counter (2.6).
class HangingTransport final : public Transport {
public:
    explicit HangingTransport(std::unique_ptr<Transport> inner)
        : inner_(std::move(inner)) {}
    IoStatus connect(std::int64_t deadline_ns) noexcept override {
        return inner_->connect(deadline_ns);
    }
    void close(bool abortive) noexcept override { inner_->close(abortive); }
    [[nodiscard]] int poll_fd() const noexcept override {
        return inner_->poll_fd();
    }
    [[nodiscard]] bool closed_by_peer() noexcept override {
        return inner_->closed_by_peer();
    }
    IoStatus send(std::span<const std::byte> frame) noexcept override {
        return inner_->send(frame);
    }
    IoStatus receive(std::span<std::byte> /*into*/, std::int64_t /*deadline*/,
                     std::size_t& received) noexcept override {
        received = 0;
        for (;;) {
            // Every signal is blocked on a worker: nothing ends this wait.
            (void)::ppoll(nullptr, 0, nullptr, nullptr);
        }
    }
    bool discard_input() noexcept override { return inner_->discard_input(); }

private:
    std::unique_ptr<Transport> inner_;
};

// Where a channel connects, beside its compiled form.
struct Endpoint {
    bool serial = false;
    std::string host_or_path;
    std::uint32_t port_or_baud = 0;
    bool hang = false;
};

struct Options {
    scada::driver::DriverSettings settings;
    CompiledConfig config;
    std::vector<Endpoint> endpoints;  // one per channel
};

template <class T>
std::optional<T> number(std::string_view text, T limit) {
    T value{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} ||
        end != text.data() + text.size() || value > limit) {
        return std::nullopt;
    }
    return value;
}

// Reads the command line; nothing on a malformed one, with a message.
class Parser {
public:
    explicit Parser(std::span<char* const> args) : args_(args) {}

    std::optional<Options> parse() {
        options_.settings.process = "drv-test";
        while (at_ < args_.size()) {
            const std::string_view arg = next();
            if (!option(arg)) {
                std::cerr << "scada-drv-test: bad argument " << arg << "\n";
                return std::nullopt;
            }
        }
        if (options_.settings.base_dir.empty() ||
            options_.config.channels.empty()) {
            std::cerr << "scada-drv-test: --base-dir and a --channel are "
                         "required\n";
            return std::nullopt;
        }
        return std::move(options_);
    }

private:
    std::string_view next() { return at_ < args_.size() ? args_[at_++] : ""; }

    template <class T>
    bool take(T& out, T limit) {
        const auto value = number<T>(next(), limit);
        if (value) {
            out = *value;
        }
        return value.has_value();
    }

    bool option(std::string_view arg) {
        if (arg == "--base-dir") {
            options_.settings.base_dir = next();
            return !options_.settings.base_dir.empty();
        }
        if (arg == "--tagd-uid") {
            return take(options_.settings.tagd_uid, MAX_UID);
        }
        if (arg == "--heartbeat-ms") {
            std::int64_t millis = 0;
            const bool valid = take(millis, MAX_MS) && millis > 0;
            options_.settings.heartbeat_period_ns = millis * NS_PER_MS;
            return valid;
        }
        if (arg == "--channel") {
            return channel();
        }
        if (options_.config.channels.empty()) {
            return false;
        }
        return channel_option(arg);
    }

    bool channel() {
        CompiledChannel channel;
        channel.name = next();
        channel.poll_groups = {
            {.name = "fast", .period_ns = FAST_MS * NS_PER_MS, .priority = 1},
            {.name = "normal",
             .period_ns = NORMAL_MS * NS_PER_MS,
             .priority = 2},
            {.name = "slow", .period_ns = SLOW_MS * NS_PER_MS, .priority = 3}};
        Endpoint endpoint;
        const std::string_view kind = next();
        endpoint.host_or_path = next();
        if (kind == "tcp") {
            channel.transport = scada::driver::TransportKind::TCP;
        } else if (kind == "serial") {
            channel.transport = scada::driver::TransportKind::SERIAL;
            endpoint.serial = true;
        } else {
            return false;
        }
        constexpr std::uint32_t MAX_PORT = 65'535;
        constexpr std::uint32_t MAX_BAUD = 4'000'000;
        if (!take(endpoint.port_or_baud,
                  endpoint.serial ? MAX_BAUD : MAX_PORT)) {
            return false;
        }
        if (endpoint.serial) {
            channel.line =
                scada::driver::LineTiming{.bit_rate = endpoint.port_or_baud,
                                          .bits_per_char = SERIAL_BITS_8N1,
                                          .turnaround_ns = 0};
        }
        options_.config.channels.push_back(std::move(channel));
        options_.endpoints.push_back(std::move(endpoint));
        return !options_.config.channels.back().name.empty();
    }

    bool channel_option(std::string_view arg) {
        CompiledChannel& channel = options_.config.channels.back();
        if (arg == "--connect-timeout-ms") {
            std::int64_t millis = 0;
            const bool valid = take(millis, MAX_MS) && millis > 0;
            channel.connection.connect_timeout_ns = millis * NS_PER_MS;
            return valid;
        }
        if (arg == "--reconnect-ms") {
            std::int64_t low = 0;
            std::int64_t high = 0;
            const bool valid = take(low, MAX_MS) && take(high, MAX_MS) &&
                               low > 0 && high >= low;
            channel.connection.reconnect_min_ns = low * NS_PER_MS;
            channel.connection.reconnect_max_ns = high * NS_PER_MS;
            return valid;
        }
        if (arg == "--hang") {
            options_.endpoints.back().hang = true;
            return true;
        }
        if (arg == "--device") {
            CompiledDevice device;
            device.name = next();
            std::uint8_t unit = 0;
            if (device.name.empty() || !take(unit, MAX_UNIT_ID)) {
                return false;
            }
            device.unit = unit;
            auto binding = std::make_unique<TestDevice>();
            binding->unit_id = unit;
            device.protocol = std::move(binding);
            channel.devices.push_back(std::move(device));
            return true;
        }
        if (channel.devices.empty()) {
            return false;
        }
        return device_option(arg, channel.devices.back());
    }

    bool device_option(std::string_view arg, CompiledDevice& device) {
        if (arg == "--timeout-ms") {
            std::int64_t millis = 0;
            const bool valid = take(millis, MAX_MS) && millis > 0;
            device.failure.timeout_ns = millis * NS_PER_MS;
            device.failure.write_timeout_ns = millis * NS_PER_MS;
            return valid;
        }
        if (arg == "--disabled") {
            device.enabled = false;  // its tags get the writes of 9.1 only
            return true;
        }
        if (arg == "--retries") {
            return take(device.failure.retries, MAX_RETRIES);
        }
        if (arg == "--point") {
            CompiledPoint point;
            point.name = next();
            auto binding = std::make_unique<TestPoint>();
            std::uint32_t group = 0;
            if (point.name.empty() || !take(binding->first, MAX_REGISTER) ||
                !take(group, MAX_GROUP)) {
                return false;
            }
            point.poll_group = group;
            point.protocol = std::move(binding);
            device.points.push_back(std::move(point));
            return true;
        }
        if (arg == "--max-age-ms" && !device.points.empty()) {
            std::int64_t millis = 0;
            const bool valid = take(millis, MAX_MS) && millis > 0;
            device.points.back().rules.max_age_ns = millis * NS_PER_MS;
            return valid;
        }
        return false;
    }

    std::span<char* const> args_;
    std::size_t at_ = 1;
    Options options_;
};

// Plans every device with the checked protocol, on the main thread.
bool plan_all(CompiledConfig& config,
              const scada::driver::FrameProtocol& protocol) {
    for (CompiledChannel& channel : config.channels) {
        for (CompiledDevice& device : channel.devices) {
            device.plan.by_group.resize(channel.poll_groups.size());
            const scada::Status planned =
                protocol.plan(device,
                              scada::driver::ReadRules{
                                  .max_units = scada::driver::test::MAX_UNITS},
                              device.plan);
            if (!planned.ok()) {
                std::cerr << "scada-drv-test: device " << device.name
                          << " cannot be planned: " << planned.message()
                          << "\n";
                return false;
            }
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // First, before the logger's thread (driver.h).
    if (!scada::driver::block_all_signals().ok()) {
        return scada::driver::EXIT_ERROR;
    }
    std::optional<Options> options =
        Parser(std::span<char* const>(argv, static_cast<std::size_t>(argc)))
            .parse();
    if (!options) {
        return scada::driver::EXIT_USAGE;
    }
    const ThreadCheckedProtocol protocol(options->config);
    if (!plan_all(options->config, protocol)) {
        return scada::driver::EXIT_USAGE;
    }
    scada::Result<std::unique_ptr<scada::LogSink>> sink =
        scada::make_default_sink();
    if (!sink.ok()) {
        return scada::driver::EXIT_ERROR;
    }
    scada::Result<scada::Logger> logger = scada::Logger::create(
        std::move(sink).value(), {.component = "scada-drv-test"});
    if (!logger.ok()) {
        return scada::driver::EXIT_ERROR;
    }
    const std::vector<Endpoint>& endpoints = options->endpoints;
    const CompiledConfig& config = options->config;
    auto transports =
        [&](const CompiledChannel& channel,
            const scada::driver::Clock& clock) -> std::unique_ptr<Transport> {
        const auto index =
            static_cast<std::size_t>(&channel - config.channels.data());
        const Endpoint& endpoint = endpoints.at(index);
        std::unique_ptr<Transport> made;
        if (endpoint.serial) {
            made = std::make_unique<scada::driver::SerialTransport>(
                scada::driver::SerialSettings{
                    .device = endpoint.host_or_path,
                    .baud = endpoint.port_or_baud,
                    .parity = scada::driver::Parity::NONE,
                    .stop_bits = 1},
                clock);
        } else {
            made = std::make_unique<scada::driver::TcpTransport>(
                scada::driver::TcpEndpoint{
                    .host = endpoint.host_or_path,
                    .port = static_cast<std::uint16_t>(endpoint.port_or_baud),
                    .user_timeout_ns =
                        scada::driver::tcp_user_timeout_ns(channel)},
                clock);
        }
        if (endpoint.hang) {
            made = std::make_unique<HangingTransport>(std::move(made));
        }
        return made;
    };
    scada::driver::SystemClock clock;
    scada::driver::Driver driver(config, protocol, transports, clock,
                                 options->settings, &logger.value());
    switch (driver.run()) {
        case scada::driver::RunEnd::STOPPED:
            return scada::driver::EXIT_OK;
        case scada::driver::RunEnd::STALLED:
            // The stalled worker cannot be joined (coding standard 7).
            std::_Exit(scada::driver::EXIT_STALL);
        case scada::driver::RunEnd::FAILED:
            break;
    }
    std::cerr << "scada-drv-test: " << driver.failure_detail() << "\n";
    return scada::driver::EXIT_ERROR;
}
