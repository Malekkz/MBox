#include "scada/tag/generation_host.h"

#include "layout.h"
#include "proc_region.h"
#include "queue_region.h"
#include "region_file.h"
#include "resolved_tag_set.h"
#include "shared_memory.h"

#include "scada/common/clock.h"
#include "scada/common/fd.h"
#include "scada/common/mapped_region.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/header_builder.h"
#include "scada/tag/liveness.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <fcntl.h>
#include <stdio.h>  // NOLINT(modernize-deprecated-headers): renameat is declared here, not in <cstdio>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::tag {

namespace {

// Mode of a generation directory between mkdir and the explicit chmod.
constexpr mode_t DIRECTORY_CREATION_MODE = 0700;

// Only the permission bits, the set-id bits and the sticky bit of a mode.
constexpr mode_t MODE_BITS = 07777;

// Longest target of `current` that can be a generation name: "gen-" and 20
// digits. A longer target is not one.
constexpr std::size_t MAX_LINK_TARGET = 64;

constexpr std::size_t GLOBAL_HEADER_OFFSET = sizeof(RegionHeader);

constexpr int OPEN_DIRECTORY = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

// One process of the generation, as the liveness check sees it.
struct Peer {
    ProcRegionParameters region;
    std::uint32_t heartbeat_timeout_ms = 0;
    std::size_t entry_offset = 0;  // of its ProcessEntry in `header`
    bool self = false;             // scada-tagd itself
    ProcProbe probe;
    // The late-heartbeat count seen at the previous check, and the mapping
    // it was seen in; nothing before the first sample of a mapping.
    std::optional<std::uint32_t> late_seen;
    std::uint64_t late_mapping = 0;
    // The same for the count of lives (`starts`), sampled only while the
    // process is up.
    std::optional<std::uint32_t> starts_seen;
    std::uint64_t starts_mapping = 0;
    // Checks that found the process up after it was down since the last
    // sample of `starts`: each is a life seen as down and up. Kept when a
    // check finds it up but cannot sample (it left RUNNING in between).
    std::uint32_t ups_since_sample = 0;
    // Down at the latest check.
    bool down = true;
};

// One reliable queue, as the liveness check sees it (issue #53).
struct WatchedQueue {
    std::string name;
    RegionIdentity identity;
    std::uint32_t capacity = 0;
    std::size_t queue_id = 0;
    std::uint16_t consumer_process = 0;
    std::uint16_t producer_process = 0;
    // EVENTS and CMD: a missing file is reported (issue #53).
    bool report_missing = false;
    // The queue region, mapped read-only, without its descriptor: a
    // generation may have 4,096 queues.
    std::optional<OpenedRegion> region;
    // Broken at the last check that could judge it.
    bool broken = false;
    // Checks in a row without a usable file while the producer was not
    // down, and whether that was reported.
    std::uint32_t missing_checks = 0;
    bool missing_reported = false;
    // The file seen at the latest check that found one, and its
    // `write_seq` then; kept while the name is missing, so that a file
    // created anew is told from the old one (issue #53).
    bool file_seen = false;
    dev_t last_device = 0;
    ino_t last_inode = 0;
    std::uint64_t last_written = 0;
    // The consumer's cursor at that check, if it was readable.
    bool last_cursor_known = false;
    std::uint64_t last_cursor = 0;
};

// Checks in a row without a queue file after which it is reported.
constexpr std::uint32_t MISSING_CHECKS_REPORTED = 2;

// Keeps `region` mapped to the file `name` of the directory open as
// `dir_fd`, as ProcProbe does for `proc` regions: drops the mapping when the
// name is gone, refers to another file or the file was shrunk (section
// 3.3), and maps the file again if it passes the checks of section 3.1.
// True if a mapping is held afterwards.
bool follow_region(int dir_fd, const std::string& name,
                   const RegionIdentity& identity,
                   std::optional<OpenedRegion>& region) {
    struct stat info {};
    if (::fstatat(dir_fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        region.reset();
        return false;
    }
    if (region.has_value() &&
        (info.st_dev != region->device || info.st_ino != region->inode ||
         info.st_size < 0 ||
         static_cast<std::uint64_t>(info.st_size) != identity.size_bytes)) {
        region.reset();
    }
    if (region.has_value()) {
        return true;
    }
    std::string ignored;
    Result<OpenedRegion> opened =
        open_region(dir_fd, name, identity, MapAccess::READ_ONLY, ignored);
    if (!opened.ok()) {
        return false;
    }
    region = std::move(opened).value();
    region->file.reset();
    return true;
}

// Region file names of the generation, each with the user that writes it.
using ExpectedEntries = std::map<std::string, std::uint32_t, std::less<>>;

// The generation directory with `header` mapped read-write.
struct OpenGeneration {
    std::uint64_t generation = 0;
    Fd directory;
    MappedRegion header;
};

Fd open_directory_at(int dir_fd, const std::string& name) {
    return open_at(dir_fd, name, OPEN_DIRECTORY);
}

// The generation `current` points to. Nothing if there is no `current` or
// its target is not a generation name, which `notes` then says. An error if
// `current` is not a symbolic link: only scada-tagd can create entries in
// the base directory, so this is damage a person has to look at.
Result<std::optional<std::uint64_t>> read_current(
    int base_fd, std::vector<std::string>& notes, std::string& detail) {
    std::array<char, MAX_LINK_TARGET> target{};
    const std::string link(CURRENT_LINK_NAME);
    const ssize_t length =
        ::readlinkat(base_fd, link.c_str(), target.data(), target.size());
    if (length < 0) {
        if (errno == ENOENT) {
            return std::optional<std::uint64_t>();
        }
        detail = "cannot read the link current: " + errno_text(errno);
        return errno == EINVAL
                   ? Status(StatusCode::FAILED_PRECONDITION,
                            "current is not a symbolic link")
                   : Status(StatusCode::IO_ERROR, "cannot read current");
    }
    // In range: readlinkat returns at most the size of the buffer.
    const std::string_view name(target.data(),
                                static_cast<std::size_t>(length));
    const std::optional<std::uint64_t> generation =
        name.size() < target.size() ? parse_generation_dir_name(name)
                                    : std::nullopt;
    if (!generation.has_value()) {
        notes.emplace_back("current does not point to a generation");
    }
    return generation;
}

// True if a process holds the lock on a `proc` region in the generation
// directory `name` (section 10): the generation has a live process.
bool has_live_process(int base_fd, const std::string& name) {
    const Fd directory = open_directory_at(base_fd, name);
    if (!directory.valid()) {
        return false;
    }
    std::string ignored;
    const Result<std::vector<std::string>> entries =
        list_directory(directory.get(), ignored);
    if (!entries.ok()) {
        return false;
    }
    return std::ranges::any_of(entries.value(), [&](const std::string& entry) {
        if (!entry.starts_with("proc.")) {
            return false;
        }
        const Fd file = open_at(directory.get(), entry,
                                O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        // The lock taken here, if any, ends when `file` is closed.
        return file.valid() && ::flock(file.get(), LOCK_EX | LOCK_NB) != 0 &&
               errno == EWOULDBLOCK;
    });
}

// Removes the generation directory `name` with what is in it. Best effort
// (section 3.3): false if something stays.
bool remove_generation(int base_fd, const std::string& name) {
    const Fd directory = open_directory_at(base_fd, name);
    if (directory.valid()) {
        std::string ignored;
        const Result<std::vector<std::string>> entries =
            list_directory(directory.get(), ignored);
        if (entries.ok()) {
            for (const std::string& entry : entries.value()) {
                if (::unlinkat(directory.get(), entry.c_str(), 0) != 0 &&
                    errno == EISDIR) {
                    static_cast<void>(::unlinkat(directory.get(), entry.c_str(),
                                                 AT_REMOVEDIR));
                }
            }
        }
    }
    return ::unlinkat(base_fd, name.c_str(), AT_REMOVEDIR) == 0;
}

// The bytes all regions of the tag set take together.
std::uint64_t total_region_bytes(const TagSet& tag_set,
                                 const ResolvedTagSet& resolved,
                                 std::size_t header_bytes) {
    std::uint64_t total = header_bytes;
    total +=
        resolved.processes.size() * proc_region_size(resolved.queues.size());
    for (const ResolvedOwner& owner : resolved.owners) {
        total += tags_region_size(owner.tag_count);
        if (owner.string_slots != 0) {
            total += strings_region_size(owner.string_slots);
        }
        total += changes_region_size(
            tag_set.owners.at(owner.definition).change_ring_capacity);
    }
    for (const ResolvedQueue& queue : resolved.queues) {
        total += queue_region_size(tag_set.queues.at(queue.definition).capacity,
                                   queue_record_size(queue.kind));
    }
    return total;
}

// Fails unless the file system of the base directory has room for every
// region of the generation, so that a generation is not published whose
// processes then cannot attach.
Status check_room(int base_fd, std::uint64_t needed, std::string& detail) {
    struct statvfs info {};
    if (::fstatvfs(base_fd, &info) != 0) {
        detail = "cannot examine the file system of the base directory: " +
                 errno_text(errno);
        return {StatusCode::IO_ERROR, "cannot examine the file system"};
    }
    const std::uint64_t blocks = info.f_bavail;
    const std::uint64_t block_size = info.f_frsize;
    // The product of two values from the operating system: checked.
    const bool overflows =
        block_size != 0 &&
        blocks > std::numeric_limits<std::uint64_t>::max() / block_size;
    if (!overflows && blocks * block_size < needed) {
        detail = "the regions need " + std::to_string(needed) +
                 " bytes, the file system of the base directory has " +
                 std::to_string(blocks * block_size) + " bytes free";
        return {StatusCode::RESOURCE_EXHAUSTED,
                "no room for the regions of the generation"};
    }
    return {};
}

RegionIdentity header_identity(std::uint64_t generation, std::size_t size,
                               std::uint32_t tagd_uid) {
    RegionIdentity identity;
    identity.kind = RegionKind::HEADER;
    identity.generation = generation;
    identity.size_bytes = size;
    identity.owner_index = 0;
    identity.queue_id = NO_QUEUE_ID;
    identity.uid = tagd_uid;
    return identity;
}

// The offset of the ProcessEntry of process `index` in a header image.
std::size_t process_entry_offset(std::span<const std::byte> image,
                                 std::size_t index) {
    const auto global = copy_from<GlobalHeader>(image, GLOBAL_HEADER_OFFSET);
    // The image was built by this library: the offset is in range.
    return static_cast<std::size_t>(global.process_table_offset) +
           (index * sizeof(ProcessEntry));
}

// Tries to reuse the generation `generation` (section 11.1, step 2).
// `image` is the header this library builds for the tag set, with
// generation `generation` and creation time 0. Nothing, with the reason in
// `notes`, if the generation cannot be reused.
std::optional<OpenGeneration> try_reuse(int base_fd, std::uint64_t generation,
                                        const HeaderImage& image,
                                        std::size_t process_count,
                                        std::uint32_t tagd_uid,
                                        std::vector<std::string>& notes) {
    const std::string name = generation_dir_name(generation);
    const auto refuse = [&](const std::string& why) {
        notes.push_back(name + " is not reused: " + why);
        return std::optional<OpenGeneration>();
    };

    OpenGeneration open;
    open.generation = generation;
    open.directory = open_directory_at(base_fd, name);
    if (!open.directory.valid()) {
        return refuse("cannot open the directory: " + errno_text(errno));
    }
    struct stat info {};
    if (::fstat(open.directory.get(), &info) != 0 || info.st_uid != tagd_uid ||
        (info.st_mode & MODE_BITS) != GENERATION_DIR_MODE) {
        return refuse("the directory has the wrong owner or mode");
    }

    std::string detail;
    Result<OpenedRegion> header =
        open_region(open.directory.get(), HEADER_REGION_NAME,
                    header_identity(generation, image.bytes.size(), tagd_uid),
                    MapAccess::READ_WRITE, detail);
    if (!header.ok()) {
        return refuse(detail);
    }
    const std::span<std::byte> bytes = header.value().memory.bytes();
    auto& global = shared_at<GlobalHeader>(bytes, GLOBAL_HEADER_OFFSET);
    if (load_shared(global.state, std::memory_order_acquire) !=
        to_raw(GenerationState::RUNNING)) {
        return refuse("its state is not RUNNING");
    }
    if (global.config_hash != image.config_hash) {
        return refuse("the tag set changed");
    }

    // The whole header must be the one this library builds for the tag set:
    // an equal hash over a header of another build of the library, or over
    // a damaged one, is not enough. Nobody writes the header now, because
    // this process holds the lock on the base directory. The fields that
    // differ by design are set to what the image has.
    std::vector<std::byte> snapshot(bytes.begin(), bytes.end());
    copy_to(snapshot, offsetof(RegionHeader, ready), std::uint32_t{0});
    copy_to(snapshot, GLOBAL_HEADER_OFFSET + offsetof(GlobalHeader, state),
            to_raw(GenerationState::BUILDING));
    copy_to(snapshot,
            GLOBAL_HEADER_OFFSET + offsetof(GlobalHeader, created_time_ns),
            std::int64_t{0});
    for (std::size_t i = 0; i < process_count; ++i) {
        copy_to(
            snapshot,
            process_entry_offset(image.bytes, i) + offsetof(ProcessEntry, down),
            std::uint32_t{0});
    }
    if (!std::ranges::equal(snapshot, image.bytes)) {
        return refuse("its header is not the one this tag set builds");
    }
    open.header = std::move(header.value().memory);
    return open;
}

// Sets the state of the generation `generation`, which `current` no longer
// points to, to RETIRING (section 11.3, step 1). Best effort: a header that
// cannot be used is left alone and `notes` says so.
void retire(int base_fd, std::uint64_t generation, std::uint32_t tagd_uid,
            std::vector<std::string>& notes) {
    const std::string name = generation_dir_name(generation);
    const Fd directory = open_directory_at(base_fd, name);
    struct stat info {};
    const Fd file =
        directory.valid()
            ? open_at(directory.get(), std::string(HEADER_REGION_NAME),
                      O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK)
            : Fd();
    const std::size_t needed = sizeof(RegionHeader) + sizeof(GlobalHeader);
    if (!file.valid() || ::fstat(file.get(), &info) != 0 ||
        !S_ISREG(info.st_mode) || info.st_uid != tagd_uid || info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) < needed) {
        notes.push_back(name +
                        " could not be set to RETIRING: no usable header");
        return;
    }
    const Result<MappedRegion> memory =
        MappedRegion::map(file.get(), needed, MapAccess::READ_WRITE);
    if (!memory.ok()) {
        notes.push_back(name + " could not be set to RETIRING: cannot map");
        return;
    }
    auto& region = shared_at<RegionHeader>(memory.value().bytes(), 0);
    if (load_shared(region.ready, std::memory_order_acquire) != 1U ||
        region.magic != REGION_MAGIC ||
        region.layout_version != LAYOUT_VERSION ||
        region.kind != to_raw(RegionKind::HEADER) ||
        region.generation != generation) {
        notes.push_back(name + " could not be set to RETIRING: another layout");
        return;
    }
    store_shared(
        shared_at<GlobalHeader>(memory.value().bytes(), GLOBAL_HEADER_OFFSET)
            .state,
        to_raw(GenerationState::RETIRING), std::memory_order_release);
    notes.push_back(name + " is set to RETIRING");
}

// Points `current` at the generation directory `target`: a symbolic link
// under a temporary name, renamed over `current` (section 11.1, step 3).
Status switch_current(int base_fd, const std::string& target,
                      std::string& detail) {
    const std::string temporary = temporary_name(CURRENT_LINK_NAME, ::getpid());
    const std::string link(CURRENT_LINK_NAME);
    static_cast<void>(::unlinkat(base_fd, temporary.c_str(), 0));
    if (::symlinkat(target.c_str(), base_fd, temporary.c_str()) != 0) {
        detail =
            "cannot create the link " + temporary + ": " + errno_text(errno);
        return {StatusCode::IO_ERROR, "cannot create the link current"};
    }
    if (::renameat(base_fd, temporary.c_str(), base_fd, link.c_str()) != 0) {
        detail = "cannot switch current: " + errno_text(errno);
        static_cast<void>(::unlinkat(base_fd, temporary.c_str(), 0));
        return {StatusCode::IO_ERROR, "cannot switch current"};
    }
    return {};
}

// Removes what earlier runs left in the base directory (section 11.1, step
// 3) and returns the highest generation number seen, `current` included.
Result<std::uint64_t> clean_base_directory(int base_fd,
                                           std::optional<std::uint64_t> current,
                                           std::vector<std::string>& notes,
                                           std::string& detail) {
    const Result<std::vector<std::string>> entries =
        list_directory(base_fd, detail);
    if (!entries.ok()) {
        return entries.status();
    }
    std::uint64_t highest = current.value_or(0);
    for (const std::string& entry : entries.value()) {
        if (is_temporary_name_of(entry, CURRENT_LINK_NAME)) {
            // Only scada-tagd writes here, and this instance holds the lock.
            static_cast<void>(::unlinkat(base_fd, entry.c_str(), 0));
            continue;
        }
        const std::optional<std::uint64_t> generation =
            parse_generation_dir_name(entry);
        if (!generation.has_value()) {
            continue;
        }
        highest = std::max(highest, *generation);
        if (generation == current) {
            continue;
        }
        if (has_live_process(base_fd, entry)) {
            notes.push_back(entry + " stays: a process is attached to it");
        } else if (remove_generation(base_fd, entry)) {
            notes.push_back(entry + " was removed");
        } else {
            notes.push_back(entry + " could not be removed");
        }
    }
    return highest;
}

// Creates the generation directory `name` with the mode and group of
// section 3.1 and returns it open.
Result<Fd> create_generation_directory(int base_fd, const std::string& name,
                                       std::string& detail) {
    if (::mkdirat(base_fd, name.c_str(), DIRECTORY_CREATION_MODE) != 0) {
        detail = "cannot create " + name + ": " + errno_text(errno);
        return Status(StatusCode::IO_ERROR,
                      "cannot create the generation directory");
    }
    Fd directory = open_directory_at(base_fd, name);
    struct stat base {};
    struct stat created {};
    // The mode is set explicitly: mkdir reduced it by the umask. The group
    // is inherited from the base directory, which is set-group-id; if a file
    // system does not do that, it is set here.
    if (!directory.valid() || ::fstat(base_fd, &base) != 0 ||
        ::fstat(directory.get(), &created) != 0 ||
        (created.st_gid != base.st_gid &&
         ::fchown(directory.get(), static_cast<uid_t>(-1), base.st_gid) != 0) ||
        ::fchmod(directory.get(), GENERATION_DIR_MODE) != 0) {
        detail = "cannot set up " + name + ": " + errno_text(errno);
        static_cast<void>(::unlinkat(base_fd, name.c_str(), AT_REMOVEDIR));
        return Status(StatusCode::IO_ERROR,
                      "cannot set up the generation directory");
    }
    return directory;
}

// Builds the generation `generation` and switches `current` to it (section
// 11.1, step 3). On an error nothing of it stays.
//
// Every process but scada-tagd starts down: none has attached to a new
// generation (section 10.1, region missing). A process that attaches before
// the first liveness check, which it can as soon as `current` is switched,
// is then a change and is reported as up (decision D-38).
Result<OpenGeneration> create_generation(int base_fd, std::uint64_t generation,
                                         const TagSet& tag_set,
                                         const ResolvedTagSet& resolved,
                                         std::uint32_t tagd_uid,
                                         std::string& detail) {
    Result<HeaderImage> image = build_header(
        tag_set, {.generation = generation, .created_time_ns = realtime_ns()},
        detail);
    if (!image.ok()) {
        return image.status();
    }
    const std::string name = generation_dir_name(generation);
    Result<Fd> directory = create_generation_directory(base_fd, name, detail);
    if (!directory.ok()) {
        return directory.status();
    }
    OpenGeneration open;
    open.generation = generation;
    open.directory = std::move(directory).value();

    const auto build = [&]() -> Status {
        Result<RegionBuilder> builder = RegionBuilder::create(
            open.directory.get(), HEADER_REGION_NAME,
            header_identity(generation, image.value().bytes.size(), tagd_uid),
            detail);
        if (!builder.ok()) {
            return builder.status();
        }
        // The image starts with the same RegionHeader the builder wrote,
        // `ready` 0, and has state BUILDING.
        std::ranges::copy(image.value().bytes, builder.value().bytes().begin());
        for (std::size_t i = 0; i < resolved.processes.size(); ++i) {
            if (tag_set.processes.at(resolved.processes[i]).name !=
                TAGD_PROCESS_NAME) {
                copy_to(builder.value().bytes(),
                        process_entry_offset(image.value().bytes, i) +
                            offsetof(ProcessEntry, down),
                        std::uint32_t{1});
            }
        }
        Result<RegionBuilder::Published> published =
            builder.value().publish(detail);
        if (!published.ok()) {
            return published.status();
        }
        open.header = std::move(published.value().memory);
        store_shared(
            shared_at<GlobalHeader>(open.header.bytes(), GLOBAL_HEADER_OFFSET)
                .state,
            to_raw(GenerationState::RUNNING), std::memory_order_release);
        return switch_current(base_fd, name, detail);
    };
    if (const Status built = build(); !built.ok()) {
        open.header.reset();
        open.directory.reset();
        static_cast<void>(remove_generation(base_fd, name));
        return built;
    }
    return open;
}

// The name of the region a temporary name belongs to: ".tmp.<name>.<pid>"
// gives "<name>". Empty if `entry` is not a temporary name.
std::string_view region_of_temporary(std::string_view entry) noexcept {
    if (!entry.starts_with(TEMPORARY_PREFIX)) {
        return {};
    }
    entry.remove_prefix(TEMPORARY_PREFIX.size());
    const std::size_t dot = entry.rfind('.');
    if (dot == std::string_view::npos) {
        return {};
    }
    const std::string_view name = entry.substr(0, dot);
    // The exact form, digits included, is checked by the caller with
    // is_temporary_name_of.
    return name;
}

}  // namespace

// Everything a GenerationHost holds. The public class forwards to it.
class GenerationHost::Impl {
public:
    // `GenerationHost::start`.
    [[nodiscard]] static Result<std::unique_ptr<Impl>> start(
        const TagSet& tag_set, const HostOptions& options, std::string& detail);

