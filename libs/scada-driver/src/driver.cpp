#include "scada/driver/driver.h"

#include "scada/common/clock.h"
#include "scada/common/fd.h"
#include "scada/common/log.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/driver/compiled.h"
#include "scada/driver/protocol.h"
#include "scada/driver/timing.h"
#include "scada/driver/transport.h"
#include "scada/service/notify.h"
#include "scada/tag/scada_tag.h"
#include "tag_writer.h"
#include "worker.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <poll.h>
#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal functions are declared here, not in <csignal>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <unistd.h>

namespace scada::driver {
namespace {

using detail::PointHandles;
using detail::Worker;

constexpr std::int64_t NS_PER_S = 1'000'000'000;
constexpr std::int64_t MAX_NS = std::numeric_limits<std::int64_t>::max();
// While tags are resolved, a heartbeat after this many (as scada-sim).
constexpr std::size_t HEARTBEAT_EVERY_TAGS = 256;
// How long the workers may take to build their engines and write the tags
// of 9.1 before the run fails.
constexpr std::int64_t START_LIMIT_NS = 10 * NS_PER_S;
// The beyond of 2.6: the other workers stop within their request deadline
// plus this.
constexpr std::int64_t STOP_GRACE_NS = NS_PER_S;
constexpr std::chrono::milliseconds LOG_FLUSH_LIMIT{1000};

std::int64_t add(std::int64_t left, std::int64_t right) noexcept {
    return left > MAX_NS - right ? MAX_NS : left + right;
}

timespec to_timespec(std::int64_t duration_ns) noexcept {
    const std::int64_t clamped = std::max<std::int64_t>(duration_ns, 0);
    return timespec{.tv_sec = static_cast<time_t>(clamped / NS_PER_S),
                    .tv_nsec = static_cast<long>(clamped % NS_PER_S)};
}

}  // namespace

std::int64_t stall_limit_ns(const CompiledChannel& channel,
                            const FrameProtocol& protocol) noexcept {
    std::int64_t longest = channel.connection.connect_timeout_ns;
    for (const CompiledDevice& device : channel.devices) {
        if (!device.enabled) {
            continue;
        }
        const std::size_t frame = protocol.device_caps(device).max_frame_bytes;
        const std::int64_t wait = std::max(device.failure.timeout_ns,
                                           device.failure.write_timeout_ns);
        const std::int64_t first =
            first_byte_deadline_ns(channel.line, 0, frame, wait);
        longest =
            std::max(longest, answer_deadline_ns(channel.line, first, frame));
    }
    const std::int64_t limit = longest > MAX_NS / 3 ? MAX_NS : longest * 3;
    return std::max(limit, MIN_STALL_LIMIT_NS);
}

Status block_all_signals() noexcept {
    sigset_t all{};
    if (::sigfillset(&all) != 0 ||
        ::pthread_sigmask(SIG_BLOCK, &all, nullptr) != 0) {
        return {StatusCode::INTERNAL, "cannot block the signals"};
    }
    return {};
}

std::string point_tag_name(const CompiledDevice& device,
                           const CompiledPoint& point) {
    return device.name + "." + point.name;
}

class Driver::Impl {
public:
    Impl(const CompiledConfig& config, const FrameProtocol& protocol,
         TransportFactory transports, Clock& clock, DriverSettings settings,
         Logger* logger)
        : config_(&config),
          protocol_(&protocol),
          transports_(std::move(transports)),
          clock_(&clock),
          settings_(std::move(settings)),
          logger_(logger) {}

