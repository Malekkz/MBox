#include "scada/ctl/arguments.h"

#include "scada/ctl/format.h"
#include "scada/ctl/pattern.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/tag_set.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace scada::ctl {

const std::string_view USAGE =
    "Usage: scadactl [options] status\n"
    "       scadactl [options] tag list [<pattern>]\n"
    "       scadactl [options] tag get <name>\n"
    "       scadactl [options] tag watch <pattern> [--count <n>]\n"
    "       scadactl --help\n"
    "\n"
    "Reads the shared-memory tag database directly. It only reads: it\n"
    "writes nothing to shared memory and needs no entry in the tag set. It\n"
    "must run as a member of the group scada, or as root.\n"
    "\n"
    "Membership of the group scada means unaudited read access to every tag:\n"
    "these reads are not recorded anywhere. It also allows creating files in\n"
    "the generation directory and locking the proc region of a process,\n"
    "which can keep that process from attaching. scadactl does neither, but\n"
    "grant the group only to accounts that are trusted that far.\n"
    "\n"
    "Commands:\n"
    "  status               The generation and its state, whether scada-tagd\n"
    "                       is up, every process with its state and heartbeat\n"
    "                       age, and every owner with its process and whether\n"
    "                       that process is down (owner_down).\n"
    "  tag list [<pattern>] Every tag whose name matches <pattern>, or every\n"
    "                       tag, with its type, owner, value, quality, flags\n"
    "                       and age, in name order.\n"
    "  tag get <name>       The tag <name>, with the same fields and its\n"
    "                       source and update times.\n"
    "  tag watch <pattern>  The current value of every tag that matches\n"
    "                       <pattern>, then each change as its owner writes\n"
    "                       it, until stopped by SIGINT (Ctrl-C) or SIGTERM.\n"
    "                       A line \"gap\" says that changes of an owner were\n"
    "                       lost, and why; its tags are then read again.\n"
    "\n"
    "Patterns: '*' matches any characters, dots included, and '?' exactly\n"
    "one. Every other character matches itself; tag names use a-z, 0-9, '_',\n"
    "'.' and '-'. Quote a pattern so that the shell does not expand it.\n"
    "\n"
    "Options:\n"
    "  --json               Print JSON: one document for status, tag list\n"
    "                       and tag get; one document per line for tag\n"
    "                       watch. Times are in ns, CLOCK_REALTIME, UTC.\n"
    "  --count <n>          tag watch only: exit after <n> changes, 1 or\n"
    "                       more. Lines of the current values and gaps are\n"
    "                       not counted.\n"
    "  --base-dir <dir>     Directory of the tag database.\n"
    "                       Default: /dev/shm/scada\n"
    "  --tagd-uid <uid>     User id that must own the database directory\n"
    "                       and its header. Default: the uid of the user\n"
    "                       scada-tagd.\n"
    "  --help               Show this text and exit.\n"
    "\n"
    "Marks: in the human-readable form the first column of a tag is '!' for\n"
    "a BAD value (OWNER_DOWN included) or one that could not be read, '~'\n"
    "for an UNCERTAIN or STALE one, and blank for a GOOD one. A value of '-'\n"
    "means there is none: the tag was never written, or its owner is\n"
    "missing. In JSON every tag carries \"bad\" and \"stale\" as true or "
    "false,\n"
    "and \"value\" null when there is none. Age is the time since the owner\n"
    "last wrote the tag, on CLOCK_MONOTONIC.\n"
    "\n"
    "Exit codes: 0 success, 1 error (no database, no such tag, no access),\n"
    "2 wrong arguments.\n";

namespace {

// Parses an unsigned decimal number with no sign, no spaces and no other
// characters, at most `maximum`.
template <typename T>
std::optional<T> parse_unsigned(std::string_view text, T maximum) {
    if (text.empty()) {
        return std::nullopt;
    }
    T value = 0;
    const char* const end = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || ptr != end || value > maximum) {
        return std::nullopt;
    }
    return value;
}

// What the options say, before the command words are looked at.
struct Scan {
    std::vector<std::string_view> words;
    bool help = false;
    bool base_dir_given = false;
};

