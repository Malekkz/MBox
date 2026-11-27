#include "worker.h"

#include "scada/common/fd.h"
#include "scada/common/status.h"
#include "scada/driver/channel_engine.h"
#include "scada/driver/compiled.h"
#include "scada/driver/protocol.h"
#include "scada/driver/transport.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/types.h"
#include "system_tags.h"
#include "tag_writer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace scada::driver::detail {
namespace {

constexpr std::int64_t NS_PER_S = 1'000'000'000;
constexpr std::int64_t NEVER = std::numeric_limits<std::int64_t>::max();

// A worker's stack (DI 2.4): 256 KiB, a build setting; the sanitizer
// builds give it more (CMake SCADA_DRIVER_WORKER_STACK_BYTES).
constexpr std::size_t WORKER_STACK_BYTES = SCADA_DRIVER_WORKER_STACK_BYTES;

timespec to_timespec(std::int64_t duration_ns) noexcept {
    const std::int64_t clamped = std::max<std::int64_t>(duration_ns, 0);
    return timespec{.tv_sec = static_cast<time_t>(clamped / NS_PER_S),
                    .tv_nsec = static_cast<long>(clamped % NS_PER_S)};
}

// Makes `descriptor`, an eventfd, readable. A full counter (EAGAIN) is
// readable already; nothing else can fail for an eventfd we own.
void raise(const Fd& descriptor) noexcept {
    if (!descriptor.valid()) {
        return;
    }
    const std::uint64_t one = 1;
    if (::write(descriptor.get(), &one, sizeof(one)) < 0) {
        return;  // EAGAIN: already readable; the flags are read anyway
    }
}

}  // namespace

Worker::Worker(const CompiledChannel& channel, const FrameProtocol& protocol,
               std::unique_ptr<Transport> transport, Clock& clock,
               scada_ctx* ctx, std::optional<std::uint16_t> owner,
               std::vector<DeviceCaps> caps,
               std::int64_t stall_limit_ns) noexcept
    : channel_(&channel),
      protocol_(&protocol),
      transport_(std::move(transport)),
      clock_(&clock),
      ctx_(ctx),
      owner_(owner),
      caps_(std::move(caps)),
      stall_limit_ns_(stall_limit_ns) {}

Worker::~Worker() {
    request_stop();
    // Never a join without a bound: a thread that has not returned is left
    // to the Driver, which then ends the process (driver.h).
    (void)join_if_returned();
}