    [[nodiscard]] std::uint64_t generation() const noexcept {
        return open_.generation;
    }
    [[nodiscard]] bool reused() const noexcept { return reused_; }
    [[nodiscard]] const std::vector<std::string>& notes() const noexcept {
        return notes_;
    }
    [[nodiscard]] std::uint32_t heartbeat_check_ms() const noexcept {
        return heartbeat_check_ms_;
    }
    [[nodiscard]] std::uint32_t heartbeat_timeout_ms() const noexcept {
        return heartbeat_timeout_ms_;
    }

    void heartbeat(std::int64_t now_mono_ns) noexcept {
        proc_.heartbeat(now_mono_ns);
    }
    [[nodiscard]] CheckReport check(std::int64_t now_mono_ns);
    [[nodiscard]] std::optional<bool> is_down(
        std::string_view process) const noexcept;
    void stop() noexcept { proc_.set_state(ProcessState::STOPPING); }

private:
    // Opens and locks the base directory and reads `current`.
    [[nodiscard]] Result<std::optional<std::uint64_t>> open_base(
        const HostOptions& options, std::string& detail);

    // Reuses the generation `current`, if there is one and it can be
    // reused, or builds the next one (section 11.1, steps 2 and 3).
    [[nodiscard]] Status open_generation(const TagSet& tag_set,
                                         const ResolvedTagSet& resolved,
                                         std::optional<std::uint64_t> current,
                                         std::string& detail);

