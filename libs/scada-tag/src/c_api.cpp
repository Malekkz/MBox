// The C interface of scada/tag/scada_tag.h: a thin layer over Attachment
// that turns pointers into checked values and Status into return codes.

#include "scada/tag/scada_tag.h"

#include "attachment.h"
#include "header_view.h"
#include "layout.h"
#include "queue.h"
#include "queue_region.h"
#include "region_file.h"
#include "subscription.h"
#include "wait_set.h"

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/handle.h"
#include "scada/tag/liveness.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// The context of the C interface. C code sees only its name.
struct
    scada_ctx {  // NOLINT(readability-identifier-naming): the name is fixed by the contract, section 13
    std::unique_ptr<scada::tag::Attachment> attachment;
};

// A subscription of the C interface.
struct
    scada_sub {  // NOLINT(readability-identifier-naming): the name is fixed by the contract, section 13
    std::unique_ptr<scada::tag::Subscription> subscription;
};

// A wait set of the C interface. Destroyed before the subscriptions in it
// or after them: either way the wait set lets go of them.
struct
    scada_waitset {  // NOLINT(readability-identifier-naming): the name is fixed by the contract, section 13
    std::unique_ptr<scada::tag::WaitSet> wait_set;
};

// A queue side of the C interface.
struct
    scada_queue {  // NOLINT(readability-identifier-naming): the name is fixed by the contract, section 13
    std::unique_ptr<scada::tag::Queue> queue;
};

// The sizes the header states, which the contract fixes (section 13). Only
// the former `reserved1` of scada_status was split, so it keeps its size
// and the offsets of the fields before it.
namespace sizes {
constexpr std::size_t STATUS = 32;
constexpr std::size_t STATUS_RECORDS_REPAIRED = 24;
constexpr std::size_t STATUS_OWNER_COUNT = 28;
constexpr std::size_t STATUS_PROCESS_COUNT = 30;
constexpr std::size_t OWNER_INFO = 48;
constexpr std::size_t PROC_STATUS = 88;
constexpr std::size_t PROC_STATUS_RESERVED = 82;
constexpr std::size_t QUEUE_STATS = 80;  // decision D-40
}  // namespace sizes
static_assert(sizeof(scada_status) == sizes::STATUS);
static_assert(offsetof(scada_status, records_repaired) ==
              sizes::STATUS_RECORDS_REPAIRED);
static_assert(offsetof(scada_status, owner_count) == sizes::STATUS_OWNER_COUNT);
static_assert(offsetof(scada_status, process_count) ==
              sizes::STATUS_PROCESS_COUNT);
static_assert(sizeof(scada_owner_info_t) == sizes::OWNER_INFO);
static_assert(sizeof(scada_proc_status_t) == sizes::PROC_STATUS);
static_assert(offsetof(scada_proc_status_t, reserved) ==
              sizes::PROC_STATUS_RESERVED);

