#include "zone_file.h"

#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "tzif.h"

#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace scada::driver::detail {

namespace {

constexpr int FILE_FLAGS = O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK;

Status open_failure(int error) noexcept {
    switch (error) {
        case ENOENT:
        case ENOTDIR:
            return {StatusCode::NOT_FOUND, "time zone not found"};
        case EXDEV:
        case ELOOP:
            return {StatusCode::PERMISSION_DENIED,
                    "time zone file outside the zone directory"};
        default:
            return {StatusCode::IO_ERROR, "time zone file not readable"};
    }
}

// A descriptor, or -1 with the error of the call that failed.
struct Opened {
    int descriptor = -1;
    int error = 0;
};

Opened failed(int error) noexcept {
    return Opened{.descriptor = -1, .error = error};
}

// openat2 beneath an open directory.
Opened open_with_openat2(const std::string& directory,
                         const std::string& name) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open has no other form
    const Fd base(::open(directory.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (!base.valid()) {
        return failed(errno);
    }
    open_how how{};
    how.flags = FILE_FLAGS;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;
    // glibc has no wrapper for openat2.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg): syscall has no other form
    const long result =
        ::syscall(SYS_openat2, base.get(), name.c_str(), &how, sizeof(how));
    // NOLINTEND(cppcoreguidelines-pro-type-vararg)
    return result < 0 ? failed(errno)
                      : Opened{.descriptor = static_cast<int>(result)};
}

// The fallback: resolve in user space, check the result lies beneath the
// resolved directory, and open it without following a final link. The zone
// directory belongs to root, so a change between the check and the open
// needs root.
Opened open_resolved(const std::string& directory,
                     const std::string& name) noexcept {
    std::error_code error;
    const std::filesystem::path root =
        std::filesystem::canonical(directory, error);
    if (error) {
        return failed(ENOENT);
    }
    const std::filesystem::path resolved =
        std::filesystem::canonical(root / name, error);
    if (error) {
        return failed(error.value() != 0 ? error.value() : ENOENT);
    }
    const std::string root_text = root.string() + "/";
    if (!resolved.string().starts_with(root_text)) {
        return failed(EXDEV);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open has no other form
    const int descriptor = ::open(resolved.c_str(), FILE_FLAGS | O_NOFOLLOW);
    return descriptor < 0 ? failed(errno) : Opened{.descriptor = descriptor};
}

}  // namespace

Result<std::vector<std::byte>> read_zone_file_beneath(
    const std::string& directory, std::string_view zone_name,
    BeneathMethod method) {
    if (directory.empty() || directory.front() != '/') {
        return Status(StatusCode::INVALID_ARGUMENT,
                      "zone directory is not an absolute path");
    }
    const std::string name(zone_name);
    Opened opened = method == BeneathMethod::AUTOMATIC
                        ? open_with_openat2(directory, name)
                        : open_resolved(directory, name);
    if (method == BeneathMethod::AUTOMATIC && opened.error == ENOSYS) {
        opened = open_resolved(directory, name);
    }
    const Fd file(opened.descriptor);
    if (!file.valid()) {
        return open_failure(opened.error);
    }
    struct stat info {};
    if (::fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) > TZIF_MAX_BYTES) {
        return Status(StatusCode::DATA_CORRUPT, "zone file not valid");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(info.st_size));
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t got =
            ::read(file.get(), std::span(bytes).subspan(done).data(),
                   bytes.size() - done);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            return Status(StatusCode::IO_ERROR, "time zone file not readable");
        }
        done += static_cast<std::size_t>(got);
    }
    return bytes;
}

}  // namespace scada::driver::detail
