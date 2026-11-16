#include "scada/ctl/database.h"

#include "scada/ctl/arguments.h"
#include "scada/ctl/format.h"
#include "scada/ctl/pattern.h"
#include "scada/ctl/view.h"
#include "scada/tag/scada_tag.h"

#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace scada::ctl {

namespace {

// After a generation change scada-tagd needs a moment to finish the new
// one; observe is tried this often, this far apart.
constexpr int REOPEN_ATTEMPTS = 20;
constexpr int REOPEN_PAUSE_MS = 250;

constexpr std::string_view DEFAULT_BASE_DIR = "/dev/shm/scada";

std::string text_of(const char* field) {
    return {field};
}

}  // namespace

std::string describe_observe_error(int code, int os_error,
                                   const std::string& base_dir) {
    std::string where;
    append_terminal_safe(where, base_dir.empty() ? DEFAULT_BASE_DIR : base_dir);
    switch (code) {
        case SCADA_E_PERMISSION:
            return "no permission to read the tag database in " + where +
                   ": scadactl must run as a member of the group scada, and "
                   "the database must have the owner and modes of its "
                   "contract";
        case SCADA_E_LAYOUT:
            return "the tag database in " + where +
                   " has another layout version than this scadactl, or its "
                   "header is not valid";
        case SCADA_E_RETIRING:
            return "the tag database in " + where +
                   " is changing its generation; try again";
        case SCADA_E_INVALID:
            return "the options for the tag database are not valid";
        case SCADA_E_IO:
            if (os_error == ENOENT) {
                return "there is no tag database in " + where +
                       ": is scada-tagd running?";
            }
            if (os_error == EAGAIN) {
                return "the tag database in " + where +
                       " is not running yet; try again";
            }
            return "cannot read the tag database in " + where + ": " +
                   std::system_category().message(os_error);
        default:
            return "cannot read the tag database in " + where + " (code " +
                   std::to_string(code) + ")";
    }
}

int Database::observe(const std::string& base_dir, std::int64_t tagd_uid,
                      scada_ctx*& ctx) {
    const scada_attach_opts options{
        .base_dir = base_dir.empty() ? nullptr : base_dir.c_str(),
        .retain_dir = nullptr,
        .tagd_uid = tagd_uid};
    return scada_tag_observe_opts(&options, &ctx);
}

std::unique_ptr<Database> Database::open(const Arguments& args,
                                         std::string& detail) {
    const std::int64_t tagd_uid =
        args.tagd_uid.has_value() ? std::int64_t{*args.tagd_uid} : -1;
    scada_ctx* ctx = nullptr;
    const int code = observe(args.base_dir, tagd_uid, ctx);
    if (code != SCADA_OK) {
        detail = describe_observe_error(code, errno, args.base_dir);
        return nullptr;
    }
    return std::make_unique<Database>(Key{}, args.base_dir, tagd_uid, ctx);
}

Database::~Database() {
    scada_tag_detach(ctx_);
}

Database::Reopened Database::reopen(std::string& detail, int stop) {
    scada_tag_detach(ctx_);
    ctx_ = nullptr;
    int code = SCADA_OK;
    int os_error = 0;
    for (int attempt = 0; attempt < REOPEN_ATTEMPTS; ++attempt) {
        code = observe(base_dir_, tagd_uid_, ctx_);
        os_error = errno;
        if (code == SCADA_OK) {
            return Reopened::YES;
        }
        // Only a database in the middle of a change is worth waiting for.
        const bool changing =
            code == SCADA_E_RETIRING ||
            (code == SCADA_E_IO && (os_error == ENOENT || os_error == EAGAIN));
        if (!changing) {
            break;
        }
        // poll ignores a negative descriptor: then this only waits.
        pollfd wait{.fd = stop, .events = POLLIN, .revents = 0};
        if (::poll(&wait, 1, REOPEN_PAUSE_MS) > 0) {
            return Reopened::STOPPED;
        }
    }
    detail = describe_observe_error(code, os_error, base_dir_);
    return Reopened::NO;
}

int Database::status(scada_status& out) {
    return scada_tag_status(ctx_, &out);
}

