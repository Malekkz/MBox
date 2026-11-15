#include "retain_file.h"

#include "crc32.h"
#include "region_file.h"

#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/handle.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <fcntl.h>
#include <stdio.h>  // NOLINT(modernize-deprecated-headers): renameat is declared here, not in <cstdio>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::tag {

namespace {

constexpr std::size_t BITS_PER_BYTE = 8;
constexpr std::size_t RECORD_ALIGNMENT = 8;
constexpr std::uint64_t LOW_BYTE = 0xFFU;
constexpr mode_t RETAIN_FILE_MODE = 0640;
constexpr mode_t CREATION_MODE = 0600;
constexpr std::size_t READ_CHUNK = std::size_t{64} * 1024;

// Bytes of padding after a name of `length` bytes.
constexpr std::size_t padding_after(std::size_t length) noexcept {
    return (RECORD_ALIGNMENT - (length % RECORD_ALIGNMENT)) % RECORD_ALIGNMENT;
}

// Appends integers little-endian.
class Writer {
public:
    explicit Writer(std::vector<std::byte>& out) noexcept : out_(out) {}

    template <typename Unsigned>
    void put(Unsigned value) {
        for (std::size_t i = 0; i < sizeof(Unsigned); ++i) {
            out_.push_back(static_cast<std::byte>(
                (static_cast<std::uint64_t>(value) >> (i * BITS_PER_BYTE)) &
                LOW_BYTE));
        }
    }

    void put_bytes(std::string_view text) {
        for (const char c : text) {
            out_.push_back(static_cast<std::byte>(c));
        }
    }

    void put_zeros(std::size_t count) {
        out_.insert(out_.end(), count, std::byte{0});
    }

private:
    std::vector<std::byte>& out_;
};

// Reads integers little-endian; every read is checked against what is left.
class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) noexcept
        : bytes_(bytes) {}

    template <typename Unsigned>
    [[nodiscard]] bool get(Unsigned& value) noexcept {
        if (bytes_.size() - position_ < sizeof(Unsigned)) {
            return false;
        }
        std::uint64_t result = 0;
        for (std::size_t i = 0; i < sizeof(Unsigned); ++i) {
            result |= std::to_integer<std::uint64_t>(bytes_[position_ + i])
                      << (i * BITS_PER_BYTE);
        }
        position_ += sizeof(Unsigned);
        // In range: built from sizeof(Unsigned) bytes.
        value = static_cast<Unsigned>(result);
        return true;
    }

    [[nodiscard]] bool get_text(std::size_t length, std::string& text) {
        if (bytes_.size() - position_ < length) {
            return false;
        }
        text.clear();
        for (const std::byte byte : bytes_.subspan(position_, length)) {
            text.push_back(
                static_cast<char>(std::to_integer<unsigned char>(byte)));
        }
        position_ += length;
        return true;
    }

    // True if the next `count` bytes are there and all zero.
    [[nodiscard]] bool skip_zeros(std::size_t count) noexcept {
        if (bytes_.size() - position_ < count) {
            return false;
        }
        const auto part = bytes_.subspan(position_, count);
        position_ += count;
        return std::ranges::all_of(
            part, [](std::byte b) { return b == std::byte{0}; });
    }

    [[nodiscard]] std::size_t position() const noexcept { return position_; }

private:
    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
};

bool retainable(TagType type) noexcept {
    return type == TagType::BOOL || type == TagType::INT64 ||
           type == TagType::UINT64 || type == TagType::FLOAT64;
}

Status corrupt(std::string& detail, std::string what) {
    detail = std::move(what);
    return {StatusCode::DATA_CORRUPT, "the retain file is not valid"};
}

// True if `entry` is ".tmp.<owner>.bin.<digits>" for some owner name.
bool is_retain_temporary(std::string_view entry) noexcept {
    if (!entry.starts_with(TEMPORARY_PREFIX)) {
        return false;
    }
    const std::size_t dot = entry.rfind('.');
    if (dot == std::string_view::npos || dot < TEMPORARY_PREFIX.size()) {
        return false;
    }
    const std::string_view file =
        entry.substr(TEMPORARY_PREFIX.size(), dot - TEMPORARY_PREFIX.size());
    return owner_of_retain_file(file).has_value() &&
           is_temporary_name_of(entry, file);
}

}  // namespace

std::string retain_file_name(std::string_view owner) {
    std::string name(owner);
    name += RETAIN_FILE_SUFFIX;
    return name;
}

