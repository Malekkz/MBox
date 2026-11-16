#ifndef SCADA_TAG_RETAIN_FILE_H
#define SCADA_TAG_RETAIN_FILE_H

// The retain files `<retain_dir>/<owner>.bin`
// (docs/contracts/tag-database.md, section 12): their format, the atomic
// write of scada-tagd and the lookup of the values of one owner. A retain
// file is outside input to everyone who reads it. Private to libscada-tag.
//
// Format version 2 (decision D-59), all integers little-endian:
//
//   offset  size  field
//        0     8  magic "SCADARTN" (0x4E54524144414353)
//        8     4  format version, 2
//       12     4  record count, at most MAX_TAGS_PER_OWNER
//       16     8  source generation: the generation the values were read from
//       24     8  created_time_ns of that generation (GlobalHeader)
//       32     8  written_time_ns, CLOCK_REALTIME, when the file was written
//       40     4  flags: bit 0 FINAL, the owner had left that generation;
//                 bit 1 CLOCK_SYNCED, the clock was synchronised at the write
//       44     4  reserved, 0
//       48    32  config_hash of the tag set scada-tagd ran with
//       80    16  boot_id of the boot that wrote the file
//       96     8  written_boottime_ns, CLOCK_BOOTTIME, when it was written
//      104     -  records, sorted by name, byte-wise, each name once
//        -     4  CRC-32 of every byte before it
//
//   record: type (1 byte; BOOL, INT64, UINT64 or FLOAT64), name length (1
//   byte, 1 to 127), quality (1 byte; GOOD or UNCERTAIN), reserved (1 byte,
//   0), quality_flags (2), reserved (2, 0), value (8), source_time_ns (8),
//   the name, then zero bytes to the next multiple of 8. The flags have no
//   unknown bit and none of NOT_INIT, OWNER_DOWN and RESTORED.
//
// Format version 1 (decision D-45) is read, never written: a header of the
// first 80 bytes above, with no flag CLOCK_SYNCED, and records whose six
// bytes after the name length are all 0. A version 1 record reads as
// UNCERTAIN with flags 0, of a file with no boot_id and no boot time.

#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <sys/types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scada::tag {

inline constexpr std::uint64_t RETAIN_MAGIC = 0x4E54'5241'4441'4353ULL;
// The version written. Version 1 is still read.
inline constexpr std::uint32_t RETAIN_FORMAT_VERSION = 2;
inline constexpr std::uint32_t RETAIN_FORMAT_VERSION_1 = 1;
inline constexpr std::uint32_t RETAIN_FLAG_FINAL = 1U;
inline constexpr std::uint32_t RETAIN_FLAG_CLOCK_SYNCED = 2U;
inline constexpr std::size_t RETAIN_HEADER_BYTES_V1 = 80;
inline constexpr std::size_t RETAIN_HEADER_BYTES = 104;
inline constexpr std::size_t RETAIN_RECORD_FIXED_BYTES = 24;
inline constexpr std::size_t RETAIN_CRC_BYTES = 4;

// The largest retain file read or written: about 440,000 records with names
// of 127 bytes. An owner with more retained tags cannot be persisted, which
// scada-tagd reports.
inline constexpr std::size_t MAX_RETAIN_FILE_BYTES =
    std::size_t{64} * 1024 * 1024;

// The suffix of a retain file name.
inline constexpr std::string_view RETAIN_FILE_SUFFIX = ".bin";

// A generation, identified across restarts of scada-tagd within one boot:
// its number and the creation time in its header. The number alone starts
// again at 1 after a boot (section 11.3), the time does not repeat.
struct GenerationIdentity {
    std::uint64_t generation = 0;
    std::int64_t created_time_ns = 0;
    friend bool operator==(const GenerationIdentity&,
                           const GenerationIdentity&) = default;
};

