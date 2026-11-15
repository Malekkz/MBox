#include "region_file.h"

#include "layout.h"
#include "shared_memory.h"

#include "scada/common/fd.h"
#include "scada/common/mapped_region.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/types.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>  // NOLINT(modernize-deprecated-headers): renameat2 and RENAME_NOREPLACE are declared here, not in <cstdio>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace scada::tag {

namespace {

// Only the permission bits, the set-id bits and the sticky bit of a mode.
constexpr mode_t MODE_BITS = 07777;

// Mode of a file between its creation and the explicit fchmod.
constexpr mode_t CREATION_MODE = 0600;

constexpr std::uint64_t CURSOR_SIZE = sizeof(std::uint64_t);

// See `last_os_error`. One value for each thread, like errno itself.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): the per-thread error number behind last_os_error, the counterpart of errno
thread_local int last_os_error_value = 0;

bool is_decimal_digit(char character) noexcept {
    return character >= '0' && character <= '9';
}

// Closes a directory stream.
struct DirCloser {
    void operator()(DIR* stream) const noexcept {
        static_cast<void>(::closedir(stream));
    }
};

std::string describe(std::string_view what, std::string_view name, int error) {
    std::string text(what);
    text += ' ';
    text += name;
    text += ": ";
    text += errno_text(error);
    return text;
}

// The checks of section 3.1 on the RegionHeader of an opened region.
bool header_matches(const RegionHeader& header,
                    const RegionIdentity& identity) noexcept {
    return header.magic == REGION_MAGIC &&
           header.layout_version == LAYOUT_VERSION &&
           header.kind == to_raw(identity.kind) &&
           header.generation == identity.generation &&
           header.size_bytes == identity.size_bytes &&
           header.owner_index == identity.owner_index &&
           header.queue_id == identity.queue_id;
}

}  // namespace

Fd open_at(int dir_fd, const std::string& name, int flags,
           mode_t mode) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): openat has no other form
    return Fd(::openat(dir_fd, name.c_str(), flags, mode));
}

std::string errno_text(int error) {
    last_os_error_value = error;
    return std::generic_category().message(error);
}

int last_os_error() noexcept {
    return last_os_error_value;
}

void clear_os_error() noexcept {
    last_os_error_value = 0;
}

std::string proc_region_name(std::string_view process) {
    std::string name("proc.");
    name += process;
    return name;
}

std::string tags_region_name(std::string_view owner) {
    std::string name("tags.");
    name += owner;
    return name;
}

std::string strings_region_name(std::string_view owner) {
    std::string name("strings.");
    name += owner;
    return name;
}

std::string changes_region_name(std::string_view owner) {
    std::string name("changes.");
    name += owner;
    return name;
}

std::string generation_dir_name(std::uint64_t generation) {
    std::string name(GENERATION_DIR_PREFIX);
    name += std::to_string(generation);
    return name;
}

std::optional<std::uint64_t> parse_generation_dir_name(
    std::string_view name) noexcept {
    if (!name.starts_with(GENERATION_DIR_PREFIX)) {
        return std::nullopt;
    }
    const std::string_view digits = name.substr(GENERATION_DIR_PREFIX.size());
    if (digits.empty() || digits.front() == '0' ||
        !std::ranges::all_of(digits, is_decimal_digit)) {
        return std::nullopt;
    }
    std::uint64_t generation = 0;
    const std::from_chars_result parsed = std::from_chars(
        digits.data(), digits.data() + digits.size(), generation);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != digits.data() + digits.size()) {
        return std::nullopt;  // does not fit 64 bits
    }
    return generation;
}

std::string temporary_name(std::string_view name, pid_t pid) {
    std::string temporary(TEMPORARY_PREFIX);
    temporary += name;
    temporary += '.';
    temporary += std::to_string(pid);
    return temporary;
}