namespace {

using scada::StatusCode;

// The C records are the structures of the contract (section 9.4), byte for
// byte: the library copies them as bytes.
template <typename C, typename Layout>
constexpr bool SAME_SIZE =
    sizeof(C) == sizeof(Layout) && alignof(C) == alignof(Layout);
static_assert(SAME_SIZE<scada_event_record, scada::tag::EventRecord> &&
              sizeof(scada_event_record) == SCADA_EVENT_RECORD_SIZE &&
              offsetof(scada_event_record, seq) ==
                  offsetof(scada::tag::EventRecord, seq) &&
              offsetof(scada_event_record, time_ns) ==
                  offsetof(scada::tag::EventRecord, time_ns) &&
              offsetof(scada_event_record, kind) ==
                  offsetof(scada::tag::EventRecord, kind) &&
              offsetof(scada_event_record, payload_len) ==
                  offsetof(scada::tag::EventRecord, payload_len) &&
              offsetof(scada_event_record, source_handle) ==
                  offsetof(scada::tag::EventRecord, source_handle) &&
              offsetof(scada_event_record, payload) ==
                  offsetof(scada::tag::EventRecord, payload) &&
              SCADA_EVENT_PAYLOAD_MAX == scada::tag::EVENT_PAYLOAD_SIZE);
static_assert(SAME_SIZE<scada_write_request, scada::tag::WriteRequest> &&
              sizeof(scada_write_request) == SCADA_WRITE_REQUEST_SIZE &&
              offsetof(scada_write_request, request_id) ==
                  offsetof(scada::tag::WriteRequest, request_id) &&
              offsetof(scada_write_request, target_handle) ==
                  offsetof(scada::tag::WriteRequest, target_handle) &&
              offsetof(scada_write_request, operation) ==
                  offsetof(scada::tag::WriteRequest, operation) &&
              offsetof(scada_write_request, source) ==
                  offsetof(scada::tag::WriteRequest, source) &&
              offsetof(scada_write_request, value_type) ==
                  offsetof(scada::tag::WriteRequest, value_type) &&
              offsetof(scada_write_request, value) ==
                  offsetof(scada::tag::WriteRequest, value) &&
              offsetof(scada_write_request, user_id) ==
                  offsetof(scada::tag::WriteRequest, user_id) &&
              offsetof(scada_write_request, expiry_mono_ns) ==
                  offsetof(scada::tag::WriteRequest, expiry_mono_ns) &&
              offsetof(scada_write_request, posted_mono_ns) ==
                  offsetof(scada::tag::WriteRequest, posted_mono_ns) &&
              offsetof(scada_write_request, select_request_id) ==
                  offsetof(scada::tag::WriteRequest, select_request_id) &&
              offsetof(scada_write_request, reserved) ==
                  offsetof(scada::tag::WriteRequest, reserved));
static_assert(SAME_SIZE<scada_write_result, scada::tag::WriteResult> &&
              sizeof(scada_write_result) == SCADA_WRITE_RESULT_SIZE &&
              offsetof(scada_write_result, request_id) ==
                  offsetof(scada::tag::WriteResult, request_id) &&
              offsetof(scada_write_result, status) ==
                  offsetof(scada::tag::WriteResult, status) &&
              offsetof(scada_write_result, protocol_error) ==
                  offsetof(scada::tag::WriteResult, protocol_error) &&
              offsetof(scada_write_result, completed_time_ns) ==
                  offsetof(scada::tag::WriteResult, completed_time_ns) &&
              offsetof(scada_write_result, reserved) ==
                  offsetof(scada::tag::WriteResult, reserved));
static_assert(sizeof(scada_queue_stats_t) == sizes::QUEUE_STATS);
constexpr std::int64_t NANOSECONDS_PER_MILLISECOND = 1'000'000;
static_assert(SCADA_REQUEST_LIFETIME_MAX_MS * NANOSECONDS_PER_MILLISECOND ==
              scada::tag::MAX_REQUEST_LIFETIME_NS);

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

// The options `opts` (NULL: the defaults) as the library takes them.
// False if tagd_uid is neither -1 nor a uid.
bool options_of(const scada_attach_opts* opts,
                scada::tag::AttachOptions& options) {
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
                return false;
            }
            // In range: checked above.
            options.tagd_uid = static_cast<std::uint32_t>(opts->tagd_uid);
        }
    }
    return true;
}

// Hands the attachment `attached` to the caller as a new context in
// `*out`, or returns the code of its error.
int hand_over(scada::Result<std::unique_ptr<scada::tag::Attachment>> attached,
              scada_ctx** out) {
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
    if (!options_of(opts, options)) {
        return SCADA_E_INVALID;
    }
    // A name longer than a process name is refused before it is copied.
    const std::string_view name(process_name);

    scada::tag::clear_os_error();
    std::string detail;
    return hand_over(scada::tag::Attachment::attach(name, options, detail),
                     out);
}