// Stores the value `value` of the option `option` in `result`. False, with
// `detail`, if it is given twice or is not valid.
bool take_option_value(std::string_view option, std::string_view value,
                       Arguments& result, Scan& scan, std::string& detail) {
    if (option == "--base-dir") {
        if (scan.base_dir_given) {
            detail = "--base-dir is given twice";
            return false;
        }
        scan.base_dir_given = true;
        result.base_dir = value;
        return true;
    }
    if (option == "--tagd-uid") {
        if (result.tagd_uid) {
            detail = "--tagd-uid is given twice";
            return false;
        }
        // 4,294,967,295 is (uid_t)-1, which is no user.
        constexpr std::uint32_t LAST_UID = 0xFFFF'FFFEU;
        result.tagd_uid = parse_unsigned<std::uint32_t>(value, LAST_UID);
        if (!result.tagd_uid) {
            detail = "--tagd-uid needs a user id from 0 to 4294967294";
            return false;
        }
        return true;
    }
    // --count
    if (result.count) {
        detail = "--count is given twice";
        return false;
    }
    result.count = parse_unsigned<std::uint64_t>(value, UINT64_MAX);
    if (!result.count || *result.count == 0) {
        detail = "--count needs a whole number of 1 or more";
        return false;
    }
    return true;
}

// Sorts `args` into options, stored in `result` and `scan`, and the
// remaining words, in `scan.words`.
bool scan_arguments(std::span<const std::string_view> args, Arguments& result,
                    Scan& scan, std::string& detail) {
    bool options_ended = false;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string_view arg = args[index];
        if (options_ended || !arg.starts_with("--")) {
            scan.words.push_back(arg);
        } else if (arg == "--") {
            options_ended = true;
        } else if (arg == "--help") {
            scan.help = true;
        } else if (arg == "--json") {
            if (result.json) {
                detail = "--json is given twice";
                return false;
            }
            result.json = true;
        } else if (arg == "--base-dir" || arg == "--tagd-uid" ||
                   arg == "--count") {
            if (index + 1 >= args.size() || args[index + 1].empty()) {
                detail.assign(arg);
                detail.append(" needs a value");
                return false;
            }
            ++index;
            if (!take_option_value(arg, args[index], result, scan, detail)) {
                return false;
            }
        } else {
            detail = "unknown option ";
            append_terminal_safe(detail, arg);
            return false;
        }
    }
    return true;
}

// The pattern `text` into `result.pattern`.
bool take_pattern(std::string_view text, Arguments& result,
                  std::string& detail) {
    auto pattern = TagPattern::parse(text, detail);
    if (!pattern) {
        return false;
    }
    result.pattern = std::move(*pattern);
    return true;
}

// The words after `tag`.
bool parse_tag_command(std::span<const std::string_view> rest,
                       Arguments& result, std::string& detail) {
    if (rest.empty()) {
        detail = "tag needs a command: list, get or watch";
        return false;
    }
    const std::string_view sub = rest.front();
    const std::span<const std::string_view> operands = rest.subspan(1);
    if (sub == "list") {
        if (operands.size() > 1) {
            detail = "tag list takes at most one pattern";
            return false;
        }
        result.command = Command::TAG_LIST;
        return operands.empty() ||
               take_pattern(operands.front(), result, detail);
    }
    if (sub == "get") {
        if (operands.size() != 1) {
            detail = "tag get takes exactly one tag name";
            return false;
        }
        if (!scada::tag::is_valid_name(operands.front(), SCADA_TAG_NAME_MAX)) {
            detail =
                "a tag name is 1 to 127 characters of a-z, 0-9, '_', '.' and "
                "'-'";
            return false;
        }
        result.command = Command::TAG_GET;
        result.name = operands.front();
        return true;
    }
    if (sub == "watch") {
        if (operands.size() != 1) {
            detail = "tag watch takes exactly one pattern";
            return false;
        }
        result.command = Command::TAG_WATCH;
        return take_pattern(operands.front(), result, detail);
    }
    detail = "unknown tag command ";
    append_terminal_safe(detail, sub);
    return false;
}

// The command words.
bool parse_command(std::span<const std::string_view> words, Arguments& result,
                   std::string& detail) {
    if (words.empty()) {
        detail = "no command given";
        return false;
    }
    const std::string_view command = words.front();
    const std::span<const std::string_view> rest = words.subspan(1);
    if (command == "status") {
        if (!rest.empty()) {
            detail = "status takes no arguments";
            return false;
        }
        result.command = Command::STATUS;
        return true;
    }
    if (command == "tag") {
        return parse_tag_command(rest, result, detail);
    }
    detail = "unknown command ";
    append_terminal_safe(detail, command);
    return false;
}

}  // namespace

std::optional<Arguments> parse_arguments(std::span<const std::string_view> args,
                                         std::string& detail) {
    Arguments result;
    Scan scan;
    if (!scan_arguments(args, result, scan, detail)) {
        return std::nullopt;
    }
    if (scan.help) {
        result.command = Command::HELP;
        return result;
    }
    if (!parse_command(scan.words, result, detail)) {
        return std::nullopt;
    }
    if (result.count && result.command != Command::TAG_WATCH) {
        detail = "--count is an option of tag watch only";
        return std::nullopt;
    }
    return result;
}

}  // namespace scada::ctl
