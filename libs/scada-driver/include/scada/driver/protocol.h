#ifndef SCADA_DRIVER_PROTOCOL_H
#define SCADA_DRIVER_PROTOCOL_H

// The interface a frame protocol implements (driver interface contract 2.2):
// one request at a time, as pure functions. The framework owns the device
// exchange, the transport, scheduling, retries, pacing and every tag write;
// the protocol plans the requests of a device, encodes one request, says
// what the bytes received so far are, and decodes a complete answer.
//
// No type of a protocol library appears here (reference 16.3).

#include "scada/common/realtime.h"
#include "scada/common/status.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/driver/compiled.h"
#include "scada/driver/exchange.h"
#include "scada/driver/plan.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::driver {

// What encode returns.
struct EncodeResult {
    enum class Kind : std::uint8_t {
        OK,
        REFUSED
    };  // REFUSED: cannot be encoded
    Kind kind = Kind::REFUSED;
    std::size_t length = 0;  // bytes written into the TxBuffer when OK
    std::size_t expected_max =
        0;  // the longest answer this request allows (7.1)
};

// What frame_state returns, given the bytes received so far.
struct FrameState {
    enum class Kind : std::uint8_t {
        NEED_MORE,  // incomplete; `need` more bytes at least (1 if unknown)
        COMPLETE,   // a whole answer of `length` bytes for this request
        STALE,      // a whole frame of `length` bytes answering an earlier
                    // request (an older transaction id): discard, keep waiting
        BROKEN      // not a valid answer: the framework discards input (7.1),
                    // and on `tcp` closes the connection (7.3)
    };
    Kind kind = Kind::NEED_MORE;
    std::size_t need = 1;
    std::size_t length = 0;
};

// What decode returns. For a read, the per-point outcomes of 5.2 go to the
// sink; the kind tells the framework what to do with the request. For a
// write, the kind is the write's outcome.
struct DecodeResult {
    enum class Kind : std::uint8_t {
        SAMPLES,         // read: answered; samples delivered to the sink
        SPLIT,           // read: refused whole as an illegal address: use its
                         // split form once (6.3)
        ABSENT,          // read: the points are not on this device (5.2, 3)
        DEVICE_INVALID,  // read: the device cannot serve the values now (5.2,
                         // 4)
        BUSY,            // read or write: busy, not executed
        UNREACHABLE,     // read or write: a gateway cannot reach this unit;
                         // counts toward that unit's demotion only (7.4)
        WRITE_OK,        // write: accepted
        REFUSED,         // write: refused; `reason` of 8.8 and `raw`
        BROKEN           // a complete frame whose content is not valid; samples
                         // already delivered for it are dropped, not written
    };
    Kind kind = Kind::BROKEN;
    std::uint16_t reason = 0;  // a reason of 8.8, for REFUSED
    std::uint16_t raw = 0;     // the protocol's code, for REFUSED
};

// Static facts of a device (2.8). Each has a default, so a protocol sets
// only what applies.
struct DeviceCaps {
    std::uint64_t max_read_units = 1;   // protocol units in one read
    std::uint64_t max_write_units = 1;  // protocol units in one write
    std::size_t max_frame_bytes = 0;    // largest legal answer (7.1)
};

// Static facts of a point (2.8).
struct PointCaps {
    enum class Select : std::uint8_t { GATEWAY = 1, NATIVE };
    enum class WriteCompletion : std::uint8_t { IMMEDIATE = 1, TWO_PHASE };
    enum class Acquisition : std::uint8_t { POLL = 1, REPORT };
    Select select = Select::GATEWAY;
    std::int64_t native_select_window_ns = 0;  // with Select::NATIVE only
    WriteCompletion write_completion = WriteCompletion::IMMEDIATE;
    bool writable_shape = false;  // the point's raw shape can be written (8.4)
    Acquisition acquisition = Acquisition::POLL;
};

// A protocol's binding as `configure` receives it: the typed configuration
// tree of the driver's generation (17, B-12), every value with its file,
// line and column (CS 13.3).
struct DriverConfig {
    const conf::Document* document = nullptr;  // never null when passed
    conf::NodeId root = 0;
};

// Where `configure` reports what it refuses, each with a location and a CS
// code (CF-650 to CF-699 for a protocol).
using ConfigErrors = conf::Diagnostics;

// A frame protocol: request and answer over a transport the framework owns
// (TCP, serial, RTU framing over TCP). The object holds no mutable state.
// It is called from the main thread and from every channel worker at once;
// all mutable state lives in the framework's objects: ChannelState (per
// connection) and DeviceState (per device).
//
// The SCADA_RT annotation is not inherited: every override of encode,
// frame_state and decode repeats it, and the `check` target reports one
// that does not.
class FrameProtocol {
public:
    FrameProtocol() = default;
    virtual ~FrameProtocol() = default;
    FrameProtocol(const FrameProtocol&) = delete;
    FrameProtocol& operator=(const FrameProtocol&) = delete;
    FrameProtocol(FrameProtocol&&) = delete;
    FrameProtocol& operator=(FrameProtocol&&) = delete;

    // Main thread, or the driver's check mode. Allocates; no I/O. Validates
    // every binding and compiles it into `out`, built aside: the running
    // configuration is not touched. Errors go to `errors`, each with file,
    // line and a CS code.
    [[nodiscard]] virtual Status configure(
        const DriverConfig& config, CompiledConfig& out,
        ConfigErrors& errors) const noexcept = 0;

    // Main thread. Static facts of a device and of each of its points (2.8).
    [[nodiscard]] virtual DeviceCaps device_caps(
        const CompiledDevice& device) const noexcept = 0;
    [[nodiscard]] virtual PointCaps point_caps(
        const CompiledPoint& point) const noexcept = 0;

    // Main thread. Groups the points of each poll group into requests within
    // the read rules (6.3), each with its split form, and names the device's
    // keep-alive and probe requests (6.5, 7.4). `out.by_group` arrives
    // with one empty list per poll group of the channel. Allocates.
    [[nodiscard]] virtual Status plan(const CompiledDevice& device,
                                      const ReadRules& rules,
                                      ReadPlan& out) const noexcept = 0;

    // Channel worker. Encodes one request into `out`, a buffer of the
    // framework sized at configure. Takes the next transaction id from
    // `channel` and records it in `attempt`: the planned request stays
    // immutable, and every attempt, a retry included, gets a new id.
    SCADA_RT [[nodiscard]] virtual EncodeResult encode(
        const CompiledDevice& device, ChannelState& channel,
        const Request& request, Attempt& attempt,
        TxBuffer& out) const noexcept = 0;

    // Channel worker. Given the bytes received so far for `request` and its
    // `attempt`, says what they are. Never reads beyond `received`.
    SCADA_RT [[nodiscard]] virtual FrameState frame_state(
        const Request& request, const Attempt& attempt,
        std::span<const std::byte> received) const noexcept = 0;

    // Channel worker. Decodes a complete answer into raw samples (5.1), or
    // into the outcome of a write. `state` is the device's mutable protocol
    // state, owned by the framework's Device.
    SCADA_RT [[nodiscard]] virtual DecodeResult decode(
        const CompiledDevice& device, DeviceState& state,
        const Request& request, const Attempt& attempt,
        std::span<const std::byte> frame, SampleSink& out) const noexcept = 0;
};

}  // namespace scada::driver

#endif  // SCADA_DRIVER_PROTOCOL_H
