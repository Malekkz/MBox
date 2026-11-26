#include "worker.h"

#include "scada/common/fd.h"
#include "scada/common/status.h"
#include "scada/driver/channel_engine.h"
#include "scada/driver/compiled.h"
#include "scada/driver/protocol.h"
#include "scada/driver/transport.h"
#include "scada/tag/scada_tag.h"
#include "tag_writer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

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

}  // namespace

Worker::Worker(const CompiledChannel& channel, const FrameProtocol& protocol,
               std::unique_ptr<Transport> transport, Clock& clock,
               scada_ctx* ctx, PointHandles handles,
               std::int64_t stall_limit_ns) noexcept
    : channel_(&channel),
      protocol_(&protocol),
      transport_(std::move(transport)),
      clock_(&clock),
      writer_(ctx, std::move(handles)),
      stall_limit_ns_(stall_limit_ns) {}

Worker::~Worker() {
    request_stop();
    join();
}

Status Worker::start() noexcept {
    stop_fd_ = Fd(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (!stop_fd_.valid()) {
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
    (void)::pthread_attr_destroy(&attributes);
    if (created != 0) {
        return {StatusCode::UNAVAILABLE, "cannot start the worker thread"};
    }
    started_ = true;
    return {};
}

void Worker::request_stop() noexcept {
    stop_.store(true, std::memory_order_release);
    if (stop_fd_.valid()) {
        const std::uint64_t one = 1;
        // A full counter already wakes the thread; nothing else can fail.
        (void)::write(stop_fd_.get(), &one, sizeof(one));
    }
}

void Worker::join() noexcept {
    if (started_) {
        (void)::pthread_join(thread_, nullptr);
        started_ = false;
    }
}

void Worker::wait_until(std::int64_t next_ns) noexcept {
    const std::int64_t now = clock_->monotonic_ns();
    // Every wait at most a third of the stall limit (2.6), so that an idle
    // channel moves its counter too.
    const std::int64_t cap = now + (stall_limit_ns_ / 3);
    const std::int64_t until = std::min(next_ns == NEVER ? cap : next_ns, cap);
    if (until <= now) {
        return;
    }
    std::array<pollfd, 2> entries{
        pollfd{.fd = stop_fd_.get(), .events = POLLIN, .revents = 0},
        pollfd{.fd = -1, .events = 0, .revents = 0}};
    // On a stream only, POLLRDHUP alone: never POLLIN (transport.h).
    if (channel_->transport != TransportKind::SERIAL) {
        entries[1] = pollfd{
            .fd = transport_->poll_fd(), .events = POLLRDHUP, .revents = 0};
    }
    const timespec left = to_timespec(until - now);
    // An interrupted or failed wait is a wake like any other: the next
    // step decides.
    (void)::ppoll(entries.data(), entries.size(), &left, nullptr);
}

void Worker::run() noexcept {
    // Built on this thread: every tag write, the first included, is this
    // thread's (TD 13.2: the first thread that writes an owner's tags owns
    // them), and so is the engine's allocation.
    std::optional<ChannelEngine> engine;
    engine.emplace(*channel_, *protocol_, *transport_, *clock_, writer_);
    if (!engine->status().ok()) {
        failure_ = engine->status();
        phase_.store(Phase::FAILED, std::memory_order_release);
        return;
    }
    // 9.1: every tag of the owner before the first request.
    writer_.write_not_initialised();
    phase_.store(Phase::RUNNING, std::memory_order_release);
    while (!stop_.load(std::memory_order_acquire)) {
        const std::int64_t next = engine->step();
        progress_.fetch_add(1, std::memory_order_relaxed);
        writer_.notify();
        if (stop_.load(std::memory_order_acquire)) {
            break;
        }
        wait_until(next);
        progress_.fetch_add(1, std::memory_order_relaxed);
    }
    // Stopped at a boundary: nothing is in flight. An orderly close.
    transport_->close(false);
    phase_.store(Phase::DONE, std::memory_order_release);
}

}  // namespace scada::driver::detail