    // Fills `peers_` and `expected_` from the tag set. `image` is the
    // header of the generation.
    void describe(const TagSet& tag_set, const ResolvedTagSet& resolved,
                  std::span<const std::byte> image);

    // Attaches `proc.tagd`, sets it RUNNING and writes the first heartbeat.
    [[nodiscard]] Status attach_own_region(std::string& detail);

    // The user that must own `entry`, or nothing if the header does not
    // list it.
    [[nodiscard]] std::optional<std::uint32_t> owner_of(
        std::string_view entry) const;

    // Removes what does not belong in the generation directory.
    void sweep(CheckReport& report);

    // Adds to `report` the late heartbeats `peer` counted since the
    // previous check (section 10.1). The first sample of a mapping is a
    // baseline: reported only if it is not 0, and marked as such.
    void note_late_beats(Peer& peer, CheckReport& report) const;

    // Adds to `report` the lives `peer` started since the previous sample
    // (section 10.1), if it is up at this check. `came_up` says that this
    // check found it up after it was down. The first sample of a mapping is
    // a baseline and is not reported.
    static void note_starts(Peer& peer, bool is_up, bool came_up,
                            CheckReport& report);
    // Applies the range rule of section 9.2 to `queue` and adds to
    // `report` a change of its state. Called after every `proc` region was
    // observed in this check.
    void judge_queue(WatchedQueue& queue, CheckReport& report);

