#ifndef SCADA_TAG_ATTACHMENT_H
#define SCADA_TAG_ATTACHMENT_H

// One process attached to the tag database
// (docs/contracts/tag-database.md, sections 3, 6.3, 10 and 11.2). The C
// interface of scada/tag/scada_tag.h is a thin layer over this class.
// Private to libscada-tag.

#include "header_view.h"
#include "own_region.h"
#include "peer_regions.h"
#include "proc_region.h"

#include "scada/common/fd.h"
#include "scada/common/realtime.h"
#include "scada/common/result.h"
#include "scada/tag/liveness.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scada::tag {

// Defaults of the contract, sections 3 and 12.
inline constexpr std::string_view DEFAULT_RETAIN_DIR = "/var/lib/scada/retain";

// The user whose uid owns the base directory and the header (section 3.1).
inline constexpr std::string_view TAGD_USER_NAME = "scada-tagd";

struct AttachOptions {
    // The database directory; empty: /dev/shm/scada.
    std::string base_dir;
    // The directory of the retained values; empty: /var/lib/scada/retain.
    // Not read yet: retained values are loaded from issue P1-09 on.
    std::string retain_dir;
    // The uid of scada-tagd; nothing: the uid of the user `scada-tagd`.
    std::optional<std::uint32_t> tagd_uid;
};

// What `Attachment::status` reports.
struct AttachmentStatus {
    std::uint64_t generation = 0;
    // `GlobalHeader.state` now; nothing for a value that is no state.
    std::optional<GenerationState> state;
    std::uint16_t process_index = 0;
    // Whether scada-tagd is down, by the rules of section 10.1 applied to
    // `proc.tagd`. REGION_MISSING also if the generation has no process
    // `tagd`.
    DownReason tagd = DownReason::REGION_MISSING;
    // What attach did with the regions this process writes.
    std::uint32_t regions_created = 0;
    std::uint32_t regions_reused = 0;
    // Tag records found with an odd sequence number and repaired
    // (section 6.3).
    std::uint32_t records_repaired = 0;
};

// The tag regions of one owner of the attached process, mapped read-write.
struct OwnedOwner {
    std::size_t owner_index = 0;
    OwnRegion tags;
    OwnRegion strings;  // holds no mapping if the owner has no string slots
    OwnRegion changes;
};

// A reliable queue the attached process produces, mapped read-write.
struct OwnedQueue {
    std::size_t queue_id = 0;
    OwnRegion region;
};

class Attachment {
public:
    // Attaches the calling process as `process` (section 11.2):
    // 1. Checks the base directory (section 3.1), resolves `current` once,
    //    checks the generation directory and opens and validates `header`.
    // 2. Finds `process` in the process table; its uid must be the
    //    effective user id of the caller.
    // 3. Attaches `proc.<process>` and takes its lock. Then, for every
    //    owner that is a thread of the process, `tags.<owner>`,
    //    `strings.<owner>` if it has string slots and `changes.<owner>`,
    //    and every queue the process produces: a usable region is reused, a
    //    reused `tags` region is repaired and the `epoch` of a reused ring
    //    is incremented (section 6.3); an unusable region of the caller is
    //    removed; an absent one is created (section 3.2).
    // 4. Brings the regions into memory, sets the state RUNNING and writes
    //    the first heartbeat.
    // 5. Starts again from step 1, a limited number of times, if the
    //    generation is RETIRING.
    //
    // The lock is taken before the other regions are touched, not after as
    // section 11.2 lists it, so that a second process of the same name
    // fails before it changes anything.
    //
    // Not done yet, each with the issue that adds it: retained values
    // (P1-09), draining the `cmd` queues and setting the cursors of a new
    // `proc` region (P1-08).
    //
    // INVALID_ARGUMENT for a name that is no process name; NOT_FOUND if the
    // process is not in the process table; PERMISSION_DENIED for a wrong
    // uid, or a directory, header or own region with the wrong owner or
    // mode; ALREADY_EXISTS if the process name is already attached;
    // UNSUPPORTED if magic or layout version differ; DATA_CORRUPT for a
    // header or `current` that is not valid; UNAVAILABLE if there is no base
    // directory, no generation or no header, or the generation is not
    // RUNNING; FAILED_PRECONDITION if every
    // generation found was RETIRING; RESOURCE_EXHAUSTED if the file system
    // has no room; IO_ERROR otherwise. On an error `detail` says what was
    // found.
    [[nodiscard]] static Result<std::unique_ptr<Attachment>> attach(
        std::string_view process, const AttachOptions& options,
        std::string& detail);

