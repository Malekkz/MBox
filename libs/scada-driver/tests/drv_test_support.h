#ifndef SCADA_DRIVER_TESTS_DRV_TEST_SUPPORT_H
#define SCADA_DRIVER_TESTS_DRV_TEST_SUPPORT_H

// The parts of scada-drv-test (drv_test_main.cpp) that are logic, so that
// unit tests reach them: the command line, the thread-checked test
// protocol, the faulty transports and the factory of the transports.

#include "scada/common/log.h"
#include "scada/driver/compiled.h"
#include "scada/driver/driver.h"
#include "scada/driver/protocol.h"
#include "scada/driver/transport.h"
#include "test_protocol.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace scada::driver::test {

// The exit status of a failed thread check (conformance test 1).
inline constexpr int EXIT_THREAD_CHECK = 3;

// How a channel's transport misbehaves, for the watchdog tests (2.6).
enum class Fault : std::uint8_t {
    NONE = 0,
    HANG,  // the first receive blocks for good, deaf to the stop
    SPIN,  // the first receive spins on the CPU for good, deaf to the stop
};

// Where a channel connects, beside its compiled form.
struct Endpoint {
    bool serial = false;
    std::string host_or_path;
    std::uint32_t port_or_baud = 0;
    Fault fault = Fault::NONE;
};

struct DrvTestOptions {
    DriverSettings settings;
    CompiledConfig config;
    std::vector<Endpoint> endpoints;  // one per channel
    bool help = false;                // `--help`: print the usage, exit 0
    bool refuse_log = false;          // `--refuse-log MS`: see refusing_sink
    std::int64_t refuse_log_ms = 0;
};

// A sink that writes every record to `inner`, takes `delay_ms` more, as a
// slow journal would, and then reports it refused: a test sees each
// refusal in `log_dropped` and the record itself in the output, and the
// records of the stop are still on their way when the run ends (PR 182
// S1).
[[nodiscard]] std::unique_ptr<LogSink> refusing_sink(
    std::unique_ptr<LogSink> inner, std::int64_t delay_ms);

// The usage text of scada-drv-test.
[[nodiscard]] std::string_view drv_test_usage() noexcept;

// Reads the command line (`args[0]` is the program); nothing on a
// malformed one, with a message on `err`. Every number is unsigned and in
// range: a sign is refused.
[[nodiscard]] std::optional<DrvTestOptions> parse_drv_test_args(
    std::span<const char* const> args, std::ostream& err);

// The test protocol, with the thread of every call checked (test 1):
// `configure`, `plan` and the caps on the thread that built it (the main
// thread, DI 2.3), `encode`, `frame_state` and `decode` on one worker
// thread per device and never on the main one. A call on another thread
// ends the process at once with EXIT_THREAD_CHECK and "thread check
// failed" on standard error.
class ThreadCheckedProtocol final : public FrameProtocol {
public:
    explicit ThreadCheckedProtocol(const CompiledConfig& config);

    [[nodiscard]] Status configure(
        const DriverConfig& config, CompiledConfig& out,
        ConfigErrors& errors) const noexcept override;
    [[nodiscard]] DeviceCaps device_caps(
        const CompiledDevice& device) const noexcept override;
    [[nodiscard]] PointCaps point_caps(
        const CompiledPoint& point) const noexcept override;
    [[nodiscard]] Status plan(const CompiledDevice& device,
                              const ReadRules& rules,
                              ReadPlan& out) const noexcept override;
    SCADA_RT [[nodiscard]] EncodeResult encode(
        const CompiledDevice& device, ChannelState& channel,
        const Request& request, Attempt& attempt,
        TxBuffer& out) const noexcept override;
    SCADA_RT [[nodiscard]] FrameState frame_state(
        const Request& request, const Attempt& attempt,
        std::span<const std::byte> received) const noexcept override;
    SCADA_RT [[nodiscard]] DecodeResult decode(
        const CompiledDevice& device, DeviceState& state,
        const Request& request, const Attempt& attempt,
        std::span<const std::byte> frame,
        SampleSink& sink) const noexcept override;

private:
    void on_main(std::string_view call) const noexcept;
    void on_worker(const CompiledDevice& device,
                   std::string_view call) const noexcept;

    TestProtocol inner_;
    std::thread::id main_;
    std::unordered_map<const CompiledDevice*, std::size_t> slots_;
    // One per device: the first worker thread that called for it.
    mutable std::vector<std::atomic<std::thread::id>> workers_;
};

// Plans every device with `protocol`, on the calling thread; false with a
// message on `err` if one cannot be planned.
[[nodiscard]] bool plan_all(CompiledConfig& config,
                            const FrameProtocol& protocol, std::ostream& err);

// The transport of `channel` as `endpoint` says: a TcpTransport or a
// SerialTransport, wrapped by its fault.
[[nodiscard]] std::unique_ptr<Transport> make_transport(
    const CompiledChannel& channel, const Endpoint& endpoint,
    const Clock& clock);

}  // namespace scada::driver::test

#endif  // SCADA_DRIVER_TESTS_DRV_TEST_SUPPORT_H