std::optional<std::string_view> owner_of_retain_file(
    std::string_view entry) noexcept {
    if (!entry.ends_with(RETAIN_FILE_SUFFIX)) {
        return std::nullopt;
    }
    const std::string_view owner =
        entry.substr(0, entry.size() - RETAIN_FILE_SUFFIX.size());
    if (!is_valid_name(owner, MAX_SHORT_NAME_LENGTH)) {
        return std::nullopt;
    }
    return owner;
}

Result<std::vector<std::byte>> encode_retain_file(const RetainFile& file,
                                                  std::string& detail) {
    if (file.values.size() > MAX_TAGS_PER_OWNER) {
        detail = "more records than an owner has tags";
        return Status(StatusCode::RESOURCE_EXHAUSTED,
                      "too many retained values");
    }
    std::size_t total = RETAIN_HEADER_BYTES + RETAIN_CRC_BYTES;
    const RetainedValue* previous = nullptr;
    for (const RetainedValue& value : file.values) {
        if (!is_valid_name(value.name, MAX_TAG_NAME_LENGTH) ||
            !retainable(value.type) ||
            (value.type == TagType::BOOL && value.value > 1)) {
            detail = "a value that a retain file cannot hold";
            return Status(StatusCode::INVALID_ARGUMENT,
                          "the retained value is not valid");
        }
        if (previous != nullptr && !(previous->name < value.name)) {
            detail = "the names are not in order or not unique";
            return Status(StatusCode::INVALID_ARGUMENT,
                          "the retained values are not sorted");
        }
        previous = &value;
        // Bounded: at most MAX_TAGS_PER_OWNER records of at most 152 bytes.
        total += RETAIN_RECORD_FIXED_BYTES + value.name.size() +
                 padding_after(value.name.size());
    }
    if (total > MAX_RETAIN_FILE_BYTES) {
        detail = "the file would take " + std::to_string(total) + " bytes";
        return Status(StatusCode::RESOURCE_EXHAUSTED,
                      "the retain file would be too large");
    }

    std::vector<std::byte> bytes;
    bytes.reserve(total);
    Writer out(bytes);
    out.put(RETAIN_MAGIC);
    out.put(RETAIN_FORMAT_VERSION);
    // In range: checked against MAX_TAGS_PER_OWNER above.
    out.put(static_cast<std::uint32_t>(file.values.size()));
    out.put(file.source.generation);
    out.put(static_cast<std::uint64_t>(file.source.created_time_ns));
    out.put(static_cast<std::uint64_t>(file.written_time_ns));
    out.put(file.final ? RETAIN_FLAG_FINAL : 0U);
    out.put(std::uint32_t{0});
    for (const std::uint8_t byte : file.config_hash) {
        out.put(byte);
    }
    for (const RetainedValue& value : file.values) {
        out.put(to_raw(value.type));
        // In range: a tag name is at most 127 bytes.
        out.put(static_cast<std::uint8_t>(value.name.size()));
        out.put_zeros(6);
        out.put(value.value);
        out.put(static_cast<std::uint64_t>(value.source_time_ns));
        out.put_bytes(value.name);
        out.put_zeros(padding_after(value.name.size()));
    }
    out.put(crc32(bytes));
    return bytes;
}

