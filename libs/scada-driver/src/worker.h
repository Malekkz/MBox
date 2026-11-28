#ifndef SCADA_DRIVER_SRC_WORKER_H
#define SCADA_DRIVER_SRC_WORKER_H

// The worker thread of one channel (driver.h; DI 2.4, 2.6, 9.1, 10; D-142).

#include "event_slot.h"
#include "scada/common/fd.h"
#include "scada/common/status.h"
#include "scada/driver/compiled.h"
#include "scada/driver/protocol.h"
#include "scada/driver/transport.h"
#include "system_tags.h"
#include "tag_writer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <pthread.h>

namespace scada::driver::detail {

class Worker {
public:
    // Where the thread is. Each phase is stored with release and read with
    // acquire, so that what the thread wrote before (the failure, the
    // writes of 9.1) is seen by the main thread that reads the phase.
    enum class Phase : std::uint8_t {
        STARTING = 1,  // writing the tags of its owner (9.1)
        WRITTEN,       // 9.1 done; waiting for its handles (`begin`)
        RUNNING,       // in the loop, or idle for a disabled channel
        FAILED,        // `failure()` says why; the thread has returned
        DONE,          // stopped and disconnected; the thread has returned
    };

    // Everything is borrowed but the transport, and must outlive the
    // worker. `transport` is null for a disabled channel, which is never
    // connected (CS 4.6). `owner` is the owner index of the channel's tags,
    // none when the channel has no tag. `caps` holds one entry per device of
    // the channel, asked on the main thread (DI 2.3).
    Worker(const CompiledChannel& channel, const FrameProtocol& protocol,
           std::unique_ptr<Transport> transport, Clock& clock, scada_ctx* ctx,
           std::optional<std::uint16_t> owner, std::vector<DeviceCaps> caps,
           std::int64_t stall_limit_ns) noexcept;
    // Asks the thread to stop and joins it only if it has returned: a
    // worker still running is never joined here, so a destructor never
    // waits without a bound (the Driver ends the process instead).
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    Worker(Worker&&) = delete;
    Worker& operator=(Worker&&) = delete;

    // Creates the stop and start eventfds and starts the thread, which
    // writes every tag of its owner at once (9.1). The calling thread must
    // have blocked the stop signals: the new thread inherits its mask
    // (DI 2.4). UNAVAILABLE if the thread or an eventfd cannot be had.
    [[nodiscard]] Status start() noexcept;

    // Hands the worker its point handles and its system tags (D-146) once
    // it is WRITTEN: it then builds its engine and runs, or, for a disabled
    // channel, writes its system tags and idles until the stop. Main thread
    // only, once.
    void begin(PointHandles handles, SystemTagHandles system) noexcept;

    // Asks the thread to stop at its next transaction boundary; a read in
    // flight and a connect are given up (DI 10). Safe from any thread, any
    // number of times.
    void request_stop() noexcept;

    // Moved at every transaction boundary and every wake of a wait (2.6).
    [[nodiscard]] std::uint64_t progress() const noexcept {
        // Relaxed: only whether it moved matters, never what it orders.
        return progress_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] Phase phase() const noexcept {
        return phase_.load(std::memory_order_acquire);
    }
    // True once the thread has returned (FAILED or DONE).
    [[nodiscard]] bool returned() const noexcept {
        const Phase now = phase();
        return now == Phase::FAILED || now == Phase::DONE;
    }
    // Valid once the phase is FAILED.
    [[nodiscard]] const Status& failure() const noexcept { return failure_; }
    [[nodiscard]] std::int64_t stall_limit_ns() const noexcept {
        return stall_limit_ns_;
    }
    [[nodiscard]] const CompiledChannel& channel() const noexcept {
        return *channel_;
    }
    // Tag writes and notifies the library refused while running (M9).
    [[nodiscard]] std::uint64_t refused_writes() const noexcept {
        // Relaxed: a count read for the log, ordering nothing.
        return refused_.load(std::memory_order_relaxed);
    }

    // The identity and restart events of device `device` since the last
    // take, for the log (D-147): false when none, or before `start`. Main
    // thread only.
    bool take_event(std::size_t device, DeviceEvent kind,
                    EventRecord& out) noexcept {
        return device < events_.size() && events_.at(device).take(kind, out);
    }

    // Joins the thread if it has returned; true when it is joined (or was
    // never started).
    bool join_if_returned() noexcept;

private:
    void run() noexcept;
    void fail(Status status) noexcept;
    // Waits for `begin` or the stop; true when begun.
    bool await_begin() noexcept;
    // Builds the engine and runs it until the stop.
    void serve(TagWriter& writer) noexcept;
    // Waits until `next_ns`, at most a third of the stall limit, for the
    // stop eventfd, `extra` (another eventfd, or -1) or, on a stream, for
    // the peer's close (transport.h).
    void wait_until(std::int64_t next_ns, int extra) noexcept;
    [[nodiscard]] bool stopping() const noexcept {
        return stop_.load(std::memory_order_acquire);
    }
    void moved() noexcept { progress_.fetch_add(1, std::memory_order_relaxed); }
    // Hands the engine's events to the event slots.
    void hand_events(ChannelEngine& engine) noexcept;

    const CompiledChannel* channel_;
    const FrameProtocol* protocol_;
    std::unique_ptr<Transport> transport_;
    Clock* clock_;
    scada_ctx* ctx_;
    std::optional<std::uint16_t> owner_;
    std::vector<DeviceCaps> caps_;
    std::int64_t stall_limit_ns_;
    Fd stop_fd_;
    Fd begin_fd_;
    // Written by `begin` before `begun_` is stored with release; read by
    // the worker after it loads `begun_` with acquire.
    PointHandles handles_;
    SystemTagHandles system_;
    // Release by `begin`, acquire by the worker: publishes `handles_` and
    // `system_`.
    std::atomic<bool> begun_{false};
    // Release by `request_stop`, acquire by the worker at every boundary.
    std::atomic<bool> stop_{false};
    // Relaxed: counters, read only to see whether they moved or for a log.
    std::atomic<std::uint64_t> progress_{0};
    std::atomic<std::uint64_t> refused_{0};
    // One per device, made by `start` before the thread runs (D-147).
    std::vector<detail::EventSlot> events_;
    std::atomic<Phase> phase_{Phase::STARTING};
    Status failure_;  // written before the phase becomes FAILED
    // A POSIX thread, for its stack size (DI 2.4).
    pthread_t thread_{};
    bool started_ = false;
};

}  // namespace scada::driver::detail

#endif  // SCADA_DRIVER_SRC_WORKER_H
