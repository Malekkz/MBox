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

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
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

void OwnProcRegion::count_late_after(
    std::uint32_t heartbeat_timeout_ms) noexcept {
    late_after_ns_ =
        std::uint64_t{heartbeat_timeout_ms} * NANOSECONDS_PER_MILLISECOND;
}

void OwnProcRegion::first_heartbeat(std::int64_t mono_ns) noexcept {
    ProcHeader& proc = header();
    // Relaxed: the release store of the time below publishes it.
    fetch_add_shared(proc.heartbeat, std::uint64_t{1},
                     std::memory_order_relaxed);
    // Release: a reader that sees this time also sees the counter. Stored
    // whatever the region held: a time left by a previous life, even one
    // ahead of the clock, must not hold back this life's heartbeats.
    store_shared(proc.heartbeat_mono_ns, mono_ns, std::memory_order_release);
}

void OwnProcRegion::heartbeat(std::int64_t mono_ns) noexcept {
    ProcHeader& proc = header();
    // Relaxed: the counter orders nothing; the release below publishes it.
    fetch_add_shared(proc.heartbeat, std::uint64_t{1},
                     std::memory_order_relaxed);
    // Advance the published time to `mono_ns` unless another thread has
    // published one as new or newer. Release on success: a reader that
    // sees this time also sees the counter. Relaxed on failure: `previous`
    // is only compared.
    std::int64_t previous =
        load_shared(proc.heartbeat_mono_ns, std::memory_order_relaxed);
    do {
        if (mono_ns <= previous) {
            return;  // not newer: nothing is published or counted
        }
    } while (!compare_exchange_shared(proc.heartbeat_mono_ns, previous, mono_ns,
                                      std::memory_order_release,
                                      std::memory_order_relaxed));

    // This thread published `mono_ns` over `previous`, so the gap between
    // them is its own. The difference of two values with previous <
    // mono_ns fits 64 bits unsigned; it is capped to the signed field.
    const std::uint64_t gap = static_cast<std::uint64_t>(mono_ns) -
                              static_cast<std::uint64_t>(previous);
    constexpr auto LIMIT =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    // In range after the comparison.
    const auto gap_ns = static_cast<std::int64_t>(std::min(gap, LIMIT));
    // Relaxed: diagnosis only; late_beats below publishes it.
    std::int64_t longest =
        load_shared(proc.longest_gap_ns, std::memory_order_relaxed);
    while (gap_ns > longest &&
           !compare_exchange_shared(proc.longest_gap_ns, longest, gap_ns,
                                    std::memory_order_relaxed,
                                    std::memory_order_relaxed)) {
    }
    // Strictly longer: a heartbeat exactly as old as the timeout is not yet
    // stale (proc_liveness), so no reader saw this process down.
    if (late_after_ns_ != 0 && gap > late_after_ns_) {
        store_shared(proc.last_late_mono_ns, mono_ns,
                     std::memory_order_relaxed);
        // Release: a reader that sees the new count also sees the gap
        // fields stored before it. Wraps modulo 2^32; readers compare.
        fetch_add_shared(proc.late_beats, 1U, std::memory_order_release);
    }
}

void OwnProcRegion::set_state(ProcessState state) noexcept {
    // Release: readers load the state with acquire and then the heartbeat.
    store_shared(header().state, to_raw(state), std::memory_order_release);
}

const std::uint32_t* OwnProcRegion::advance_cycle() noexcept {
    std::uint32_t& cycle = header().cycle_seq;
    // Release: a waiter that sees the new count also sees what the cycle
    // wrote before it. Wraps modulo 2^32; waiters only compare.
    fetch_add_shared(cycle, 1U, std::memory_order_release);
    return &cycle;
}

std::uint32_t* cycle_field(std::span<std::byte> region) noexcept {
    if (!fits(region.size(), sizeof(RegionHeader), sizeof(ProcHeader))) {
        return nullptr;
    }
    return &shared_at<ProcHeader>(region, sizeof(RegionHeader)).cycle_seq;
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
        ++mappings_;
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

std::optional<LateBeats> late_beats_of(std::span<std::byte> region) noexcept {
    if (!fits(region.size(), sizeof(RegionHeader), sizeof(ProcHeader))) {
        return std::nullopt;
    }
    auto& proc = shared_at<ProcHeader>(region, sizeof(RegionHeader));
    LateBeats beats;
    // Acquire: pairs with the release increment of the owner, so the gap
    // fields are at least as new as the count.
    beats.count = load_shared(proc.late_beats, std::memory_order_acquire);
    beats.longest_gap_ns =
        load_shared(proc.longest_gap_ns, std::memory_order_relaxed);
    beats.last_late_mono_ns =
        load_shared(proc.last_late_mono_ns, std::memory_order_relaxed);
    return beats;
}

std::optional<LateBeats> ProcProbe::late_beats() const noexcept {
    if (!region_.has_value()) {
        return std::nullopt;
    }
    return late_beats_of(region_->memory.bytes());
}

}  // namespace scada::tag