    [[nodiscard]] std::uint32_t& down_flag(const Peer& peer) const noexcept {
        return shared_at<ProcessEntry>(open_.header.bytes(), peer.entry_offset)
            .down;
    }

    Fd base_;  // holds the lock of this instance
    OpenGeneration open_;
    OwnProcRegion proc_;
    bool reused_ = false;
    std::vector<std::string> notes_;
    std::uint32_t heartbeat_check_ms_ = 0;
    std::uint32_t heartbeat_timeout_ms_ = 0;
    std::uint32_t tagd_uid_ = 0;
    std::vector<Peer> peers_;           // in process index order
    std::vector<WatchedQueue> queues_;  // in queue id order
    ExpectedEntries expected_;
    // Entries that could not be removed and were reported.
    std::set<std::string, std::less<>> stuck_;
};

void GenerationHost::Impl::describe(const TagSet& tag_set,
                                    const ResolvedTagSet& resolved,
                                    std::span<const std::byte> image) {
    expected_.emplace(HEADER_REGION_NAME, tagd_uid_);
    for (std::size_t i = 0; i < resolved.processes.size(); ++i) {
        const ProcessDefinition& process =
            tag_set.processes.at(resolved.processes[i]);
        Peer peer;
        peer.region.process = process.name;
        // Below MAX_PROCESSES, which fits 16 bits.
        peer.region.process_index = static_cast<std::uint16_t>(i);
        peer.region.generation = open_.generation;
        peer.region.queue_count = resolved.queues.size();
        peer.region.uid = process.uid;
        peer.heartbeat_timeout_ms = process.heartbeat_timeout_ms;
        peer.entry_offset = process_entry_offset(image, i);
        peer.self = process.name == TAGD_PROCESS_NAME;
        expected_.emplace(proc_region_name(process.name), process.uid);
        peers_.push_back(std::move(peer));
    }
    for (const ResolvedOwner& owner : resolved.owners) {
        const std::string& name = tag_set.owners.at(owner.definition).name;
        const std::uint32_t uid = peers_.at(owner.process_index).region.uid;
        expected_.emplace("tags." + name, uid);
        expected_.emplace("changes." + name, uid);
        if (owner.string_slots != 0) {
            expected_.emplace("strings." + name, uid);
        }
    }
    for (std::size_t i = 0; i < resolved.queues.size(); ++i) {
        const ResolvedQueue& queue = resolved.queues[i];
        const QueueDefinition& definition = tag_set.queues.at(queue.definition);
        // The region of a queue is written by its producer.
        const std::uint32_t uid = peers_.at(queue.producer_process).region.uid;
        expected_.emplace(definition.name, uid);
        WatchedQueue watched;
        watched.name = definition.name;
        watched.capacity = definition.capacity;
        watched.queue_id = i;
        watched.consumer_process = queue.consumer_process;
        watched.producer_process = queue.producer_process;
        watched.report_missing =
            queue.kind == QueueKind::EVENTS || queue.kind == QueueKind::CMD;
        watched.identity.kind = queue_region_kind(queue.kind);
        watched.identity.generation = open_.generation;
        watched.identity.size_bytes = queue_region_size(
            definition.capacity, queue_record_size(queue.kind));
        // As the library maps it (PeerRegions): the owner index of a queue
        // region is its producer's process index. Below MAX_QUEUES, which
        // fits 16 bits.
        watched.identity.owner_index = queue.producer_process;
        watched.identity.queue_id = static_cast<std::uint16_t>(i);
        watched.identity.uid = uid;
        queues_.push_back(std::move(watched));
    }
}

std::optional<std::uint32_t> GenerationHost::Impl::owner_of(
    std::string_view entry) const {
    if (const auto listed = expected_.find(entry); listed != expected_.end()) {
        return listed->second;
    }
    // A region being created (section 3.2) has the owner of the region.
    const std::string_view region = region_of_temporary(entry);
    if (const auto listed = expected_.find(region);
        listed != expected_.end() && is_temporary_name_of(entry, region)) {
        return listed->second;
    }
    return std::nullopt;
}

void GenerationHost::Impl::sweep(CheckReport& report) {
    std::string ignored;
    const Result<std::vector<std::string>> entries =
        list_directory(open_.directory.get(), ignored);
    if (!entries.ok()) {
        return;  // tried again at the next check
    }
    for (const std::string& entry : entries.value()) {
        const std::optional<std::uint32_t> owner = owner_of(entry);
        if (owner.has_value()) {
            struct stat info {};
            if (::fstatat(open_.directory.get(), entry.c_str(), &info,
                          AT_SYMLINK_NOFOLLOW) != 0 ||
                info.st_uid == *owner) {
                continue;  // gone meanwhile, or rightly there
            }
        }
        bool removed = ::unlinkat(open_.directory.get(), entry.c_str(), 0) == 0;
        if (!removed && errno == EISDIR) {
            removed = ::unlinkat(open_.directory.get(), entry.c_str(),
                                 AT_REMOVEDIR) == 0;
        }
        if (removed) {
            stuck_.erase(entry);
            report.swept.push_back(SweptEntry{entry, true});
        } else if (errno != ENOENT && stuck_.insert(entry).second) {
            report.swept.push_back(SweptEntry{entry, false});
        }
    }
}

Result<std::optional<std::uint64_t>> GenerationHost::Impl::open_base(
    const HostOptions& options, std::string& detail) {
    Result<Fd> base =
        open_base_directory(options.base_dir, options.tagd_uid, detail);
    if (!base.ok()) {
        return base.status();
    }
    base_ = std::move(base).value();
    if (::flock(base_.get(), LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        detail = "cannot lock the base directory: " + errno_text(error);
        return error == EWOULDBLOCK
                   ? Status(StatusCode::ALREADY_EXISTS,
                            "another scada-tagd uses the base directory")
                   : Status(StatusCode::IO_ERROR,
                            "cannot lock the base directory");
    }
    return read_current(base_.get(), notes_, detail);
}

Status GenerationHost::Impl::open_generation(
    const TagSet& tag_set, const ResolvedTagSet& resolved,
    std::optional<std::uint64_t> current, std::string& detail) {
    // The header this tag set builds, for the generation `current` points
    // to if there is one. Its size and hash do not depend on the generation.
    const Result<HeaderImage> image = build_header(
        tag_set, {.generation = current.value_or(0), .created_time_ns = 0},
        detail);
    if (!image.ok()) {
        return image.status();
    }
    std::optional<OpenGeneration> open;
    if (current.has_value()) {
        open = try_reuse(base_.get(), *current, image.value(),
                         resolved.processes.size(), tagd_uid_, notes_);
    }
    reused_ = open.has_value();
    if (open.has_value()) {
        open_ = std::move(*open);
        return {};
    }

    const Result<std::uint64_t> highest =
        clean_base_directory(base_.get(), current, notes_, detail);
    if (!highest.ok()) {
        return highest.status();
    }
    if (highest.value() == std::numeric_limits<std::uint64_t>::max()) {
        detail = "the generation numbers are used up";
        return {StatusCode::OUT_OF_RANGE, "no generation number is left"};
    }
    // Before anything is published: a generation whose processes then
    // cannot create their regions is worse than none.
    const Status room = check_room(
        base_.get(),
        total_region_bytes(tag_set, resolved, image.value().bytes.size()),
        detail);
    if (!room.ok()) {
        return room;
    }
    Result<OpenGeneration> created = create_generation(
        base_.get(), highest.value() + 1, tag_set, resolved, tagd_uid_, detail);
    if (!created.ok()) {
        return created.status();
    }
    open_ = std::move(created).value();
    if (current.has_value()) {
        retire(base_.get(), *current, tagd_uid_, notes_);
    }
    return {};
}

Status GenerationHost::Impl::attach_own_region(std::string& detail) {
    const auto own = std::ranges::find_if(
        peers_, [](const Peer& peer) { return peer.self; });
    if (own == peers_.end()) {
        // `start` checked that the tag set holds the process.
        return {StatusCode::INTERNAL, "the process of scada-tagd is missing"};
    }
    Result<OwnProcRegion> proc =
        OwnProcRegion::attach(open_.directory.get(), own->region, detail);
    if (!proc.ok()) {
        return proc.status();
    }
    proc_ = std::move(proc).value();
    proc_.count_late_after(own->heartbeat_timeout_ms);
    // Counted like the lives of other processes (section 10.1), for the
    // readers of `proc.tagd`; scada-tagd does not watch its own count.
    proc_.count_start();
    proc_.set_state(ProcessState::RUNNING);
    proc_.first_heartbeat(monotonic_ns());
    return {};
}

Result<std::unique_ptr<GenerationHost::Impl>> GenerationHost::Impl::start(
    const TagSet& tag_set, const HostOptions& options, std::string& detail) {
    const Result<ResolvedTagSet> resolved = resolve(tag_set, detail);
    if (!resolved.ok()) {
        return resolved.status();
    }
    const auto self = std::ranges::find_if(
        tag_set.processes, [](const ProcessDefinition& process) {
            return process.name == TAGD_PROCESS_NAME;
        });
    if (self == tag_set.processes.end()) {
        detail = "the tag set has no process named tagd";
        return Status(StatusCode::INVALID_ARGUMENT,
                      "the tag set has no process for scada-tagd");
    }
    if (self->uid != options.tagd_uid) {
        detail = "the process tagd has uid " + std::to_string(self->uid) +
                 " in the tag set, but scada-tagd runs as user " +
                 std::to_string(options.tagd_uid);
        return Status(StatusCode::INVALID_ARGUMENT,
                      "the tag set names another user for scada-tagd");
    }

    auto impl = std::make_unique<Impl>();
    impl->tagd_uid_ = options.tagd_uid;
    impl->heartbeat_check_ms_ = tag_set.heartbeat_check_ms;
    impl->heartbeat_timeout_ms_ = self->heartbeat_timeout_ms;

    const Result<std::optional<std::uint64_t>> current =
        impl->open_base(options, detail);
    if (!current.ok()) {
        return current.status();
    }
    if (const Status opened = impl->open_generation(tag_set, resolved.value(),
                                                    current.value(), detail);
        !opened.ok()) {
        return opened;
    }
    impl->describe(tag_set, resolved.value(), impl->open_.header.bytes());
    if (impl->reused_) {
        // A file under the name `proc.tagd` that another user created would
        // stop the attach below; scada-tagd is the one that removes it.
        CheckReport ignored;
        impl->sweep(ignored);
    }
    if (const Status attached = impl->attach_own_region(detail);
        !attached.ok()) {
        return attached;
    }
    return impl;
}

CheckReport GenerationHost::Impl::check(std::int64_t now_mono_ns) {
    CheckReport report;
    for (Peer& peer : peers_) {
        const DownReason reason =
            peer.self
                ? DownReason::NONE
                : peer.probe.observe(open_.directory.get(), peer.region,
                                     peer.heartbeat_timeout_ms, now_mono_ns);
        const bool down = reason != DownReason::NONE;
        peer.down = down;
        std::uint32_t& flag = down_flag(peer);
        // Relaxed load: this process is the only writer of the flag.
        const bool changed =
            (load_shared(flag, std::memory_order_relaxed) != 0U) != down;
        if (changed) {
            store_shared(flag, down ? 1U : 0U, std::memory_order_release);
            report.changes.push_back(
                ProcessChange{peer.region.process, reason});
        }
        if (!peer.self) {
            note_starts(peer, !down, changed && !down, report);
        }
        note_late_beats(peer, report);
    }
    for (WatchedQueue& queue : queues_) {
        judge_queue(queue, report);
    }
    sweep(report);
    return report;
}

void GenerationHost::Impl::judge_queue(WatchedQueue& queue,
                                       CheckReport& report) {
    if (!follow_region(open_.directory.get(), queue.name, queue.identity,
                       queue.region) ||
        !queue.region.has_value()) {
        // A producer that is down may not have created it yet. One that is
        // up created it at attach; a check may fall into a recreation, so
        // a single miss is not reported.
        if (!queue.report_missing || peers_.at(queue.producer_process).down) {
            queue.missing_checks = 0;
        } else if (++queue.missing_checks == MISSING_CHECKS_REPORTED &&
                   !queue.missing_reported) {
            queue.missing_reported = true;
            report.queues.push_back(
                QueueChange{.queue = queue.name, .event = QueueEvent::MISSING});
        }
        return;  // nothing to judge
    }
    queue.missing_checks = 0;
    if (queue.missing_reported) {
        queue.missing_reported = false;
        report.queues.push_back(
            QueueChange{.queue = queue.name, .event = QueueEvent::FOUND});
    }
    const Peer& consumer = peers_.at(queue.consumer_process);
    const std::span<std::byte> proc =
        consumer.self ? proc_.bytes() : consumer.probe.bytes();
    const std::uint64_t* const cursor = cursor_field(proc, queue.queue_id);
    const std::span<std::byte> file = queue.region->memory.bytes();
    if (queue.file_seen && (queue.region->device != queue.last_device ||
                            queue.region->inode != queue.last_inode)) {
        QueueChange change{.queue = queue.name,
                           .event = QueueEvent::RECREATED,
                           .write_seq = queue_write_seq(file),
                           .capacity = queue.capacity};
        change.base_seq = queue_base(file);
        change.old_write_seq = queue.last_written;
        // The cursor at the last check of the old file, not now: the
        // consumer may have jumped to the new base since (review of PR
        // #77, B1).
        change.cursor = queue.last_cursor;
        change.cursor_known = queue.last_cursor_known;
        report.queues.push_back(change);
    }
    queue.file_seen = true;
    queue.last_device = queue.region->device;
    queue.last_inode = queue.region->inode;
    queue.last_written = queue_write_seq(file);
    queue.last_cursor_known = cursor != nullptr;
    if (cursor != nullptr) {
        queue.last_cursor = load_shared(*cursor, std::memory_order_acquire);
    }
    if (cursor == nullptr) {
        return;  // the consumer's region is missing
    }
    // Only a consumer that is RUNNING has set its cursors: a `proc` region
    // created anew holds 0 from its rename until attach sets them to
    // `write_seq` (section 9.2), and that is no broken queue (review of
    // PR #68). Acquire: pairs with the release store of the state.
    if (const std::optional<ProcFields> fields = read_proc_fields(proc);
        !fields.has_value() || fields->state != to_raw(ProcessState::RUNNING)) {
        return;
    }
    const std::span<std::byte> bytes = queue.region->memory.bytes();
    // Both sides may run while this reads: in this order neither can make
    // a valid pair look broken (queue_pair_broken). Acquire, as the two
    // sides load each other's field.
    const std::uint64_t written_before = queue_write_seq(bytes);
    const std::uint64_t read = load_shared(*cursor, std::memory_order_acquire);
    const std::uint64_t written_after = queue_write_seq(bytes);
    const bool broken =
        queue_pair_broken(written_before, read, written_after, queue.capacity);
    if (broken == queue.broken) {
        return;
    }
    queue.broken = broken;
    report.queues.push_back(QueueChange{
        .queue = queue.name,
        .event = broken ? QueueEvent::BROKEN : QueueEvent::NO_LONGER_BROKEN,
        .cursor = read,
        .write_seq = written_after,
        .capacity = queue.capacity,
        .replaced = read == DEAD_CURSOR,
    });
}

}  // namespace scada::tag

scada::tag::LostIds scada::tag::lost_ids(const QueueChange& change) noexcept {
    LostIds lost;
    lost.from = change.cursor_known ? change.cursor : 0;
    lost.bounded = change.base_seq.has_value();
    lost.until = lost.bounded ? *change.base_seq : change.old_write_seq;
    lost.until = std::max(lost.until, lost.from);
    return lost;
}

namespace scada::tag {

void GenerationHost::Impl::note_late_beats(Peer& peer,
                                           CheckReport& report) const {
    // scada-tagd reads its own region too: while its heartbeat was late,
    // readers saw SCADA_E_TAGD_DOWN. That mapping is never replaced.
    const std::optional<LateBeats> beats =
        peer.self ? late_beats_of(proc_.bytes()) : peer.probe.late_beats();
    const std::uint64_t mapping = peer.self ? 0 : peer.probe.mappings();
    if (!beats.has_value()) {
        peer.late_seen.reset();
        return;
    }
    const bool baseline =
        !peer.late_seen.has_value() || peer.late_mapping != mapping;
    // Modulo 2^32: the count wraps.
    const std::uint32_t added =
        baseline ? beats->count : beats->count - *peer.late_seen;
    peer.late_seen = beats->count;
    peer.late_mapping = mapping;
    if (added == 0) {
        return;
    }
    report.late.push_back(LateHeartbeats{
        .process = peer.region.process,
        .added = added,
        .total = beats->count,
        .longest_gap_ns = beats->longest_gap_ns,
        .last_late_mono_ns = beats->last_late_mono_ns,
        .baseline = baseline,
    });
}

void GenerationHost::Impl::note_starts(Peer& peer, bool is_up, bool came_up,
                                       CheckReport& report) {
    if (came_up &&
        peer.ups_since_sample < std::numeric_limits<std::uint32_t>::max()) {
        ++peer.ups_since_sample;
    }
    if (!is_up) {
        // Sampled only while up: a life counts itself just before RUNNING,
        // so a sample while starting would be taken before the "up" it
        // belongs to. The previous sample stays.
        return;
    }
    const std::optional<std::uint32_t> starts = peer.probe.starts();
    if (!starts.has_value()) {
        // Not RUNNING any more since `observe`: sampled at the next check.
        return;
    }
    const std::uint64_t mapping = peer.probe.mappings();
    const bool baseline =
        !peer.starts_seen.has_value() || peer.starts_mapping != mapping;
    // Modulo 2^32: the count wraps.
    const std::uint32_t added = baseline ? 0U : *starts - *peer.starts_seen;
    const std::uint32_t seen = std::exchange(peer.ups_since_sample, 0U);
    peer.starts_seen = starts;
    peer.starts_mapping = mapping;
    if (added == 0) {
        return;
    }
    // Each check that found the process up after it was down accounts for
    // one of the lives, which scada-tagd saw as down and up; the others
    // came and went between two checks. A down and up without a restart
    // (a heartbeat that came back) also counts, so `unseen` is a lower
    // bound.
    const std::uint32_t unseen = added - std::min(added, seen);
    report.restarts.push_back(Restarts{
        .process = peer.region.process,
        .added = added,
        .unseen = unseen,
        .total = *starts,
    });
}

std::optional<bool> GenerationHost::Impl::is_down(
    std::string_view process) const noexcept {
    const auto peer = std::ranges::find_if(peers_, [&](const Peer& candidate) {
        return candidate.region.process == process;
    });
    if (peer == peers_.end()) {
        return std::nullopt;
    }
    return load_shared(down_flag(*peer), std::memory_order_acquire) != 0U;
}

GenerationHost::GenerationHost(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

GenerationHost::~GenerationHost() = default;
GenerationHost::GenerationHost(GenerationHost&&) noexcept = default;
GenerationHost& GenerationHost::operator=(GenerationHost&&) noexcept = default;

Result<GenerationHost> GenerationHost::start(const TagSet& tag_set,
                                             const HostOptions& options,
                                             std::string& detail) {
    Result<std::unique_ptr<Impl>> impl = Impl::start(tag_set, options, detail);
    if (!impl.ok()) {
        return impl.status();
    }
    return GenerationHost(std::move(impl).value());
}

std::uint64_t GenerationHost::generation() const noexcept {
    return impl_->generation();
}

bool GenerationHost::reused() const noexcept {
    return impl_->reused();
}

const std::vector<std::string>& GenerationHost::notes() const noexcept {
    return impl_->notes();
}

std::uint32_t GenerationHost::heartbeat_check_ms() const noexcept {
    return impl_->heartbeat_check_ms();
}

std::uint32_t GenerationHost::heartbeat_timeout_ms() const noexcept {
    return impl_->heartbeat_timeout_ms();
}

void GenerationHost::heartbeat(std::int64_t now_mono_ns) noexcept {
    impl_->heartbeat(now_mono_ns);
}

CheckReport GenerationHost::check(std::int64_t now_mono_ns) {
    return impl_->check(now_mono_ns);
}

std::optional<bool> GenerationHost::is_down(
    std::string_view process) const noexcept {
    return impl_->is_down(process);
}

void GenerationHost::stop() noexcept {
    impl_->stop();
}

}  // namespace scada::tag