bool is_temporary_name_of(std::string_view entry,
                          std::string_view name) noexcept {
    if (!entry.starts_with(TEMPORARY_PREFIX)) {
        return false;
    }
    entry.remove_prefix(TEMPORARY_PREFIX.size());
    if (!entry.starts_with(name)) {
        return false;
    }
    entry.remove_prefix(name.size());
    if (!entry.starts_with('.')) {
        return false;
    }
    entry.remove_prefix(1);
    return !entry.empty() && std::ranges::all_of(entry, is_decimal_digit);
}

std::uint64_t proc_region_size(std::size_t queue_count) noexcept {
    return sizeof(RegionHeader) + sizeof(ProcHeader) +
           (queue_count * CURSOR_SIZE);
}

std::uint64_t tags_region_size(std::uint64_t tag_count) noexcept {
    return sizeof(RegionHeader) + (tag_count * sizeof(TagRecord));
}

std::uint64_t strings_region_size(std::uint64_t string_slots) noexcept {
    return sizeof(RegionHeader) + (string_slots * sizeof(StringSlot));
}

std::uint64_t changes_region_size(std::uint64_t capacity) noexcept {
    return sizeof(RegionHeader) + sizeof(ChangeRingHeader) +
           (capacity * sizeof(ChangeEntry));
}

std::uint64_t queue_region_size(std::uint64_t capacity,
                                std::uint64_t record_size) noexcept {
    return sizeof(RegionHeader) + sizeof(QueueHeader) +
           (capacity * record_size);
}

