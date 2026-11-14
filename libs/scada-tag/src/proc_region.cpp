#include "proc_region.h"

#include "layout.h"
#include "region_file.h"
#include "shared_memory.h"

#include "scada/common/fd.h"
#include "scada/common/mapped_region.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/liveness.h"
#include "scada/tag/types.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace scada::tag {

namespace {

constexpr std::uint64_t NANOSECONDS_PER_MILLISECOND = 1'000'000;

// Attempts of `attach` when another process creates the region at the same
// moment: the second attempt finds the region of the other.
constexpr int ATTACH_ATTEMPTS = 2;

// Takes the exclusive lock of section 10 without waiting.
Status lock_exclusively(int descriptor, std::string_view name,
                        std::string& detail) {
    if (::flock(descriptor, LOCK_EX | LOCK_NB) == 0) {
        return {};
    }
    const int error = errno;
    detail = "cannot lock " + std::string(name) + ": " + errno_text(error);
    return error == EWOULDBLOCK
               ? Status(StatusCode::ALREADY_EXISTS,
                        "a process with this name is already attached")
               : Status(StatusCode::IO_ERROR, "cannot lock a region file");
}

// Removes the entry `name`, which is not a usable region, if it belongs to
// `uid` (section 11.2, step 3). An entry another process holds locked is in
// use and stays.
Status remove_own_unusable(int dir_fd, const std::string& name,
                           std::uint32_t uid, std::string& detail) {
    struct stat info {};
    if (::fstatat(dir_fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return {};  // gone meanwhile
        }
        detail = "cannot examine " + name + ": " + errno_text(errno);
        return {StatusCode::IO_ERROR, "cannot examine a region file"};
    }
    if (info.st_uid != uid) {
        detail = name + " is owned by user " + std::to_string(info.st_uid) +
                 ", not by user " + std::to_string(uid);
        return {StatusCode::PERMISSION_DENIED,
                "a region file has the wrong owner"};
    }
    if (S_ISDIR(info.st_mode)) {
        detail = name + " is a directory";
        return {StatusCode::DATA_CORRUPT,
                "a directory is in the place of a region"};
    }
    if (S_ISREG(info.st_mode)) {
        const Fd file = open_at(dir_fd, name,
                                O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (file.valid()) {
            const Status locked = lock_exclusively(file.get(), name, detail);
            if (!locked.ok()) {
                return locked;
            }
        }
    }
    if (::unlinkat(dir_fd, name.c_str(), 0) != 0 && errno != ENOENT) {
        detail = "cannot remove " + name + ": " + errno_text(errno);
        return {StatusCode::IO_ERROR, "cannot remove a region file"};
    }
    return {};
}

}  // namespace

std::string_view to_string(DownReason reason) noexcept {
    switch (reason) {
        case DownReason::NONE:
            return "not down";
        case DownReason::REGION_MISSING:
            return "region missing";
        case DownReason::NOT_RUNNING:
            return "not running";
        case DownReason::HEARTBEAT_STALE:
            return "heartbeat stale";
    }
    return "unknown";
}

RegionIdentity proc_region_identity(
    const ProcRegionParameters& parameters) noexcept {
    RegionIdentity identity;
    identity.kind = RegionKind::PROC;
    identity.generation = parameters.generation;
    identity.size_bytes = proc_region_size(parameters.queue_count);
    identity.owner_index = parameters.process_index;
    identity.queue_id = NO_QUEUE_ID;
    identity.uid = parameters.uid;
    return identity;
}

OwnProcRegion::OwnProcRegion(Fd file, MappedRegion memory, bool reused) noexcept
    : file_(std::move(file)), memory_(std::move(memory)), reused_(reused) {}

ProcHeader& OwnProcRegion::header() const noexcept {
    return shared_at<ProcHeader>(memory_.bytes(), sizeof(RegionHeader));
}

Result<OwnProcRegion> OwnProcRegion::attach(
    int dir_fd, const ProcRegionParameters& parameters, std::string& detail) {
    const std::string name = proc_region_name(parameters.process);
    const RegionIdentity identity = proc_region_identity(parameters);
    // In range: a process id is positive.
    const auto pid = static_cast<std::uint32_t>(::getpid());

    for (int attempt = 0; attempt < ATTACH_ATTEMPTS; ++attempt) {
        Result<OpenedRegion> existing =
            open_region(dir_fd, name, identity, MapAccess::READ_WRITE, detail);
        if (existing.ok()) {
            const Status locked =
                lock_exclusively(existing.value().file.get(), name, detail);
            if (!locked.ok()) {
                return locked;
            }
            OwnProcRegion region(std::move(existing.value().file),
                                 std::move(existing.value().memory), true);
            // The previous life of the process left its state and process
            // id behind. Only this process writes them now: it holds the
            // lock.
            region.set_state(ProcessState::STARTING);
            region.header().pid = pid;
            return region;
        }
        if (existing.status().code() != StatusCode::NOT_FOUND) {
            const Status removed =
                remove_own_unusable(dir_fd, name, parameters.uid, detail);
            if (!removed.ok()) {
                return removed;
            }
        }

        remove_stale_temporaries(dir_fd, name, parameters.uid);
        Result<RegionBuilder> builder =
            RegionBuilder::create(dir_fd, name, identity, detail);
        if (!builder.ok()) {
            return builder.status();
        }
        auto& header = shared_at<ProcHeader>(builder.value().bytes(),
                                             sizeof(RegionHeader));
        // Not shared yet: the file has no name others look for.
        header.pid = pid;
        header.state = to_raw(ProcessState::STARTING);
        // Locked before it gets its name, so that no second process can
        // attach in between. The lock belongs to the open file and stays
        // through the rename.
        const Status locked =
            lock_exclusively(builder.value().descriptor(), name, detail);
        if (!locked.ok()) {
            return locked;
        }
        Result<RegionBuilder::Published> published =
            builder.value().publish(detail);
        if (published.ok()) {
            return OwnProcRegion(std::move(published.value().file),
                                 std::move(published.value().memory), false);
        }
        if (published.status().code() != StatusCode::ALREADY_EXISTS) {
            return published.status();
        }
        // Another process created the region first; look at it.
    }
    return Status(StatusCode::ALREADY_EXISTS,
                  "a process with this name is already attached");
}

void OwnProcRegion::heartbeat(std::int64_t mono_ns) noexcept {
    ProcHeader& proc = header();
    // Relaxed: this thread is the only writer of the counter.
    const std::uint64_t count =
        load_shared(proc.heartbeat, std::memory_order_relaxed);
    store_shared(proc.heartbeat, count + 1, std::memory_order_relaxed);
    // Release: a reader that sees this time also sees the counter.
    store_shared(proc.heartbeat_mono_ns, mono_ns, std::memory_order_release);
}

void OwnProcRegion::set_state(ProcessState state) noexcept {
    // Release: readers load the state with acquire and then the heartbeat.
    store_shared(header().state, to_raw(state), std::memory_order_release);
}

DownReason ProcProbe::observe(int dir_fd,
                              const ProcRegionParameters& parameters,
                              std::uint32_t heartbeat_timeout_ms,
                              std::int64_t now_mono_ns) {
    const std::string name = proc_region_name(parameters.process);
    const RegionIdentity identity = proc_region_identity(parameters);

    struct stat info {};
    if (::fstatat(dir_fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        region_.reset();
        return DownReason::REGION_MISSING;
    }
    // The process may have replaced its region, or shrunk the file
    // (section 3.3): the old mapping must not be used then.
    if (region_.has_value() &&
        (info.st_dev != region_->device || info.st_ino != region_->inode ||
         info.st_size < 0 ||
         static_cast<std::uint64_t>(info.st_size) != identity.size_bytes)) {
        region_.reset();
    }
    if (!region_.has_value()) {
        std::string ignored;
        Result<OpenedRegion> opened =
            open_region(dir_fd, name, identity, MapAccess::READ_ONLY, ignored);
        if (!opened.ok()) {
            return DownReason::REGION_MISSING;
        }
        region_ = std::move(opened).value();
    }

    auto& proc =
        shared_at<ProcHeader>(region_->memory.bytes(), sizeof(RegionHeader));
    // Acquire: pairs with the release stores of the owner.
    if (load_shared(proc.state, std::memory_order_acquire) !=
        to_raw(ProcessState::RUNNING)) {
        return DownReason::NOT_RUNNING;
    }
    const std::int64_t beat =
        load_shared(proc.heartbeat_mono_ns, std::memory_order_acquire);
    if (beat >= now_mono_ns) {
        return DownReason::NONE;  // written after `now` was read
    }
    // Unsigned, so that a nonsense value cannot overflow: the difference of
    // two 64-bit values with beat < now fits 64 bits.
    const std::uint64_t age = static_cast<std::uint64_t>(now_mono_ns) -
                              static_cast<std::uint64_t>(beat);
    const std::uint64_t timeout =
        std::uint64_t{heartbeat_timeout_ms} * NANOSECONDS_PER_MILLISECOND;
    return age > timeout ? DownReason::HEARTBEAT_STALE : DownReason::NONE;
}

}  // namespace scada::tag
