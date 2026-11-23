// Stage 1 of validation: reads a configuration tree without following links
// and without leaving its root (configuration schemas contract, section 1).

#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/tree.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::conf {

namespace {

// Diagnostic codes of stage 1 (section 10.1).
constexpr std::uint16_t CF_NOT_A_TREE_FILE = 101;
constexpr std::uint16_t CF_SYSTEM_MISSING = 103;
constexpr std::uint16_t CF_LIMIT = 113;

// The directories a tree may hold besides its root: six at the top and one
// per protocol under drivers/ (section 1). They do not count as files.
constexpr std::size_t TREE_DIRECTORIES = 8;

// Bytes read from a file per system call.
constexpr std::size_t READ_CHUNK = std::size_t{64} * 1024;

Status io_failure(int error) {
    switch (error) {
        case ENOENT:
        case ENOTDIR:
            return {StatusCode::NOT_FOUND, "configuration tree not found"};
        case EACCES:
        case EPERM:
            return {StatusCode::PERMISSION_DENIED,
                    "configuration tree not readable"};
        default:
            return {StatusCode::IO_ERROR,
                    "configuration tree could not be read"};
    }
}

// Opens `name` below the directory `directory_fd`.
Fd open_at(int directory_fd, const std::string& name, int flags) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): openat has no other form
    return Fd(openat(directory_fd, name.c_str(), flags));
}

// Closes a directory stream.
struct DirCloser {
    void operator()(DIR* dir) const noexcept {
        // Nothing to do on an error: the stream is gone either way.
        static_cast<void>(closedir(dir));
    }
};
using DirStream = std::unique_ptr<DIR, DirCloser>;

// Text of a size limit for a message: "8 MiB", or bytes for a limit that is
// not a whole number of MiB (tests).
std::string size_text(std::size_t bytes) {
    if (bytes >= MIB && bytes % MIB == 0) {
        return std::to_string(bytes / MIB) + " MiB";
    }
    return std::to_string(bytes) + " bytes";
}

std::string join(std::string_view directory, std::string_view name) {
    if (directory.empty()) {
        return std::string(name);
    }
    std::string path(directory);
    path += '/';
    path += name;
    return path;
}

Location at(std::string path) {
    return Location{.file = std::move(path), .line = 1, .column = 1};
}

// A directory still to be read: its descriptor and its path in the tree
// (empty for the root).
struct PendingDirectory {
    Fd fd;
    std::string path;
};

// Reads up to `most` bytes of `file` into `content`.
Status read_at_most(const Fd& file, std::size_t most, std::string& content) {
    while (content.size() < most) {
        const std::size_t before = content.size();
        const std::size_t want = std::min(READ_CHUNK, most - before);
        content.resize(before + want);
        const std::span<char> space = std::span<char>(content).subspan(before);
        const ssize_t got = read(file.get(), space.data(), want);
        if (got < 0) {
            const int error = errno;
            content.resize(before);
            if (error == EINTR) {
                continue;
            }
            return io_failure(error);
        }
        content.resize(before + static_cast<std::size_t>(got));
        if (got == 0) {
            break;
        }
    }
    return {};
}

class Scanner {
public:
    // `diagnostics` is borrowed for the life of the scanner.
    Scanner(Diagnostics& diagnostics, const ScanLimits& limits)
        : diagnostics_(&diagnostics), limits_(limits) {}

    // Reads the whole tree below `root_fd`.
    Status run(Fd root_fd) {
        std::vector<PendingDirectory> stack;
        stack.push_back(PendingDirectory{.fd = std::move(root_fd), .path = {}});
        while (!stack.empty() && !stopped_) {
            const PendingDirectory current = std::move(stack.back());
            stack.pop_back();
            std::vector<PendingDirectory> children;
            const Status status = read_directory(current, children);
            if (!status.ok()) {
                return status;
            }
            // Depth first in path order: push the children in reverse.
            for (PendingDirectory& child : std::views::reverse(children)) {
                stack.push_back(std::move(child));
            }
        }
        if (!stopped_ && !system_seen_) {
            diagnostics_->error(CF_SYSTEM_MISSING, at("system.yaml"),
                                "system.yaml is missing");
        }
        std::ranges::sort(tree_.files, {}, &TreeFile::path);
        return {};
    }

    [[nodiscard]] Tree take_tree() { return std::move(tree_); }

private:
    void refuse(std::string path) {
        std::string message = path;
        message += " is not a file of the configuration tree";
        diagnostics_->error(CF_NOT_A_TREE_FILE, at(std::move(path)),
                            std::move(message));
    }

    [[nodiscard]] std::string files_text() const {
        return std::to_string(limits_.max_entries) + " files";
    }

    // Counts one entry toward the file limit of section 1.3: every entry but
    // a directory the tree may hold, refused ones too (D-105). False, with
    // the limit reported and the scan stopped, when it is passed.
    bool count_file(const std::string& path) {
        ++files_;
        if (files_ > limits_.max_entries) {
            tree_limit(path, files_text());
            return false;
        }
        return true;
    }

    void tree_limit(std::string path, const std::string& limit) {
        diagnostics_->error(CF_LIMIT, at(std::move(path)),
                            "the tree exceeds its limit of " + limit);
        stopped_ = true;
    }