int observe(const scada_attach_opts* opts, scada_ctx** out) {
    if (out == nullptr) {
        return SCADA_E_INVALID;
    }
    *out = nullptr;
    scada::tag::AttachOptions options;
    if (!options_of(opts, options)) {
        return SCADA_E_INVALID;
    }
    // Not used by an observer (section 11.4).
    options.retain_dir.clear();
    scada::tag::clear_os_error();
    std::string detail;
    return hand_over(scada::tag::Attachment::observe(options, detail), out);
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
    // Within the limits of section 14, which fit 16 bits.
    const scada::tag::HeaderContents& contents =
        ctx.attachment->header().contents();
    report.owner_count = static_cast<std::uint16_t>(contents.owners.size());
    report.process_count =
        static_cast<std::uint16_t>(contents.processes.size());
    out = report;
    if (status.state == scada::tag::GenerationState::RETIRING || status.moved) {
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

int scada_tag_observe(scada_ctx** out) {
    return guarded([&] { return observe(nullptr, out); });
}

int scada_tag_observe_opts(const scada_attach_opts* opts, scada_ctx** out) {
    return guarded([&] { return observe(opts, out); });
}

void scada_tag_detach(scada_ctx* ctx) {
    // Takes back the ownership scada_tag_attach handed to the caller.
    const std::unique_ptr<scada_ctx> owned(ctx);
}

SCADA_RT int scada_tag_heartbeat(scada_ctx* ctx) {
    if (ctx == nullptr) {
        return SCADA_E_INVALID;
    }
    return ctx->attachment->heartbeat();
}

int scada_tag_notify(scada_ctx* ctx) {
    if (ctx == nullptr) {
        return SCADA_E_INVALID;
    }
    return guarded([&] { return ctx->attachment->notify(); });
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

int scada_owner_info(scada_ctx* ctx, uint16_t owner_index,
                     scada_owner_info_t* out) {
    if (ctx == nullptr || out == nullptr) {
        return SCADA_E_INVALID;
    }
    return guarded(
        [&] { return ctx->attachment->owner_info(owner_index, *out); });
}

SCADA_RT uint32_t scada_tag_handle(uint16_t owner_index, uint32_t slot) {
    const std::optional<std::uint32_t> handle =
        scada::tag::pack_handle(owner_index, slot);
    return handle.value_or(SCADA_INVALID_HANDLE);
}

int scada_proc_resolve(scada_ctx* ctx, const char* process_name,
                       uint16_t* index) {
    if (ctx == nullptr || process_name == nullptr || index == nullptr) {
        return SCADA_E_INVALID;
    }
    return guarded(
        [&] { return ctx->attachment->proc_resolve(process_name, *index); });
}

SCADA_RT int scada_proc_status(scada_ctx* ctx, uint16_t index,
                               scada_proc_status_t* out) {
    if (ctx == nullptr || out == nullptr) {
        return SCADA_E_INVALID;
    }
    return ctx->attachment->proc_status(index, *out);
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
    if (ctx == nullptr || (buf == nullptr && cap != 0) || len == nullptr) {
        return SCADA_E_INVALID;
    }
    // The caller states that `buf` holds `cap` bytes. A null `buf` with
    // `cap` 0 asks for the length alone and stays a null span.
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

int scada_sub_open(scada_ctx* ctx, const char* owner, scada_sub** out) {
    if (out == nullptr) {
        return SCADA_E_INVALID;
    }
    *out = nullptr;
    if (ctx == nullptr || owner == nullptr) {
        return SCADA_E_INVALID;
    }
    return guarded([&] {
        auto sub = std::make_unique<scada_sub>();
        const int code = scada::tag::Subscription::open(*ctx->attachment, owner,
                                                        sub->subscription);
        if (code == SCADA_OK) {
            // The caller owns it from here and gives it back to
            // scada_sub_close.
            *out = sub.release();
        }
        return code;
    });
}

int scada_sub_next(scada_sub* sub, scada_change* out, int timeout_ms) {
    if (sub == nullptr || out == nullptr || timeout_ms < 0) {
        return SCADA_E_INVALID;
    }
    return guarded([&] { return sub->subscription->next(*out, timeout_ms); });
}

int scada_sub_stats(scada_sub* sub, scada_sub_stats_t* out) {
    if (sub == nullptr || out == nullptr) {
        return SCADA_E_INVALID;
    }
    *out = sub->subscription->stats();
    return SCADA_OK;
}

void scada_sub_close(scada_sub* sub) {
    // Takes back the ownership scada_sub_open handed to the caller.
    const std::unique_ptr<scada_sub> owned(sub);
}

int scada_queue_open(scada_ctx* ctx, const char* queue_name,
                     scada_queue** out) {
    if (out == nullptr) {
        return SCADA_E_INVALID;
    }
    *out = nullptr;
    if (ctx == nullptr || queue_name == nullptr) {
        return SCADA_E_INVALID;
    }
    return guarded([&] {
        auto queue = std::make_unique<scada_queue>();
        const int code =
            scada::tag::Queue::open(*ctx->attachment, queue_name, queue->queue);
        if (code == SCADA_OK) {
            // The caller owns it from here and gives it back to
            // scada_queue_close.
            *out = queue.release();
        }
        return code;
    });
}

SCADA_RT int scada_queue_post(scada_queue* queue, const void* record,
                              size_t size) {
    if (queue == nullptr || record == nullptr) {
        return SCADA_E_INVALID;
    }
    // The caller passes `size` bytes at `record`; the queue checks the size
    // before it reads them.
    return queue->queue->post(std::span<const std::byte>(
        static_cast<const std::byte*>(record), size));
}

int scada_queue_next(scada_queue* queue, void* record, size_t size,
                     int timeout_ms) {
    if (queue == nullptr || record == nullptr || timeout_ms < 0) {
        return SCADA_E_INVALID;
    }
    return guarded([&] {
        return queue->queue->next(
            std::span<std::byte>(static_cast<std::byte*>(record), size),
            timeout_ms);
    });
}

int scada_queue_ack(scada_queue* queue) {
    if (queue == nullptr) {
        return SCADA_E_INVALID;
    }
    return queue->queue->ack();
}

int scada_queue_stats(scada_queue* queue, scada_queue_stats_t* out) {
    if (queue == nullptr || out == nullptr) {
        return SCADA_E_INVALID;
    }
    *out = queue->queue->stats();
    return SCADA_OK;
}

void scada_queue_close(scada_queue* queue) {
    // Takes back the ownership scada_queue_open handed to the caller.
    const std::unique_ptr<scada_queue> owned(queue);
}

int scada_waitset_create(scada_ctx* ctx, scada_waitset** out) {
    if (out == nullptr) {
        return SCADA_E_INVALID;
    }
    *out = nullptr;
    if (ctx == nullptr) {
        return SCADA_E_INVALID;
    }
    return guarded([&] {
        auto set = std::make_unique<scada_waitset>();
        set->wait_set = std::make_unique<scada::tag::WaitSet>(*ctx->attachment);
        // The caller owns it from here and gives it back to
        // scada_waitset_destroy.
        *out = set.release();
        return SCADA_OK;
    });
}

int scada_waitset_add_sub(scada_waitset* set, scada_sub* sub) {
    if (set == nullptr || sub == nullptr) {
        return SCADA_E_INVALID;
    }
    return guarded([&] { return set->wait_set->add_sub(*sub->subscription); });
}

int scada_waitset_add_queue(scada_waitset* set, scada_queue* queue) {
    if (set == nullptr || queue == nullptr) {
        return SCADA_E_INVALID;
    }
    return guarded([&] { return set->wait_set->add_queue(*queue->queue); });
}

int scada_waitset_add_cycle(scada_waitset* set, uint16_t process_index) {
    if (set == nullptr) {
        return SCADA_E_INVALID;
    }
    return guarded([&] { return set->wait_set->add_cycle(process_index); });
}

int scada_waitset_wait(scada_waitset* set, int timeout_ms) {
    if (set == nullptr || timeout_ms < 0) {
        return SCADA_E_INVALID;
    }
    return guarded([&] { return set->wait_set->wait(timeout_ms); });
}

int scada_waitset_fd(scada_waitset* set, int* descriptor) {
    if (set == nullptr || descriptor == nullptr) {
        return SCADA_E_INVALID;
    }
    return guarded([&] { return set->wait_set->fd(*descriptor); });
}

void scada_waitset_destroy(scada_waitset* set) {
    // Takes back the ownership scada_waitset_create handed to the caller.
    const std::unique_ptr<scada_waitset> owned(set);
}

}  // extern "C"
