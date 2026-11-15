#ifndef SCADA_TAG_RETAIN_FILE_H
#define SCADA_TAG_RETAIN_FILE_H

// The retain files `<retain_dir>/<owner>.bin`
// (docs/contracts/tag-database.md, section 12): their format, the atomic
// write of scada-tagd and the lookup of the values of one owner. A retain
// file is outside input to everyone who reads it. Private to libscada-tag.
//
// Format version 1, all integers little-endian:
//
//   offset  size  field
//        0     8  magic "SCADARTN" (0x4E54524144414353)
//        8     4  format version, 1
//       12     4  record count, at most MAX_TAGS_PER_OWNER
//       16     8  source generation: the generation the values were read from
//       24     8  created_time_ns of that generation (GlobalHeader)
//       32     8  written_time_ns, CLOCK_REALTIME, when the file was written
//       40     4  flags: bit 0 FINAL, the owner had left that generation
//       44     4  reserved, 0
//       48    32  config_hash of the tag set scada-tagd ran with
//       80     -  records, sorted by name, byte-wise, each name once
//        -     4  CRC-32 of every byte before it
//
//   record: type (1 byte; BOOL, INT64, UINT64 or FLOAT64), name length (1
//   byte, 1 to 127), 6 reserved bytes (0), value (8), source_time_ns (8),
//   the name, then zero bytes to the next multiple of 8.

#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <sys/types.h>

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
inline constexpr std::uint32_t RETAIN_FORMAT_VERSION = 1;
inline constexpr std::uint32_t RETAIN_FLAG_FINAL = 1U;
inline constexpr std::size_t RETAIN_HEADER_BYTES = 80;
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
    bool operator==(const GenerationIdentity&) const = default;
};

// One retained value.
struct RetainedValue {
    std::string name;              // a tag name
    TagType type = TagType::BOOL;  // never STRING
    std::uint64_t value = 0;       // bit pattern; a BOOL is 0 or 1
    std::int64_t source_time_ns = 0;
};

// The contents of a retain file.
struct RetainFile {
    GenerationIdentity source;
    std::int64_t written_time_ns = 0;
    bool final = false;
    ConfigHash config_hash{};
    // Sorted by name, byte-wise, each name once.
    std::vector<RetainedValue> values;
};

// The file name of the retained values of `owner`: "<owner>.bin".
[[nodiscard]] std::string retain_file_name(std::string_view owner);

// The owner of the retain file name `entry`, or nothing if `entry` is not
// "<owner>.bin" with a valid owner name. `entry` is outside input.
[[nodiscard]] std::optional<std::string_view> owner_of_retain_file(
    std::string_view entry) noexcept;

// The bytes of `file`. INVALID_ARGUMENT if a value is not what the format
// allows (a STRING, a BOOL that is neither 0 nor 1, a name that is no tag
// name, names out of order or twice), RESOURCE_EXHAUSTED if the file would
// be larger than MAX_RETAIN_FILE_BYTES.
[[nodiscard]] Result<std::vector<std::byte>> encode_retain_file(
    const RetainFile& file, std::string& detail);

// Parses `bytes` with every check of the format. DATA_CORRUPT, with
// `detail` saying what was found, for anything else: a wrong magic, version,
// length, check sum, reserved field, flag, type, value, name or order.
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
                                     const std::string& to) noexcept = 0;
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

// What `load_retained` found.
struct LoadedValues {
    // One for each wanted tag, in the order asked; nothing if no file has it
    // with that type.
    std::vector<std::optional<RetainedValue>> values;
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
