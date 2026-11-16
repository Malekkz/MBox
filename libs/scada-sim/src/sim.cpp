#include "scada/sim/sim.h"

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/sim/layout.h"
#include "scada/sim/pattern.h"
#include "scada/sim/tag_set.h"
#include "scada/tag/handle.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/tag_set.h"
#include "writer.h"

#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal functions are declared here, not in <csignal>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <functional>
#include <ios>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace scada::sim {

namespace {

using detail::Clock;
using detail::OwnerPlan;
using detail::OwnerProgress;

constexpr std::string_view USAGE =
    "Usage: scada-sim tag-set [<layout>] [--uid <n>] [--tagd-uid <n>]\n"
    "                 [--reader <name>]... [--heartbeat-check-ms <n>]\n"
    "                 [--heartbeat-timeout-ms <n>] [--ring-capacity <n>]\n"
    "                 [--output <file>]\n"
    "       scada-sim run [<layout>] [--instance <n>] [--rate <n>]\n"
    "                 [--rounds <n> | --seconds <n>] [--notify batch|each]\n"
    "                 [--amplitude <x>] [--period <n>] [--seed <n>]\n"
    "                 [--base-dir <directory>] [--tagd-uid <n>]\n"
    "       scada-sim --help\n"
    "\n"
    "Simulates owners of the tag database: processes whose threads write\n"
    "tags at a set rate with selectable patterns. `tag-set` writes the\n"
    "tag-set file of a simulation for scada-tagd; `run` runs one simulator\n"
    "process of it. Give both the same <layout>.\n"
    "\n"
    "<layout>:\n"
    "  --prefix <name>      Name of the process, or prefix of the process\n"
    "                       names <prefix><n> if there are several (sim).\n"
    "  --processes <n>      Simulator processes (1).\n"
    "  --owners <n>         Owners of all processes together, a multiple of\n"
    "                       the processes; each is a thread (1). Owners are\n"
    "                       named like their process if it has one, else\n"
    "                       <process>.w<k>.\n"
    "  --tags <n>           Tags of all owners together, shared out evenly\n"
    "                       (1000). Tags are named <owner>.t<i>.\n"
    "  --pattern <list>     Comma-separated patterns, given to the tags of\n"
    "                       each owner in turn (check). check: UINT64 whose\n"
    "                       fields are derived from the write count, so that\n"
    "                       a reader can tell a torn read. ramp, sine, step,\n"
    "                       walk: FLOAT64 waveforms, quality GOOD.\n"
    "\n"
    "tag-set:\n"
    "  --uid <n>            User id of the simulator processes and readers\n"
    "                       (the user running this command).\n"
    "  --tagd-uid <n>       User id of scada-tagd (the --uid).\n"
    "  --reader <name>      A further process without tags, for example a\n"
    "                       reader; may be repeated.\n"
    "  --heartbeat-check-ms <n>    How often scada-tagd checks heartbeats\n"
    "                              (250).\n"
    "  --heartbeat-timeout-ms <n>  Heartbeat timeout of every process\n"
    "                              (3000).\n"
    "  --ring-capacity <n>  Change ring entries of every owner (65536, or\n"
    "                       more for an owner with more than 32768 tags).\n"
    "  --output <file>      Where to write the file (standard output).\n"
    "\n"
    "run:\n"
    "  --instance <n>       Which simulator process of the layout this is,\n"
    "                       from 0 (0).\n"
    "  --rate <n>           Updates per second of the whole simulation, all\n"
    "                       processes together, shared out by tag count, so\n"
    "                       that every tag is written equally often; 0: as\n"
    "                       fast as possible (a tenth of the tags).\n"
    "  --rounds <n>         Stop writing once every tag is written n times.\n"
    "  --seconds <n>        Stop writing after n seconds.\n"
    "  --hold               After attaching and resolving, print \"ready\" and\n"
    "                       wait for SIGUSR1 before the first write; the\n"
    "                       heartbeat goes on, and the stop rules below apply.\n"
    "  --notify batch|each  Wake consumers after the writes of each 1 ms tick\n"
    "                       (batch) or after every write (each) (batch).\n"
    "  --amplitude <x>      Amplitude of the waveforms (100).\n"
    "  --period <n>         Writes of one tag per cycle of a waveform (100).\n"
    "  --seed <n>           Seed of the walk (1).\n"
    "  --base-dir <directory>  The tag database (/dev/shm/scada).\n"
    "  --tagd-uid <n>       User id of scada-tagd (that of the user\n"
    "                       scada-tagd).\n"
    "\n"
    "run prints \"attached\", with --hold \"ready\", then \"writing\"; when a\n"
    "run bounded by --rounds or --seconds has ended, \"done updates=<n>\n"
    "elapsed_ms=<n> behind_max_ms=<n>\". It stays attached and writes its\n"
    "heartbeat until SIGTERM, SIGINT or SIGHUP; stopped earlier, it prints\n"
    "\"stopped updates=<n> elapsed_ms=<n>\", while held \"stopped updates=0\n"
    "elapsed_ms=0\". It exits with code 1 if scada-tagd has been down for\n"
    "5 s or the generation retires, while held too. A closed standard output\n"
    "does not stop it. It starts no processes: run one instance for each\n"
    "simulator process of the layout.\n"
    "\n"
    "Exit codes: 0 done or stopped on request, 1 error, 2 wrong arguments.\n";

constexpr Status USAGE_ERROR(StatusCode::INVALID_ARGUMENT,
                             "the command line is not valid");

// The largest --rounds: rounds times the tags of an owner stays far below
// 2^64.
constexpr std::uint64_t MAX_ROUNDS = 1'000'000'000'000;
constexpr std::uint64_t MAX_SEED = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t MAX_UID = scada::tag::MAX_UID;
constexpr std::uint64_t MAX_RING_CAPACITY =
    scada::tag::MAX_CHANGE_RING_CAPACITY;
constexpr std::uint64_t MAX_INSTANCE = scada::tag::MAX_PROCESSES;
constexpr std::uint64_t RATE_DIVISOR = 10;  // the default rate: tags / 10

// The heartbeat period of a simulator process, far below every timeout the
// tag set can give it (at least 1 ms, but tests use 500 ms and more).
constexpr long HEARTBEAT_PERIOD_NS = 20'000'000;
constexpr std::int64_t NANOSECONDS_PER_MILLISECOND = 1'000'000;
// While the tags are resolved, a heartbeat after this many tags.
constexpr std::uint64_t HEARTBEAT_EVERY_TAGS = 256;

// The whole of `text` as a decimal number of at most `limit`.
std::optional<std::uint64_t> parse_number(std::string_view text,
                                          std::uint64_t limit) {
    std::uint64_t value = 0;
    const std::from_chars_result parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size() || value > limit) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::vector<Pattern>> parse_patterns(std::string_view text) {
    std::vector<Pattern> patterns;
    while (patterns.size() < MAX_PATTERNS) {
        const std::size_t comma = text.find(',');
        const std::optional<Pattern> pattern =
            pattern_from_name(text.substr(0, comma));
        if (!pattern.has_value()) {
            return std::nullopt;
        }
        patterns.push_back(*pattern);
        if (comma == std::string_view::npos) {
            return patterns;
        }
        text.remove_prefix(comma + 1);
    }
    return std::nullopt;
}

// The options of each command, and what each does with its value. A setter
// returns false if the value is not valid. A flag takes no value; its
// setter gets an empty one.
struct OptionSpec {
    std::string_view name;
    bool tag_set = false;  // allowed with `tag-set`
    bool run = false;      // allowed with `run`
    bool repeatable = false;
    std::function<bool(Options&, std::string_view)> set;
    bool flag = false;
};

// Sets `field` to the number in `text` if it is at most `limit`.
template <typename T>
bool set_number(T& field, std::string_view text, std::uint64_t limit) {
    const std::optional<std::uint64_t> number = parse_number(text, limit);
    if (!number.has_value()) {
        return false;
    }
    // In range: at most `limit`, which fits T.
    field = static_cast<T>(*number);
    return true;
}

template <typename T>
bool set_optional(std::optional<T>& field, std::string_view text,
                  std::uint64_t limit) {
    T value{};
    if (!set_number(value, text, limit)) {
        return false;
    }
    field = value;
    return true;
}

const std::vector<OptionSpec>& option_specs() {
    static const std::vector<OptionSpec> SPECS{
        {"--prefix", true, true, false,
         [](Options& options, std::string_view value) {
             options.layout.prefix = std::string(value);
             return true;
         }},
        {"--processes", true, true, false,
         [](Options& options, std::string_view value) {
             return set_number(options.layout.processes, value, MAX_INSTANCE);
         }},
        {"--owners", true, true, false,
         [](Options& options, std::string_view value) {
             return set_number(options.layout.owners, value,
                               scada::tag::MAX_OWNERS);
         }},
        {"--tags", true, true, false,
         [](Options& options, std::string_view value) {
             return set_number(options.layout.tags, value,
                               scada::tag::MAX_TAGS);
         }},
        {"--pattern", true, true, false,
         [](Options& options, std::string_view value) {
             std::optional<std::vector<Pattern>> patterns =
                 parse_patterns(value);
             if (!patterns.has_value()) {
                 return false;
             }
             options.layout.patterns = std::move(*patterns);
             return true;
         }},
        {"--uid", true, false, false,
         [](Options& options, std::string_view value) {
             return set_optional(options.uid, value, MAX_UID);
         }},
        {"--tagd-uid", true, true, false,
         [](Options& options, std::string_view value) {
             // The same option names the user scada-tagd runs as for
             // both commands.
             std::optional<std::uint32_t> uid;
             if (!set_optional(uid, value, MAX_UID)) {
                 return false;
             }
             options.tagd_uid = uid;
             options.attach_tagd_uid = uid;
             return true;
         }},
        {"--reader", true, false, true,
         [](Options& options, std::string_view value) {
             if (options.readers.size() >= MAX_READERS ||
                 !scada::tag::is_valid_name(
                     value, scada::tag::MAX_SHORT_NAME_LENGTH)) {
                 return false;
             }
             options.readers.emplace_back(value);
             return true;
         }},
        {"--heartbeat-check-ms", true, false, false,
         [](Options& options, std::string_view value) {
             return set_optional(options.heartbeat_check_ms, value,
                                 MAX_HEARTBEAT_MS) &&
                    *options.heartbeat_check_ms != 0;
         }},
        {"--heartbeat-timeout-ms", true, false, false,
         [](Options& options, std::string_view value) {
             return set_optional(options.heartbeat_timeout_ms, value,
                                 MAX_HEARTBEAT_MS) &&
                    *options.heartbeat_timeout_ms != 0;
         }},
        {"--ring-capacity", true, false, false,
         [](Options& options, std::string_view value) {
             return set_number(options.ring_capacity, value,
                               MAX_RING_CAPACITY) &&
                    options.ring_capacity != 0;
         }},
        {"--output", true, false, false,
         [](Options& options, std::string_view value) {
             options.output = std::string(value);
             return !value.empty();
         }},
        {"--instance", false, true, false,
         [](Options& options, std::string_view value) {
             return set_number(options.instance, value, MAX_INSTANCE);
         }},
        {"--rate", false, true, false,
         [](Options& options, std::string_view value) {
             return set_optional(options.rate, value, MAX_RATE);
         }},
        {"--rounds", false, true, false,
         [](Options& options, std::string_view value) {
             return set_optional(options.rounds, value, MAX_ROUNDS) &&
                    *options.rounds != 0;
         }},
        {"--seconds", false, true, false,
         [](Options& options, std::string_view value) {
             return set_optional(options.seconds, value, MAX_SECONDS) &&
                    *options.seconds != 0;
         }},
        {"--hold", false, true, false,
         [](Options& options, std::string_view /*value*/) {
             options.hold = true;
             return true;
         },
         true},
        {"--notify", false, true, false,
         [](Options& options, std::string_view value) {
             if (value == "batch") {
                 options.notify = Notify::BATCH;
             } else if (value == "each") {
                 options.notify = Notify::EACH;
             } else {
                 return false;
             }
             return true;
         }},
        {"--amplitude", false, true, false,
         [](Options& options, std::string_view value) {
             double amplitude = 0.0;
             const std::from_chars_result parsed = std::from_chars(
                 value.data(), value.data() + value.size(), amplitude);
             if (value.empty() || parsed.ec != std::errc{} ||
                 parsed.ptr != value.data() + value.size() ||
                 !std::isfinite(amplitude) || amplitude <= 0.0 ||
                 amplitude > MAX_AMPLITUDE) {
                 return false;
             }
             options.waveform.amplitude = amplitude;
             return true;
         }},
        {"--period", false, true, false,
         [](Options& options, std::string_view value) {
             return set_number(options.waveform.period, value, MAX_PERIOD) &&
                    options.waveform.period >= MIN_PERIOD;
         }},
        {"--seed", false, true, false,
         [](Options& options, std::string_view value) {
             return set_number(options.waveform.seed, value, MAX_SEED);
         }},
        {"--base-dir", false, true, false,
         [](Options& options, std::string_view value) {
             options.base_dir = std::string(value);
             return !value.empty();
         }},
    };
    return SPECS;
}

const OptionSpec* find_option(std::string_view name) {
    for (const OptionSpec& spec : option_specs()) {
        if (spec.name == name) {
            return &spec;
        }
    }
    return nullptr;
}

// Applies the option at `args[index]` and its value, if it takes one, to
// `options`, and sets `used` to the number of arguments it took; `seen`
// holds the options applied before.
Status apply_option(Options& options, std::span<const std::string_view> args,
                    std::size_t index, std::vector<std::string_view>& seen,
                    std::size_t& used, std::string& detail) {
    const std::string_view name = args[index];
    const OptionSpec* const spec = find_option(name);
    const bool allowed =
        spec != nullptr &&
        (options.command == Command::TAG_SET ? spec->tag_set : spec->run);
    if (!allowed) {
        // An unknown name is outside input: it is repeated only if it is
        // harmless text.
        detail = "unknown option for this command";
        if (scada::tag::is_valid_name(
                name.substr(name.starts_with("--") ? 2 : 0),
                scada::tag::MAX_SHORT_NAME_LENGTH)) {
            detail += ": " + std::string(name);
        }
        return USAGE_ERROR;
    }
    if (!spec->repeatable &&
        std::find(seen.begin(), seen.end(), name) != seen.end()) {
        detail = "option given twice: " + std::string(name);
        return USAGE_ERROR;
    }
    seen.push_back(name);
    if (spec->flag) {
        used = 1;
        // The setter of a flag does not fail.
        static_cast<void>(spec->set(options, {}));
        return {};
    }
    used = 2;
    if (index + 1 >= args.size()) {
        detail = "option without a value: " + std::string(name);
        return USAGE_ERROR;
    }
    if (!spec->set(options, args[index + 1])) {
        detail = "not a valid value for " + std::string(name);
        return USAGE_ERROR;
    }
    return {};
}

// Checks what depends on more than one option.
Status check_combination(const Options& options, std::string& detail) {
    if (const Status layout = check_layout(options.layout, detail);
        !layout.ok()) {
        return layout;
    }
    if (options.command != Command::RUN) {
        return {};
    }
    if (options.instance >= options.layout.processes) {
        detail = "--instance must be below --processes";
        return USAGE_ERROR;
    }
    if (options.rounds.has_value() && options.seconds.has_value()) {
        detail = "--rounds and --seconds cannot both be given";
        return USAGE_ERROR;
    }
    return {};
}

// Writes the tag-set file. Returns the exit code.
int write_tag_set(const Options& options, std::ostream& out,
                  std::ostream& err) {
    const TagSetOptions tag_set_options =
        to_tag_set_options(options, ::geteuid());
    std::string detail;
    const Result<scada::tag::TagSet> tag_set =
        make_tag_set(tag_set_options, detail);
    if (!tag_set.ok()) {
        err << "scada-sim: " << tag_set.status().message() << ": " << detail
            << "\n";
        return EXIT_ERROR;
    }
    const std::string text = tag_set_json(tag_set.value());
    if (options.output.empty()) {
        out << text << std::flush;
        return out.good() ? EXIT_OK : EXIT_ERROR;
    }
    std::ofstream file(options.output, std::ios::binary | std::ios::trunc);
    file << text;
    file.close();
    if (!file.good()) {
        err << "scada-sim: cannot write " << options.output << "\n";
        return EXIT_ERROR;
    }
    return EXIT_OK;
}

// The tags owners before `owner` have together.
std::uint64_t tags_before(const Layout& layout, std::uint32_t owner) {
    const std::uint64_t share = layout.tags / layout.owners;
    const std::uint64_t remainder = layout.tags % layout.owners;
    return (owner * share) + std::min<std::uint64_t>(owner, remainder);
}

// floor(total * tags_before(owner) / tags), without an intermediate product
// that could overflow: total is at most MAX_RATE * MAX_SECONDS and the tags
// at most MAX_TAGS.
std::uint64_t share_up_to(std::uint64_t total, const Layout& layout,
                          std::uint32_t owner) {
    const std::uint64_t before = tags_before(layout, owner);
    return ((total / layout.tags) * before) +
           (((total % layout.tags) * before) / layout.tags);
}

// Sets the rate and the end of the run of owner `owner`.
void plan_run(const Options& options, std::uint32_t owner, OwnerPlan& plan) {
    const Layout& layout = options.layout;
    const std::uint64_t rate = effective_rate(options);
    const std::uint64_t tags = tags_of_owner(layout, owner);
    // In range: rate and tags are bounded by MAX_RATE and MAX_TAGS.
    plan.per_second = static_cast<double>(rate) * static_cast<double>(tags) /
                      static_cast<double>(layout.tags);
    if (options.rounds.has_value()) {
        plan.updates = *options.rounds * tags;
    } else if (options.seconds.has_value() && rate != 0) {
        // The updates due in that time, shared out by tags so that all
        // owners together write rate * seconds, none lost to rounding. The
        // owner writes its share evenly over the time.
        const std::uint64_t total = rate * *options.seconds;
        const std::uint64_t updates = share_up_to(total, layout, owner + 1) -
                                      share_up_to(total, layout, owner);
        plan.updates = updates;
        plan.per_second = static_cast<double>(updates) /
                          static_cast<double>(*options.seconds);
    } else if (options.seconds.has_value()) {
        plan.run_for = std::chrono::seconds(*options.seconds);
    }
}

// Resolves the tags of owner `owner` into `plan`, checks that each belongs
// to this process and has the type of its pattern, and takes the write
// count of each CHECK tag from its record. Errors go to `err`.
bool resolve_tags(scada_ctx* ctx, const Layout& layout, std::uint32_t owner,
                  OwnerPlan& plan, std::ostream& err) {
    const std::uint64_t tags = tags_of_owner(layout, owner);
    plan.handles.reserve(tags);
    plan.patterns.reserve(tags);
    plan.first_counts.reserve(tags);
    for (std::uint64_t tag = 0; tag < tags; ++tag) {
        // Resolving many tags can take longer than the heartbeat timeout
        // on a slow machine; the process must not read as down meanwhile.
        // A heartbeat makes no system call; a retiring generation is
        // reported by the heartbeat of the main loop.
        if (tag % HEARTBEAT_EVERY_TAGS == 0) {
            static_cast<void>(scada_tag_heartbeat(ctx));
        }
        const std::string name = tag_name(layout, owner, tag);
        const Pattern pattern = pattern_of(layout, tag);
        std::uint32_t handle = SCADA_INVALID_HANDLE;
        scada_tag_info_t info{};
        scada_value value{};
        if (scada_tag_resolve(ctx, name.c_str(), &handle) != SCADA_OK ||
            scada_tag_info(ctx, handle, &info) != SCADA_OK ||
            scada_tag_read(ctx, handle, &value) != SCADA_OK) {
            err << "scada-sim: the tag set has no tag " << name << "\n";
            return false;
        }
        if (info.owned == 0 ||
            info.type != static_cast<std::uint8_t>(tag_type(pattern))) {
            err << "scada-sim: tag " << name
                << " is not of this process or not of the type of the "
                   "pattern "
                << to_string(pattern) << "\n";
            return false;
        }
        plan.handles.push_back(handle);
        plan.patterns.push_back(pattern);
        // A region reused from an earlier run keeps its change count; CHECK
        // goes on from it, so that the change count still equals the value
        // (pattern.h).
        plan.first_counts.push_back(
            pattern == Pattern::CHECK ? std::uint64_t{value.change_count} : 0);
    }
    return true;
}

// The plans of the owners of `options.instance`. Errors go to `err`.
std::optional<std::vector<OwnerPlan>> make_plans(scada_ctx* ctx,
                                                 const Options& options,
                                                 std::ostream& err) {
    const std::uint32_t per_process = owners_per_process(options.layout);
    std::vector<OwnerPlan> plans(per_process);
    for (std::uint32_t local = 0; local < per_process; ++local) {
        const std::uint32_t owner = (options.instance * per_process) + local;
        OwnerPlan& plan = plans[local];
        plan.waveform = options.waveform;
        plan.notify = options.notify;
        plan_run(options, owner, plan);
        if (!resolve_tags(ctx, options.layout, owner, plan, err)) {
            return std::nullopt;
        }
    }
    return plans;
}

// The sums the main thread reports.
struct Totals {
    std::uint64_t written = 0;
    std::int64_t finished_ns = 0;  // of the last owner to finish
    std::int64_t behind_max_ns = 0;
    std::size_t finished = 0;
    bool failed = false;
};

Totals add_up(std::span<const OwnerProgress> progress) {
    Totals totals;
    for (const OwnerProgress& owner : progress) {
        // Acquire: pairs with the release stores of the owner thread.
        const std::int64_t finished =
            owner.finished_ns.load(std::memory_order_acquire);
        totals.failed =
            totals.failed || owner.failed.load(std::memory_order_acquire);
        // Relaxed: figures for reports.
        totals.written += owner.written.load(std::memory_order_relaxed);
        totals.behind_max_ns =
            std::max(totals.behind_max_ns,
                     owner.behind_max_ns.load(std::memory_order_relaxed));
        if (finished >= 0) {
            ++totals.finished;
            totals.finished_ns = std::max(totals.finished_ns, finished);
        }
    }
    return totals;
}

enum class Ending : std::uint8_t { SIGNALLED, FAILED };

// How a hold of --hold ended.
enum class HoldEnd : std::uint8_t { RELEASED, SIGNALLED, FAILED };

// Watches scada-tagd from the main loop. A process must tolerate a restart
// of scada-tagd (reference 3.3), so a simulator goes on while scada-tagd is
// down for a short time; one left behind by a test that stopped scada-tagd
// stops by itself instead of writing until it is killed.
class TagdWatch {
public:
    // Looks again if the last look was CHECK_PERIOD ago. False once
    // scada-tagd has been down for DOWN_LIMIT or the generation retires;
    // the reason is written to `err`.
    bool still_running(scada_ctx* ctx, std::ostream& err) {
        const Clock::time_point now = Clock::now();
        if (now - last_look_ < CHECK_PERIOD) {
            return true;
        }
        last_look_ = now;
        scada_status status{};
        const int code = scada_tag_status(ctx, &status);
        if (code == SCADA_E_RETIRING) {
            err << "scada-sim: the generation is retiring\n";
            return false;
        }
        if (code != SCADA_E_TAGD_DOWN) {
            down_since_.reset();
            return true;
        }
        if (!down_since_.has_value()) {
            down_since_ = now;
        }
        if (now - *down_since_ >= DOWN_LIMIT) {
            err << "scada-sim: scada-tagd has been down for "
                << std::chrono::duration_cast<std::chrono::seconds>(DOWN_LIMIT)
                       .count()
                << " s\n";
            return false;
        }
        return true;
    }

