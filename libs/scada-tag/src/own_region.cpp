#include "own_region.h"

#include "region_file.h"

#include "scada/common/fd.h"
#include "scada/common/mapped_region.h"
#include "scada/common/result.h"
#include "scada/common/status.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace scada::tag {

namespace {

// Attempts when another process creates the region at the same moment: the
// second attempt finds the region of the other.
constexpr int ATTACH_ATTEMPTS = 2;

// Takes the exclusive lock of section 10 without waiting.
Status lock_exclusively(int descriptor, std::string_view name,
                        std::string& detail) {
    if (::flock(descriptor, LOCK_EX | LOCK_NB) == 0) {
        return {};
    }
    const int error = errno;
    detail = "cannot lock " + std::string(name) + ": " + errno_text(error);
    return error == EWOULDBLOCK
               ? Status(StatusCode::ALREADY_EXISTS,
                        "a process with this name is already attached")
               : Status(StatusCode::IO_ERROR, "cannot lock a region file");
}

// Removes the entry `name`, which is not a usable region, if it belongs to
// `uid` (section 11.2, step 3). An entry another process holds locked is in
// use and stays.
Status remove_own_unusable(int dir_fd, const std::string& name,
                           std::uint32_t uid, std::string& detail) {
    struct stat info {};
    if (::fstatat(dir_fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return {};  // gone meanwhile
        }
        detail = "cannot examine " + name + ": " + errno_text(errno);
        return {StatusCode::IO_ERROR, "cannot examine a region file"};
    }
    if (info.st_uid != uid) {
        detail = name + " is owned by user " + std::to_string(info.st_uid) +
                 ", not by user " + std::to_string(uid);
        return {StatusCode::PERMISSION_DENIED,
                "a region file has the wrong owner"};
    }
    if (S_ISDIR(info.st_mode)) {
        detail = name + " is a directory";
        return {StatusCode::DATA_CORRUPT,
                "a directory is in the place of a region"};
    }
    if (S_ISREG(info.st_mode)) {
        const Fd file = open_at(dir_fd, name,
                                O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (file.valid()) {
            const Status locked = lock_exclusively(file.get(), name, detail);
            if (!locked.ok()) {
                return locked;
            }
        }
    }
    if (::unlinkat(dir_fd, name.c_str(), 0) != 0 && errno != ENOENT) {
        detail = "cannot remove " + name + ": " + errno_text(errno);
        return {StatusCode::IO_ERROR, "cannot remove a region file"};
    }
    return {};
}

// Looks at the entry under the name of the region. The region if it can be
// reused; nothing if there is none now, an unusable entry of the caller
// having been removed; an error otherwise.
Result<std::optional<OwnRegion>> reuse_existing(int dir_fd,
                                                const OwnRegionSpec& spec,
                                                std::string& detail) {
    Result<OpenedRegion> existing = open_region(
        dir_fd, spec.name, spec.identity, MapAccess::READ_WRITE, detail);
    if (!existing.ok() && existing.status().code() == StatusCode::NOT_FOUND) {
        return std::optional<OwnRegion>();
    }
    if (existing.ok()) {
        if (spec.lock) {
            const Status locked = lock_exclusively(existing.value().file.get(),
                                                   spec.name, detail);
            if (!locked.ok()) {
                return locked;
            }
        }
        if (!spec.usable || spec.usable(existing.value().memory.bytes())) {
            return std::optional<OwnRegion>(
                OwnRegion{std::move(existing.value().file),
                          std::move(existing.value().memory), true});
        }
        detail = spec.name + " does not hold what this generation needs";
        // Given up before it is removed: the removal tests the lock, and
        // must not meet the one taken above.
        existing.value().memory.reset();
        existing.value().file.reset();
    }
    const Status removed =
        remove_own_unusable(dir_fd, spec.name, spec.identity.uid, detail);
    if (!removed.ok()) {
        return removed;
    }
    return std::optional<OwnRegion>();
}

// Creates the region (section 3.2). Nothing if another process gave the
// name a file first.
Result<std::optional<OwnRegion>> create_new(int dir_fd,
                                            const OwnRegionSpec& spec,
                                            std::string& detail) {
    remove_stale_temporaries(dir_fd, spec.name, spec.identity.uid);
    Result<RegionBuilder> builder =
        RegionBuilder::create(dir_fd, spec.name, spec.identity, detail);
    if (!builder.ok()) {
        return builder.status();
    }
    if (spec.initialise) {
        // Not shared yet: the file has no name others look for.
        spec.initialise(builder.value().bytes());
    }
    if (spec.lock) {
        // Locked before it gets its name, so that no second process can
        // attach in between. The lock belongs to the open file and stays
        // through the rename.
        const Status locked =
            lock_exclusively(builder.value().descriptor(), spec.name, detail);
        if (!locked.ok()) {
            return locked;
        }
    }
    Result<RegionBuilder::Published> published =
        builder.value().publish(detail);
    if (published.ok()) {
        return std::optional<OwnRegion>(
            OwnRegion{std::move(published.value().file),
                      std::move(published.value().memory), false});
    }
    if (published.status().code() != StatusCode::ALREADY_EXISTS) {
        return published.status();
    }
    return std::optional<OwnRegion>();
}

}  // namespace

Result<OwnRegion> attach_own_region(int dir_fd, const OwnRegionSpec& spec,
                                    std::string& detail) {
    for (int attempt = 0; attempt < ATTACH_ATTEMPTS; ++attempt) {
        Result<std::optional<OwnRegion>> existing =
            reuse_existing(dir_fd, spec, detail);
        if (!existing.ok()) {
            return existing.status();
        }
        if (std::optional<OwnRegion> region = std::move(existing).value();
            region.has_value()) {
            return std::move(*region);
        }
        Result<std::optional<OwnRegion>> created =
            create_new(dir_fd, spec, detail);
        if (!created.ok()) {
            return created.status();
        }
        if (std::optional<OwnRegion> region = std::move(created).value();
            region.has_value()) {
            return std::move(*region);
        }
        // Another process created the region first; look at it.
    }
    return Status(StatusCode::ALREADY_EXISTS,
                  "a process with this name is already attached");
}

}  // namespace scada::tag
