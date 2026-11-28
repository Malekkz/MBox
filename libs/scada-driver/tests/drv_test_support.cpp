#include "drv_test_support.h"

#include "scada/common/log.h"
#include "scada/common/status.h"
#include "scada/driver/compiled.h"
#include "scada/driver/connections.h"
#include "scada/driver/driver.h"
#include "scada/driver/exchange.h"
#include "scada/driver/identity.h"
#include "scada/driver/plan.h"
#include "scada/driver/point_rules.h"
#include "scada/driver/protocol.h"
#include "scada/driver/tag_value.h"
#include "scada/driver/timing.h"
#include "scada/driver/transport.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/types.h"
#include "test_protocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
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

// The one system tag write that `--refuse-write` refuses (PR 159 review):
// set before any thread starts, then read by the thread that owns the tag,
// which alone counts down `skip`.
struct Refusal {
    std::array<char, SCADA_TAG_NAME_SIZE> name{};
    std::uint64_t bits = 0;
    std::uint64_t skip = 0;
};

Refusal& refusal() noexcept {
    static Refusal instance;
    return instance;
}

// The library's write, but the chosen one refused as a full ring or a lost
// owner would be.
int refusing_write(scada_ctx* ctx, std::uint32_t handle,
                   const scada_value* value) {
    Refusal& refused = refusal();
    scada_tag_info_t info{};
    if (value->value == refused.bits &&
        scada_tag_info(ctx, handle, &info) == SCADA_OK &&
        std::string_view(std::data(info.name)) ==
            std::string_view(refused.name.data())) {
        if (refused.skip == 0) {
            return SCADA_E_NOT_OWNER;
        }
        --refused.skip;
    }
    return scada_tag_write(ctx, handle, value);
}

constexpr std::int64_t NS_PER_MS = 1'000'000;
constexpr std::int64_t MAX_MS = 3'600'000;
constexpr std::uint16_t SERIAL_BITS_8N1 = 10;
// The limits of the command line: those of CS 4.6 and A.1 where it has them.
constexpr std::int64_t MAX_UID = std::int64_t{1} << 31;
// The value and the count of `--refuse-write`.
constexpr std::uint64_t MAX_REFUSED = std::uint64_t{1} << 32;
constexpr std::uint8_t MAX_UNIT_ID = 247;
constexpr std::uint8_t MAX_RETRIES = 10;
constexpr std::uint32_t MAX_REGISTER = 1'000'000;
constexpr std::uint32_t MAX_GROUP = 2;
// A unit of the test protocol is 16 bits; a substitute is any INT64 the
// command line can give.
constexpr std::uint64_t MAX_RAW = 0xFFFFU;
constexpr std::int64_t MAX_SUBSTITUTE = 1'000'000'000;
// A test point's raw width: one 16-bit unit (test_protocol.h).
constexpr std::uint8_t TEST_RAW_WIDTH = 16;
constexpr std::uint32_t MAX_WRITES_PER_DAY = 1'000'000;
constexpr std::uint32_t MAX_PORT = 65'535;
constexpr std::uint32_t MAX_BAUD = 4'000'000;
// The CS 4.6 default poll groups, in ms.
constexpr std::int64_t FAST_MS = 200;
constexpr std::int64_t NORMAL_MS = 1'000;
constexpr std::int64_t SLOW_MS = 10'000;
constexpr std::size_t MAX_LOG_QUEUE = LOG_MAX_QUEUE_CAPACITY;

