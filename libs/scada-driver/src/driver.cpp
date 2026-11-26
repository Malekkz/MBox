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
#include <map>
#include <memory>
#include <optional>
#include <span>
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
constexpr std::int64_t NS_PER_MS = 1'000'000;
constexpr std::int64_t MAX_NS = std::numeric_limits<std::int64_t>::max();
// While tags are resolved, a heartbeat after this many (as scada-sim).
constexpr std::size_t HEARTBEAT_EVERY_TAGS = 256;
// How long the workers may take to write the tags of 9.1 and build their
// engines before the run fails.
constexpr std::int64_t START_LIMIT_NS = 10 * NS_PER_S;
// After a stall, the other workers stop within their request deadline
// plus this (2.6).
constexpr std::int64_t STOP_GRACE_NS = NS_PER_S;
// A change of a channel's refused tag writes is logged at the heartbeat
// step, at most this often per channel (review M9; 5b-3 replaces it).
constexpr std::int64_t REFUSED_LOG_PERIOD_NS = NS_PER_S;
// A check of the workers that comes later than this many heartbeat
// periods after the one before means the whole process was not running
// (stopped, frozen, a paused machine): the workers are not judged by that
// time (review M7).
constexpr std::int64_t PAUSE_PERIODS = 2;
constexpr std::chrono::milliseconds LOG_FLUSH_LIMIT{1000};

std::int64_t add(std::int64_t left, std::int64_t right) noexcept {
    return left > MAX_NS - right ? MAX_NS : left + right;
}

timespec to_timespec(std::int64_t duration_ns) noexcept {
    const std::int64_t clamped = std::max<std::int64_t>(duration_ns, 0);
    return timespec{.tv_sec = static_cast<time_t>(clamped / NS_PER_S),
                    .tv_nsec = static_cast<long>(clamped % NS_PER_S)};
}

// A bounded pause of the main thread.
void pause_for(std::int64_t duration_ns) noexcept {
    const timespec pause = to_timespec(duration_ns);
    // An interrupted pause only ends early: every caller loops on a time.
    (void)::ppoll(nullptr, 0, &pause, nullptr);
}

bool stop_signals(sigset_t& out) noexcept {
    return ::sigemptyset(&out) == 0 && ::sigaddset(&out, SIGTERM) == 0 &&
           ::sigaddset(&out, SIGINT) == 0 && ::sigaddset(&out, SIGHUP) == 0;
}

Status validate(const DriverSettings& settings) noexcept {
    if (settings.process.empty()) {
        return {StatusCode::INVALID_ARGUMENT, "no process name"};
    }
    if (settings.heartbeat_period_ns <= 0 ||
        settings.heartbeat_period_ns > MAX_HEARTBEAT_PERIOD_NS) {
        return {StatusCode::INVALID_ARGUMENT,
                "the heartbeat period is out of range"};
    }
    return {};
}

// A NUL-terminated name of the tag library's fixed-size fields, as text;
// never past the field, NUL or not.
std::string text_of(std::span<const char> field) {
    return {field.begin(), std::find(field.begin(), field.end(), '\0')};
}