    static constexpr std::chrono::seconds CHECK_PERIOD{1};
    static constexpr std::chrono::seconds DOWN_LIMIT{5};

private:
    Clock::time_point last_look_ = Clock::now();
    std::optional<Clock::time_point> down_since_;
};

// The main thread while the owners write: the heartbeat every
// HEARTBEAT_PERIOD_NS until a signal in `signals` arrives or something
// fails. Prints "done" once every owner has finished.
Ending beat_until_stopped(scada_ctx* ctx, const sigset_t& signals,
                          std::span<const OwnerProgress> progress,
                          std::ostream& out, std::ostream& err) {
    const timespec wait{.tv_sec = 0, .tv_nsec = HEARTBEAT_PERIOD_NS};
    bool reported = false;
    TagdWatch tagd;
    for (;;) {
        if (::sigtimedwait(&signals, nullptr, &wait) >= 0) {
            return Ending::SIGNALLED;
        }
        if (errno != EAGAIN && errno != EINTR) {
            err << "scada-sim: waiting for a signal failed\n";
            return Ending::FAILED;
        }
        if (scada_tag_heartbeat(ctx) == SCADA_E_RETIRING) {
            err << "scada-sim: the generation is retiring\n";
            return Ending::FAILED;
        }
        if (!tagd.still_running(ctx, err)) {
            return Ending::FAILED;
        }
        const Totals totals = add_up(progress);
        if (totals.failed) {
            err << "scada-sim: a write failed\n";
            return Ending::FAILED;
        }
        if (!reported && totals.finished == progress.size()) {
            reported = true;
            out << "done updates=" << totals.written << " elapsed_ms="
                << totals.finished_ns / NANOSECONDS_PER_MILLISECOND
                << " behind_max_ms="
                << totals.behind_max_ns / NANOSECONDS_PER_MILLISECOND << "\n"
                << std::flush;
        }
    }
}

// The main thread of a simulator started with --hold, before the first
// write: the heartbeat every HEARTBEAT_PERIOD_NS until SIGUSR1 releases it
// or a signal in `stop_signals` stops it. The rule for a simulator left
// behind (TagdWatch) applies here too, so that one whose test died does not
// wait for ever.
HoldEnd hold_until_released(scada_ctx* ctx, const sigset_t& stop_signals,
                            std::ostream& err) {
    sigset_t signals = stop_signals;
    if (::sigaddset(&signals, SIGUSR1) != 0) {
        err << "scada-sim: cannot wait for SIGUSR1\n";
        return HoldEnd::FAILED;
    }
    const timespec wait{.tv_sec = 0, .tv_nsec = HEARTBEAT_PERIOD_NS};
    TagdWatch tagd;
    for (;;) {
        const int taken = ::sigtimedwait(&signals, nullptr, &wait);
        if (taken == SIGUSR1) {
            return HoldEnd::RELEASED;
        }
        if (taken >= 0) {
            return HoldEnd::SIGNALLED;
        }
        if (errno != EAGAIN && errno != EINTR) {
            err << "scada-sim: waiting for a signal failed\n";
            return HoldEnd::FAILED;
        }
        if (scada_tag_heartbeat(ctx) == SCADA_E_RETIRING) {
            err << "scada-sim: the generation is retiring\n";
            return HoldEnd::FAILED;
        }
        if (!tagd.still_running(ctx, err)) {
            return HoldEnd::FAILED;
        }
    }
}

// Attaches, writes and detaches. Returns the exit code.
int run_simulator(const Options& options, std::ostream& out,
                  std::ostream& err) {
    // SIGTERM, SIGINT and SIGHUP are stop requests, taken by sigtimedwait.
    // SIGPIPE is blocked too, but never taken: a write to a standard output
    // whose reader is gone then fails with EPIPE instead of killing the
    // process, and the simulator goes on.
    sigset_t signals{};
    sigset_t blocked{};
    if (::sigemptyset(&signals) != 0 || ::sigaddset(&signals, SIGTERM) != 0 ||
        ::sigaddset(&signals, SIGINT) != 0 ||
        ::sigaddset(&signals, SIGHUP) != 0 || ::sigemptyset(&blocked) != 0 ||
        ::sigaddset(&blocked, SIGTERM) != 0 ||
        ::sigaddset(&blocked, SIGINT) != 0 ||
        ::sigaddset(&blocked, SIGHUP) != 0 ||
        ::sigaddset(&blocked, SIGPIPE) != 0 ||
        // With --hold, SIGUSR1 is the release, taken by sigtimedwait; one
        // that comes later stays pending and does nothing.
        (options.hold && ::sigaddset(&blocked, SIGUSR1) != 0) ||
        ::pthread_sigmask(SIG_BLOCK, &blocked, nullptr) != 0) {
        err << "scada-sim: cannot block signals\n";
        return EXIT_ERROR;
    }
    const std::string process = process_name(options.layout, options.instance);
    const scada_attach_opts attach_options{
        .base_dir = options.base_dir.c_str(),
        .retain_dir = nullptr,
        .tagd_uid = options.attach_tagd_uid.has_value()
                        ? std::int64_t{*options.attach_tagd_uid}
                        : -1};
    scada_ctx* ctx = nullptr;
    const int attached =
        scada_tag_attach_opts(process.c_str(), &attach_options, &ctx);
    if (attached != SCADA_OK) {
        // Taken before any output, which may change errno.
        const int error = errno;
        err << "scada-sim: attach as " << process << " failed with code "
            << attached;
        if (attached == SCADA_E_IO) {
            err << ": " << std::generic_category().message(error);
        }
        err << "\n";
        return EXIT_ERROR;
    }
    out << "attached\n" << std::flush;

    std::optional<std::vector<OwnerPlan>> plans = make_plans(ctx, options, err);
    if (!plans.has_value()) {
        scada_tag_detach(ctx);
        return EXIT_ERROR;
    }
    if (options.hold) {
        out << "ready\n" << std::flush;
        const HoldEnd held = hold_until_released(ctx, signals, err);
        if (held != HoldEnd::RELEASED) {
            if (held == HoldEnd::SIGNALLED) {
                out << "stopped updates=0 elapsed_ms=0\n" << std::flush;
            }
            scada_tag_detach(ctx);
            return held == HoldEnd::SIGNALLED ? EXIT_OK : EXIT_ERROR;
        }
    }
    std::vector<OwnerProgress> progress(plans->size());
    std::atomic<bool> stop{false};
    Ending ending = Ending::FAILED;
    const Clock::time_point start = Clock::now();
    {
        out << "writing\n" << std::flush;
        std::vector<std::jthread> owners;
        owners.reserve(plans->size());
        bool started = true;
        try {
            for (std::size_t i = 0; i < plans->size(); ++i) {
                // The plan and progress of one owner, the context and the
                // stop flag outlive the thread: it is joined at the end of
                // this block.
                owners.emplace_back([ctx, &plan = (*plans)[i], start, &stop,
                                     &owner_progress = progress[i]] {
                    detail::write_as_owner(ctx, plan, start, stop,
                                           owner_progress);
                });
            }
        } catch (const std::system_error& error) {
            // No exception leaves the library: the threads started so far
            // are stopped and joined, and the run fails.
            err << "scada-sim: cannot start an owner thread: "
                << error.code().message() << "\n";
            started = false;
        }
        if (started) {
            ending = beat_until_stopped(ctx, signals, progress, out, err);
        }
        // Relaxed: the owners are joined before anything they wrote is
        // read.
        stop.store(true, std::memory_order_relaxed);
    }
    const Totals totals = add_up(progress);
    if (ending == Ending::SIGNALLED && totals.finished != progress.size()) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                                  start);
        out << "stopped updates=" << totals.written
            << " elapsed_ms=" << elapsed.count() << "\n"
            << std::flush;
    }
    scada_tag_detach(ctx);
    return ending == Ending::SIGNALLED && !totals.failed ? EXIT_OK : EXIT_ERROR;
}

}  // namespace