Result<Fd> open_base_directory(const std::string& path, std::uint32_t tagd_uid,
                               std::string& detail) {
    Fd directory = open_at(AT_FDCWD, path,
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (!directory.valid()) {
        const int error = errno;
        detail = describe("cannot open the base directory", path, error);
        if (error == ENOENT) {
            return Status(StatusCode::NOT_FOUND,
                          "the base directory does not exist");
        }
        if (error == ENOTDIR || error == ELOOP) {
            return Status(StatusCode::PERMISSION_DENIED,
                          "the base directory is not a directory");
        }
        // The caller is not in the group of the database (section 3.1).
        if (error == EACCES || error == EPERM) {
            return Status(StatusCode::PERMISSION_DENIED,
                          "no permission to open the base directory");
        }
        return Status(StatusCode::IO_ERROR, "cannot open the base directory");
    }
    struct stat info {};
    if (::fstat(directory.get(), &info) != 0) {
        detail = describe("cannot examine the base directory", path, errno);
        return Status(StatusCode::IO_ERROR,
                      "cannot examine the base directory");
    }
    if (info.st_uid != tagd_uid) {
        detail = "the base directory " + path + " is owned by user " +
                 std::to_string(info.st_uid) + ", not by user " +
                 std::to_string(tagd_uid);
        return Status(StatusCode::PERMISSION_DENIED,
                      "the base directory has the wrong owner");
    }
    if ((info.st_mode & MODE_BITS) != BASE_DIR_MODE) {
        detail = "the base directory " + path + " does not have mode 2750";
        return Status(StatusCode::PERMISSION_DENIED,
                      "the base directory has the wrong mode");
    }
    return directory;
}

Result<Fd> open_retain_directory(const std::string& path,
                                 std::uint32_t tagd_uid, int base_fd,
                                 std::string& detail) {
    Result<Fd> directory = open_base_directory(path, tagd_uid, detail);
    if (!directory.ok()) {
        detail = "retain directory: " + detail;
        return directory;
    }
    struct stat retain {};
    struct stat base {};
    if (::fstat(directory.value().get(), &retain) != 0 ||
        ::fstat(base_fd, &base) != 0) {
        detail = describe("cannot examine the retain directory", path, errno);
        return Status(StatusCode::IO_ERROR,
                      "cannot examine the retain directory");
    }
    if (retain.st_gid != base.st_gid) {
        detail = "the retain directory " + path + " has group " +
                 std::to_string(retain.st_gid) +
                 ", not the group of the base directory, " +
                 std::to_string(base.st_gid);
        return Status(StatusCode::PERMISSION_DENIED,
                      "the retain directory has the wrong group");
    }
    return directory;
}

Result<std::vector<std::string>> list_directory(int dir_fd,
                                                std::string& detail) {
    // A descriptor of its own: the stream takes it over, moves its position
    // and closes it.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): openat has no other form
    const int copy = ::openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (copy < 0) {
        detail = "cannot open the directory: " + errno_text(errno);
        return Status(StatusCode::IO_ERROR, "cannot open a directory");
    }
    const std::unique_ptr<DIR, DirCloser> stream(::fdopendir(copy));
    if (stream == nullptr) {
        const int error = errno;
        const Fd unused(copy);  // not taken over: closed here
        detail = "cannot read the directory: " + errno_text(error);
        return Status(StatusCode::IO_ERROR, "cannot read a directory");
    }

    std::vector<std::string> names;
    while (names.size() < MAX_DIRECTORY_ENTRIES) {
        errno = 0;
        // NOLINTNEXTLINE(concurrency-mt-unsafe): the stream belongs to this call alone; readdir is safe on separate streams
        const dirent* const entry = ::readdir(stream.get());
        if (entry == nullptr) {
            if (errno != 0) {
                detail = "cannot read the directory: " + errno_text(errno);
                return Status(StatusCode::IO_ERROR, "cannot read a directory");
            }
            break;
        }
        // d_name is the one C array of the interface; it is NUL-terminated.
        const std::string_view name(std::data(
            entry
                ->d_name));  // NOLINT(bugprone-suspicious-stringview-data-usage,cppcoreguidelines-pro-bounds-array-to-pointer-decay): NUL-terminated by the operating system
        if (name != "." && name != "..") {
            names.emplace_back(name);
        }
    }
    // The operating system gives no order; a fixed one makes what callers
    // do, and what they log, the same on every file system.
    std::ranges::sort(names);
    return names;
}

RegionBuilder::RegionBuilder(int dir_fd, std::string name,
                             std::string temporary, Fd file,
                             MappedRegion memory) noexcept
    : dir_fd_(dir_fd),
      name_(std::move(name)),
      temporary_(std::move(temporary)),
      file_(std::move(file)),
      memory_(std::move(memory)) {}

RegionBuilder::RegionBuilder(RegionBuilder&& other) noexcept
    : dir_fd_(other.dir_fd_),
      name_(std::move(other.name_)),
      temporary_(std::exchange(other.temporary_, {})),
      file_(std::move(other.file_)),
      memory_(std::move(other.memory_)) {}

RegionBuilder::~RegionBuilder() {
    discard();
}

void RegionBuilder::discard() noexcept {
    if (!temporary_.empty()) {
        // Best effort: a file that stays is removed as stale at the next
        // start.
        static_cast<void>(::unlinkat(dir_fd_, temporary_.c_str(), 0));
        temporary_.clear();
    }
}

Result<RegionBuilder> RegionBuilder::create(int dir_fd, std::string_view name,
                                            const RegionIdentity& identity,
                                            std::string& detail) {
    if (identity.size_bytes < sizeof(RegionHeader) ||
        identity.size_bytes >
            static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        detail = "region " + std::string(name) + " has an impossible size";
        return Status(StatusCode::INVALID_ARGUMENT,
                      "a region has an impossible size");
    }
    // In range: checked above, and off_t is 64 bits on the supported
    // targets.
    const auto size = static_cast<off_t>(identity.size_bytes);
    const auto mapped_size = static_cast<std::size_t>(identity.size_bytes);

    std::string temporary = temporary_name(name, ::getpid());
    // A file this process id left behind in an earlier life.
    static_cast<void>(::unlinkat(dir_fd, temporary.c_str(), 0));
    Fd file = open_at(dir_fd, temporary,
                      O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                      CREATION_MODE);
    if (!file.valid()) {
        detail = describe("cannot create", temporary, errno);
        return Status(StatusCode::IO_ERROR, "cannot create a region file");
    }
    // From here the temporary file is removed on every error.
    RegionBuilder builder(dir_fd, std::string(name), std::move(temporary),
                          std::move(file), MappedRegion());

    // The creation mode was reduced by the umask (section 3.1).
    if (::fchmod(builder.file_.get(), REGION_FILE_MODE) != 0 ||
        ::ftruncate(builder.file_.get(), size) != 0) {
        const int error = errno;
        detail = describe("cannot size", builder.temporary_, error);
        return error == ENOSPC || error == EFBIG
                   ? Status(StatusCode::RESOURCE_EXHAUSTED,
                            "no room for a region file")
                   : Status(StatusCode::IO_ERROR, "cannot size a region file");
    }
    // Returns the error number; errno is not set.
    int allocated = ::posix_fallocate(builder.file_.get(), 0, size);
    while (allocated == EINTR) {
        allocated = ::posix_fallocate(builder.file_.get(), 0, size);
    }
    if (allocated != 0) {
        detail = describe("cannot allocate", builder.temporary_, allocated);
        return allocated == ENOSPC || allocated == EFBIG
                   ? Status(StatusCode::RESOURCE_EXHAUSTED,
                            "no room for a region file")
                   : Status(StatusCode::IO_ERROR,
                            "cannot allocate a region file");
    }

    Result<MappedRegion> memory = MappedRegion::map(
        builder.file_.get(), mapped_size, MapAccess::READ_WRITE);
    if (!memory.ok()) {
        detail = describe("cannot map", builder.temporary_, errno);
        return memory.status();
    }
    builder.memory_ = std::move(memory).value();

    RegionHeader header;
    header.magic = REGION_MAGIC;
    header.layout_version = LAYOUT_VERSION;
    header.kind = to_raw(identity.kind);
    header.generation = identity.generation;
    header.size_bytes = identity.size_bytes;
    header.owner_index = identity.owner_index;
    header.queue_id = identity.queue_id;
    header.ready = 0;
    // Nobody else has the file mapped yet: it has no name others look for.
    shared_at<RegionHeader>(builder.memory_.bytes(), 0) = header;
    return builder;
}

Result<RegionBuilder::Published> RegionBuilder::publish(std::string& detail) {
    if (temporary_.empty()) {
        return Status(StatusCode::FAILED_PRECONDITION,
                      "the region was already published");
    }
    // Written last (section 4): the release order publishes everything the
    // caller stored before.
    store_shared(shared_at<RegionHeader>(memory_.bytes(), 0).ready, 1U,
                 std::memory_order_release);
    if (::renameat2(dir_fd_, temporary_.c_str(), dir_fd_, name_.c_str(),
                    RENAME_NOREPLACE) != 0) {
        const int error = errno;
        detail = describe("cannot rename to", name_, error);
        discard();
        return error == EEXIST ? Status(StatusCode::ALREADY_EXISTS,
                                        "the region file already exists")
                               : Status(StatusCode::IO_ERROR,
                                        "cannot rename a region file");
    }
    temporary_.clear();
    return Published{std::move(file_), std::move(memory_)};
}

Result<OpenedRegion> open_region(int dir_fd, std::string_view name,
                                 const RegionIdentity& identity,
                                 MapAccess access, std::string& detail) {
    const std::string path(name);
    // O_NONBLOCK: a FIFO under a region name must not block the open.
    const int flags = (access == MapAccess::READ_WRITE ? O_RDWR : O_RDONLY) |
                      O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
    OpenedRegion region;
    region.file = open_at(dir_fd, path, flags);
    if (!region.file.valid()) {
        const int error = errno;
        detail = describe("cannot open", path, error);
        switch (error) {
            case ENOENT:
                return Status(StatusCode::NOT_FOUND, "no such region file");
            case ELOOP:
                return Status(StatusCode::DATA_CORRUPT,
                              "a symbolic link is in the place of a region");
            case EACCES:
                return Status(StatusCode::PERMISSION_DENIED,
                              "the region file cannot be opened");
            default:
                return Status(StatusCode::IO_ERROR,
                              "cannot open a region file");
        }
    }
    struct stat info {};
    if (::fstat(region.file.get(), &info) != 0) {
        detail = describe("cannot examine", path, errno);
        return Status(StatusCode::IO_ERROR, "cannot examine a region file");
    }
    if (!S_ISREG(info.st_mode)) {
        detail = path + " is not a regular file";
        return Status(StatusCode::DATA_CORRUPT,
                      "a region is not a regular file");
    }
    if (info.st_uid != identity.uid) {
        detail = path + " is owned by user " + std::to_string(info.st_uid) +
                 ", not by user " + std::to_string(identity.uid);
        return Status(StatusCode::PERMISSION_DENIED,
                      "a region file has the wrong owner");
    }
    if (info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) != identity.size_bytes ||
        identity.size_bytes < sizeof(RegionHeader)) {
        detail = path + " does not have the expected size";
        return Status(StatusCode::DATA_CORRUPT,
                      "a region file has the wrong size");
    }
    region.device = info.st_dev;
    region.inode = info.st_ino;

    // In range: equal to the size of an existing file.
    Result<MappedRegion> memory = MappedRegion::map(
        region.file.get(), static_cast<std::size_t>(identity.size_bytes),
        access);
    if (!memory.ok()) {
        detail = describe("cannot map", path, errno);
        return memory.status();
    }
    region.memory = std::move(memory).value();

    auto& header = shared_at<RegionHeader>(region.memory.bytes(), 0);
    // `ready` first (section 4): the other fields are complete only then.
    if (load_shared(header.ready, std::memory_order_acquire) != 1U) {
        detail = path + " is not ready";
        return Status(StatusCode::UNAVAILABLE, "a region is not ready");
    }
    if (!header_matches(header, identity)) {
        detail = path + " has a region header that does not match";
        return Status(StatusCode::DATA_CORRUPT,
                      "a region header does not match");
    }
    return region;
}

