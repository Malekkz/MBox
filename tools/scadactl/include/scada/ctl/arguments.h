#ifndef SCADA_CTL_ARGUMENTS_H
#define SCADA_CTL_ARGUMENTS_H

// The command line of scadactl.

#include "scada/ctl/pattern.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace scada::ctl {

// The usage text of `scadactl --help`. It documents every command and
// option.
extern const std::string_view USAGE;

enum class Command : std::uint8_t {
    HELP,
    STATUS,
    TAG_LIST,
    TAG_GET,
    TAG_WATCH,
};

struct Arguments {
    Command command = Command::HELP;
    // --json: JSON in place of the human-readable form.
    bool json = false;
    // --base-dir; empty: the default of the library, /dev/shm/scada.
    std::string base_dir;
    // --tagd-uid; nothing: the uid of the user scada-tagd.
    std::optional<std::uint32_t> tagd_uid;
    // The pattern of `tag list` (every tag if none was given) and `tag
    // watch`.
    TagPattern pattern = TagPattern::all();
    // The name of `tag get`.
    std::string name;
    // --count of `tag watch`: stop after this many changes; nothing: run
    // until SIGINT or SIGTERM.
    std::optional<std::uint64_t> count;
};

// Parses the arguments `args` (without the program name), which are
// untrusted. Options may stand before or after the command words; `--`
// ends the options. Nothing if the arguments are not valid; `detail` then
// says why, in one sentence.
[[nodiscard]] std::optional<Arguments> parse_arguments(
    std::span<const std::string_view> args, std::string& detail);

}  // namespace scada::ctl

#endif  // SCADA_CTL_ARGUMENTS_H
