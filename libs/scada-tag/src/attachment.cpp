#include "attachment.h"

#include "header_view.h"
#include "layout.h"
#include "own_region.h"
#include "peer_regions.h"
#include "proc_region.h"
#include "region_file.h"
#include "shared_memory.h"

#include "scada/common/clock.h"
#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/generation_host.h"
#include "scada/tag/liveness.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::tag {

namespace {

// Only the permission bits, the set-id bits and the sticky bit of a mode.
constexpr mode_t MODE_BITS = 07777;

// How often attach starts again because the generation it found was
// RETIRING (section 11.2, step 7). scada-tagd switches `current` before it
// retires a generation, so the second attempt finds the new one; the limit
// only keeps a damaged database from holding the caller for ever.
constexpr int RETIRING_ATTEMPTS = 5;

// Longest target of `current` that can be a generation name: "gen-" and 20
// digits. One byte more shows that a target is longer.
constexpr std::size_t CURRENT_TARGET_BUFFER = 25;

// Buffer sizes for the user database lookup.
constexpr std::size_t USER_BUFFER_START = 1024;
constexpr std::size_t USER_BUFFER_LIMIT = std::size_t{1024} * 1024;

RegionKind region_kind_of(QueueKind kind) noexcept {
    switch (kind) {
        case QueueKind::EVENTS:
            return RegionKind::EVENTS;
        case QueueKind::CMD:
            return RegionKind::CMD;
        case QueueKind::RESULT:
            return RegionKind::RESULT;
    }
    return RegionKind::EVENTS;
}

// The uid of the user `scada-tagd` (section 3.1: looked up by name at
// attach).
Result<std::uint32_t> lookup_tagd_uid(std::string& detail) {
    const std::string user(TAGD_USER_NAME);
    std::vector<char> buffer(USER_BUFFER_START);
    for (;;) {
        passwd entry{};
        passwd* found = nullptr;
        const int error = ::getpwnam_r(user.c_str(), &entry, buffer.data(),
                                       buffer.size(), &found);
        if (error == ERANGE && buffer.size() < USER_BUFFER_LIMIT) {
            buffer.resize(buffer.size() * 2);
            continue;
        }
        if (error != 0) {
            detail =
                "cannot look up the user " + user + ": " + errno_text(error);
            return Status(StatusCode::IO_ERROR, "cannot look up a user");
        }
        if (found == nullptr) {
            detail = "there is no user " + user + " to own the base directory";
            return Status(StatusCode::PERMISSION_DENIED,
                          "the user scada-tagd does not exist");
        }
        return std::uint32_t{entry.pw_uid};
    }
}

// The generation `current` points to in the base directory open as
// `base_fd`, read once (section 3.1).
Result<std::uint64_t> read_current(int base_fd, std::string& detail) {
    std::array<char, CURRENT_TARGET_BUFFER> target{};
    const std::string link(CURRENT_LINK_NAME);
    const ssize_t length =
        ::readlinkat(base_fd, link.c_str(), target.data(), target.size());
    if (length < 0) {
        const int error = errno;
        detail = "cannot read current: " + errno_text(error);
        if (error == ENOENT) {
            return Status(StatusCode::UNAVAILABLE,
                          "there is no current generation");
        }
        return error == EINVAL
                   ? Status(StatusCode::DATA_CORRUPT,
                            "current is not a symbolic link")
                   : Status(StatusCode::IO_ERROR, "cannot read current");
    }
    // In range: not negative, and at most the size of the buffer.
    const std::string_view name(target.data(),
                                static_cast<std::size_t>(length));
    const std::optional<std::uint64_t> generation =
        parse_generation_dir_name(name);
    if (!generation.has_value()) {
        detail = "current does not point to a generation";
        return Status(StatusCode::DATA_CORRUPT,
                      "current does not point to a generation");
    }
    return *generation;
}

// Opens the directory of `generation` in the base directory open as
// `base_fd` and checks its owner and mode (section 3.1).
Result<Fd> open_generation_directory(int base_fd, std::uint64_t generation,
                                     std::uint32_t tagd_uid,
                                     std::string& detail) {
    const std::string name = generation_dir_name(generation);
    Fd directory =
        open_at(base_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (!directory.valid()) {
        const int error = errno;
        detail = "cannot open " + name + ": " + errno_text(error);
        // Gone between reading `current` and here: a generation change.
        return error == ENOENT ? Status(StatusCode::UNAVAILABLE,
                                        "the current generation is gone")
                               : Status(StatusCode::IO_ERROR,
                                        "cannot open the generation directory");
    }
    struct stat info {};
    if (::fstat(directory.get(), &info) != 0) {
        detail = "cannot examine " + name + ": " + errno_text(errno);
        return Status(StatusCode::IO_ERROR,
                      "cannot examine the generation directory");
    }
    if (info.st_uid != tagd_uid ||
        (info.st_mode & MODE_BITS) != GENERATION_DIR_MODE) {
        detail = name + " does not have owner " + std::to_string(tagd_uid) +
                 " and mode 3770";
        return Status(StatusCode::PERMISSION_DENIED,
                      "the generation directory has the wrong owner or mode");
    }
    return directory;
}

// The uid of scada-tagd: the one of `options`, or the one of the user.
Result<std::uint32_t> tagd_uid_of(const AttachOptions& options,
                                  std::string& detail) {
    if (options.tagd_uid.has_value()) {
        return *options.tagd_uid;
    }
    return lookup_tagd_uid(detail);
}

// The current generation: its directory and its header.
struct CurrentGeneration {
    Fd directory;
    HeaderView header;
};

// NOT_FOUND from `attach` means "the process is not in the process table".
// A base directory or header that is not there is another matter: the
// database is not up.
Status absent_as_unavailable(const Status& status) noexcept {
    return status.code() == StatusCode::NOT_FOUND
               ? Status(StatusCode::UNAVAILABLE,
                        "the tag database is not there")
               : status;
}

// Step 1 of attach: checks the base directory `base_dir`, resolves
// `current` once and opens the generation it points to.
Result<CurrentGeneration> open_current_generation(const std::string& base_dir,
                                                  std::uint32_t tagd_uid,
                                                  std::string& detail) {
    const Result<Fd> base = open_base_directory(base_dir, tagd_uid, detail);
    if (!base.ok()) {
        return absent_as_unavailable(base.status());
    }
    const Result<std::uint64_t> generation =
        read_current(base.value().get(), detail);
    if (!generation.ok()) {
        return generation.status();
    }
    Result<Fd> directory = open_generation_directory(
        base.value().get(), generation.value(), tagd_uid, detail);
    if (!directory.ok()) {
        return directory.status();
    }
    Result<HeaderView> header = HeaderView::open(
        directory.value().get(), generation.value(), tagd_uid, detail);
    if (!header.ok()) {
        return absent_as_unavailable(header.status());
    }
    return CurrentGeneration{std::move(directory).value(),
                             std::move(header).value()};
}

// The index of `process` in the process table of `header`, if the caller
// runs as the user given there (section 11.2, step 2).
Result<std::uint16_t> find_own_process(const HeaderView& header,
                                       std::string_view process,
                                       std::string& detail) {
    const std::optional<std::uint16_t> index = header.find_process(process);
    if (!index.has_value()) {
        detail = "process " + std::string(process) +
                 " is not in the process table of generation " +
                 std::to_string(header.contents().generation);
        return Status(StatusCode::NOT_FOUND,
                      "the process is not in the process table");
    }
    const std::uint32_t expected_uid =
        header.contents().processes.at(*index).uid;
    if (::geteuid() != expected_uid) {
        detail = "process " + std::string(process) + " runs as user " +
                 std::to_string(::geteuid()) + ", not as user " +
                 std::to_string(expected_uid);
        return Status(StatusCode::PERMISSION_DENIED,
                      "the process runs as the wrong user");
    }
    return *index;
}

// The types of the slots of owner `owner_index`, in slot order.
Result<std::vector<TagType>> slot_types(const HeaderView& header,
                                        std::size_t owner_index,
                                        std::string& detail) {
    const OwnerInfo& owner = header.contents().owners.at(owner_index);
    std::vector<TagType> types;
    types.reserve(owner.tag_count);
    for (std::uint32_t slot = 0; slot < owner.tag_count; ++slot) {
        const std::optional<TagType> type = header.slot_type(owner_index, slot);
        if (!type.has_value()) {
            detail = "slot " + std::to_string(slot) + " of owner " +
                     owner.name + " has no valid entry in the tag directory";
            return Status(StatusCode::DATA_CORRUPT,
                          "the tag directory is not valid");
        }
        types.push_back(*type);
    }
    if (std::ranges::count(types, TagType::STRING) != owner.string_slots) {
        detail = "owner " + owner.name +
                 " does not have one string slot for each STRING tag";
        return Status(StatusCode::DATA_CORRUPT,
                      "the tag directory is not valid");
    }
    return types;
}

std::size_t record_offset(std::size_t slot) noexcept {
    return sizeof(RegionHeader) + (slot * sizeof(TagRecord));
}

// True if the records of the `tags` region `tags` have the types `types`.
bool has_tag_types(std::span<std::byte> tags,
                   std::span<const TagType> types) noexcept {
    for (std::size_t slot = 0; slot < types.size(); ++slot) {
        // Nobody writes the region: its owner is the caller, which holds
        // the lock of the process.
        if (shared_at<TagRecord>(tags, record_offset(slot)).type !=
            to_raw(types[slot])) {
            return false;
        }
    }
    return true;
}

}  // namespace

void initialise_tag_records(std::span<std::byte> tags,
                            std::span<const TagType> types) noexcept {
    std::uint64_t string_slot = 0;
    for (std::size_t slot = 0; slot < types.size(); ++slot) {
        TagRecord record;
        record.type = to_raw(types[slot]);
        record.quality = to_raw(Quality::BAD);
        record.quality_flags = to_raw(QualityFlag::NOT_INIT);
        if (types[slot] == TagType::STRING) {
            record.value = string_slot;
            ++string_slot;
        }
        // Not shared yet: the file has no name others look for.
        shared_at<TagRecord>(tags, record_offset(slot)) = record;
    }
}

std::uint32_t repair_tag_records(std::span<std::byte> tags,
                                 std::span<std::byte> strings,
                                 std::span<const TagType> types) noexcept {
    std::uint32_t repaired = 0;
    std::size_t string_slot = 0;
    for (std::size_t slot = 0; slot < types.size(); ++slot) {
        const bool is_string = types[slot] == TagType::STRING;
        auto& record = shared_at<TagRecord>(tags, record_offset(slot));
        // Relaxed: the caller is the only writer of its region.
        const std::uint32_t seq =
            load_shared(record.seq, std::memory_order_relaxed);
        if (seq % 2 == 1) {
            // Relaxed: readers use what they copy only after they have
            // seen the even `seq` stored below.
            store_shared(record.quality, to_raw(Quality::BAD),
                         std::memory_order_relaxed);
            store_shared(record.quality_flags, to_raw(QualityFlag::NOT_INIT),
                         std::memory_order_relaxed);
            const std::size_t slot_offset =
                sizeof(RegionHeader) + (string_slot * sizeof(StringSlot));
            if (is_string &&
                fits(strings.size(), slot_offset, sizeof(StringSlot))) {
                store_shared(shared_at<StringSlot>(strings, slot_offset).len,
                             std::uint16_t{0}, std::memory_order_relaxed);
            }
            // Release: publishes the stores above (section 6.1).
            store_shared(record.seq, seq + 1, std::memory_order_release);
            ++repaired;
        }
        if (is_string) {
            ++string_slot;
        }
    }
    return repaired;
}

Attachment::Attachment(Key /*key*/, Fd directory, HeaderView header,
                       std::uint16_t process_index)
    : directory_(std::move(directory)),
      header_(std::move(header)),
      process_index_(process_index),
      tagd_index_(header_.find_process(TAGD_PROCESS_NAME)),
      peers_(header_.contents()) {}

Attachment::~Attachment() {
    if (proc_.valid()) {
        proc_.set_state(ProcessState::STOPPING);
    }
}

void Attachment::count(const OwnRegion& region) noexcept {
    if (region.reused) {
        ++regions_reused_;
    } else {
        ++regions_created_;
    }
}

Status Attachment::attach_owner(std::size_t owner_index, std::string& detail) {
    const HeaderContents& contents = header_.contents();
    const OwnerInfo& owner = contents.owners.at(owner_index);
    Result<std::vector<TagType>> types =
        slot_types(header_, owner_index, detail);
    if (!types.ok()) {
        return types.status();
    }
    const std::span<const TagType> slots(types.value());

    OwnedOwner owned;
    owned.owner_index = owner_index;

    OwnRegionSpec spec;
    spec.identity.generation = contents.generation;
    // Below MAX_OWNERS, which fits 16 bits.
    spec.identity.owner_index = static_cast<std::uint16_t>(owner_index);
    spec.identity.uid = owner.uid;

    spec.name = tags_region_name(owner.name);
    spec.identity.kind = RegionKind::TAGS;
    spec.identity.size_bytes = tags_region_size(owner.tag_count);
    spec.initialise = [slots](std::span<std::byte> bytes) {
        initialise_tag_records(bytes, slots);
    };
    spec.usable = [slots](std::span<std::byte> bytes) {
        return has_tag_types(bytes, slots);
    };
    Result<OwnRegion> tags = attach_own_region(directory_.get(), spec, detail);
    if (!tags.ok()) {
        return tags.status();
    }
    owned.tags = std::move(tags).value();
    count(owned.tags);

    if (owner.string_slots != 0) {
        spec.name = strings_region_name(owner.name);
        spec.identity.kind = RegionKind::STRINGS;
        spec.identity.size_bytes = strings_region_size(owner.string_slots);
        spec.initialise = nullptr;  // a new region is zero: every length 0
        spec.usable = nullptr;
        Result<OwnRegion> strings =
            attach_own_region(directory_.get(), spec, detail);
        if (!strings.ok()) {
            return strings.status();
        }
        owned.strings = std::move(strings).value();
        count(owned.strings);
    }

    const std::uint32_t capacity = owner.change_ring_capacity;
    spec.name = changes_region_name(owner.name);
    spec.identity.kind = RegionKind::CHANGES;
    spec.identity.size_bytes = changes_region_size(capacity);
    spec.initialise = [capacity](std::span<std::byte> bytes) {
        shared_at<ChangeRingHeader>(bytes, sizeof(RegionHeader)).capacity =
            capacity;
    };
    spec.usable = [capacity](std::span<std::byte> bytes) {
        return shared_at<ChangeRingHeader>(bytes, sizeof(RegionHeader))
                   .capacity == capacity;
    };
    Result<OwnRegion> changes =
        attach_own_region(directory_.get(), spec, detail);
    if (!changes.ok()) {
        return changes.status();
    }
    owned.changes = std::move(changes).value();
    count(owned.changes);

    // Section 6.3: repair, then tell the consumers of the ring that the
    // owner started again.
    if (owned.tags.reused) {
        records_repaired_ += repair_tag_records(
            owned.tags.memory.bytes(), owned.strings.memory.bytes(), slots);
    }
    if (owned.changes.reused) {
        auto& ring = shared_at<ChangeRingHeader>(owned.changes.memory.bytes(),
                                                 sizeof(RegionHeader));
        // Relaxed: the caller is the only writer of its ring.
        const std::uint32_t epoch =
            load_shared(ring.epoch, std::memory_order_relaxed);
        // Release: consumers load with acquire.
        store_shared(ring.epoch, epoch + 1, std::memory_order_release);
    }

    for (const OwnRegion* region :
         {&owned.tags, &owned.strings, &owned.changes}) {
        if (region->memory.valid() && !region->memory.prefault().ok()) {
            detail = "cannot bring the regions of owner " + owner.name +
                     " into memory: " + errno_text(errno);
            return {StatusCode::IO_ERROR, "cannot bring a region into memory"};
        }
    }
    owners_.push_back(std::move(owned));
    return {};
}

Status Attachment::attach_queue(std::size_t queue_id, std::string& detail) {
    const HeaderContents& contents = header_.contents();
    const QueueInfo& queue = contents.queues.at(queue_id);
    const std::uint32_t capacity = queue.capacity;
    const std::uint32_t record_size = queue.record_size;

    OwnRegionSpec spec;
    spec.name = queue.name;
    spec.identity.kind = region_kind_of(queue.kind);
    spec.identity.generation = contents.generation;
    spec.identity.size_bytes = queue_region_size(capacity, record_size);
    spec.identity.owner_index = process_index_;
    // Below MAX_QUEUES, which fits 16 bits.
    spec.identity.queue_id = static_cast<std::uint16_t>(queue_id);
    spec.identity.uid = contents.processes.at(process_index_).uid;
    spec.initialise = [capacity, record_size](std::span<std::byte> bytes) {
        auto& header = shared_at<QueueHeader>(bytes, sizeof(RegionHeader));
        header.capacity = capacity;
        header.record_size = record_size;
    };
    spec.usable = [capacity, record_size](std::span<std::byte> bytes) {
        const auto& header =
            shared_at<QueueHeader>(bytes, sizeof(RegionHeader));
        return header.capacity == capacity && header.record_size == record_size;
    };
    Result<OwnRegion> region =
        attach_own_region(directory_.get(), spec, detail);
    if (!region.ok()) {
        return region.status();
    }
    if (!region.value().memory.prefault().ok()) {
        detail = "cannot bring queue " + queue.name +
                 " into memory: " + errno_text(errno);
        return {StatusCode::IO_ERROR, "cannot bring a region into memory"};
    }
    count(region.value());
    queues_.push_back({queue_id, std::move(region).value()});
    return {};
}

Status Attachment::attach_regions(std::string& detail) {
    const HeaderContents& contents = header_.contents();
    const ProcessInfo& process = contents.processes.at(process_index_);

    ProcRegionParameters parameters;
    parameters.process = process.name;
    parameters.process_index = process_index_;
    parameters.generation = contents.generation;
    parameters.queue_count = contents.queues.size();
    parameters.uid = process.uid;
    Result<OwnProcRegion> proc =
        OwnProcRegion::attach(directory_.get(), parameters, detail);
    if (!proc.ok()) {
        return proc.status();
    }
    proc_ = std::move(proc).value();
    if (proc_.reused()) {
        ++regions_reused_;
    } else {
        ++regions_created_;
    }
    if (!proc_.prefault().ok()) {
        detail = "cannot bring proc." + process.name +
                 " into memory: " + errno_text(errno);
        return {StatusCode::IO_ERROR, "cannot bring a region into memory"};
    }

    for (std::size_t i = 0; i < contents.owners.size(); ++i) {
        if (contents.owners[i].process_index != process_index_) {
            continue;
        }
        if (Status attached = attach_owner(i, detail); !attached.ok()) {
            return attached;
        }
    }
    for (std::size_t i = 0; i < contents.queues.size(); ++i) {
        if (contents.queues[i].producer_process != process_index_) {
            continue;
        }
        if (Status attached = attach_queue(i, detail); !attached.ok()) {
            return attached;
        }
    }

    // Section 11.2, step 6. From here the caller writes the heartbeat.
    proc_.heartbeat(monotonic_ns());
    proc_.set_state(ProcessState::RUNNING);
    if (tagd_index_.has_value() && *tagd_index_ != process_index_) {
        peers_.want(directory_.get(), peers_.proc_slot(*tagd_index_),
                    monotonic_ns());
    }
    return {};
}

Result<std::unique_ptr<Attachment>> Attachment::attach(
    std::string_view process, const AttachOptions& options,
    std::string& detail) {
    if (!is_valid_name(process, MAX_SHORT_NAME_LENGTH)) {
        detail = "a process name is 1 to 31 bytes of [a-z0-9_.-]";
        return Status(StatusCode::INVALID_ARGUMENT,
                      "the process name is not valid");
    }
    const Result<std::uint32_t> tagd_uid = tagd_uid_of(options, detail);
    if (!tagd_uid.ok()) {
        return tagd_uid.status();
    }
    const std::string base_dir = options.base_dir.empty()
                                     ? std::string(DEFAULT_BASE_DIR)
                                     : options.base_dir;

    for (int attempt = 0; attempt < RETIRING_ATTEMPTS; ++attempt) {
        Result<CurrentGeneration> current =
            open_current_generation(base_dir, tagd_uid.value(), detail);
        if (!current.ok()) {
            return current.status();
        }
        const HeaderView& header = current.value().header;
        const std::optional<GenerationState> state = header.state();
        if (state == GenerationState::RETIRING) {
            continue;  // `current` has moved on: resolve it again
        }
        if (state != GenerationState::RUNNING) {
            detail = "generation " +
                     std::to_string(header.contents().generation) +
                     " is not RUNNING";
            return Status(StatusCode::UNAVAILABLE,
                          "the generation is not running");
        }
        const Result<std::uint16_t> index =
            find_own_process(header, process, detail);
        if (!index.ok()) {
            return index.status();
        }

        std::unique_ptr<Attachment> attachment = std::make_unique<Attachment>(
            Key{}, std::move(current.value().directory),
            std::move(current.value().header), index.value());
        if (Status attached = attachment->attach_regions(detail);
            !attached.ok()) {
            return attached;
        }
        // Section 11.2, step 7: the generation may have been retired while
        // the regions were attached.
        if (attachment->header_.state() == GenerationState::RETIRING) {
            continue;  // detaches
        }
        return attachment;
    }
    detail = "every generation found was RETIRING";
    return Status(StatusCode::FAILED_PRECONDITION,
                  "the generation is retiring");
}

bool Attachment::heartbeat() noexcept {
    proc_.heartbeat(monotonic_ns());
    return header_.state() != GenerationState::RETIRING;
}

AttachmentStatus Attachment::status() {
    const std::int64_t now = monotonic_ns();
    static_cast<void>(peers_.refresh(directory_.get(), now));

    AttachmentStatus status;
    status.generation = header_.contents().generation;
    status.state = header_.state();
    status.process_index = process_index_;
    status.regions_created = regions_created_;
    status.regions_reused = regions_reused_;
    status.records_repaired = records_repaired_;
    if (tagd_index_.has_value()) {
        status.tagd =
            *tagd_index_ == process_index_
                ? DownReason::NONE
                : proc_liveness(peers_.bytes(peers_.proc_slot(*tagd_index_)),
                                header_.contents()
                                    .processes.at(*tagd_index_)
                                    .heartbeat_timeout_ms,
                                now);
    }
    return status;
}

}  // namespace scada::tag
