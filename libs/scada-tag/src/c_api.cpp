// The C interface of scada/tag/scada_tag.h: a thin layer over Attachment
// that turns pointers into checked values and Status into return codes.

#include "scada/tag/scada_tag.h"

#include "attachment.h"
#include "layout.h"
#include "region_file.h"

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/liveness.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// The context of the C interface. C code sees only its name.
struct
    scada_ctx {  // NOLINT(readability-identifier-naming): the name is fixed by the contract, section 13
    std::unique_ptr<scada::tag::Attachment> attachment;
};

namespace {

using scada::StatusCode;

// The return code for `status`, which is not OK. Sets errno for SCADA_E_IO
// (section 13.3).
int code_of(const scada::Status& status) noexcept {
    switch (status.code()) {
        case StatusCode::INVALID_ARGUMENT:
        case StatusCode::OUT_OF_RANGE:
            return SCADA_E_INVALID;
        case StatusCode::NOT_FOUND:
            return SCADA_E_NOT_FOUND;
        case StatusCode::PERMISSION_DENIED:
            return SCADA_E_PERMISSION;
        case StatusCode::ALREADY_EXISTS:
            return SCADA_E_BUSY;
        case StatusCode::UNSUPPORTED:
        case StatusCode::DATA_CORRUPT:
            return SCADA_E_LAYOUT;
        case StatusCode::FAILED_PRECONDITION:
            return SCADA_E_RETIRING;
        case StatusCode::RESOURCE_EXHAUSTED:
            errno = ENOSPC;
            return SCADA_E_IO;
        default:
            break;
    }
    // The error number of the call that failed, as the library recorded
    // it; EAGAIN where no call of the operating system failed (a
    // generation that is not running).
    const int os_error = scada::tag::last_os_error();
    errno = os_error != 0 ? os_error : EAGAIN;
    return SCADA_E_IO;
}

int attach(const char* process_name, const scada_attach_opts* opts,
           scada_ctx** out) {
    if (out == nullptr) {
        return SCADA_E_INVALID;
    }
    *out = nullptr;
    if (process_name == nullptr) {
        return SCADA_E_INVALID;
    }
    scada::tag::AttachOptions options;
    if (opts != nullptr) {
        if (opts->base_dir != nullptr) {
            options.base_dir = opts->base_dir;
        }
        if (opts->retain_dir != nullptr) {
            options.retain_dir = opts->retain_dir;
        }
        if (opts->tagd_uid != -1) {
            if (opts->tagd_uid < 0 ||
                opts->tagd_uid > std::int64_t{scada::tag::MAX_UID}) {
                return SCADA_E_INVALID;
            }
            // In range: checked above.
            options.tagd_uid = static_cast<std::uint32_t>(opts->tagd_uid);
        }
    }
    // A name longer than a process name is refused before it is copied.
    const std::string_view name(process_name);

    scada::tag::clear_os_error();
    std::string detail;
    scada::Result<std::unique_ptr<scada::tag::Attachment>> attached =
        scada::tag::Attachment::attach(name, options, detail);
    if (!attached.ok()) {
        return code_of(attached.status());
    }
    auto context = std::make_unique<scada_ctx>();
    context->attachment = std::move(attached).value();
    // The caller owns the context from here and gives it back to
    // scada_tag_detach.
    *out = context.release();
    return SCADA_OK;
}

int report_status(scada_ctx& ctx, scada_status& out) {
    const scada::tag::AttachmentStatus status = ctx.attachment->status();
    scada_status report{};
    report.generation = status.generation;
    report.generation_state =
        status.state.has_value() ? scada::tag::to_raw(*status.state) : 0;
    report.process_index = status.process_index;
    report.tagd_down = status.tagd == scada::tag::DownReason::NONE ? 0 : 1;
    report.clock_syscall = status.clock_syscall ? 1 : 0;
    report.regions_created = status.regions_created;
    report.regions_reused = status.regions_reused;
    report.records_repaired = status.records_repaired;
    out = report;
    if (status.state == scada::tag::GenerationState::RETIRING) {
        return SCADA_E_RETIRING;
    }
    return report.tagd_down != 0 ? SCADA_E_TAGD_DOWN : SCADA_OK;
}

// Runs `call`, which returns a return code. No exception leaves the C
// interface: the only ones the library can meet are those of a failed
// allocation, reported as SCADA_E_IO with errno ENOMEM.
template <typename Call>
int guarded(Call&& call) noexcept {
    try {
        return std::forward<Call>(call)();
    } catch (const std::exception&) {
        errno = ENOMEM;
        return SCADA_E_IO;
    }
}

}  // namespace