std::vector<TagRef> Database::find(const TagPattern& pattern) {
    std::vector<TagRef> found;
    scada_status status{};
    static_cast<void>(scada_tag_status(ctx_, &status));
    for (std::uint16_t owner = 0; owner < status.owner_count; ++owner) {
        scada_owner_info_t info{};
        if (scada_owner_info(ctx_, owner, &info) != SCADA_OK) {
            continue;
        }
        for (std::uint32_t slot = 0; slot < info.tag_count; ++slot) {
            scada_tag_info_t tag{};
            if (scada_tag_info(ctx_, scada_tag_handle(owner, slot), &tag) !=
                SCADA_OK) {
                continue;
            }
            const std::string name = text_of(std::data(tag.name));
            if (!pattern.matches(name)) {
                continue;
            }
            // Resolved so that the regions of its owner are mapped; a tag
            // that was never resolved reads as if its owner were down.
            std::uint32_t handle = SCADA_INVALID_HANDLE;
            if (scada_tag_resolve(ctx_, std::data(tag.name), &handle) !=
                SCADA_OK) {
                continue;
            }
            found.push_back({.name = name,
                             .owner = text_of(std::data(tag.owner)),
                             .owner_index = owner,
                             .handle = handle,
                             .type = tag.type});
        }
    }
    std::ranges::sort(found, {}, &TagRef::name);
    return found;
}

std::optional<TagRef> Database::find_one(const std::string& name) {
    std::uint32_t handle = SCADA_INVALID_HANDLE;
    if (scada_tag_resolve(ctx_, name.c_str(), &handle) != SCADA_OK) {
        return std::nullopt;
    }
    scada_tag_info_t tag{};
    if (scada_tag_info(ctx_, handle, &tag) != SCADA_OK) {
        return std::nullopt;
    }
    return TagRef{.name = name,
                  .owner = text_of(std::data(tag.owner)),
                  .owner_index = tag.owner_index,
                  .handle = handle,
                  .type = tag.type};
}

TagRow Database::read(const TagRef& tag) {
    TagRow row;
    row.name = tag.name;
    row.owner = tag.owner;
    row.type = tag.type;
    if (tag.type == SCADA_TYPE_STRING) {
        std::string text(SCADA_STRING_MAX, '\0');
        std::size_t length = 0;
        row.read_code = scada_tag_read_string(ctx_, tag.handle, text.data(),
                                              text.size(), &length, &row.value);
        text.resize(row.read_code == SCADA_OK ? length : 0);
        row.text = std::move(text);
    } else {
        row.read_code = scada_tag_read(ctx_, tag.handle, &row.value);
    }
    return row;
}

StatusView Database::status_view() {
    StatusView view;
    view.status_code = scada_tag_status(ctx_, &view.status);
    for (std::uint16_t index = 0; index < view.status.process_count; ++index) {
        ProcessRow row;
        // A process not resolved yet gives SCADA_E_INVALID with its name
        // from the process table; resolved by that name, which maps its
        // region, it gives its state.
        const int first = scada_proc_status(ctx_, index, &row.status);
        if (first == SCADA_E_INVALID &&
            row.status.down_reason == SCADA_DOWN_NOT_RESOLVED) {
            std::uint16_t resolved = 0;
            if (scada_proc_resolve(ctx_, std::data(row.status.name),
                                   &resolved) == SCADA_OK) {
                static_cast<void>(
                    scada_proc_status(ctx_, resolved, &row.status));
            }
        } else if (first != SCADA_OK) {
            continue;
        }
        view.processes.push_back(row);
    }
    for (std::uint16_t index = 0; index < view.status.owner_count; ++index) {
        OwnerRow row;
        if (scada_owner_info(ctx_, index, &row.info) != SCADA_OK) {
            continue;
        }
        // Found by its index, not by its place: a process whose status
        // could not be read has no row. An owner without its process is
        // shown down.
        const auto process = std::ranges::find_if(
            view.processes, [&row](const ProcessRow& candidate) {
                return candidate.status.process_index == row.info.process_index;
            });
        if (process != view.processes.end()) {
            row.process = text_of(std::data(process->status.name));
            row.owner_down = process->status.down != 0;
        } else {
            row.owner_down = true;
        }
        view.owners.push_back(row);
    }
    return view;
}

}  // namespace scada::ctl