Result<RetainFile> decode_retain_file(std::span<const std::byte> bytes,
                                      std::string& detail) {
    if (bytes.size() > MAX_RETAIN_FILE_BYTES) {
        return corrupt(detail, "the file is too large");
    }
    if (bytes.size() < RETAIN_HEADER_BYTES + RETAIN_CRC_BYTES) {
        return corrupt(detail, "the file is too short");
    }
    const std::size_t body = bytes.size() - RETAIN_CRC_BYTES;
    Reader crc_reader(bytes.subspan(body));
    std::uint32_t stored_crc = 0;
    if (!crc_reader.get(stored_crc) || stored_crc != crc32(bytes.first(body))) {
        return corrupt(detail, "the check sum does not match");
    }

    Reader in(bytes.first(body));
    std::uint64_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t count = 0;
    std::uint64_t created = 0;
    std::uint64_t written = 0;
    std::uint32_t flags = 0;
    RetainFile file;
    if (!in.get(magic) || magic != RETAIN_MAGIC) {
        return corrupt(detail, "the magic is not that of a retain file");
    }
    if (!in.get(version) || version != RETAIN_FORMAT_VERSION) {
        return corrupt(detail, "format version " + std::to_string(version) +
                                   " is not known");
    }
    if (!in.get(count) || !in.get(file.source.generation) || !in.get(created) ||
        !in.get(written) || !in.get(flags) || !in.skip_zeros(4)) {
        return corrupt(detail, "a reserved field is not 0");
    }
    if ((flags & ~RETAIN_FLAG_FINAL) != 0) {
        return corrupt(detail, "an unknown flag is set");
    }
    // Bit patterns of int64_t times, written as such.
    file.source.created_time_ns = static_cast<std::int64_t>(created);
    file.written_time_ns = static_cast<std::int64_t>(written);
    file.final = (flags & RETAIN_FLAG_FINAL) != 0;
    for (std::uint8_t& byte : file.config_hash) {
        if (!in.get(byte)) {
            return corrupt(detail, "the file is too short");
        }
    }
    // Each record takes at least 32 bytes, so the count is checked against
    // the bytes there before anything is reserved for it.
    const std::size_t smallest = RETAIN_RECORD_FIXED_BYTES + RECORD_ALIGNMENT;
    if (count > MAX_TAGS_PER_OWNER ||
        count > (body - RETAIN_HEADER_BYTES) / smallest) {
        return corrupt(detail, "the record count does not fit the file");
    }
    file.values.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint8_t type = 0;
        std::uint8_t length = 0;
        std::uint64_t source_time = 0;
        RetainedValue value;
        if (!in.get(type) || !in.get(length) || !in.skip_zeros(6) ||
            !in.get(value.value) || !in.get(source_time) ||
            !in.get_text(length, value.name) ||
            !in.skip_zeros(padding_after(length))) {
            return corrupt(detail,
                           "record " + std::to_string(i) +
                               " is cut off or has a reserved byte set");
        }
        const std::optional<TagType> tag_type = tag_type_from_raw(type);
        if (!tag_type.has_value() || !retainable(*tag_type) ||
            (*tag_type == TagType::BOOL && value.value > 1)) {
            return corrupt(detail,
                           "record " + std::to_string(i) +
                               " has a type or value that is not valid");
        }
        if (!is_valid_name(value.name, MAX_TAG_NAME_LENGTH)) {
            return corrupt(detail, "record " + std::to_string(i) +
                                       " has a name that is no tag name");
        }
        if (!file.values.empty() && !(file.values.back().name < value.name)) {
            return corrupt(detail, "record " + std::to_string(i) +
                                       " is out of order or repeated");
        }
        value.type = *tag_type;
        value.source_time_ns = static_cast<std::int64_t>(source_time);
        file.values.push_back(std::move(value));
    }
    if (in.position() != body) {
        return corrupt(detail, "bytes follow the last record");
    }
    return file;
}

Result<RetainFile> read_retain_file(int dir_fd, std::string_view name,
                                    std::uint32_t tagd_uid,
                                    std::string& detail) {
    const std::string file_name(name);
    const Fd file = open_at(dir_fd, file_name,
                            O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (!file.valid()) {
        const int error = errno;
        detail = "cannot open " + file_name + ": " + errno_text(error);
        if (error == ENOENT) {
            return Status(StatusCode::NOT_FOUND, "there is no retain file");
        }
        if (error == ELOOP) {
            return corrupt(detail, file_name + " is a symbolic link");
        }
        return Status(StatusCode::IO_ERROR, "cannot open the retain file");
    }
    struct stat info {};
    if (::fstat(file.get(), &info) != 0) {
        detail = "cannot examine " + file_name + ": " + errno_text(errno);
        return Status(StatusCode::IO_ERROR, "cannot examine the retain file");
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) > MAX_RETAIN_FILE_BYTES) {
        return corrupt(detail,
                       file_name + " is not a regular file of at most " +
                           std::to_string(MAX_RETAIN_FILE_BYTES) + " bytes");
    }
    // A second name is a hard link someone made; the file may then change
    // under another name (decision D-44).
    if (info.st_nlink != 1) {
        return corrupt(detail, file_name + " has more than one name");
    }
    if (info.st_uid != tagd_uid) {
        detail =
            file_name + " is not owned by user " + std::to_string(tagd_uid);
        return Status(StatusCode::PERMISSION_DENIED,
                      "the retain file has the wrong owner");
    }
    std::vector<std::byte> bytes;
    // In range: at most MAX_RETAIN_FILE_BYTES, checked above.
    bytes.reserve(static_cast<std::size_t>(info.st_size));
    std::vector<std::byte> chunk(READ_CHUNK);
    for (;;) {
        const ssize_t got = ::read(file.get(), chunk.data(), chunk.size());
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            detail = "cannot read " + file_name + ": " + errno_text(errno);
            return Status(StatusCode::IO_ERROR, "cannot read the retain file");
        }
        if (got == 0) {
            break;
        }
        // In range: read returns at most the size of the chunk.
        const auto part = std::span<const std::byte>(chunk).first(
            static_cast<std::size_t>(got));
        bytes.insert(bytes.end(), part.begin(), part.end());
        // The file may have grown since fstat.
        if (bytes.size() > MAX_RETAIN_FILE_BYTES) {
            return corrupt(detail, file_name + " is too large");
        }
    }
    Result<RetainFile> decoded = decode_retain_file(bytes, detail);
    if (!decoded.ok()) {
        detail = file_name + ": " + detail;
    }
    return decoded;
}

