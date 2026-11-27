#ifndef SCADA_DRIVER_SRC_TAG_WRITER_H
#define SCADA_DRIVER_SRC_TAG_WRITER_H

// The TagSink of a channel worker over the tag library (driver.h, D-142):
// each point's resolved handle, written on the worker's own thread, which
// owns the channel's tags (DI 2.4, CS 12.3), the writes of 9.1, and the
// system tags of the channel and its devices (11.1, D-145).

#include "scada/common/status.h"
#include "scada/driver/channel_engine.h"
#include "scada/driver/compiled.h"
#include "scada/driver/diagnostics.h"
#include "scada/driver/failure.h"
#include "scada/driver/sample_processing.h"
#include "scada/driver/tag_value.h"
#include "scada/tag/scada_tag.h"
#include "system_tags.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace scada::driver::detail {

// The handles of a channel's points: `handles[first[device] + point]`;
// SCADA_INVALID_HANDLE for a point without a tag (`tag: false`).
struct PointHandles {
    std::vector<std::uint32_t> handles;
    std::vector<std::size_t> first;  // one per device of the channel
};

// 9.1, by owner slot: every tag of owner `owner` is written with its value
// as stored, BAD with NOT_INIT and source time 0, except a tag still marked
// RESTORED from a retain file, which keeps its value and quality until the
// device is read; then one notify. It runs on the owner's thread before
// anything else, so it covers tags no point names and needs no name
// resolved (review M6). INTERNAL when the library refuses a read or a
// write: the start fails (review M9). Not real-time.
[[nodiscard]] Status write_owner_not_initialised(scada_ctx* ctx,
                                                 std::uint16_t owner) noexcept;

class TagWriter final : public TagSink {
public:
    // `ctx` is borrowed and outlives the writer; it must only ever be used
    // for these tags from the thread that calls the writer. `strings` is the
    // number of STRING points of the channel: room for one pending write of
    // each is allocated here. `system` holds the channel's system tags.
    TagWriter(scada_ctx* ctx, PointHandles handles, std::size_t strings,
              SystemTagHandles system);

    // Writes the point's own tag (bits: their tags come with the compile
    // step, slice 8). A STRING value is copied into its pending write,
    // since `scada_tag_write_string` is not real-time; `notify` writes it.
    // A point without a tag is skipped; a write the library refuses is
    // counted, never retried.
    SCADA_RT void write_point(std::uint32_t device, std::uint32_t point,
                              const ProcessedSample& sample) noexcept override;

    // `sys.dev.<device>.state` is written by `publish_system` after the
    // step that changed it (D-145).
    SCADA_RT void device_state(std::uint32_t /*device*/,
                               DeviceStatus /*state*/) noexcept override {}

    // `sys.drv.<channel>.state` at once; CONNECTING also wakes the readers
    // at once, since the connect that follows blocks the worker up to its
    // `connect_timeout` (D-145). Not real-time: the notify is a system call.
    void channel_link(ChannelLink link) noexcept override;

    // The system tags of the channel and its devices (SystemTags::publish).
    void publish_system(ChannelEngine& engine, const CompiledChannel& channel,
                        std::int64_t now_ns, bool all) noexcept;

    // The system tags of a channel that has no engine, a disabled one
    // (SystemTags::publish_idle).
    void publish_idle(const CompiledChannel& channel) noexcept;

    // When `publish_system` next writes the counters.
    [[nodiscard]] std::int64_t system_due_ns() const noexcept {
        return system_.next_due_ns();
    }

    // Gives `engine` the value each point's tag holds now, after 9.1, as
    // its last value (review M4). Not real-time.
    void seed(ChannelEngine& engine, const CompiledChannel& channel) noexcept;

    // Writes the pending STRING values, then wakes the consumers once if
    // anything was written since the last call: one `scada_tag_notify` per
    // delivery (reference 5.3). A notify the library refuses is counted
    // with the refused writes. Not real-time.
    void notify() noexcept;

    // Writes and notifies the library refused since the writer was made,
    // the system tags' included.
    [[nodiscard]] std::uint64_t refused() const noexcept {
        return refused_ + system_.refused();
    }

private:
    [[nodiscard]] std::uint32_t handle(std::uint32_t device,
                                       std::uint32_t point) const noexcept;

    // A STRING write that waits for `notify`.
    struct PendingText {
        std::uint32_t handle = SCADA_INVALID_HANDLE;
        std::uint8_t quality = 0;
        std::uint16_t flags = 0;
        std::int64_t source_time_ns = 0;
        std::size_t length = 0;
        std::array<char, SCADA_STRING_MAX> text{};
    };

    scada_ctx* ctx_;
    PointHandles handles_;
    std::vector<PendingText> pending_;  // capacity: the STRING points
    SystemTags system_;
    bool written_ = false;
    std::uint64_t refused_ = 0;
};

// The value of `write` as the tag library takes it, for every type but
// STRING.
[[nodiscard]] scada_value to_scada_value(const TagWrite& write) noexcept;

// The value a library read gave, as a TagValue of its type; a STRING's
// text is `text`, which must outlive the result.
[[nodiscard]] TagValue from_scada_value(const scada_value& value,
                                        std::span<const char> text) noexcept;

}  // namespace scada::driver::detail

#endif  // SCADA_DRIVER_SRC_TAG_WRITER_H
