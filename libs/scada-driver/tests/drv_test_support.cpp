#include "drv_test_support.h"

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
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <poll.h>
#include <unistd.h>

namespace scada::driver::test {
namespace {

constexpr std::int64_t NS_PER_MS = 1'000'000;
constexpr std::int64_t MAX_MS = 3'600'000;
constexpr std::uint16_t SERIAL_BITS_8N1 = 10;
// The limits of the command line: those of CS 4.6 and A.1 where it has them.
constexpr std::int64_t MAX_UID = std::int64_t{1} << 31;
constexpr std::uint8_t MAX_UNIT_ID = 247;
constexpr std::uint8_t MAX_RETRIES = 10;
constexpr std::uint32_t MAX_REGISTER = 1'000'000;
constexpr std::uint32_t MAX_GROUP = 2;
constexpr std::uint32_t MAX_PORT = 65'535;
constexpr std::uint32_t MAX_BAUD = 4'000'000;
// The CS 4.6 default poll groups, in ms.
constexpr std::int64_t FAST_MS = 200;
constexpr std::int64_t NORMAL_MS = 1'000;
constexpr std::int64_t SLOW_MS = 10'000;

constexpr std::string_view USAGE =
    "usage: scada-drv-test --base-dir DIR [--retain-dir DIR] [--tagd-uid N]\n"
    "           [--heartbeat-ms N]\n"
    "         ( --channel NAME (tcp HOST PORT | serial PATH BAUD)\n"
    "               [--connect-timeout-ms N] [--reconnect-ms MIN MAX]\n"
    "               [--disabled] [--hang | --spin]\n"
    "           ( --device NAME UNIT [--timeout-ms N] [--retries N] "
    "[--disabled]\n"
    "             ( --point NAME FIRST GROUP [--max-age-ms N] )... )... )...\n"
    "       scada-drv-test --help\n"
    "\n"
    "A driver of the test protocol. A point reads one unit of space 0 into\n"
    "an INT64 tag <device>.<point>; GROUP is 0 (200 ms), 1 (1 s) or 2\n"
    "(10 s). --hang and --spin make the channel's first receive block or\n"
    "spin for good, deaf to the stop. Every number is unsigned.\n"
    "Exit status: 0 stopped, 1 failed, 2 usage, 3 thread check, 4 stall.\n";

[[noreturn]] void thread_check_failed(std::string_view call) noexcept {
    // Not through the logger: the check stops the process at once.
    const std::string line = "thread check failed: " + std::string(call) + "\n";
    // The status says it if the line cannot be written.
    if (::write(STDERR_FILENO, line.data(), line.size()) < 0) {
        std::_Exit(EXIT_THREAD_CHECK);
    }
    std::_Exit(EXIT_THREAD_CHECK);
}

// A transport whose first receive never returns and never looks at the
// stop: a worker that stops moving its progress counter (2.6).
class FaultyTransport final : public Transport {
public:
    FaultyTransport(std::unique_ptr<Transport> inner, Fault fault)
        : inner_(std::move(inner)), fault_(fault) {}
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
        std::atomic<std::uint64_t> turns{0};
        for (;;) {
            if (fault_ == Fault::SPIN) {
                // Busy, never blocked: the CPU is used, the counter is not.
                turns.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // The stop signals are blocked on a worker: nothing ends this
            // wait.
            (void)::ppoll(nullptr, 0, nullptr, nullptr);
        }
    }
    bool discard_input() noexcept override { return inner_->discard_input(); }

private:
    std::unique_ptr<Transport> inner_;
    Fault fault_;
};

template <class T>
std::optional<T> number(std::string_view text, T limit) {
    // from_chars takes a minus for a signed type: a sign is refused here.
    if (text.empty() || text.front() == '-' || text.front() == '+') {
        return std::nullopt;
    }
    T value{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() ||
        value > limit) {
        return std::nullopt;
    }
    return value;
}

class Parser {
public:
    Parser(std::span<const char* const> args, std::ostream& err)
        : args_(args), err_(&err) {}

    std::optional<DrvTestOptions> parse() {
        options_.settings.process = "drv-test";
        while (at_ < args_.size()) {
            const std::string_view arg = next();
            if (arg == "--help") {
                options_.help = true;
                return std::move(options_);
            }
            if (!option(arg)) {
                *err_ << "scada-drv-test: bad argument " << arg << "\n"
                      << USAGE;
                return std::nullopt;
            }
        }
        if (options_.settings.base_dir.empty() ||
            options_.config.channels.empty()) {
            *err_ << "scada-drv-test: --base-dir and a --channel are "
                     "required\n"
                  << USAGE;
            return std::nullopt;
        }
        return std::move(options_);
    }

private:
    std::string_view next() {
        return at_ < args_.size() && args_[at_] != nullptr ? args_[at_++] : "";
    }

    template <class T>
    bool take(T& out, T limit) {
        const auto value = number<T>(next(), limit);
        if (value) {
            out = *value;
        }
        return value.has_value();
    }

    bool millis(std::int64_t& out_ns) {
        std::int64_t value = 0;
        const bool valid = take(value, MAX_MS) && value > 0;
        out_ns = value * NS_PER_MS;
        return valid;
    }

