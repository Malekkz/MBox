#ifndef SCADA_DRIVER_SRC_TAG_WRITER_H
#define SCADA_DRIVER_SRC_TAG_WRITER_H

// The TagSink of a channel worker over the tag library (driver.h, D-142):
// each point's resolved handle, written on the worker's own thread, which
// owns the channel's tags (DI 2.4, CS 12.3).

#include "scada/driver/channel_engine.h"
#include "scada/driver/failure.h"
#include "scada/driver/sample_processing.h"
#include "scada/tag/scada_tag.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace scada::driver::detail {

// The handles of a channel's points: `handles[first[device] + point]`.
struct PointHandles {
    std::vector<std::uint32_t> handles;
    std::vector<std::size_t> first;  // one per device of the channel
};

class TagWriter final : public TagSink {
public:
    // `ctx` is borrowed and outlives the writer; it must only ever be used
    // for these tags from the thread that calls the writer.
    TagWriter(scada_ctx* ctx, PointHandles handles) noexcept;

    // Writes the point's own tag (bits: their tags come with the compile
    // step, slice 8). A STRING value goes through `scada_tag_write_string`,
    // which is not real-time; a protocol worker is not (DI 2.5). A write the
    // library refuses is counted, never retried.
    SCADA_RT void write_point(std::uint32_t device, std::uint32_t point,
                              const ProcessedSample& sample) noexcept override;

    // `sys.dev.<device>.state` comes with the system tags (slice 5b).
    SCADA_RT void device_state(std::uint32_t /*device*/,
                               DeviceStatus /*state*/) noexcept override {}

    // Wakes the consumers once if anything was written since the last call:
    // one `scada_tag_notify` per delivery (reference 5.3). Not real-time.
    void notify() noexcept;

    // 9.1: before the first request every point tag is written with its
    // value as stored, BAD with NOT_INIT and source time 0, except a tag
    // still marked RESTORED from a retain file, which keeps its value and
    // quality until the device is read. Then one notify. Not real-time.
    void write_not_initialised() noexcept;

    // Writes the library refused, and notifies it refused.
    [[nodiscard]] std::uint64_t refused() const noexcept { return refused_; }

private:
    [[nodiscard]] std::uint32_t handle(std::uint32_t device,
                                       std::uint32_t point) const noexcept;

    scada_ctx* ctx_;
    PointHandles handles_;
    bool written_ = false;
    std::uint64_t refused_ = 0;
};

// The value of `write` as the tag library takes it, for every type but
// STRING.
[[nodiscard]] scada_value to_scada_value(const TagWrite& write) noexcept;

}  // namespace scada::driver::detail

#endif  // SCADA_DRIVER_SRC_TAG_WRITER_H