// The flags a value keeps when it is restored (decision D-59): those that
// describe the value itself. STALE and COMM_FAIL describe the live link.
inline constexpr std::uint16_t RETAIN_CARRIED_FLAGS =
    static_cast<std::uint16_t>(
        to_raw(QualityFlag::SUBSTITUTED) | to_raw(QualityFlag::BLOCKED) |
        to_raw(QualityFlag::TEST) | to_raw(QualityFlag::OUT_OF_RANGE) |
        to_raw(QualityFlag::OVERFLOW) | to_raw(QualityFlag::LOCAL_TIME) |
        to_raw(QualityFlag::TIME_UNCERTAIN));

// The flags a retain file may not hold: they describe a record, not a value.
inline constexpr std::uint16_t RETAIN_REFUSED_FLAGS =
    static_cast<std::uint16_t>(to_raw(QualityFlag::NOT_INIT) |
                               to_raw(QualityFlag::OWNER_DOWN) |
                               to_raw(QualityFlag::RESTORED));

// The flags of `flags` a retain file can hold: `flags` without the refused
// ones and the bits that are no flag.
[[nodiscard]] constexpr std::uint16_t retainable_flags(
    std::uint16_t flags) noexcept {
    return static_cast<std::uint16_t>(flags & QUALITY_FLAGS_MASK &
                                      ~RETAIN_REFUSED_FLAGS);
}

// The `boot_id` of a boot (/proc/sys/kernel/random/boot_id, 16 bytes); all
// zero when it is not known.
inline constexpr std::size_t BOOT_ID_BYTES = 16;
using BootId = std::array<std::uint8_t, BOOT_ID_BYTES>;

// One retained value.
struct RetainedValue {
    std::string name;              // a tag name
    TagType type = TagType::BOOL;  // never STRING
    std::uint64_t value = 0;       // bit pattern; a BOOL is 0 or 1
    std::int64_t source_time_ns = 0;
    // As the owner stored it (decision D-59): GOOD or UNCERTAIN, and flags
    // without NOT_INIT, OWNER_DOWN and RESTORED.
    Quality quality = Quality::UNCERTAIN;
    std::uint16_t quality_flags = 0;
    friend bool operator==(const RetainedValue&,
                           const RetainedValue&) = default;
};

// When and by which boot a retain file was written (decision D-59).
struct RetainStamp {
    std::int64_t written_time_ns = 0;      // CLOCK_REALTIME
    std::int64_t written_boottime_ns = 0;  // CLOCK_BOOTTIME; 0 in version 1
    BootId boot_id{};                      // all zero in version 1
    bool clock_synced = false;             // CLOCK_REALTIME synchronised
    friend bool operator==(const RetainStamp&, const RetainStamp&) = default;
};

// The clocks of the reader, for the age of a retained value.
struct RetainClockSample {
    std::int64_t realtime_ns = 0;
    std::int64_t boottime_ns = 0;
    BootId boot_id{};  // all zero if not known
    bool clock_synced = false;
};

// The clocks now: CLOCK_REALTIME, CLOCK_BOOTTIME, the boot_id, and whether
// adjtimex(2) with modes 0 reports the clock synchronised (neither
// TIME_ERROR nor STA_UNSYNC). Not real-time safe.
[[nodiscard]] RetainClockSample retain_clock_now() noexcept;

// The age of a value of a file stamped `stamp` (decision D-59): in the same
// boot (equal boot ids, neither all zero), CLOCK_BOOTTIME now minus the
// written boot time; in another boot, CLOCK_REALTIME now minus the written
// time, only if the clock was synchronised at the write and is now. Nothing
// (unknown) otherwise, and for a difference below 0.
[[nodiscard]] std::optional<std::int64_t> retained_age_ns(
    const RetainStamp& stamp, const RetainClockSample& now) noexcept;

// The contents of a retain file.
struct RetainFile {
    GenerationIdentity source;
    std::int64_t written_time_ns = 0;
    bool final = false;
    ConfigHash config_hash{};
    // Version 2 only; zero and false in a file of version 1.
    std::int64_t written_boottime_ns = 0;
    BootId boot_id{};
    bool clock_synced = false;
    // The version the file was decoded from; `encode_retain_file` ignores it.
    std::uint32_t version = RETAIN_FORMAT_VERSION;
    // Sorted by name, byte-wise, each name once.
    std::vector<RetainedValue> values;
};

