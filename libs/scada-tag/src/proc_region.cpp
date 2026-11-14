#include "proc_region.h"

#include "layout.h"
#include "own_region.h"
#include "region_file.h"
#include "shared_memory.h"

#include "scada/common/fd.h"
#include "scada/common/mapped_region.h"
#include "scada/common/result.h"
#include "scada/tag/liveness.h"
#include "scada/tag/types.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace scada::tag {

namespace {

constexpr std::uint64_t NANOSECONDS_PER_MILLISECOND = 1'000'000;

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
    // In range: a process id is positive.
    const auto pid = static_cast<std::uint32_t>(::getpid());

    OwnRegionSpec spec;
    spec.name = proc_region_name(parameters.process);
    spec.identity = proc_region_identity(parameters);
    spec.lock = true;
    spec.initialise = [pid](std::span<std::byte> bytes) {
        auto& header = shared_at<ProcHeader>(bytes, sizeof(RegionHeader));
        header.pid = pid;
        header.state = to_raw(ProcessState::STARTING);
    };
    Result<OwnRegion> attached = attach_own_region(dir_fd, spec, detail);
    if (!attached.ok()) {
        return attached.status();
    }
    OwnProcRegion region(std::move(attached.value().file),
                         std::move(attached.value().memory),
                         attached.value().reused);
    if (region.reused()) {
        // The previous life of the process left its state and process id
        // behind. Only this process writes them now: it holds the lock.
        region.set_state(ProcessState::STARTING);
        region.header().pid = pid;
    }
    return region;
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

    return proc_liveness(region_->memory.bytes(), heartbeat_timeout_ms,
                         now_mono_ns);
}

DownReason proc_liveness(std::span<std::byte> region,
                         std::uint32_t heartbeat_timeout_ms,
                         std::int64_t now_mono_ns) noexcept {
    if (!fits(region.size(), sizeof(RegionHeader), sizeof(ProcHeader))) {
        return DownReason::REGION_MISSING;
    }
    auto& proc = shared_at<ProcHeader>(region, sizeof(RegionHeader));
    // Acquire: pairs with the release stores of the owner.
    if (load_shared(proc.state, std::memory_order_acquire) !=
        to_raw(ProcessState::RUNNING)) {
        return DownReason::NOT_RUNNING;
    }
    const std::int64_t beat =
        load_shared(proc.heartbeat_mono_ns, std::memory_order_acquire);
    const std::uint64_t timeout =
        std::uint64_t{heartbeat_timeout_ms} * NANOSECONDS_PER_MILLISECOND;
    // A heartbeat newer than `now` was written after `now` was read, which
    // is fresh, but only by as much as the reader can be delayed between
    // reading its clock and the heartbeat. One further ahead than the
    // timeout comes from a corrupt region or another clock, and must not
    // keep a dead process alive. Unsigned, so that a nonsense value cannot
    // overflow: the difference of two 64-bit values fits 64 bits.
    const std::uint64_t distance =
        beat >= now_mono_ns ? static_cast<std::uint64_t>(beat) -
                                  static_cast<std::uint64_t>(now_mono_ns)
                            : static_cast<std::uint64_t>(now_mono_ns) -
                                  static_cast<std::uint64_t>(beat);
    return distance > timeout ? DownReason::HEARTBEAT_STALE : DownReason::NONE;
}

}  // namespace scada::tag
