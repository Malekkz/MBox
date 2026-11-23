// Which paths a configuration tree may hold (configuration schemas contract,
// section 1).

#include "scada/conf/tree.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace scada::conf {

namespace {

// A path component without its extension: `[a-z0-9_]+` (section 1).
bool is_stem(std::string_view text) noexcept {
    return !text.empty() && std::ranges::all_of(text, [](char letter) {
        return (letter >= 'a' && letter <= 'z') ||
               (letter >= '0' && letter <= '9') || letter == '_';
    });
}

// Splits `path` at `/` into at most 3 components. Nothing if it has more,
// or an empty component (a leading, trailing or doubled `/`).
std::optional<std::array<std::string_view, 3>> split(std::string_view path,
                                                     std::size_t& count) {
    std::array<std::string_view, 3> parts{};
    count = 0;
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = path.find('/', start);
        const std::string_view part = path.substr(
            start, slash == std::string_view::npos ? std::string_view::npos
                                                   : slash - start);
        if (part.empty() || count == parts.size()) {
            return std::nullopt;
        }
        parts.at(count) = part;
        ++count;
        if (slash == std::string_view::npos) {
            return parts;
        }
        start = slash + 1;
    }
}

// A file name `<stem><extension>` with a valid stem; returns the stem.
std::optional<std::string_view> stem_of(std::string_view name,
                                        std::string_view extension) {
    if (name.size() <= extension.size() || !name.ends_with(extension)) {
        return std::nullopt;
    }
    const std::string_view stem =
        name.substr(0, name.size() - extension.size());
    if (!is_stem(stem)) {
        return std::nullopt;
    }
    return stem;
}

PathClass file(FileType type, std::string_view stem,
               std::string_view protocol = {}) {
    return PathClass{.kind = PathClass::Kind::FILE,
                     .type = type,
                     .protocol = std::string(protocol),
                     .stem = std::string(stem)};
}

PathClass directory(std::string_view protocol = {}) {
    return PathClass{.kind = PathClass::Kind::DIRECTORY,
                     .type = FileType::SYSTEM,
                     .protocol = std::string(protocol),
                     .stem = {}};
}

std::optional<PathClass> classify_top(std::string_view name,
                                      bool is_directory) {
    if (is_directory) {
        constexpr std::array<std::string_view, 6> DIRECTORIES = {
            "templates", "drivers", "tags", "outputs", "alarms", "control"};
        for (const std::string_view known : DIRECTORIES) {
            if (name == known) {
                return directory();
            }
        }
        return std::nullopt;
    }
    const std::optional<std::string_view> stem = stem_of(name, ".yaml");
    if (!stem) {
        return std::nullopt;
    }
    if (*stem == "system") {
        return file(FileType::SYSTEM, *stem);
    }
    if (*stem == "areas") {
        return file(FileType::AREAS, *stem);
    }
    if (*stem == "enums") {
        return file(FileType::ENUMS, *stem);
    }
    if (*stem == "logging") {
        return file(FileType::LOGGING, *stem);
    }
    // security.yaml, notifications.yaml and link.yaml are reserved; they are
    // refused like any other unknown file.
    return std::nullopt;
}

std::optional<PathClass> classify_second(std::string_view parent,
                                         std::string_view name,
                                         bool is_directory) {
    if (parent == "drivers") {
        // `drivers/link` is reserved (phase 11): not a known protocol.
        if (is_directory && is_known_protocol(name)) {
            return directory(name);
        }
        return std::nullopt;
    }
    if (is_directory) {
        // No subdirectories elsewhere (section 1: templates have none).
        return std::nullopt;
    }
    if (parent == "control") {
        if (const auto stem = stem_of(name, ".st")) {
            return file(FileType::STRUCTURED_TEXT, *stem);
        }
        const auto stem = stem_of(name, ".yaml");
        if (!stem) {
            return std::nullopt;
        }
        return file(*stem == "tasks" ? FileType::TASKS : FileType::PROGRAM,
                    *stem);
    }
    const auto stem = stem_of(name, ".yaml");
    if (!stem) {
        return std::nullopt;
    }
    if (parent == "templates") {
        return file(FileType::TEMPLATE, *stem);
    }
    if (parent == "tags") {
        return file(FileType::TAGS, *stem);
    }
    if (parent == "outputs") {
        return file(FileType::OUTPUTS, *stem);
    }
    if (parent == "alarms") {
        return file(FileType::ALARMS, *stem);
    }
    return std::nullopt;
}

}  // namespace

bool is_known_protocol(std::string_view name) noexcept {
    return name == "modbus" || name == "io";
}

std::optional<PathClass> classify_path(std::string_view path,
                                       bool is_directory) {
    std::size_t count = 0;
    const auto parts = split(path, count);
    if (!parts) {
        return std::nullopt;
    }
    const std::array<std::string_view, 3>& part = *parts;
    if (count == 1) {
        return classify_top(part[0], is_directory);
    }
    if (count == 2) {
        return classify_second(part[0], part[1], is_directory);
    }
    // count == 3: only drivers/<protocol>/<channel>.yaml.
    if (is_directory || part[0] != "drivers" || !is_known_protocol(part[1])) {
        return std::nullopt;
    }
    const auto stem = stem_of(part[2], ".yaml");
    if (!stem) {
        return std::nullopt;
    }
    return file(FileType::CHANNEL, *stem, part[1]);
}

std::size_t file_size_limit(FileType type) noexcept {
    return type == FileType::STRUCTURED_TEXT ? MAX_STRUCTURED_TEXT_BYTES
                                             : MAX_FILE_BYTES;
}

}  // namespace scada::conf