// The file name of the retained values of `owner`: "<owner>.bin".
[[nodiscard]] std::string retain_file_name(std::string_view owner);

// The owner of the retain file name `entry`, or nothing if `entry` is not
// "<owner>.bin" with a valid owner name. `entry` is outside input.
[[nodiscard]] std::optional<std::string_view> owner_of_retain_file(
    std::string_view entry) noexcept;

// The stamp of `file`.
[[nodiscard]] RetainStamp stamp_of(const RetainFile& file) noexcept;

// The bytes of `file` in format version 2. INVALID_ARGUMENT if a value is
// not what the format allows (a STRING, a BOOL that is neither 0 nor 1, a
// name that is no tag name, names out of order or twice, a quality other
// than GOOD or UNCERTAIN, a flag bit that is no flag or one of NOT_INIT,
// OWNER_DOWN and RESTORED), RESOURCE_EXHAUSTED if the file would be larger
// than MAX_RETAIN_FILE_BYTES.
[[nodiscard]] Result<std::vector<std::byte>> encode_retain_file(
    const RetainFile& file, std::string& detail);

// The bytes of `file` in format version 1, which scada-tagd no longer
// writes: for tests and the fuzz target, which check that version 1 is
// still read. INVALID_ARGUMENT, besides the cases of `encode_retain_file`,
// for a value that version 1 cannot hold (quality other than UNCERTAIN,
// flags other than 0) and for a stamp of version 2 (a boot time, a
// boot_id, CLOCK_SYNCED).
[[nodiscard]] Result<std::vector<std::byte>> encode_retain_file_v1(
    const RetainFile& file, std::string& detail);

// Parses `bytes`, version 1 or 2, with every check of the format.
// DATA_CORRUPT, with `detail` saying what was found, for anything else: a
// wrong magic, version, length, check sum, reserved field, flag, type,
// value, quality, quality flag, name or order. A record of version 1 reads
// as UNCERTAIN with flags 0.
[[nodiscard]] Result<RetainFile> decode_retain_file(
    std::span<const std::byte> bytes, std::string& detail);

// Reads and parses the retain file `name` in the directory open as `dir_fd`
// (section 12): opened without following a symbolic link, a regular file
// owned by `tagd_uid`, at most MAX_RETAIN_FILE_BYTES. NOT_FOUND if there is
// no such file, PERMISSION_DENIED for another owner, DATA_CORRUPT for a file
// that is not regular, too large or fails `decode_retain_file`, IO_ERROR
// otherwise.
[[nodiscard]] Result<RetainFile> read_retain_file(int dir_fd,
                                                  std::string_view name,
                                                  std::uint32_t tagd_uid,
                                                  std::string& detail);

// A file being written by `write_retain_file`. Each call returns 0 or an
// error number. Destroying it closes it.
class RetainWriteFile {
public:
    RetainWriteFile() = default;
    virtual ~RetainWriteFile() = default;
    RetainWriteFile(const RetainWriteFile&) = delete;
    RetainWriteFile& operator=(const RetainWriteFile&) = delete;
    RetainWriteFile(RetainWriteFile&&) = delete;
    RetainWriteFile& operator=(RetainWriteFile&&) = delete;

    // fchmod(2).
    [[nodiscard]] virtual int set_mode(mode_t mode) noexcept = 0;
    // write(2) of a part of `bytes`; `written` is how much.
    [[nodiscard]] virtual int write(std::span<const std::byte> bytes,
                                    std::size_t& written) noexcept = 0;
    // fsync(2).
    [[nodiscard]] virtual int sync() noexcept = 0;
};

