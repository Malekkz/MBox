#ifndef SCADA_SIM_SIM_H
#define SCADA_SIM_SIM_H

// scada-sim: the simulator writer of the tag database (issue P1-10;
// reference sections 15.3 and 18.2). One command writes the tag-set file of
// a simulation, the other runs one simulator process of it: one thread per
// owner, each writing its tags at a set rate with the patterns of
// `pattern.h`.

#include "scada/common/result.h"
#include "scada/sim/layout.h"
#include "scada/sim/pattern.h"
#include "scada/sim/tag_set.h"

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scada::sim {

// Process exit codes of `scada-sim`.
inline constexpr int EXIT_OK = 0;
inline constexpr int EXIT_ERROR = 1;
inline constexpr int EXIT_USAGE = 2;

inline constexpr std::string_view DEFAULT_BASE_DIR = "/dev/shm/scada";

// When an owner thread calls scada_tag_notify.
enum class Notify : std::uint8_t {
    BATCH,  // after the writes of each 1 ms tick, or of each NOTIFY_BATCH
    EACH,   // after every write
};

// Writes between two notifies of an owner that writes as fast as it can.
inline constexpr std::uint64_t NOTIFY_BATCH = 1024;

// Limits of the options, so that no product of them overflows.
inline constexpr std::uint64_t MAX_RATE = 100'000'000;    // updates per second
inline constexpr std::uint64_t MAX_SECONDS = 31'536'000;  // one year
inline constexpr std::uint32_t MAX_HEARTBEAT_MS = 3'600'000;
inline constexpr std::size_t MAX_READERS = 64;

enum class Command : std::uint8_t { HELP, TAG_SET, RUN };

// The parsed command line.
struct Options {
    Command command = Command::HELP;
    Layout layout;

    // tag-set
    std::optional<std::uint32_t> uid;       // default: the user running it
    std::optional<std::uint32_t> tagd_uid;  // default: `uid`
    std::vector<std::string> readers;
    std::optional<std::uint32_t> heartbeat_check_ms;
    std::optional<std::uint32_t> heartbeat_timeout_ms;
    std::uint32_t ring_capacity = 0;  // 0: see TagSetOptions
    std::string output;               // empty: the standard output

    // run
    std::uint32_t instance = 0;  // the process of the layout to run
    // Updates per second of the whole simulation, all processes together;
    // 0: as fast as it can. Default: a tenth of the tags (reference 15.3).
    std::optional<std::uint64_t> rate;
    std::optional<std::uint64_t> rounds;   // stop after writing every tag so
                                           // often
    std::optional<std::uint64_t> seconds;  // stop after this long
    Notify notify = Notify::BATCH;
    Waveform waveform;
    std::string base_dir{DEFAULT_BASE_DIR};
    std::optional<std::uint32_t> attach_tagd_uid;  // default: user scada-tagd
};

// Parses the command line `args` (without the program name), which is
// untrusted. Checks every value against its limits and the layout with
// `check_layout`; the tag set itself is checked when it is made. On an error
// `detail` is replaced by a sentence for the user; otherwise it is left
// unchanged.
[[nodiscard]] Result<Options> parse_options(
    std::span<const std::string_view> args, std::string& detail);

// What `tag-set` makes the tag set from: the layout and tag-set options of
// `options`, with `user` as the uid where `--uid` was not given.
[[nodiscard]] TagSetOptions to_tag_set_options(const Options& options,
                                               std::uint32_t user);

// The rate a simulation runs at: `options.rate`, or its default.
[[nodiscard]] std::uint64_t effective_rate(const Options& options) noexcept;

// Runs the `scada-sim` command. `args` are the command-line arguments without
// the program name. Normal output goes to `out`, diagnostics to `err`.
// Returns the process exit code.
//
// - `--help`, or no command: prints the usage text and returns EXIT_OK.
// - `tag-set`: writes the tag-set file of the layout (`make_tag_set`,
//   `tag_set_json`) to `--output` or `out`.
// - `run`: attaches as process `--instance` of the layout and writes the
//   tags of each of its owners from a thread of its own. It prints
//   "attached" once attached, "writing" when the owner threads start, and,
//   when every owner has finished a run bounded by `--rounds` or
//   `--seconds`, "done updates=<n> elapsed_ms=<n> behind_max_ms=<n>". It
//   writes the heartbeat from the calling thread until SIGTERM, SIGINT or
//   SIGHUP, also after "done", so that the last values stay good for
//   readers. On the signal it stops the owners, prints "stopped
//   updates=<n> elapsed_ms=<n>" if they had not finished, detaches and
//   returns EXIT_OK. It stops with EXIT_ERROR once scada-tagd has been down
//   for 5 s or the generation retires. A standard output whose reader is
//   gone does not stop it. It starts no other process: each simulator
//   process of a layout is one call, as one process.
//
// Wrong arguments give EXIT_USAGE; a tag set that cannot be made, a file
// that cannot be written, a failed attach, a tag of the wrong type or a
// failed write give EXIT_ERROR.
//
// `run` blocks SIGTERM, SIGINT, SIGHUP and SIGPIPE for the calling thread
// and leaves them blocked. Call it from the main thread before starting
// any other.
[[nodiscard]] int run(std::span<const std::string_view> args, std::ostream& out,
                      std::ostream& err);

}  // namespace scada::sim

#endif  // SCADA_SIM_SIM_H