constexpr std::string_view USAGE =
    "usage: scada-drv-test --base-dir DIR [--retain-dir DIR] [--tagd-uid N]\n"
    "           [--heartbeat-ms N] [--refuse-write TAG VALUE SKIP]\n"
    "           [--worker-log-queue N] [--refuse-log MS]\n"
    "         ( --channel NAME (tcp HOST PORT | serial PATH BAUD)\n"
    "               [--connect-timeout-ms N] [--reconnect-ms MIN MAX]\n"
    "               [--disabled] [--hang | --spin]\n"
    "           ( --device NAME UNIT [--timeout-ms N] [--retries N] "
    "[--disabled]\n"
    "             ( --point NAME FIRST GROUP [--max-age-ms N]\n"
    "                 [--substitute RAW VALUE] )...\n"
    "             [--identify-equals POINT VALUE]... [--on-mismatch-warn]\n"
    "             [--restart-decreases POINT]...\n"
    "             [--output NAME KIND [--sbo] [--on-only] [--min N] [--max N]\n"
    "                [--local-panel TAG] [--write-min-interval-ms N]\n"
    "                [--write-max-per-day N]]... )... )...\n"
    "       scada-drv-test --help\n"
    "\n"
    "A driver of the test protocol. A point reads one unit of space 0 into\n"
    "an INT64 tag <device>.<point>; GROUP is 0 (200 ms), 1 (1 s) or 2\n"
    "(10 s). --hang and --spin make the channel's first receive block or\n"
    "spin for good, deaf to the stop. --refuse-write refuses the writes of\n"
    "VALUE to the system tag TAG once SKIP of them were written.\n"
    "--worker-log-queue sets the records of each worker's log queue (by\n"
    "default two per device of its channel, at least 64). --refuse-log\n"
    "writes every log record, waits MS ms (0 to 1000) and then reports it\n"
    "refused, as a slow journal that refuses it would, so that\n"
    "log_dropped counts it.\n"
    "--substitute writes VALUE, UNCERTAIN with SUBSTITUTED, when the point\n"
    "reads RAW (`on_invalid: { substitute: VALUE }` of that pattern).\n"
    "--identify-equals checks that the device's POINT (its index among\n"
    "the device's points so far) reads VALUE before any point is\n"
    "published; a mismatch is MISMATCH, or with --on-mismatch-warn the\n"
    "device runs. --restart-decreases makes a decrease of POINT a\n"
    "restart of the device (`restart_indication: { decreases: ... }`).\n"
    "--output makes the device's last point the target of an output NAME of\n"
    "KIND (single, double, setpoint, step, pulse or trigger), its mode tag\n"
    "NAME.mode; --sbo, --on-only, --min, --max and --local-panel (the panel\n"
    "input of local_blocks_auto) set its keys, and the --write-* options its\n"
    "target's write_limits.\n"
    "Every number is unsigned.\n"
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
        if (arg == "--refuse-write") {
            return refuse_write();
        }
        if (arg == "--refuse-log") {
            constexpr std::int64_t MOST_MS = 1'000;
            if (!take(options_.refuse_log_ms, MOST_MS)) {
                return false;
            }
            options_.refuse_log = true;
            return true;
        }
        if (arg == "--worker-log-queue") {
            std::size_t records = 0;
            if (!take(records, MAX_LOG_QUEUE) || records == 0) {
                return false;
            }
            options_.settings.worker_log_queue = records;
            return true;
        }
        if (arg == "--channel") {
            return channel();
        }
        if (options_.config.channels.empty()) {
            return false;
        }
        return channel_option(arg);
    }

    // `--refuse-write TAG VALUE SKIP`.
    bool refuse_write() {
        const std::string_view tag = next();
        Refusal& refused = refusal();
        if (tag.empty() || tag.size() >= refused.name.size() ||
            !take(refused.bits, MAX_REFUSED) ||
            !take(refused.skip, MAX_REFUSED)) {
            return false;
        }
        std::copy(tag.begin(), tag.end(), refused.name.begin());
        options_.settings.system_write = &refusing_write;
        return true;
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
        if (arg == "--identify-equals" || arg == "--restart-decreases") {
            return identity_option(arg, device);
        }
        if (arg == "--on-mismatch-warn") {
            device.refuse_on_mismatch = false;
            return true;
        }
        if (arg == "--output" && !device.points.empty()) {
            return output(device);
        }
        if (!device.outputs.empty() && output_option(arg, device)) {
            return true;
        }
        if (arg == "--substitute" && !device.points.empty()) {
            std::uint64_t raw = 0;
            std::int64_t value = 0;
            if (!take(raw, MAX_RAW) || !take(value, MAX_SUBSTITUTE)) {
                return false;
            }
            device.points.back().rules.markers.push_back(
                InvalidMarker{.pattern = raw,
                              .action = InvalidAction::SUBSTITUTE,
                              .substitute = int64_value(value)});
            return true;
        }
        return false;
    }

    // `--output NAME KIND`: the device's last point is the target of an
    // output (D-150; the compile step's until slice 8). Test points are
    // INT64 tags of one 16-bit unit.
    bool output(CompiledDevice& device) {
        CompiledOutput made;
        made.name = next();
        const std::string_view kind = next();
        static constexpr std::array<std::pair<std::string_view, OutputKind>, 6>
            KINDS{{{"single", OutputKind::SINGLE},
                   {"double", OutputKind::DOUBLE},
                   {"setpoint", OutputKind::SETPOINT},
                   {"step", OutputKind::STEP},
                   {"pulse", OutputKind::PULSE},
                   {"trigger", OutputKind::TRIGGER}}};
        const auto* found = std::find_if(
            KINDS.begin(), KINDS.end(),
            [&](const auto& entry) { return entry.first == kind; });
        if (made.name.empty() || found == KINDS.end()) {
            return false;
        }
        made.kind = found->second;
        made.on_only =
            made.kind == OutputKind::PULSE || made.kind == OutputKind::TRIGGER;
        made.point = static_cast<std::uint32_t>(device.points.size() - 1);
        made.raw_width = TEST_RAW_WIDTH;
        device.points.back().access = Access::RW;
        device.outputs.push_back(std::move(made));
        return true;
    }

    // The keys of the device's last output, and the write limits of its
    // target.
    bool output_option(std::string_view arg, CompiledDevice& device) {
        CompiledOutput& last = device.outputs.back();
        CompiledPoint& target = device.points.at(last.point);
        std::int64_t number = 0;
        if (arg == "--sbo") {
            last.model = ControlModel::SBO;
            return true;
        }
        if (arg == "--on-only") {
            last.on_only = true;
            return true;
        }
        if (arg == "--min" || arg == "--max") {
            if (!take(number, MAX_SUBSTITUTE)) {
                return false;
            }
            (arg == "--min" ? last.limits.min : last.limits.max) =
                static_cast<double>(number);
            return true;
        }
        if (arg == "--local-panel") {
            last.local_blocks_auto = true;
            last.panel_input = next();
            return !last.panel_input.empty();
        }
        if (arg == "--write-min-interval-ms") {
            return millis(target.write_limits.min_interval_ns);
        }
        if (arg == "--write-max-per-day") {
            return take(target.write_limits.max_per_day, MAX_WRITES_PER_DAY);
        }
        return false;
    }

    // `--identify-equals POINT VALUE` and `--restart-decreases POINT`, on a
    // point the device already has.
    bool identity_option(std::string_view arg, CompiledDevice& device) {
        std::uint32_t point = 0;
        if (!take(point, MAX_REGISTER) || point >= device.points.size()) {
            return false;
        }
        if (arg == "--restart-decreases") {
            device.restart_indication.push_back(RestartRule{
                .point = point, .kind = RestartRule::Kind::DECREASES});
            return true;
        }
        std::int64_t value = 0;
        if (!take(value, MAX_SUBSTITUTE)) {
            return false;
        }
        IdentityCheck check;
        check.point = point;
        check.kind = IdentityCheck::Kind::EQUALS;
        check.values = {ExpectedValue{.type = tag::TagType::INT64, .i = value}};
        device.identify.push_back(std::move(check));
        return true;
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

namespace {

class RefusingSink final : public LogSink {
public:
    RefusingSink(std::unique_ptr<LogSink> inner, std::int64_t delay_ms)
        : inner_(std::move(inner)), delay_(delay_ms) {}
    [[nodiscard]] Status write(const LogRecord& record) noexcept override {
        (void)inner_->write(record);
        // The journal's slowness, not a wait for something.
        std::this_thread::sleep_for(delay_);
        return {StatusCode::UNAVAILABLE, "refused by --refuse-log"};
    }

private:
    std::unique_ptr<LogSink> inner_;
    std::chrono::milliseconds delay_;
};

}  // namespace

std::unique_ptr<LogSink> refusing_sink(std::unique_ptr<LogSink> inner,
                                       std::int64_t delay_ms) {
    return std::make_unique<RefusingSink>(std::move(inner), delay_ms);
}

}  // namespace scada::driver::test
