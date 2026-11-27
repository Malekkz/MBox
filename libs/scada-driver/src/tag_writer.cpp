#include "tag_writer.h"

#include "scada/common/status.h"
#include "scada/driver/channel_engine.h"
#include "scada/driver/compiled.h"
#include "scada/driver/sample_processing.h"
#include "scada/driver/tag_value.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/types.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace scada::driver::detail {
namespace {

// The flags of the 9.1 write of a tag that holds `stored`: NOT_INIT, and
// SUBSTITUTED if the stored value carries it.
std::uint16_t kept_flags(const scada_value& stored) noexcept {
    return static_cast<std::uint16_t>(
        SCADA_FLAG_NOT_INIT | (stored.quality_flags & SCADA_FLAG_SUBSTITUTED));
}

// The write of 9.1 for one tag: value as stored, BAD with NOT_INIT, source
// time 0; a RESTORED tag is left alone. SUBSTITUTED describes the stored
// value and stays with it (review R1 of #157). SCADA_OK or the library's
// code.
int write_one_not_initialised(scada_ctx* ctx, std::uint32_t target) noexcept {
    scada_value stored{};
    int code = scada_tag_read(ctx, target, &stored);
    if (code != SCADA_OK) {
        return code;
    }
    if ((stored.quality_flags & SCADA_FLAG_RESTORED) != 0) {
        return SCADA_OK;  // keeps its restored value until the device is read
    }
    if (stored.type == SCADA_TYPE_STRING) {
        std::array<char, SCADA_STRING_MAX> text{};
        std::size_t length = 0;
        code = scada_tag_read_string(ctx, target, text.data(), text.size(),
                                     &length, nullptr);
        if (code != SCADA_OK) {
            return code;
        }
        return scada_tag_write_string(ctx, target, text.data(), length,
                                      SCADA_QUALITY_BAD, kept_flags(stored), 0);
    }
    scada_value value{};
    value.type = stored.type;
    value.value = stored.value;
    value.quality = SCADA_QUALITY_BAD;
    value.quality_flags = kept_flags(stored);
    value.source_time_ns = 0;
    return scada_tag_write(ctx, target, &value);
}

}  // namespace

scada_value to_scada_value(const TagWrite& write) noexcept {
    scada_value out{};
    out.type = static_cast<std::uint8_t>(write.value.type);
    out.quality = static_cast<std::uint8_t>(write.quality);
    out.quality_flags = write.flags;
    out.source_time_ns = write.source_time_ns;
    switch (write.value.type) {
        case tag::TagType::BOOL:
            out.value = write.value.b ? 1U : 0U;
            break;
        case tag::TagType::INT64:
            out.value = std::bit_cast<std::uint64_t>(write.value.i);
            break;
        case tag::TagType::UINT64:
            out.value = write.value.u;
            break;
        case tag::TagType::FLOAT64:
            out.value = std::bit_cast<std::uint64_t>(write.value.f);
            break;
        case tag::TagType::STRING:
            break;  // written by text, never by value
    }
    return out;
}

TagValue from_scada_value(const scada_value& value,
                          std::span<const char> text) noexcept {
    switch (value.type) {
        case SCADA_TYPE_BOOL:
            return bool_value(value.value != 0);
        case SCADA_TYPE_INT64:
            return int64_value(std::bit_cast<std::int64_t>(value.value));
        case SCADA_TYPE_UINT64:
            return TagValue{.type = tag::TagType::UINT64, .u = value.value};
        case SCADA_TYPE_FLOAT64:
            return TagValue{.type = tag::TagType::FLOAT64,
                            .f = std::bit_cast<double>(value.value)};
        case SCADA_TYPE_STRING:
            return TagValue{.type = tag::TagType::STRING,
                            .text = std::string_view(text.data(), text.size())};
        default:
            return TagValue{};
    }
}

Status write_owner_not_initialised(scada_ctx* ctx,
                                   std::uint16_t owner) noexcept {
    scada_owner_info_t info{};
    if (scada_owner_info(ctx, owner, &info) != SCADA_OK || info.owned == 0) {
        return {StatusCode::INTERNAL, "the owner of the channel is not ours"};
    }
    for (std::uint32_t slot = 0; slot < info.tag_count; ++slot) {
        if (write_one_not_initialised(ctx, scada_tag_handle(owner, slot)) !=
            SCADA_OK) {
            return {StatusCode::INTERNAL, "a write of 9.1 was refused"};
        }
    }
    if (info.tag_count > 0) {
        // A refused notify is no reason to fail the start: the values are
        // written, and the consumers still wake within 100 ms (TD 8.4).
        (void)scada_tag_notify(ctx);
    }
    return {};
}

