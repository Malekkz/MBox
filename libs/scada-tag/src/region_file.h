#ifndef SCADA_TAG_REGION_FILE_H
#define SCADA_TAG_REGION_FILE_H

// The files of the tag database: the base directory, the generation
// directories and the region files in them, with the creation and checking
// rules of docs/contracts/tag-database.md, section 3. Private to
// libscada-tag.

#include "layout.h"

#include "scada/common/fd.h"
#include "scada/common/mapped_region.h"
#include "scada/common/result.h"
#include "scada/common/status.h"

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scada::tag {

inline constexpr std::string_view HEADER_REGION_NAME = "header";
inline constexpr std::string_view CURRENT_LINK_NAME = "current";
inline constexpr std::string_view GENERATION_DIR_PREFIX = "gen-";
inline constexpr std::string_view TEMPORARY_PREFIX = ".tmp.";

// Modes of section 3.1.
inline constexpr mode_t BASE_DIR_MODE = 02750;
inline constexpr mode_t GENERATION_DIR_MODE = 03770;
inline constexpr mode_t REGION_FILE_MODE = 0640;

// The most entries one scan of a directory returns. A generation has at
// most 1 + 1,024 + 3 * 1,023 + 4,096 regions; anything beyond this limit is
// left for the next scan.
inline constexpr std::size_t MAX_DIRECTORY_ENTRIES = 65'536;

// Opens `name` relative to the directory open as `dir_fd` (AT_FDCWD: the
// working directory, or `name` is absolute) with the flags `flags` of
// openat(2), and `mode` if it creates the file. An invalid Fd on failure,
// with errno set. The one place of this library that calls the variadic
// open functions.
[[nodiscard]] Fd open_at(int dir_fd, const std::string& name, int flags,
                         mode_t mode = 0) noexcept;

// Text for the error number `error`, for a detail string. The number is
// also remembered for `last_os_error`: every place of this library that
// reports an error of the operating system passes through here.
[[nodiscard]] std::string errno_text(int error);

// The error number last given to `errno_text` by the calling thread since
// its last `clear_os_error`, or 0. It lets the C interface hand `errno` back
// with SCADA_E_IO (contract, section 13.3), which a Status cannot carry.
[[nodiscard]] int last_os_error() noexcept;
void clear_os_error() noexcept;

// "proc.<process>".
[[nodiscard]] std::string proc_region_name(std::string_view process);

// "tags.<owner>", "strings.<owner>" and "changes.<owner>".
[[nodiscard]] std::string tags_region_name(std::string_view owner);
[[nodiscard]] std::string strings_region_name(std::string_view owner);
[[nodiscard]] std::string changes_region_name(std::string_view owner);

// "gen-<generation>".
[[nodiscard]] std::string generation_dir_name(std::uint64_t generation);

// The generation of a directory name "gen-<generation>": decimal, without a
// leading zero or a sign. Nothing for any other name. `name` is outside
// input.
[[nodiscard]] std::optional<std::uint64_t> parse_generation_dir_name(
    std::string_view name) noexcept;

// ".tmp.<name>.<pid>" (section 3.2).
[[nodiscard]] std::string temporary_name(std::string_view name, pid_t pid);

// True if `entry` is exactly ".tmp.<name>.<digits>", the temporary name of
// the region `name` for some process id. `entry` is outside input.
[[nodiscard]] bool is_temporary_name_of(std::string_view entry,
                                        std::string_view name) noexcept;

// Sizes of the regions in bytes, from the counts in the header (sections 6
// to 10). The counts are within the limits of section 14, so the results do
// not overflow.
[[nodiscard]] std::uint64_t proc_region_size(std::size_t queue_count) noexcept;
[[nodiscard]] std::uint64_t tags_region_size(std::uint64_t tag_count) noexcept;
[[nodiscard]] std::uint64_t strings_region_size(
    std::uint64_t string_slots) noexcept;
[[nodiscard]] std::uint64_t changes_region_size(
    std::uint64_t capacity) noexcept;
[[nodiscard]] std::uint64_t queue_region_size(
    std::uint64_t capacity, std::uint64_t record_size) noexcept;

// The region kind of a queue of kind `kind`.
[[nodiscard]] RegionKind queue_region_kind(QueueKind kind) noexcept;

// What a region file must be to be used (section 3.1): the user that writes
// it and the fields of its RegionHeader.
struct RegionIdentity {
    RegionKind kind = RegionKind::HEADER;
    std::uint64_t generation = 0;
    std::uint64_t size_bytes = 0;
    std::uint16_t owner_index = 0;
    std::uint16_t queue_id = NO_QUEUE_ID;
    std::uint32_t uid = 0;
};

// Opens the base directory `path` and checks it (section 3.1): a directory,
// not reached through a symlink in the last component, owned by `tagd_uid`,
// with mode 2750 exactly. PERMISSION_DENIED if a check fails, NOT_FOUND if
// it does not exist, IO_ERROR otherwise. On an error `detail` says what was
// found.
[[nodiscard]] Result<Fd> open_base_directory(const std::string& path,
                                             std::uint32_t tagd_uid,
                                             std::string& detail);

// Opens the retain directory `path` (contract, section 12) with the checks
// of `open_base_directory`: a directory, not a symbolic link, owned by
// `tagd_uid`, mode 2750; and the group of the base directory open as
// `base_fd`, which is the group of the database. The codes of
// `open_base_directory`; PERMISSION_DENIED for another group.
[[nodiscard]] Result<Fd> open_retain_directory(const std::string& path,
                                               std::uint32_t tagd_uid,
                                               int base_fd,
                                               std::string& detail);

// The names in the directory open as `dir_fd`, without "." and "..", at
// most MAX_DIRECTORY_ENTRIES of them, sorted by their bytes. The names are
// outside input.
[[nodiscard]] Result<std::vector<std::string>> list_directory(
    int dir_fd, std::string& detail);

// A region file under construction (section 3.2). It exists under its
// temporary name until `publish`; if the builder is destroyed before that,
// the temporary file is removed.
class RegionBuilder {
public:
    // Creates ".tmp.<name>.<pid>" in the directory open as `dir_fd` with
    // mode 0640, gives it its full size with every block allocated, so that
    // a full file system fails here and not later with a signal, maps it
    // read-write and writes the RegionHeader with `ready` 0. The rest of the
    // file is zero.
    //
    // `dir_fd` is borrowed and must stay open for the life of the builder.
    // `name` is a region name built from validated names. A temporary file
    // of the same name and process id left by an earlier run is replaced.
    // RESOURCE_EXHAUSTED if the file system has no room, IO_ERROR otherwise.
    [[nodiscard]] static Result<RegionBuilder> create(
        int dir_fd, std::string_view name, const RegionIdentity& identity,
        std::string& detail);

    ~RegionBuilder();
    RegionBuilder(RegionBuilder&& other) noexcept;
    RegionBuilder& operator=(RegionBuilder&&) = delete;
    RegionBuilder(const RegionBuilder&) = delete;
    RegionBuilder& operator=(const RegionBuilder&) = delete;

    // The whole file, for the caller to initialise.
    [[nodiscard]] std::span<std::byte> bytes() const noexcept {
        return memory_.bytes();
    }

    // The descriptor of the file, for a lock. Borrowed.
    [[nodiscard]] int descriptor() const noexcept { return file_.get(); }

    // Sets `ready` and renames the file to its real name. The rename never
    // replaces a file: ALREADY_EXISTS if the name is taken, and the
    // temporary file is removed. On success the builder is empty and the
    // region is returned.
    struct Published {
        Fd file;
        MappedRegion memory;
    };
    [[nodiscard]] Result<Published> publish(std::string& detail);

private:
    RegionBuilder(int dir_fd, std::string name, std::string temporary, Fd file,
                  MappedRegion memory) noexcept;

    void discard() noexcept;

    int dir_fd_ = -1;
    std::string name_;
    std::string temporary_;  // empty once published or discarded
    Fd file_;
    MappedRegion memory_;
};

// An existing region, opened and mapped.
struct OpenedRegion {
    Fd file;
    MappedRegion memory;
    // Identifies the file, to notice when the name refers to another one.
    dev_t device = 0;
    ino_t inode = 0;
};

// Opens the region `name` in the directory open as `dir_fd` and applies
// every check of section 3.1: opened without following a symlink, a regular
// file, owned by `identity.uid`, of the size and with the RegionHeader that
// `identity` gives, and `ready`.
//
// NOT_FOUND if there is no such entry; PERMISSION_DENIED if its owner is
// wrong; UNAVAILABLE if it is not ready; DATA_CORRUPT if it is not a regular
// file or its size or header differ; IO_ERROR otherwise. Every outcome but
// success means "region missing" to a reader.
[[nodiscard]] Result<OpenedRegion> open_region(int dir_fd,
                                               std::string_view name,
                                               const RegionIdentity& identity,
                                               MapAccess access,
                                               std::string& detail);

// Removes the temporary files ".tmp.<name>.<digits>" owned by `uid` from the
// directory open as `dir_fd` (section 3.2: a process removes its own stale
// temporary files). The caller makes sure no other instance of the process
// is building the region. Errors are ignored: a file that stays is harmless.
void remove_stale_temporaries(int dir_fd, std::string_view name,
                              std::uint32_t uid) noexcept;

// True if a process holds the exclusive lock it keeps on the `proc` region
// `name` in the directory open as `dir_fd` for its whole life (section 10).
// To find out, the lock is tried without waiting and, if it was free,
// released at once; a process that attaches at that very moment fails with
// "already attached" and may try again. False only if there is no such
// file or the lock could be taken: any other error counts as held, so that
// a live process is never taken as gone (decision D-45).
[[nodiscard]] bool proc_lock_held(int dir_fd, const std::string& name) noexcept;

// True if a process holds the lock on any `proc` region in the generation
// directory `name` of the base directory open as `base_fd`: the generation
// has a live process (decision D-27).
[[nodiscard]] bool has_live_process(int base_fd, const std::string& name);

// Removes the generation directory `name` of the base directory open as
// `base_fd` with what is in it. Best effort (section 3.3): false if
// something stays.
[[nodiscard]] bool remove_generation(int base_fd, const std::string& name);

}  // namespace scada::tag

#endif  // SCADA_TAG_REGION_FILE_H
