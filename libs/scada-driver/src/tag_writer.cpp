#include "tag_writer.h"

#include "scada/driver/sample_processing.h"
#include "scada/driver/tag_value.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/types.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace scada::driver::detail {

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

TagWriter::TagWriter(scada_ctx* ctx, PointHandles handles) noexcept
    : ctx_(ctx), handles_(std::move(handles)) {}

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
    if (!sample.write_tag) {
        return;
    }
    const std::uint32_t target = handle(device, point);
    if (target == SCADA_INVALID_HANDLE) {
        ++refused_;
        return;
    }
    const TagWrite& write = sample.tag;
    int code = SCADA_OK;
    if (write.value.type == tag::TagType::STRING) {
        code = scada_tag_write_string(ctx_, target, write.value.text.data(),
                                      write.value.text.size(),
                                      static_cast<std::uint8_t>(write.quality),
                                      write.flags, write.source_time_ns);
    } else {
        const scada_value value = to_scada_value(write);
        code = scada_tag_write(ctx_, target, &value);
    }
    if (code == SCADA_OK) {
        written_ = true;
    } else {
        ++refused_;
    }
}

void TagWriter::notify() noexcept {
    if (!written_) {
        return;
    }
    written_ = false;
    if (scada_tag_notify(ctx_) != SCADA_OK) {
        ++refused_;  // the consumers still wake within 100 ms (TD 8.4)
    }
}

void TagWriter::write_not_initialised() noexcept {
    for (const std::uint32_t target : handles_.handles) {
        scada_value stored{};
        if (scada_tag_read(ctx_, target, &stored) != SCADA_OK) {
            ++refused_;
            continue;
        }
        if ((stored.quality_flags & SCADA_FLAG_RESTORED) != 0) {
            continue;  // keeps its restored value until the device is read
        }
        int code = SCADA_OK;
        if (stored.type == SCADA_TYPE_STRING) {
            std::array<char, SCADA_STRING_MAX> text{};
            std::size_t length = 0;
            code = scada_tag_read_string(ctx_, target, text.data(), text.size(),
                                         &length, nullptr);
            if (code == SCADA_OK) {
                code = scada_tag_write_string(ctx_, target, text.data(), length,
                                              SCADA_QUALITY_BAD,
                                              SCADA_FLAG_NOT_INIT, 0);
            }
        } else {
            scada_value value{};
            value.type = stored.type;
            value.value = stored.value;
            value.quality = SCADA_QUALITY_BAD;
            value.quality_flags = SCADA_FLAG_NOT_INIT;
            value.source_time_ns = 0;
            code = scada_tag_write(ctx_, target, &value);
        }
        if (code == SCADA_OK) {
            written_ = true;
        } else {
            ++refused_;
        }
    }
    notify();
}

}  // namespace scada::driver::detail