extern "C" {

int scada_tag_attach(const char* process_name, scada_ctx** out) {
    return guarded([&] { return attach(process_name, nullptr, out); });
}

int scada_tag_attach_opts(const char* process_name,
                          const scada_attach_opts* opts, scada_ctx** out) {
    return guarded([&] { return attach(process_name, opts, out); });
}

void scada_tag_detach(scada_ctx* ctx) {
    // Takes back the ownership scada_tag_attach handed to the caller.
    const std::unique_ptr<scada_ctx> owned(ctx);
}

SCADA_RT int scada_tag_heartbeat(scada_ctx* ctx) {
    if (ctx == nullptr) {
        return SCADA_E_INVALID;
    }
    return ctx->attachment->heartbeat() ? SCADA_OK : SCADA_E_RETIRING;
}

int scada_tag_status(scada_ctx* ctx, scada_status* out) {
    if (ctx == nullptr || out == nullptr) {
        return SCADA_E_INVALID;
    }
    return guarded([&] { return report_status(*ctx, *out); });
}

int scada_tag_resolve(scada_ctx* ctx, const char* name, uint32_t* handle) {
    if (handle == nullptr) {
        return SCADA_E_INVALID;
    }
    *handle = SCADA_INVALID_HANDLE;
    if (ctx == nullptr || name == nullptr) {
        return SCADA_E_INVALID;
    }
    return guarded([&] { return ctx->attachment->resolve(name, *handle); });
}

int scada_tag_info(scada_ctx* ctx, uint32_t handle, scada_tag_info_t* out) {
    if (ctx == nullptr || out == nullptr) {
        return SCADA_E_INVALID;
    }
    return guarded([&] { return ctx->attachment->info(handle, *out); });
}

SCADA_RT int scada_tag_read(scada_ctx* ctx, uint32_t handle, scada_value* out) {
    if (ctx == nullptr || out == nullptr) {
        return SCADA_E_INVALID;
    }
    return ctx->attachment->read(handle, *out);
}

SCADA_RT int scada_tag_read_many(scada_ctx* ctx, const uint32_t* handles,
                                 size_t n, scada_value* out, int* status) {
    if (ctx == nullptr || (n != 0 && (handles == nullptr || out == nullptr))) {
        return SCADA_E_INVALID;
    }
    // The caller states that each array holds `n` elements.
    return ctx->attachment->read_many(
        std::span<const std::uint32_t>(handles, n),
        std::span<scada_value>(out, n),
        std::span<int>(status, status == nullptr ? 0 : n));
}

SCADA_RT int scada_tag_write(scada_ctx* ctx, uint32_t handle,
                             const scada_value* value) {
    if (ctx == nullptr || value == nullptr) {
        return SCADA_E_INVALID;
    }
    return ctx->attachment->write(handle, *value);
}

SCADA_RT int scada_tag_write_many(scada_ctx* ctx, const uint32_t* handles,
                                  size_t n, const scada_value* values) {
    if (ctx == nullptr ||
        (n != 0 && (handles == nullptr || values == nullptr))) {
        return SCADA_E_INVALID;
    }
    // The caller states that each array holds `n` elements.
    return ctx->attachment->write_many(
        std::span<const std::uint32_t>(handles, n),
        std::span<const scada_value>(values, n));
}

int scada_tag_read_string(scada_ctx* ctx, uint32_t handle, char* buf,
                          size_t cap, size_t* len, scada_value* meta) {
    if (ctx == nullptr || buf == nullptr || len == nullptr) {
        return SCADA_E_INVALID;
    }
    // The caller states that `buf` holds `cap` bytes.
    return ctx->attachment->read_string(handle, std::span<char>(buf, cap), *len,
                                        meta);
}

int scada_tag_write_string(scada_ctx* ctx, uint32_t handle, const char* text,
                           size_t len, uint8_t quality, uint16_t flags,
                           int64_t source_time_ns) {
    if (ctx == nullptr || (text == nullptr && len != 0) ||
        len > SCADA_STRING_MAX) {
        return SCADA_E_INVALID;
    }
    // The caller states that `text` holds `len` bytes.
    return ctx->attachment->write_string(
        handle, len == 0 ? std::string_view() : std::string_view(text, len),
        quality, flags, source_time_ns);
}

}  // extern "C"