namespace {

class SystemWriteFile final : public RetainWriteFile {
public:
    explicit SystemWriteFile(Fd file) noexcept : file_(std::move(file)) {}

    int set_mode(mode_t mode) noexcept override {
        return ::fchmod(file_.get(), mode) == 0 ? 0 : errno;
    }

    int write(std::span<const std::byte> bytes,
              std::size_t& written) noexcept override {
        const ssize_t done = ::write(file_.get(), bytes.data(), bytes.size());
        if (done < 0) {
            written = 0;
            return errno;
        }
        // In range: write returns at most the size asked for.
        written = static_cast<std::size_t>(done);
        return 0;
    }

    int sync() noexcept override {
        return ::fsync(file_.get()) == 0 ? 0 : errno;
    }

private:
    Fd file_;
};

class SystemFileOps final : public RetainFileOps {
public:
    int create(int dir_fd, const std::string& name,
               std::unique_ptr<RetainWriteFile>& out) override {
        Fd file = open_at(dir_fd, name,
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                          CREATION_MODE);
        if (!file.valid()) {
            return errno;
        }
        out = std::make_unique<SystemWriteFile>(std::move(file));
        return 0;
    }

    int rename(int dir_fd, const std::string& from,
               const std::string& to) noexcept override {
        return ::renameat(dir_fd, from.c_str(), dir_fd, to.c_str()) == 0
                   ? 0
                   : errno;
    }

    int sync_directory(int dir_fd) noexcept override {
        return ::fsync(dir_fd) == 0 ? 0 : errno;
    }

    int unlink(int dir_fd, const std::string& name) noexcept override {
        return ::unlinkat(dir_fd, name.c_str(), 0) == 0 ? 0 : errno;
    }
};

}  // namespace

RetainFileOps& system_retain_file_ops() noexcept {
    // One stateless object for the process; it holds nothing that changes.
    static SystemFileOps
        ops;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): stateless; the interface needs a non-const object
    return ops;
}

Status write_retain_file(int dir_fd, std::string_view owner,
                         const RetainFile& file, std::string& detail,
                         RetainFileOps& ops) {
    Result<std::vector<std::byte>> bytes = encode_retain_file(file, detail);
    if (!bytes.ok()) {
        return bytes.status();
    }
    const std::string name = retain_file_name(owner);
    const std::string temporary = temporary_name(name, ::getpid());
    // A temporary file of this process id is one this process left.
    static_cast<void>(ops.unlink(dir_fd, temporary));
    std::unique_ptr<RetainWriteFile> out;
    if (const int error = ops.create(dir_fd, temporary, out); error != 0) {
        detail = "cannot create " + temporary + ": " + errno_text(error);
        return {error == ENOSPC ? StatusCode::RESOURCE_EXHAUSTED
                                : StatusCode::IO_ERROR,
                "cannot create a retain file"};
    }
    const auto fail = [&](std::string_view what, int error) {
        detail = std::string(what) + " " + temporary + ": " + errno_text(error);
        out.reset();
        static_cast<void>(ops.unlink(dir_fd, temporary));
        return Status(error == ENOSPC ? StatusCode::RESOURCE_EXHAUSTED
                                      : StatusCode::IO_ERROR,
                      "cannot write a retain file");
    };
    // The mode is set explicitly: the creation mode was reduced by the umask.
    if (const int error = out->set_mode(RETAIN_FILE_MODE); error != 0) {
        return fail("cannot set the mode of", error);
    }
    std::span<const std::byte> left(bytes.value());
    while (!left.empty()) {
        std::size_t written = 0;
        const int error = out->write(left, written);
        if (error == EINTR) {
            continue;
        }
        if (error != 0 || written == 0 || written > left.size()) {
            return fail("cannot write", error != 0 ? error : EIO);
        }
        left = left.subspan(written);
    }
    // Without it a power loss can leave an empty or damaged file under the
    // real name once the rename is on disk.
    if (const int error = out->sync(); error != 0) {
        return fail("cannot flush", error);
    }
    if (const int error = ops.rename(dir_fd, temporary, name); error != 0) {
        return fail("cannot rename", error);
    }
    out.reset();
    // Without it the old file can come back after a power loss.
    if (const int error = ops.sync_directory(dir_fd); error != 0) {
        detail = "cannot flush the retain directory: " + errno_text(error);
        return {StatusCode::IO_ERROR, "cannot flush the retain directory"};
    }
    return {};
}