Result<Options> parse_options(std::span<const std::string_view> args,
                              std::string& detail) {
    Options options;
    if (args.empty() || args.front() == "--help" || args.front() == "-h") {
        if (args.size() > 1) {
            detail = "--help takes no further arguments";
            return USAGE_ERROR;
        }
        return options;
    }
    if (args.front() == "tag-set") {
        options.command = Command::TAG_SET;
    } else if (args.front() == "run") {
        options.command = Command::RUN;
    } else {
        detail = "the command must be tag-set or run";
        return USAGE_ERROR;
    }
    std::vector<std::string_view> seen;
    for (std::size_t i = 1; i < args.size();) {
        std::size_t used = 0;
        if (const Status applied =
                apply_option(options, args, i, seen, used, detail);
            !applied.ok()) {
            return applied;
        }
        i += used;
    }
    if (const Status combined = check_combination(options, detail);
        !combined.ok()) {
        return combined;
    }
    return options;
}

TagSetOptions to_tag_set_options(const Options& options, std::uint32_t user) {
    TagSetOptions tag_set_options;
    tag_set_options.layout = options.layout;
    tag_set_options.uid = options.uid.value_or(user);
    tag_set_options.tagd_uid = options.tagd_uid.value_or(tag_set_options.uid);
    tag_set_options.readers = options.readers;
    tag_set_options.heartbeat_check_ms = options.heartbeat_check_ms.value_or(
        scada::tag::DEFAULT_HEARTBEAT_CHECK_MS);
    tag_set_options.heartbeat_timeout_ms =
        options.heartbeat_timeout_ms.value_or(
            scada::tag::DEFAULT_HEARTBEAT_TIMEOUT_MS);
    tag_set_options.ring_capacity = options.ring_capacity;
    return tag_set_options;
}

std::uint64_t effective_rate(const Options& options) noexcept {
    if (options.rate.has_value()) {
        return *options.rate;
    }
    return std::max<std::uint64_t>(1, options.layout.tags / RATE_DIVISOR);
}

int run(std::span<const std::string_view> args, std::ostream& out,
        std::ostream& err) {
    std::string detail;
    const Result<Options> options = parse_options(args, detail);
    if (!options.ok()) {
        err << "scada-sim: " << detail << "\n"
            << "Try 'scada-sim --help'.\n";
        return EXIT_USAGE;
    }
    switch (options.value().command) {
        case Command::HELP:
            out << USAGE;
            return EXIT_OK;
        case Command::TAG_SET:
            return write_tag_set(options.value(), out, err);
        case Command::RUN:
            return run_simulator(options.value(), out, err);
    }
    return EXIT_ERROR;
}

}  // namespace scada::sim