TagWriter::TagWriter(scada_ctx* ctx, PointHandles handles, std::size_t strings)
    : ctx_(ctx), handles_(std::move(handles)) {
    pending_.reserve(strings);
}

std::uint32_t TagWriter::handle(std::uint32_t device,
                                std::uint32_t point) const noexcept {
    if (device >= handles_.first.size()) {
        return SCADA_INVALID_HANDLE;
    }
    const std::size_t index = handles_.first[device] + point;
    const std::size_t end = device + 1 < handles_.first.size()
                                ? handles_.first[device + 1]
                                : handles_.handles.size();
    return index < end ? handles_.handles[index] : SCADA_INVALID_HANDLE;
}

void TagWriter::write_point(std::uint32_t device, std::uint32_t point,
                            const ProcessedSample& sample) noexcept {
    const std::uint32_t target = handle(device, point);
    if (!sample.write_tag || target == SCADA_INVALID_HANDLE) {
        return;  // a point without a tag (`tag: false`)
    }
    const TagWrite& write = sample.tag;
    if (write.value.type == tag::TagType::STRING) {
        // A second write of the same tag in one step replaces the first;
        // the others fit the capacity reserved at construction, so nothing
        // allocates.
        auto found = std::find_if(
            pending_.begin(), pending_.end(),
            [&](const PendingText& text) { return text.handle == target; });
        if (found == pending_.end() && pending_.size() == pending_.capacity()) {
            ++refused_;
            return;
        }
        PendingText& text =
            found != pending_.end() ? *found : pending_.emplace_back();
        text.handle = target;
        text.quality = static_cast<std::uint8_t>(write.quality);
        text.flags = write.flags;
        text.source_time_ns = write.source_time_ns;
        text.length = std::min(write.value.text.size(), text.text.size());
        std::copy_n(write.value.text.data(), text.length, text.text.data());
        return;
    }
    const scada_value value = to_scada_value(write);
    if (scada_tag_write(ctx_, target, &value) == SCADA_OK) {
        written_ = true;
    } else {
        ++refused_;
    }
}

namespace {

// The engine as the target of TagWriter::seed.
class EngineSeeds final : public PointSeeds {
public:
    explicit EngineSeeds(ChannelEngine& engine) noexcept : engine_(engine) {}
    EngineSeeds(const EngineSeeds&) = delete;
    EngineSeeds& operator=(const EngineSeeds&) = delete;
    EngineSeeds(EngineSeeds&&) = delete;
    EngineSeeds& operator=(EngineSeeds&&) = delete;
    ~EngineSeeds() override = default;

    void seed_point(std::uint32_t device, std::uint32_t point,
                    const TagValue& stored,
                    bool substituted) noexcept override {
        engine_.seed_point(device, point, stored, substituted);
    }

private:
    ChannelEngine& engine_;
};

}  // namespace

void TagWriter::seed(ChannelEngine& engine,
                     const CompiledChannel& channel) noexcept {
    EngineSeeds seeds(engine);
    seed(seeds, channel);
}

void TagWriter::seed(PointSeeds& seeds,
                     const CompiledChannel& channel) noexcept {
    for (std::uint32_t device = 0; device < channel.devices.size(); ++device) {
        const CompiledDevice& compiled = channel.devices[device];
        for (std::uint32_t point = 0; point < compiled.points.size(); ++point) {
            const std::uint32_t target = handle(device, point);
            if (target == SCADA_INVALID_HANDLE) {
                continue;
            }
            scada_value stored{};
            std::array<char, SCADA_STRING_MAX> text{};
            std::size_t length = 0;
            if (scada_tag_read(ctx_, target, &stored) != SCADA_OK ||
                (stored.type == SCADA_TYPE_STRING &&
                 scada_tag_read_string(ctx_, target, text.data(), text.size(),
                                       &length, nullptr) != SCADA_OK)) {
                ++refused_;
                continue;  // no stored value: the point starts from 0
            }
            seeds.seed_point(
                device, point,
                from_scada_value(stored, std::span(text).first(length)),
                (stored.quality_flags & SCADA_FLAG_SUBSTITUTED) != 0);
        }
    }
}

void TagWriter::notify() noexcept {
    for (const PendingText& text : pending_) {
        if (scada_tag_write_string(ctx_, text.handle, text.text.data(),
                                   text.length, text.quality, text.flags,
                                   text.source_time_ns) == SCADA_OK) {
            written_ = true;
        } else {
            ++refused_;
        }
    }
    pending_.clear();  // keeps its capacity
    if (!written_) {
        return;
    }
    written_ = false;
    if (scada_tag_notify(ctx_) != SCADA_OK) {
        ++refused_;  // the consumers still wake within 100 ms (TD 8.4)
    }
}

}  // namespace scada::driver::detail
