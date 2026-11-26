#ifndef SCADA_DRIVER_SRC_WORKER_H
#define SCADA_DRIVER_SRC_WORKER_H

// The worker thread of one channel (driver.h; DI 2.4, 2.6, 9.1; D-142).

#include "scada/common/fd.h"
#include "scada/common/status.h"
#include "scada/driver/compiled.h"
#include "scada/driver/protocol.h"
#include "scada/driver/transport.h"
#include "tag_writer.h"

#include <atomic>
#include <cstdint>
#include <memory>

#include <pthread.h>

namespace scada::driver::detail {

class Worker {
public:
    // Where the thread is.
    enum class Phase : std::uint8_t {
        STARTING = 1,  // building the engine, writing the tags of 9.1
        RUNNING,       // in the loop
        FAILED,        // the engine refused the channel: `failure()`
        DONE,          // stopped and disconnected
    };

    // Everything is borrowed but the transport and the handles, and must
    // outlive the worker.
    Worker(const CompiledChannel& channel, const FrameProtocol& protocol,
           std::unique_ptr<Transport> transport, Clock& clock, scada_ctx* ctx,
           PointHandles handles, std::int64_t stall_limit_ns) noexcept;
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    Worker(Worker&&) = delete;
    Worker& operator=(Worker&&) = delete;

    // Creates the stop eventfd and starts the thread. The calling thread
    // must have blocked every signal: the new thread inherits its mask
    // (DI 2.4). UNAVAILABLE if the thread or the eventfd cannot be had.
    [[nodiscard]] Status start() noexcept;

    // Asks the thread to stop at its next transaction boundary; it then
    // disconnects. Safe from any thread, any number of times.
    void request_stop() noexcept;

    // Moved at every transaction boundary and every wake of a wait (2.6).
    [[nodiscard]] std::uint64_t progress() const noexcept {
        return progress_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] Phase phase() const noexcept {
        return phase_.load(std::memory_order_acquire);
    }
    // Valid once the phase is FAILED.
    [[nodiscard]] const Status& failure() const noexcept { return failure_; }
    [[nodiscard]] std::int64_t stall_limit_ns() const noexcept {
        return stall_limit_ns_;
    }
    [[nodiscard]] const CompiledChannel& channel() const noexcept {
        return *channel_;
    }

    // Joins the thread. Never for a stalled worker: after a stall the
    // process ends with `std::_Exit` and no Worker is destroyed (driver.h).
    void join() noexcept;

private:
    void run() noexcept;
    // Waits until `next_ns`, at most a third of the stall limit, for the
    // stop eventfd or, on a stream, for the peer's close (transport.h).
    void wait_until(std::int64_t next_ns) noexcept;

    const CompiledChannel* channel_;
    const FrameProtocol* protocol_;
    std::unique_ptr<Transport> transport_;
    Clock* clock_;
    TagWriter writer_;
    std::int64_t stall_limit_ns_;
    Fd stop_fd_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> progress_{0};
    std::atomic<Phase> phase_{Phase::STARTING};
    Status failure_;  // written before the phase becomes FAILED
    // A POSIX thread, for its stack size (DI 2.4); `join` and the
    // destructor join it.
    pthread_t thread_{};
    bool started_ = false;
};

}  // namespace scada::driver::detail

#endif  // SCADA_DRIVER_SRC_WORKER_H