// The file system calls of `write_retain_file`, so that a test can put a
// model of a power loss in their place (decision D-45). Each call returns
// 0 or an error number. `dir_fd` is the retain directory.
class RetainFileOps {
public:
    RetainFileOps() = default;
    virtual ~RetainFileOps() = default;
    RetainFileOps(const RetainFileOps&) = delete;
    RetainFileOps& operator=(const RetainFileOps&) = delete;
    RetainFileOps(RetainFileOps&&) = delete;
    RetainFileOps& operator=(RetainFileOps&&) = delete;

    // openat(2) with O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
    // mode 0600.
    [[nodiscard]] virtual int create(int dir_fd, const std::string& name,
                                     std::unique_ptr<RetainWriteFile>& out) = 0;
    // renameat(2) within `dir_fd`.
    [[nodiscard]] virtual int rename(int dir_fd, const std::string& from,
                                     const std::string& target) noexcept = 0;
    // fsync(2) of the directory.
    [[nodiscard]] virtual int sync_directory(int dir_fd) noexcept = 0;
    // unlinkat(2).
    [[nodiscard]] virtual int unlink(int dir_fd,
                                     const std::string& name) noexcept = 0;
};

// The operating system's calls.
[[nodiscard]] RetainFileOps& system_retain_file_ops() noexcept;

// Writes `file` as the retained values of `owner` in the directory open as
// `dir_fd`, atomically (section 12), in exactly this order: a temporary
// file ".tmp.<owner>.bin.<pid>" created exclusively (one of this process
// id left by an earlier run is unlinked first), fchmod 0640, write in a
// loop, fsync, rename over "<owner>.bin", fsync of the directory. On any
// error the temporary file is unlinked and the old file stays; a later
// call starts with a fresh file, and an fsync is never tried twice on one
// descriptor. A crash at any point leaves the old file or the new one
// under the real name, never a part of one, provided the storage keeps
// what fsync reported as written.
[[nodiscard]] Status write_retain_file(
    int dir_fd, std::string_view owner, const RetainFile& file,
    std::string& detail, RetainFileOps& ops = system_retain_file_ops());

// Removes the temporary files of `write_retain_file` that the calling user
// owns from the directory open as `dir_fd` (section 12: stale temporary
// files are removed at start). Only scada-tagd writes there, one instance
// at a time. Returns how many were removed.
std::size_t remove_retain_temporaries(int dir_fd) noexcept;

// A retained tag one owner asks for.
struct WantedValue {
    std::string_view name;
    TagType type = TagType::BOOL;
};

// A retain file that failed its checks and was ignored.
struct RejectedRetainFile {
    std::string file;  // the entry name; outside input
    std::string detail;
};

// The file a loaded value came from.
struct RetainSource {
    bool own = false;    // the owner's own file; otherwise another owner's
    bool final = false;  // that file is marked FINAL
    RetainStamp stamp;
};

// What `load_retained` found.
struct LoadedValues {
    // One for each wanted tag, in the order asked; nothing if no file has it
    // with that type.
    std::vector<std::optional<RetainedValue>> values;
    // Where each value of `values` came from; the same size, meaningful
    // where `values` has a value.
    std::vector<RetainSource> sources;
    // The owner's own file, if it could be read.
    std::optional<RetainFile> own;
    std::vector<RejectedRetainFile> rejected;
};

// The retained values of the tags `wanted` of `owner`, from the directory
// open as `dir_fd`: each from the owner's own file if it has the name with
// that type; otherwise from another owner's file that has it (a tag that
// moved to another owner, decision D-45), the most recently written file
// first. A record whose type differs is skipped. A file that fails its
// checks is ignored and listed in `rejected`.
[[nodiscard]] LoadedValues load_retained(int dir_fd, std::uint32_t tagd_uid,
                                         std::string_view owner,
                                         std::span<const WantedValue> wanted);

// The value of `name` in `file`, or null. `file.values` is sorted.
[[nodiscard]] const RetainedValue* find_retained(
    const RetainFile& file, std::string_view name) noexcept;

}  // namespace scada::tag

#endif  // SCADA_TAG_RETAIN_FILE_H