Status Worker::start() noexcept {
    stop_fd_ = Fd(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    begin_fd_ = Fd(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (!stop_fd_.valid() || !begin_fd_.valid()) {
        return {StatusCode::UNAVAILABLE, "no eventfd for a worker"};
    }
    pthread_attr_t attributes{};
    if (::pthread_attr_init(&attributes) != 0) {
        return {StatusCode::UNAVAILABLE, "no thread attributes"};
    }
    const bool sized =
        ::pthread_attr_setstacksize(&attributes, WORKER_STACK_BYTES) == 0;
    const int created = sized ? ::pthread_create(
                                    &thread_, &attributes,
                                    [](void* self) -> void* {
                                        static_cast<Worker*>(self)->run();
                                        return nullptr;
                                    },
                                    this)
                              : -1;
    // Destroying attributes that were initialised cannot fail.
    (void)::pthread_attr_destroy(&attributes);
    if (created != 0) {
        return {StatusCode::UNAVAILABLE, "cannot start the worker thread"};
    }
    started_ = true;
    return {};
}

void Worker::begin(PointHandles handles, SystemTagHandles system) noexcept {
    handles_ = std::move(handles);
    system_ = std::move(system);
    begun_.store(true, std::memory_order_release);
    raise(begin_fd_);
}

void Worker::request_stop() noexcept {
    stop_.store(true, std::memory_order_release);
    raise(stop_fd_);
}

bool Worker::join_if_returned() noexcept {
    if (!started_) {
        return true;
    }
    if (!returned()) {
        return false;
    }
    // The thread has stored its last phase and returns at once: the join
    // is bounded. A thread joined once is not joined again.
    (void)::pthread_join(thread_, nullptr);
    started_ = false;
    return true;
}

void Worker::fail(Status status) noexcept {
    failure_ = status;
    phase_.store(Phase::FAILED, std::memory_order_release);
}

void Worker::wait_until(std::int64_t next_ns, int extra) noexcept {
    const std::int64_t now = clock_->monotonic_ns();
    // Every wait at most a third of the stall limit (2.6), so that an idle
    // channel moves its counter too.
    const std::int64_t cap = now + (stall_limit_ns_ / 3);
    const std::int64_t until = std::min(next_ns == NEVER ? cap : next_ns, cap);
    if (until <= now) {
        return;
    }
    std::array<pollfd, 3> entries{
        pollfd{.fd = stop_fd_.get(), .events = POLLIN, .revents = 0},
        pollfd{.fd = extra, .events = POLLIN, .revents = 0},
        pollfd{.fd = -1, .events = 0, .revents = 0}};
    // On a stream only, POLLRDHUP alone: never POLLIN (transport.h).
    if (transport_ != nullptr && channel_->transport != TransportKind::SERIAL) {
        entries[2] = pollfd{
            .fd = transport_->poll_fd(), .events = POLLRDHUP, .revents = 0};
    }
    const timespec left = to_timespec(until - now);
    // An interrupted or failed wait is a wake like any other: the next
    // step decides.
    (void)::ppoll(entries.data(), entries.size(), &left, nullptr);
}

bool Worker::await_begin() noexcept {
    for (;;) {
        if (stopping()) {
            return false;
        }
        if (begun_.load(std::memory_order_acquire)) {
            return true;
        }
        wait_until(NEVER, begin_fd_.get());
        moved();
    }
}

void Worker::serve(TagWriter& writer) noexcept {
    // Built on this thread: the engine's allocation is this thread's, and
    // so is every tag write (TD 13.2).
    std::optional<ChannelEngine> engine;
    engine.emplace(*channel_, *protocol_, *transport_, *clock_, writer,
                   std::span<const DeviceCaps>(caps_));
    if (!engine->status().ok()) {
        fail(engine->status());
        return;
    }
    writer.seed(*engine, *channel_);
    // 9.1: "Diagnostic tags are written with their current values", after
    // the walk of the owner wrote them NOT_INIT (D-146).
    writer.publish_system(*engine, *channel_, clock_->monotonic_ns(), true);
    writer.notify();
    // A stop gives up a read in flight and a connect (DI 10, R-14).
    transport_->set_wake(stop_fd_.get());
    phase_.store(Phase::RUNNING, std::memory_order_release);
    while (!stopping()) {
        const std::int64_t next = engine->step();
        moved();
        writer.publish_system(*engine, *channel_, clock_->monotonic_ns(),
                              false);
        writer.notify();
        refused_.store(writer.refused(), std::memory_order_relaxed);
        // After a stop the stop eventfd is readable: this wait returns at
        // once, and the loop ends at its check.
        wait_until(std::min(next, writer.system_due_ns()), -1);
        moved();
    }
    // Stopped at a boundary: nothing more is read. The close is orderly (a
    // FIN), but on a stream with unread bytes, an abandoned read's answer,
    // the kernel sends a reset instead.
    transport_->close(false);
    transport_->set_wake(-1);
    // The counters' last values, which the period may have held back: a
    // reader after the stop sees every request the channel sent (D-146).
    writer.flush_system(*engine, *channel_, clock_->monotonic_ns());
    // The channel's state says so (11.1), and its devices' as for a channel
    // not connected (3.5; review M3).
    writer.stopped_system(*channel_);
    writer.notify();
    refused_.store(writer.refused(), std::memory_order_relaxed);
}

void Worker::run() noexcept {
    // 9.1 first, by owner slot: before this worker waits for anything, so
    // that the tags of the previous life read GOOD for as short a time as
    // the thread's start allows (review M6).
    if (owner_.has_value()) {
        const Status written = write_owner_not_initialised(ctx_, *owner_);
        if (!written.ok()) {
            fail(written);
            return;
        }
    }
    phase_.store(Phase::WRITTEN, std::memory_order_release);
    if (!await_begin()) {
        phase_.store(Phase::DONE, std::memory_order_release);
        return;
    }
    std::size_t strings = 0;
    for (const CompiledDevice& device : channel_->devices) {
        strings += static_cast<std::size_t>(std::count_if(
            device.points.begin(), device.points.end(), [](const auto& point) {
                return point.rules.tag_type == tag::TagType::STRING;
            }));
    }
    TagWriter writer(ctx_, std::move(handles_), strings, std::move(system_));
    if (!channel_->enabled || transport_ == nullptr) {
        // A disabled channel: its tags are this thread's and written (9.1);
        // it is never connected (CS 4.6), says so in its system tags, and
        // waits for the stop.
        writer.publish_idle(*channel_);
        writer.notify();
        // Counted and logged as a running channel's (review M4).
        refused_.store(writer.refused(), std::memory_order_relaxed);
        phase_.store(Phase::RUNNING, std::memory_order_release);
        while (!stopping()) {
            wait_until(NEVER, -1);
            moved();
        }
        phase_.store(Phase::DONE, std::memory_order_release);
        return;
    }
    serve(writer);
    if (phase() != Phase::FAILED) {
        phase_.store(Phase::DONE, std::memory_order_release);
    }
}

}  // namespace scada::driver::detail