    // Reads the names of one directory, sorted, without reading more than
    // the entry limit allows.
    Status list_names(const PendingDirectory& directory,
                      std::vector<std::string>& names) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl has no other form
        const int copy = fcntl(directory.fd.get(), F_DUPFD_CLOEXEC, 0);
        if (copy < 0) {
            return io_failure(errno);
        }
        // On success the stream owns the descriptor and closes it.
        const DirStream stream(fdopendir(copy));
        if (!stream) {
            const int error = errno;
            const Fd unowned(copy);
            return io_failure(error);
        }
        while (true) {
            errno = 0;
            // NOLINTNEXTLINE(concurrency-mt-unsafe): the stream belongs to this call alone; readdir is safe on separate streams
            const dirent* entry = readdir(stream.get());
            if (entry == nullptr) {
                if (errno != 0) {
                    return io_failure(errno);
                }
                break;
            }
            const std::string_view name(
                static_cast<const char*>(entry->d_name));
            if (name == "." || name == "..") {
                continue;
            }
            // A bound on the names held, checked before one more is kept.
            // Precise counting is in `read_entry`; this only keeps a
            // directory of millions of entries from being listed (D-105).
            ++entries_read_;
            if (entries_read_ > limits_.max_entries + TREE_DIRECTORIES) {
                tree_limit(
                    directory.path.empty() ? std::string(".") : directory.path,
                    files_text());
                return {};
            }
            names.emplace_back(name);
        }
        std::ranges::sort(names);
        return {};
    }

    Status read_directory(const PendingDirectory& directory,
                          std::vector<PendingDirectory>& children) {
        std::vector<std::string> names;
        if (const Status status = list_names(directory, names); !status.ok()) {
            return status;
        }
        for (const std::string& name : names) {
            if (stopped_) {
                return {};
            }
            const Status status = read_entry(directory, name, children);
            if (!status.ok()) {
                return status;
            }
        }
        return {};
    }

    Status read_entry(const PendingDirectory& directory,
                      const std::string& name,
                      std::vector<PendingDirectory>& children) {
        std::string path = join(directory.path, name);
        struct stat before {};
        if (fstatat(directory.fd.get(), name.c_str(), &before,
                    AT_SYMLINK_NOFOLLOW) != 0) {
            return io_failure(errno);
        }
        const bool is_directory = S_ISDIR(before.st_mode);
        if (!is_directory && !S_ISREG(before.st_mode)) {
            // A symbolic link, FIFO, socket or device node.
            if (count_file(path)) {
                refuse(std::move(path));
            }
            return {};
        }
        std::optional<PathClass> path_class = classify_path(path, is_directory);
        if (!path_class) {
            if (count_file(path)) {
                refuse(std::move(path));
            }
            return {};
        }
        if (path == "system.yaml") {
            system_seen_ = true;
        }
        if (!is_directory && !count_file(path)) {
            return {};
        }
        const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY |
                          (is_directory ? O_DIRECTORY : O_NONBLOCK);
        Fd opened = open_at(directory.fd.get(), name, flags);
        if (!opened.valid()) {
            // Replaced by a link or by something that is not a directory
            // since it was looked at.
            if (errno == ELOOP || errno == ENOTDIR) {
                refuse(std::move(path));
                return {};
            }
            return io_failure(errno);
        }
        struct stat after {};
        if (fstat(opened.get(), &after) != 0) {
            return io_failure(errno);
        }
        if (after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
            (after.st_mode & S_IFMT) != (before.st_mode & S_IFMT)) {
            refuse(std::move(path));
            return {};
        }
        if (is_directory) {
            children.push_back(PendingDirectory{.fd = std::move(opened),
                                                .path = std::move(path)});
            return {};
        }
        return read_file(opened, std::move(path), std::move(*path_class));
    }

    Status read_file(const Fd& file, std::string path, PathClass path_class) {
        const std::size_t limit =
            limits_.max_file_bytes.value_or(file_size_limit(path_class.type));
        std::string content;
        // One byte more than the limit tells an oversized file from one that
        // is exactly at it, whatever its size claims to be.
        const Status status = read_at_most(file, limit + 1, content);
        if (!status.ok()) {
            return status;
        }
        if (content.size() > limit) {
            diagnostics_->error(
                CF_LIMIT, at(std::move(path)),
                "the file exceeds its limit of " + size_text(limit));
            return {};
        }
        if (content.size() > limits_.max_tree_bytes - tree_bytes_) {
            tree_limit(std::move(path), size_text(limits_.max_tree_bytes));
            return {};
        }
        tree_bytes_ += content.size();
        tree_.files.push_back(TreeFile{.path = std::move(path),
                                       .path_class = std::move(path_class),
                                       .content = std::move(content)});
        return {};
    }

    Diagnostics* diagnostics_;
    ScanLimits limits_;
    Tree tree_;
    std::size_t entries_read_ = 0;
    std::size_t files_ = 0;
    std::size_t tree_bytes_ = 0;
    bool system_seen_ = false;
    bool stopped_ = false;
};

}  // namespace

Result<Tree> scan_tree(const std::filesystem::path& root,
                       Diagnostics& diagnostics, const ScanLimits& limits) {
    // The root itself may be a link (/etc/scada names the active tree,
    // section 13.2); nothing below it is followed.
    Fd root_fd =
        open_at(AT_FDCWD, root.string(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (!root_fd.valid()) {
        return io_failure(errno);
    }
    Scanner scanner(diagnostics, limits);
    if (const Status status = scanner.run(std::move(root_fd)); !status.ok()) {
        return status;
    }
    return scanner.take_tree();
}

}  // namespace scada::conf