std::size_t remove_retain_temporaries(int dir_fd) noexcept {
    std::string ignored;
    const Result<std::vector<std::string>> entries =
        list_directory(dir_fd, ignored);
    if (!entries.ok()) {
        return 0;
    }
    std::size_t removed = 0;
    for (const std::string& entry : entries.value()) {
        struct stat info {};
        if (is_retain_temporary(entry) &&
            ::fstatat(dir_fd, entry.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 &&
            info.st_uid == ::geteuid() &&
            ::unlinkat(dir_fd, entry.c_str(), 0) == 0) {
            ++removed;
        }
    }
    return removed;
}

const RetainedValue* find_retained(const RetainFile& file,
                                   std::string_view name) noexcept {
    const auto found = std::ranges::lower_bound(
        file.values, name, std::less<>{},
        [](const RetainedValue& value) -> std::string_view {
            return value.name;
        });
    return found != file.values.end() && found->name == name ? &*found
                                                             : nullptr;
}

LoadedValues load_retained(int dir_fd, std::uint32_t tagd_uid,
                           std::string_view owner,
                           std::span<const WantedValue> wanted) {
    LoadedValues loaded;
    loaded.values.resize(wanted.size());
    std::size_t missing = wanted.size();
    const auto take_from = [&](const RetainFile& file) {
        for (std::size_t i = 0; i < wanted.size(); ++i) {
            const RetainedValue* value = find_retained(file, wanted[i].name);
            if (value != nullptr && value->type == wanted[i].type) {
                loaded.values[i] = *value;
                --missing;
            }
        }
    };

    const std::string own_name = retain_file_name(owner);
    std::string detail;
    Result<RetainFile> own =
        read_retain_file(dir_fd, own_name, tagd_uid, detail);
    if (own.ok()) {
        take_from(own.value());
        loaded.own = std::move(own).value();
    } else if (own.status().code() != StatusCode::NOT_FOUND) {
        loaded.rejected.push_back({own_name, detail});
    }
    if (missing == 0) {
        return loaded;
    }

    // A tag that moved: the other files, one at a time, keeping for each
    // missing tag the value of the most recently written file that has it.
    // At most MAX_OWNERS files are read, each within MAX_RETAIN_FILE_BYTES.
    const Result<std::vector<std::string>> entries =
        list_directory(dir_fd, detail);
    if (!entries.ok()) {
        return loaded;
    }
    std::vector<std::int64_t> found_written(wanted.size());
    std::vector<std::optional<RetainedValue>> others(wanted.size());
    std::size_t files = 0;
    for (const std::string& entry : entries.value()) {
        if (entry == own_name || !owner_of_retain_file(entry).has_value()) {
            continue;
        }
        if (++files > MAX_OWNERS) {
            break;
        }
        const Result<RetainFile> other =
            read_retain_file(dir_fd, entry, tagd_uid, detail);
        if (!other.ok()) {
            if (other.status().code() != StatusCode::NOT_FOUND) {
                loaded.rejected.push_back({entry, detail});
            }
            continue;
        }
        for (std::size_t i = 0; i < wanted.size(); ++i) {
            if (loaded.values[i].has_value()) {
                continue;  // from the owner's own file
            }
            const RetainedValue* value =
                find_retained(other.value(), wanted[i].name);
            if (value != nullptr && value->type == wanted[i].type &&
                (!others[i].has_value() ||
                 other.value().written_time_ns > found_written[i])) {
                others[i] = *value;
                found_written[i] = other.value().written_time_ns;
            }
        }
    }
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        if (!loaded.values[i].has_value()) {
            loaded.values[i] = std::move(others[i]);
        }
    }
    return loaded;
}

}  // namespace scada::tag