void remove_stale_temporaries(int dir_fd, std::string_view name,
                              std::uint32_t uid) noexcept {
    std::string ignored;
    const Result<std::vector<std::string>> entries =
        list_directory(dir_fd, ignored);
    if (!entries.ok()) {
        return;
    }
    for (const std::string& entry : entries.value()) {
        if (!is_temporary_name_of(entry, name)) {
            continue;
        }
        struct stat info {};
        if (::fstatat(dir_fd, entry.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 &&
            info.st_uid == uid) {
            static_cast<void>(::unlinkat(dir_fd, entry.c_str(), 0));
        }
    }
}

bool proc_lock_held(int dir_fd, const std::string& name) noexcept {
    const Fd file =
        open_at(dir_fd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (!file.valid()) {
        // Only a region that is not there is known to have no process.
        // Any other error (EMFILE, EACCES, ...) leaves it unknown, which
        // counts as held: a live process must never be taken as gone.
        return errno != ENOENT;
    }
    // The lock taken here, if any, ends when `file` is closed. EWOULDBLOCK
    // means held; any other failure is unknown, and counts as held too.
    return ::flock(file.get(), LOCK_EX | LOCK_NB) != 0;
}

bool has_live_process(int base_fd, const std::string& name) {
    const Fd directory =
        open_at(base_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
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
        return entry.starts_with("proc.") &&
               proc_lock_held(directory.get(), entry);
    });
}

bool remove_generation(int base_fd, const std::string& name) {
    const Fd directory =
        open_at(base_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
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

}  // namespace scada::tag