std::vector<DeviceCaps> caps_of(const CompiledChannel& channel,
                                const FrameProtocol& protocol) {
    std::vector<DeviceCaps> caps;
    caps.reserve(channel.devices.size());
    for (const CompiledDevice& device : channel.devices) {
        caps.push_back(protocol.device_caps(device));
    }
    return caps;
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

Status block_stop_signals() noexcept {
    sigset_t stops{};
    if (!stop_signals(stops) ||
        ::pthread_sigmask(SIG_BLOCK, &stops, nullptr) != 0) {
        return {StatusCode::INTERNAL, "cannot block the stop signals"};
    }
    return {};
}

int exit_status(RunEnd end) noexcept {
    switch (end) {
        case RunEnd::STOPPED:
            return EXIT_OK;
        case RunEnd::STALLED:
            return EXIT_STALL;
        case RunEnd::FAILED:
            break;
    }
    return EXIT_ERROR;
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
          settings_status_(validate(settings_)),
          logger_(logger) {}

    ~Impl() {
        for (const std::unique_ptr<Worker>& worker : workers_) {
            worker->request_stop();
        }
        if (!all_joined()) {
            // A worker still runs: it is never freed and the context never
            // detached under it. `main` ends the process at once (driver.h),
            // so nothing is leaked for long.
            for (std::unique_ptr<Worker>& worker : workers_) {
                if (!worker->returned()) {
                    [[maybe_unused]] const Worker* left = worker.release();
                }
            }
            return;
        }
        workers_.clear();
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
    [[nodiscard]] bool joined() const noexcept {
        return std::all_of(workers_.begin(), workers_.end(),
                           [](const auto& worker) {
                               return !worker || worker->returned();
                           }) &&
               joined_;
    }

private:
    // What the heartbeat said.
    enum class Beat : std::uint8_t { OK = 1, RETIRING, REFUSED };

    void log(LogLevel level, std::string_view message,
             std::initializer_list<LogField> fields = {}) noexcept {
        if (logger_ != nullptr) {
            logger_->log(level, message, fields);
        }
    }
    [[nodiscard]] Status fail_start(Status status, std::string detail) {
        detail_ = std::move(detail);
        return status;
    }
    // Signals, the service manager, attach, the workers: everything before
    // the loop.
    [[nodiscard]] Status start() noexcept;
    [[nodiscard]] Status take_signals() noexcept;
    [[nodiscard]] Status take_notify() noexcept;
    [[nodiscard]] Status attach() noexcept;
    // The owner of the channel's tags, from its first tag (none without a
    // tag), checked to be this process's.
    [[nodiscard]] Status owner_of(const CompiledChannel& channel,
                                  std::optional<std::uint16_t>& owner) noexcept;
    [[nodiscard]] Status make_workers() noexcept;
    [[nodiscard]] Status resolve(const CompiledChannel& channel,
                                 std::optional<std::uint16_t> owner,
                                 PointHandles& out) noexcept;
    [[nodiscard]] Status begin_workers() noexcept;
    [[nodiscard]] Status await_running() noexcept;
    // One heartbeat and watchdog notification.
    Beat beat() noexcept;
    void feed_watchdog() noexcept;
    // The worker that has stalled, if any (2.6).
    [[nodiscard]] Worker* stalled(std::int64_t now) noexcept;
    RunEnd after_stall(Worker& stalled) noexcept;
    // The bounded stop of every end but a stall (DI 10, review M1).
    RunEnd finish(RunEnd end) noexcept;
    // Waits for a signal up to `until`; true when a stop signal came.
    bool signalled(std::int64_t until) noexcept;
    void stop_all() noexcept;
    bool all_joined() noexcept;
    void log_refused(std::int64_t now) noexcept;

    const CompiledConfig* config_;
    const FrameProtocol* protocol_;
    TransportFactory transports_;
    Clock* clock_;
    DriverSettings settings_;
    Status settings_status_;
    Logger* logger_;
    bool ran_ = false;
    bool joined_ = true;
    std::optional<service::Notifier> notifier_;
    std::optional<std::chrono::microseconds> watchdog_interval_;
    std::int64_t last_watchdog_ns_ = 0;
    bool feeding_ = true;
    bool heartbeat_logged_ = false;
    Fd signals_;
    scada_ctx* ctx_ = nullptr;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::vector<std::optional<std::uint16_t>> owners_;  // per worker
    // Per worker: the progress last seen and when it last moved.
    std::vector<std::uint64_t> seen_;
    std::vector<std::int64_t> moved_ns_;
    std::int64_t last_check_ns_ = 0;
    // Per worker: the refused writes last logged.
    std::vector<std::uint64_t> logged_refused_;
    std::int64_t next_refused_log_ns_ = 0;
    std::string detail_;  // what failed, with names, for the log
};

Status Driver::Impl::take_signals() noexcept {
    // The stop signals are blocked on every thread (block_stop_signals, here
    // again in case `main` did not); this thread takes them synchronously
    // from a signalfd, so a stop never interrupts a worker's wait (DI 2.4).
    Status blocked = block_stop_signals();
    if (!blocked.ok()) {
        return blocked;
    }
    sigset_t stops{};
    if (!stop_signals(stops)) {
        return {StatusCode::INTERNAL, "cannot build the signal set"};
    }
    signals_ = Fd(::signalfd(-1, &stops, SFD_CLOEXEC | SFD_NONBLOCK));
    if (!signals_.valid()) {
        return {StatusCode::UNAVAILABLE, "no signalfd"};
    }
    return {};
}

Status Driver::Impl::take_notify() noexcept {
    // As scada-tagd: an environment that names a service manager wrongly
    // fails the start, so that READY and the watchdog are never silently
    // lost (review M10).
    Result<service::NotifySettings> notify = service::parse_notify_environment(
        service::read_notify_environment(), ::getpid());
    if (!notify.ok()) {
        return fail_start(
            notify.status(),
            "invalid environment: " + std::string(notify.status().message()));
    }
    watchdog_interval_ = notify.value().watchdog_interval;
    notifier_.emplace(notify.value());
    return {};
}

Status Driver::Impl::attach() noexcept {
    const scada_attach_opts options{
        .base_dir =
            settings_.base_dir.empty() ? nullptr : settings_.base_dir.c_str(),
        .retain_dir = settings_.retain_dir.empty()
                          ? nullptr
                          : settings_.retain_dir.c_str(),
        .tagd_uid = settings_.tagd_uid};
    const int code =
        scada_tag_attach_opts(settings_.process.c_str(), &options, &ctx_);
    if (code != SCADA_OK) {
        ctx_ = nullptr;
        return fail_start({StatusCode::UNAVAILABLE, "attach failed"},
                          "attach as " + settings_.process +
                              " failed with code " + std::to_string(code));
    }
    return {};
}

Status Driver::Impl::owner_of(const CompiledChannel& channel,
                              std::optional<std::uint16_t>& owner) noexcept {
    owner.reset();
    for (const CompiledDevice& device : channel.devices) {
        for (const CompiledPoint& point : device.points) {
            if (!point.rules.has_tag) {
                continue;
            }
            const std::string name = point_tag_name(device, point);
            std::uint32_t handle = SCADA_INVALID_HANDLE;
            scada_tag_info_t info{};
            if (scada_tag_resolve(ctx_, name.c_str(), &handle) != SCADA_OK ||
                scada_tag_info(ctx_, handle, &info) != SCADA_OK) {
                return fail_start({StatusCode::NOT_FOUND, "a point has no tag"},
                                  "the tag set has no tag " + name);
            }
            if (info.owned == 0) {
                return fail_start({StatusCode::FAILED_PRECONDITION,
                                   "a point's tag is not this process's"},
                                  "tag " + name + " is owned by " +
                                      text_of(info.owner) +
                                      ", not a thread of " + settings_.process);
            }
            owner = info.owner_index;
            return {};
        }
    }
    return {};
}

Status Driver::Impl::make_workers() noexcept {
    // One worker per channel, disabled ones too: every tag has one writing
    // thread (TD 13.2), which writes 9.1 before anything else (review M2,
    // M6). Two channels on one owner would be two writers: refused before
    // any thread starts (review M3).
    std::map<std::uint16_t, const CompiledChannel*> claimed;
    for (const CompiledChannel& channel : config_->channels) {
        std::optional<std::uint16_t> owner;
        Status found = owner_of(channel, owner);
        if (!found.ok()) {
            return found;
        }
        if (owner.has_value()) {
            const auto [at, fresh] = claimed.emplace(*owner, &channel);
            if (!fresh) {
                scada_owner_info_t info{};
                (void)scada_owner_info(ctx_, *owner, &info);  // for the name
                return fail_start({StatusCode::FAILED_PRECONDITION,
                                   "two channels share an owner"},
                                  "channels " + at->second->name + " and " +
                                      channel.name + " both write owner " +
                                      text_of(info.name));
            }
        }
        std::unique_ptr<Transport> transport;
        if (channel.enabled) {
            // The factory is the caller's code: an exception from it ends
            // here, never crossing the library boundary.
            try {
                transport = transports_(channel, *clock_);
            } catch (...) {
                transport.reset();
            }
            if (transport == nullptr) {
                return fail_start(
                    {StatusCode::INVALID_ARGUMENT, "no transport"},
                    "no transport for channel " + channel.name);
            }
        }
        owners_.push_back(owner);
        workers_.push_back(std::make_unique<Worker>(
            channel, *protocol_, std::move(transport), *clock_, ctx_, owner,
            caps_of(channel, *protocol_), stall_limit_ns(channel, *protocol_)));
    }
    seen_.assign(workers_.size(), 0);
    moved_ns_.assign(workers_.size(), scada::monotonic_ns());
    logged_refused_.assign(workers_.size(), 0);
    for (const std::unique_ptr<Worker>& worker : workers_) {
        const Status started = worker->start();
        if (!started.ok()) {
            return fail_start(started, "channel " + worker->channel().name +
                                           ": " +
                                           std::string(started.message()));
        }
    }
    return {};
}

Status Driver::Impl::resolve(const CompiledChannel& channel,
                             std::optional<std::uint16_t> owner,
                             PointHandles& out) noexcept {
    // Every device, disabled ones too: their tags are the channel's.
    std::size_t count = 0;
    for (const CompiledDevice& device : channel.devices) {
        out.first.push_back(out.handles.size());
        for (const CompiledPoint& point : device.points) {
            if (++count % HEARTBEAT_EVERY_TAGS == 0) {
                (void)beat();  // a retiring generation is seen by the loop
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
                return fail_start({StatusCode::NOT_FOUND, "a point has no tag"},
                                  "the tag set has no tag " + name);
            }
            if (info.type != static_cast<std::uint8_t>(point.rules.tag_type)) {
                return fail_start(
                    {StatusCode::FAILED_PRECONDITION,
                     "a point's tag is not of its type"},
                    "tag " + name + " is not of the point's type");
            }
            // Every tag of a channel has the channel's owner (review M3).
            if (!owner.has_value() || info.owner_index != *owner) {
                return fail_start({StatusCode::FAILED_PRECONDITION,
                                   "a channel's tags have two owners"},
                                  "tag " + name + " of channel " +
                                      channel.name + " is owned by " +
                                      text_of(info.owner) +
                                      ", not by the channel's owner");
            }
            out.handles.push_back(handle);
        }
    }
    return {};
}

Status Driver::Impl::begin_workers() noexcept {
    for (std::size_t index = 0; index < workers_.size(); ++index) {
        Worker& worker = *workers_[index];
        PointHandles handles;
        Status resolved = resolve(worker.channel(), owners_[index], handles);
        if (!resolved.ok()) {
            return resolved;
        }
        worker.begin(std::move(handles));
    }
    return {};
}

Status Driver::Impl::await_running() noexcept {
    const std::int64_t limit = add(scada::monotonic_ns(), START_LIMIT_NS);
    for (;;) {
        bool waiting = false;
        for (const std::unique_ptr<Worker>& worker : workers_) {
            const Worker::Phase phase = worker->phase();
            if (phase == Worker::Phase::FAILED) {
                return fail_start(worker->failure(),
                                  "channel " + worker->channel().name + ": " +
                                      std::string(worker->failure().message()));
            }
            waiting = waiting || phase != Worker::Phase::RUNNING;
        }
        if (!waiting) {
            return {};
        }
        if (beat() != Beat::OK) {
            return fail_start({StatusCode::UNAVAILABLE,
                               "the heartbeat was refused at the start"},
                              "the heartbeat was refused at the start");
        }
        if (scada::monotonic_ns() > limit) {
            return fail_start(
                {StatusCode::TIMEOUT, "the workers did not start"},
                "the workers did not start within 10 s");
        }
        pause_for(settings_.heartbeat_period_ns);
    }
}

void Driver::Impl::feed_watchdog() noexcept {
    if (!notifier_.has_value() || !watchdog_interval_.has_value()) {
        return;
    }
    // At half the interval, the usual period (notify.h).
    const std::int64_t now = scada::monotonic_ns();
    const std::int64_t half =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            *watchdog_interval_)
            .count() /
        2;
    if (now - last_watchdog_ns_ < half) {
        return;
    }
    last_watchdog_ns_ = now;
    const bool fed = notifier_->watchdog().ok();
    if (!fed && feeding_) {
        // As scada-tagd: keep running; if feeding stays broken the service
        // manager restarts the process, which is its job. Logged once.
        log(LogLevel::ERROR, "cannot feed the watchdog",
            {{"MESSAGE_ID", "driver-watchdog-refused"}});
    }
    feeding_ = fed;
}

Driver::Impl::Beat Driver::Impl::beat() noexcept {
    const int code = scada_tag_heartbeat(ctx_);
    feed_watchdog();
    if (code == SCADA_OK) {
        return Beat::OK;
    }
    if (code == SCADA_E_RETIRING) {
        return Beat::RETIRING;
    }
    if (!heartbeat_logged_) {
        heartbeat_logged_ = true;
        log(LogLevel::ERROR, "heartbeat refused",
            {{"MESSAGE_ID", "driver-heartbeat-refused"},
             {"CODE", std::to_string(code)}});
    }
    return Beat::REFUSED;
}

Worker* Driver::Impl::stalled(std::int64_t now) noexcept {
    const std::int64_t late = settings_.heartbeat_period_ns * PAUSE_PERIODS;
    if (last_check_ns_ != 0 && now - last_check_ns_ > late) {
        // The main thread itself did not run for that time: the process
        // was paused. Judge the workers from now on, by time this thread
        // observes (review M7).
        log(LogLevel::NOTICE, "process paused",
            {{"MESSAGE_ID", "driver-paused"},
             {"PAUSE_MS", std::to_string((now - last_check_ns_) / NS_PER_MS)}});
        for (std::size_t index = 0; index < workers_.size(); ++index) {
            seen_[index] = workers_[index]->progress();
            moved_ns_[index] = now;
        }
        last_check_ns_ = now;
        return nullptr;
    }
    last_check_ns_ = now;
    for (std::size_t index = 0; index < workers_.size(); ++index) {
        Worker& worker = *workers_[index];
        if (worker.returned()) {
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
        // A flush that times out loses only records: the exit goes on.
        (void)logger_->flush(LOG_FLUSH_LIMIT);
    }
    joined_ = false;  // the stalled thread is never joined
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
        std::size_t stopped = 0;
        bool waiting = false;
        for (const std::unique_ptr<Worker>& worker : workers_) {
            if (worker.get() == &stalled) {
                continue;
            }
            if (worker->join_if_returned()) {
                ++stopped;
            } else {
                waiting = true;
            }
        }
        const std::int64_t now = scada::monotonic_ns();
        if (!waiting || now >= until) {
            log(LogLevel::INFO, "workers stopped after a stall",
                {{"MESSAGE_ID", "driver-stall-stopped"},
                 {"STOPPED", std::to_string(stopped)}});
            if (logger_ != nullptr) {
                // A flush that times out loses only records.
                (void)logger_->flush(LOG_FLUSH_LIMIT);
            }
            return RunEnd::STALLED;
        }
        pause_for(std::min(until - now, settings_.heartbeat_period_ns));
    }
}

RunEnd Driver::Impl::finish(RunEnd end) noexcept {
    stop_all();
    const std::int64_t until = add(scada::monotonic_ns(), STOP_BUDGET_NS);
    for (;;) {
        if (all_joined()) {
            return end;
        }
        const std::int64_t now = scada::monotonic_ns();
        // The stall check goes on while stopping: a worker stuck in its
        // stop ends the run as a stall would (review M1).
        Worker* stuck = stalled(now);
        if (stuck == nullptr && now >= until) {
            for (const std::unique_ptr<Worker>& worker : workers_) {
                if (!worker->returned()) {
                    stuck = worker.get();
                    break;
                }
            }
        }
        if (stuck != nullptr) {
            log(LogLevel::ERROR, "channel did not stop",
                {{"MESSAGE_ID", "driver-stop-overrun"},
                 {"CHANNEL", stuck->channel().name}});
            if (logger_ != nullptr) {
                // A flush that times out loses only records.
                (void)logger_->flush(LOG_FLUSH_LIMIT);
            }
            joined_ = false;
            return end == RunEnd::STOPPED ? RunEnd::STALLED : end;
        }
        if (ctx_ != nullptr) {
            // The heartbeat goes on until the workers are stopped; what it
            // says no longer changes the end.
            (void)beat();
        }
        pause_for(std::min(until - now, settings_.heartbeat_period_ns));
    }
}

bool Driver::Impl::signalled(std::int64_t until) noexcept {
    pollfd entry{.fd = signals_.get(), .events = POLLIN, .revents = 0};
    const timespec left = to_timespec(until - scada::monotonic_ns());
    if (::ppoll(&entry, 1, &left, nullptr) <= 0) {
        return false;  // the time came, or an interruption: the loop decides
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

bool Driver::Impl::all_joined() noexcept {
    bool all = true;
    for (const std::unique_ptr<Worker>& worker : workers_) {
        all = worker->join_if_returned() && all;
    }
    return all;
}

void Driver::Impl::log_refused(std::int64_t now) noexcept {
    if (now < next_refused_log_ns_) {
        return;
    }
    next_refused_log_ns_ = add(now, REFUSED_LOG_PERIOD_NS);
    for (std::size_t index = 0; index < workers_.size(); ++index) {
        const std::uint64_t refused = workers_[index]->refused_writes();
        if (refused != logged_refused_[index]) {
            log(LogLevel::WARNING, "tag writes refused",
                {{"MESSAGE_ID", "driver-writes-refused"},
                 {"CHANNEL", workers_[index]->channel().name},
                 {"COUNT", std::to_string(refused)}});
            logged_refused_[index] = refused;
        }
    }
}

Status Driver::Impl::start() noexcept {
    Status status = take_signals();
    if (status.ok()) {
        status = take_notify();
    }
    if (status.ok()) {
        status = attach();
    }
    if (status.ok()) {
        status = make_workers();
    }
    if (status.ok()) {
        status = begin_workers();
    }
    if (status.ok()) {
        status = await_running();
    }
    if (status.ok() && notifier_.has_value() && notifier_->enabled() &&
        !notifier_->ready().ok()) {
        status =
            fail_start({StatusCode::IO_ERROR, "the notification was not sent"},
                       "cannot report readiness to the service manager");
    }
    return status;
}

RunEnd Driver::Impl::run(Status& failure) noexcept {
    if (ran_) {
        failure = {StatusCode::FAILED_PRECONDITION, "the driver ran already"};
        detail_ = "run was called a second time";
        return RunEnd::FAILED;
    }
    ran_ = true;
    failure = settings_status_.ok() ? start() : settings_status_;
    if (!failure.ok()) {
        if (detail_.empty()) {
            detail_ = std::string(failure.message());
        }
        log(LogLevel::ERROR, "driver start failed",
            {{"MESSAGE_ID", "driver-start-failed"}, {"REASON", detail_}});
        return finish(RunEnd::FAILED);
    }
    log(LogLevel::INFO, "driver running",
        {{"MESSAGE_ID", "driver-running"},
         {"CHANNELS", std::to_string(workers_.size())}});
    std::int64_t next_beat = scada::monotonic_ns();
    for (;;) {
        if (signalled(next_beat)) {
            // The stop of 10: the workers stop at their next boundary and
            // disconnect; the heartbeat goes on until they have.
            log(LogLevel::INFO, "driver stopping",
                {{"MESSAGE_ID", "driver-stopping"}});
            // Best effort, as scada-tagd: the process stops either way, and
            // the service manager sees the exit.
            if (notifier_.has_value()) {
                (void)notifier_->stopping();
            }
            return finish(RunEnd::STOPPED);
        }
        const std::int64_t now = scada::monotonic_ns();
        if (now < next_beat) {
            continue;
        }
        next_beat = add(now, settings_.heartbeat_period_ns);
        if (Worker* worker = stalled(now); worker != nullptr) {
            return after_stall(*worker);
        }
        log_refused(now);
        const Beat said = beat();
        if (said == Beat::RETIRING) {
            failure = {StatusCode::UNAVAILABLE, "the generation is retiring"};
            detail_ =
                "the generation is retiring; the generation change "
                "comes with slice 8";
            log(LogLevel::ERROR, "generation retiring",
                {{"MESSAGE_ID", "driver-retiring"}});
            return finish(RunEnd::FAILED);
        }
        if (said == Beat::REFUSED) {
            failure = {StatusCode::UNAVAILABLE, "the heartbeat was refused"};
            detail_ = "the heartbeat was refused";
            return finish(RunEnd::FAILED);
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

bool Driver::joined() const noexcept {
    return impl_->joined();
}

const std::string& Driver::failure_detail() const noexcept {
    return impl_->detail();
}

}  // namespace scada::driver