    // Detaches: the state becomes STOPPING, so that the process reads as
    // down at once (section 10.1), and mappings and the lock are released.
    // The region files stay.
    ~Attachment();
    Attachment(const Attachment&) = delete;
    Attachment& operator=(const Attachment&) = delete;
    Attachment(Attachment&&) = delete;
    Attachment& operator=(Attachment&&) = delete;

    // Writes the heartbeat of the process, taken now. False if the
    // generation is RETIRING: the caller detaches and attaches again
    // (section 11.3). Call it from the main loop or cycle of the process.
    SCADA_RT [[nodiscard]] bool heartbeat() noexcept;

    // The state of the attachment now. It also looks again for the regions
    // of other processes that were asked for and are missing
    // (`PeerRegions::refresh`). Not real-time.
    [[nodiscard]] AttachmentStatus status();

    [[nodiscard]] const HeaderView& header() const noexcept { return header_; }

    // The regions this process writes.
    [[nodiscard]] const std::vector<OwnedOwner>& owners() const noexcept {
        return owners_;
    }
    [[nodiscard]] const std::vector<OwnedQueue>& queues() const noexcept {
        return queues_;
    }

    // The regions of other processes, and the generation directory to pass
    // to `PeerRegions::want`. The descriptor is borrowed.
    [[nodiscard]] PeerRegions& peers() noexcept { return peers_; }
    [[nodiscard]] int directory() const noexcept { return directory_.get(); }

private:
    // Only `attach` can name it, so only `attach` can construct.
    struct Key {
        explicit Key() = default;
    };

public:
    // For `attach` alone.
    Attachment(Key key, Fd directory, HeaderView header,
               std::uint16_t process_index);

private:
    // Steps 3 and 4 of `attach`.
    [[nodiscard]] Status attach_regions(std::string& detail);
    [[nodiscard]] Status attach_owner(std::size_t owner_index,
                                      std::string& detail);
    [[nodiscard]] Status attach_queue(std::size_t queue_id,
                                      std::string& detail);
    void count(const OwnRegion& region) noexcept;

    Fd directory_;  // the generation directory
    HeaderView header_;
    std::uint16_t process_index_ = 0;
    std::optional<std::uint16_t> tagd_index_;
    OwnProcRegion proc_;
    std::vector<OwnedOwner> owners_;
    std::vector<OwnedQueue> queues_;
    PeerRegions peers_;
    std::uint32_t regions_created_ = 0;
    std::uint32_t regions_reused_ = 0;
    std::uint32_t records_repaired_ = 0;
};

// Initial state of the `tags` region `tags` of an owner whose slots have
// the types `types` (section 6): `seq` 0, quality BAD, flag NOT_INIT, times
// 0. The value of a STRING record is its string slot: its rank among the
// owner's STRING tags in slot order. `tags` is the whole region, large
// enough for `types.size()` records.
void initialise_tag_records(std::span<std::byte> tags,
                            std::span<const TagType> types) noexcept;

// Repairs the `tags` region `tags` of an owner after a restart (section
// 6.3): every record with an odd `seq` gets quality BAD and the flag
// NOT_INIT, the string slot of such a STRING record in `strings` gets
// length 0, and then `seq + 1` is stored. Returns the number of records
// repaired. Only the owner calls it, before it sets itself running.
// `strings` may be empty if `types` holds no STRING.
std::uint32_t repair_tag_records(std::span<std::byte> tags,
                                 std::span<std::byte> strings,
                                 std::span<const TagType> types) noexcept;

}  // namespace scada::tag

#endif  // SCADA_TAG_ATTACHMENT_H