    ~Impl() {
        for (const std::unique_ptr<Worker>& worker : workers_) {
            worker->request_stop();
        }
        workers_.clear();  // joins
        if (ctx_ != nullptr) {
            scada_tag_detach(ctx_);
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    RunEnd run(Status& failure) noexcept;
    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

private:
    void log(LogLevel level, std::string_view message,
             std::initializer_list<LogField> fields = {}) noexcept {
        if (logger_ != nullptr) {
            logger_->log(level, message, fields);
        }
    }
    // Signals, the service manager, attach, the workers: everything before
    // the loop.
    [[nodiscard]] Status start() noexcept;
    void begin_stop() noexcept;
    [[nodiscard]] Status take_signals() noexcept;
    [[nodiscard]] Status attach() noexcept;
    [[nodiscard]] Status make_workers() noexcept;
    [[nodiscard]] Status resolve(const CompiledChannel& channel,
                                 PointHandles& out) noexcept;
    [[nodiscard]] Status await_started() noexcept;
    // One heartbeat and watchdog notification; false when the generation
    // retires (the generation change is slice 8).
    bool beat() noexcept;
    // The worker that has stalled, if any (2.6).
    [[nodiscard]] Worker* stalled(std::int64_t now) noexcept;
    RunEnd after_stall(Worker& stalled) noexcept;
    // Waits for a signal up to `until`; true when a stop signal came.
    bool signalled(std::int64_t until) noexcept;
    void stop_all() noexcept;
    [[nodiscard]] bool all_done() const noexcept;

    const CompiledConfig* config_;
    const FrameProtocol* protocol_;
    TransportFactory transports_;
    Clock* clock_;
    DriverSettings settings_;
    Logger* logger_;
    std::optional<service::Notifier> notifier_;
    std::optional<std::chrono::microseconds> watchdog_interval_;
    std::int64_t last_watchdog_ns_ = 0;
    Fd signals_;
    scada_ctx* ctx_ = nullptr;
    std::vector<std::unique_ptr<Worker>> workers_;
    // Per worker: the progress last seen and when it last moved.
    std::vector<std::uint64_t> seen_;
    std::vector<std::int64_t> moved_ns_;
    std::string detail_;  // what failed, with names, for the log
};

Status Driver::Impl::take_signals() noexcept {
    // Every thread blocks every signal (block_all_signals, again here in
    // case `main` did not); this thread takes the stop signals
    // synchronously from a signalfd, so a stop never interrupts a worker's
    // wait (DI 2.4).
    Status blocked = block_all_signals();
    if (!blocked.ok()) {
        return blocked;
    }
    sigset_t stops{};
    if (::sigemptyset(&stops) != 0 || ::sigaddset(&stops, SIGTERM) != 0 ||
        ::sigaddset(&stops, SIGINT) != 0 || ::sigaddset(&stops, SIGHUP) != 0) {
        return {StatusCode::INTERNAL, "cannot build the signal set"};
    }
    signals_ = Fd(::signalfd(-1, &stops, SFD_CLOEXEC | SFD_NONBLOCK));
    if (!signals_.valid()) {
        return {StatusCode::UNAVAILABLE, "no signalfd"};
    }
    return {};
}

Status Driver::Impl::attach() noexcept {
    const scada_attach_opts options{
        .base_dir =
            settings_.base_dir.empty() ? nullptr : settings_.base_dir.c_str(),
        .retain_dir = nullptr,
        .tagd_uid = settings_.tagd_uid};
    const int code =
        scada_tag_attach_opts(settings_.process.c_str(), &options, &ctx_);
    if (code != SCADA_OK) {
        ctx_ = nullptr;
        detail_ = "attach as " + settings_.process + " failed with code " +
                  std::to_string(code);
        return {StatusCode::UNAVAILABLE, "attach failed"};
    }
    return {};
}

Status Driver::Impl::resolve(const CompiledChannel& channel,
                             PointHandles& out) noexcept {
    // Every device, disabled ones too: 9.1 writes every tag of the owner.
    std::size_t count = 0;
    for (const CompiledDevice& device : channel.devices) {
        out.first.push_back(out.handles.size());
        for (const CompiledPoint& point : device.points) {
            if (++count % HEARTBEAT_EVERY_TAGS == 0) {
                (void)scada_tag_heartbeat(ctx_);
            }
            if (!point.rules.has_tag) {
                out.handles.push_back(SCADA_INVALID_HANDLE);
                continue;
            }
            const std::string name = point_tag_name(device, point);
            std::uint32_t handle = SCADA_INVALID_HANDLE;
            scada_tag_info_t info{};
            if (scada_tag_resolve(ctx_, name.c_str(), &handle) != SCADA_OK ||
                scada_tag_info(ctx_, handle, &info) != SCADA_OK) {
                detail_ = "the tag set has no tag " + name;
                return {StatusCode::NOT_FOUND, "a point has no tag"};
            }
            if (info.owned == 0 ||
                info.type != static_cast<std::uint8_t>(point.rules.tag_type)) {
                detail_ = "tag " + name +
                          " is not this process's or not of the point's type";
                return {StatusCode::FAILED_PRECONDITION,
                        "a point's tag does not match"};
            }
            out.handles.push_back(handle);
        }
    }
    return {};
}

Status Driver::Impl::make_workers() noexcept {
    for (const CompiledChannel& channel : config_->channels) {
        if (!channel.enabled) {
            continue;  // not connected; its tags stay as the library has
                       // them (CS 4.6)
        }
        PointHandles handles;
        Status resolved = resolve(channel, handles);
        if (!resolved.ok()) {
            return resolved;
        }
        std::unique_ptr<Transport> transport = transports_(channel, *clock_);
        if (transport == nullptr) {
            detail_ = "no transport for channel " + channel.name;
            return {StatusCode::INVALID_ARGUMENT, "no transport"};
        }
        workers_.push_back(std::make_unique<Worker>(
            channel, *protocol_, std::move(transport), *clock_, ctx_,
            std::move(handles), stall_limit_ns(channel, *protocol_)));
    }
    seen_.assign(workers_.size(), 0);
    moved_ns_.assign(workers_.size(), scada::monotonic_ns());
    for (const std::unique_ptr<Worker>& worker : workers_) {
        Status started = worker->start();
        if (!started.ok()) {
            return started;
        }
    }
    return {};
}

Status Driver::Impl::await_started() noexcept {
    const std::int64_t limit = add(scada::monotonic_ns(), START_LIMIT_NS);
    for (;;) {
        bool waiting = false;
        for (const std::unique_ptr<Worker>& worker : workers_) {
            if (worker->phase() == Worker::Phase::FAILED) {
                detail_ = "channel " + worker->channel().name + ": " +
                          std::string(worker->failure().message());
                return worker->failure();
            }
            waiting = waiting || worker->phase() == Worker::Phase::STARTING;
        }
        if (!waiting) {
            return {};
        }
        if (!beat() || scada::monotonic_ns() > limit) {
            detail_ = "the workers did not start";
            return {StatusCode::TIMEOUT, "the workers did not start"};
        }
        const timespec pause = to_timespec(settings_.heartbeat_period_ns);
        (void)::ppoll(nullptr, 0, &pause, nullptr);  // a bounded beat wait
    }
}

bool Driver::Impl::beat() noexcept {
    const bool retiring = scada_tag_heartbeat(ctx_) == SCADA_E_RETIRING;
    if (notifier_.has_value() && watchdog_interval_.has_value()) {
        // At half the interval, the usual period (notify.h).
        const std::int64_t now = scada::monotonic_ns();
        const std::int64_t half =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                *watchdog_interval_)
                .count() /
            2;
        if (now - last_watchdog_ns_ >= half) {
            (void)notifier_->watchdog();
            last_watchdog_ns_ = now;
        }
    }
    return !retiring;
}

Worker* Driver::Impl::stalled(std::int64_t now) noexcept {
    for (std::size_t index = 0; index < workers_.size(); ++index) {
        Worker& worker = *workers_[index];
        if (worker.phase() == Worker::Phase::DONE) {
            continue;
        }
        const std::uint64_t progress = worker.progress();
        if (progress != seen_[index]) {
            seen_[index] = progress;
            moved_ns_[index] = now;
        } else if (now - moved_ns_[index] >= worker.stall_limit_ns()) {
            return &worker;
        }
    }
    return nullptr;
}

RunEnd Driver::Impl::after_stall(Worker& stalled) noexcept {
    log(LogLevel::ERROR, "channel stalled",
        {{"MESSAGE_ID", "driver-stall"}, {"CHANNEL", stalled.channel().name}});
    if (logger_ != nullptr) {
        (void)logger_->flush(LOG_FLUSH_LIMIT);
    }
    // No heartbeat and no watchdog notification from here: the tags read
    // OWNER_DOWN, and the service manager restarts the process (2.6).
    std::int64_t grace = 0;
    for (const std::unique_ptr<Worker>& worker : workers_) {
        if (worker.get() != &stalled) {
            worker->request_stop();
            grace = std::max(grace, worker->stall_limit_ns() / 3);
        }
    }
    const std::int64_t until =
        add(scada::monotonic_ns(), add(grace, STOP_GRACE_NS));
    for (;;) {
        bool waiting = false;
        for (const std::unique_ptr<Worker>& worker : workers_) {
            waiting = waiting || (worker.get() != &stalled &&
                                  worker->phase() != Worker::Phase::DONE);
        }
        const std::int64_t now = scada::monotonic_ns();
        if (!waiting || now >= until) {
            return RunEnd::STALLED;
        }
        const timespec pause =
            to_timespec(std::min(until - now, settings_.heartbeat_period_ns));
        (void)::ppoll(nullptr, 0, &pause, nullptr);
    }
}

bool Driver::Impl::signalled(std::int64_t until) noexcept {
    pollfd entry{.fd = signals_.get(), .events = POLLIN, .revents = 0};
    const timespec left = to_timespec(until - scada::monotonic_ns());
    if (::ppoll(&entry, 1, &left, nullptr) <= 0) {
        return false;
    }
    signalfd_siginfo info{};
    bool stop = false;
    while (::read(signals_.get(), &info, sizeof(info)) ==
           static_cast<ssize_t>(sizeof(info))) {
        // SIGHUP is the reload of 12, which comes with slice 8: taken and
        // ignored, so that it never stops the process.
        if (info.ssi_signo == SIGHUP) {
            log(LogLevel::INFO, "reload signal ignored",
                {{"MESSAGE_ID", "driver-reload-ignored"}});
        } else {
            stop = true;
        }
    }
    return stop;
}

void Driver::Impl::stop_all() noexcept {
    for (const std::unique_ptr<Worker>& worker : workers_) {
        worker->request_stop();
    }
}

bool Driver::Impl::all_done() const noexcept {
    return std::all_of(workers_.begin(), workers_.end(),
                       [](const auto& worker) {
                           return worker->phase() == Worker::Phase::DONE;
                       });
}

Status Driver::Impl::start() noexcept {
    Status status = take_signals();
    if (!status.ok()) {
        return status;
    }
    Result<service::NotifySettings> notify = service::parse_notify_environment(
        service::read_notify_environment(), ::getpid());
    if (notify.ok()) {
        watchdog_interval_ = notify.value().watchdog_interval;
        notifier_.emplace(notify.value());
    }
    status = attach();
    if (status.ok()) {
        status = make_workers();
    }
    if (status.ok()) {
        status = await_started();
    }
    return status;
}

void Driver::Impl::begin_stop() noexcept {
    // The stop of 10: the workers stop at their next boundary and
    // disconnect; the heartbeat goes on until they have.
    log(LogLevel::INFO, "driver stopping", {{"MESSAGE_ID", "driver-stopping"}});
    if (notifier_.has_value()) {
        (void)notifier_->stopping();
    }
    stop_all();
}

RunEnd Driver::Impl::run(Status& failure) noexcept {
    failure = start();
    if (!failure.ok()) {
        if (detail_.empty()) {
            detail_ = std::string(failure.message());
        }
        log(LogLevel::ERROR, "driver start failed",
            {{"MESSAGE_ID", "driver-start-failed"}, {"REASON", detail_}});
        return RunEnd::FAILED;
    }
    log(LogLevel::INFO, "driver running",
        {{"MESSAGE_ID", "driver-running"},
         {"CHANNELS", std::to_string(workers_.size())}});
    if (notifier_.has_value()) {
        (void)notifier_->ready();
    }
    bool stopping = false;
    std::int64_t next_beat = scada::monotonic_ns();
    for (;;) {
        if (signalled(next_beat) && !stopping) {
            stopping = true;
            begin_stop();
        }
        const std::int64_t now = scada::monotonic_ns();
        if (now < next_beat) {
            continue;
        }
        next_beat = add(now, settings_.heartbeat_period_ns);
        if (Worker* worker = stalled(now); worker != nullptr) {
            return after_stall(*worker);
        }
        if (stopping && all_done()) {
            for (const std::unique_ptr<Worker>& done : workers_) {
                done->join();
            }
            return RunEnd::STOPPED;
        }
        if (!beat()) {
            failure = {StatusCode::UNAVAILABLE, "the generation is retiring"};
            detail_ =
                "the generation is retiring; the generation change "
                "comes with slice 8";
            log(LogLevel::ERROR, "generation retiring",
                {{"MESSAGE_ID", "driver-retiring"}});
            stop_all();
            return RunEnd::FAILED;
        }
    }
}

Driver::Driver(const CompiledConfig& config, const FrameProtocol& protocol,
               TransportFactory transports, Clock& clock,
               DriverSettings settings, Logger* logger)
    : impl_(std::make_unique<Impl>(config, protocol, std::move(transports),
                                   clock, std::move(settings), logger)) {}

Driver::~Driver() = default;

RunEnd Driver::run() noexcept {
    return impl_->run(failure_);
}

const std::string& Driver::failure_detail() const noexcept {
    return impl_->detail();
}

}  // namespace scada::driver