    bool option(std::string_view arg) {
        if (arg == "--base-dir") {
            options_.settings.base_dir = next();
            return !options_.settings.base_dir.empty();
        }
        if (arg == "--retain-dir") {
            options_.settings.retain_dir = next();
            return !options_.settings.retain_dir.empty();
        }
        if (arg == "--tagd-uid") {
            return take(options_.settings.tagd_uid, MAX_UID);
        }
        if (arg == "--heartbeat-ms") {
            return millis(options_.settings.heartbeat_period_ns);
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
            channel.transport = TransportKind::TCP;
        } else if (kind == "serial") {
            channel.transport = TransportKind::SERIAL;
            endpoint.serial = true;
        } else {
            return false;
        }
        if (!take(endpoint.port_or_baud,
                  endpoint.serial ? MAX_BAUD : MAX_PORT)) {
            return false;
        }
        if (endpoint.serial) {
            channel.line = LineTiming{.bit_rate = endpoint.port_or_baud,
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
            return millis(channel.connection.connect_timeout_ns);
        }
        if (arg == "--reconnect-ms") {
            const bool valid = millis(channel.connection.reconnect_min_ns) &&
                               millis(channel.connection.reconnect_max_ns);
            return valid && channel.connection.reconnect_max_ns >=
                                channel.connection.reconnect_min_ns;
        }
        if (arg == "--hang" || arg == "--spin") {
            options_.endpoints.back().fault =
                arg == "--hang" ? Fault::HANG : Fault::SPIN;
            return true;
        }
        if (arg == "--disabled" && channel.devices.empty()) {
            channel.enabled = false;  // its worker writes 9.1 only (M2)
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
            const bool valid = millis(device.failure.timeout_ns);
            device.failure.write_timeout_ns = device.failure.timeout_ns;
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
            return millis(device.points.back().rules.max_age_ns);
        }
        return false;
    }

    std::span<const char* const> args_;
    std::ostream* err_;
    std::size_t at_ = 1;
    DrvTestOptions options_;
};

}  // namespace

std::string_view drv_test_usage() noexcept {
    return USAGE;
}

std::optional<DrvTestOptions> parse_drv_test_args(
    std::span<const char* const> args, std::ostream& err) {
    return Parser(args, err).parse();
}

ThreadCheckedProtocol::ThreadCheckedProtocol(const CompiledConfig& config)
    : main_(std::this_thread::get_id()) {
    std::size_t slot = 0;
    for (const CompiledChannel& channel : config.channels) {
        for (const CompiledDevice& device : channel.devices) {
            slots_.emplace(&device, slot++);
        }
    }
    workers_ = std::vector<std::atomic<std::thread::id>>(slot);
}

Status ThreadCheckedProtocol::configure(const DriverConfig& config,
                                        CompiledConfig& out,
                                        ConfigErrors& errors) const noexcept {
    on_main("configure");
    return inner_.configure(config, out, errors);
}

DeviceCaps ThreadCheckedProtocol::device_caps(
    const CompiledDevice& device) const noexcept {
    on_main("device_caps");  // DI 2.3: Main (review M12)
    return inner_.device_caps(device);
}

PointCaps ThreadCheckedProtocol::point_caps(
    const CompiledPoint& point) const noexcept {
    on_main("point_caps");
    return inner_.point_caps(point);
}

Status ThreadCheckedProtocol::plan(const CompiledDevice& device,
                                   const ReadRules& rules,
                                   ReadPlan& out) const noexcept {
    on_main("plan");
    return inner_.plan(device, rules, out);
}

EncodeResult ThreadCheckedProtocol::encode(const CompiledDevice& device,
                                           ChannelState& channel,
                                           const Request& request,
                                           Attempt& attempt,
                                           TxBuffer& out) const noexcept {
    on_worker(device, "encode");
    return inner_.encode(device, channel, request, attempt, out);
}

FrameState ThreadCheckedProtocol::frame_state(
    const Request& request, const Attempt& attempt,
    std::span<const std::byte> received) const noexcept {
    if (std::this_thread::get_id() == main_) {
        thread_check_failed("frame_state on the main thread");
    }
    return inner_.frame_state(request, attempt, received);
}

DecodeResult ThreadCheckedProtocol::decode(const CompiledDevice& device,
                                           DeviceState& state,
                                           const Request& request,
                                           const Attempt& attempt,
                                           std::span<const std::byte> frame,
                                           SampleSink& sink) const noexcept {
    on_worker(device, "decode");
    return inner_.decode(device, state, request, attempt, frame, sink);
}

void ThreadCheckedProtocol::on_main(std::string_view call) const noexcept {
    if (std::this_thread::get_id() != main_) {
        thread_check_failed(call);
    }
}

void ThreadCheckedProtocol::on_worker(const CompiledDevice& device,
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

bool plan_all(CompiledConfig& config, const FrameProtocol& protocol,
              std::ostream& err) {
    for (CompiledChannel& channel : config.channels) {
        for (CompiledDevice& device : channel.devices) {
            device.plan.by_group.resize(channel.poll_groups.size());
            const Status planned = protocol.plan(
                device, ReadRules{.max_units = MAX_UNITS}, device.plan);
            if (!planned.ok()) {
                err << "scada-drv-test: device " << device.name
                    << " cannot be planned: " << planned.message() << "\n";
                return false;
            }
        }
    }
    return true;
}

std::unique_ptr<Transport> make_transport(const CompiledChannel& channel,
                                          const Endpoint& endpoint,
                                          const Clock& clock) {
    std::unique_ptr<Transport> made;
    if (endpoint.serial) {
        made = std::make_unique<SerialTransport>(
            SerialSettings{.device = endpoint.host_or_path,
                           .baud = endpoint.port_or_baud,
                           .parity = Parity::NONE,
                           .stop_bits = 1},
            clock);
    } else {
        made = std::make_unique<TcpTransport>(
            TcpEndpoint{
                .host = endpoint.host_or_path,
                .port = static_cast<std::uint16_t>(endpoint.port_or_baud),
                .user_timeout_ns = tcp_user_timeout_ns(channel)},
            clock);
    }
    if (endpoint.fault != Fault::NONE) {
        made =
            std::make_unique<FaultyTransport>(std::move(made), endpoint.fault);
    }
    return made;
}

}  // namespace scada::driver::test
