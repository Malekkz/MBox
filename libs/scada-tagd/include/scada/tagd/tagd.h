#ifndef SCADA_TAGD_TAGD_H
#define SCADA_TAGD_TAGD_H

// scada-tagd: the service that creates the tag database from the tag-set
// definition and watches the liveness of every process (reference section 5;
// docs/contracts/tag-database.md, sections 3, 10 and 11.1).

#include <iosfwd>
#include <span>
#include <string_view>

namespace scada::tagd {

// Process exit codes of `scada-tagd`.
inline constexpr int EXIT_OK = 0;
inline constexpr int EXIT_ERROR = 1;
inline constexpr int EXIT_USAGE = 2;

// Where the tag-set file is read from unless `--tag-set` names another.
inline constexpr std::string_view DEFAULT_TAG_SET_PATH =
    "/etc/scada/tag-set.json";

// Runs the `scada-tagd` command. `args` are the command-line arguments
// without the program name and are treated as untrusted. Normal output goes
// to `out`, diagnostics to `err`. Returns the process exit code.
//
// With `--help` it prints the usage text and returns EXIT_OK. With unknown,
// repeated or incomplete arguments it returns EXIT_USAGE. Otherwise it runs
// as a service and blocks until it is told to stop:
// - it reads the tag-set file (`--tag-set <file>`), which must hold a
//   process named `tagd` with the user id this process runs as;
// - it reuses the current generation in the base directory
//   (`--base-dir <directory>`) if the tag set is unchanged, and creates the
//   next generation otherwise;
// - it reports readiness to the service manager named in the environment
//   (NOTIFY_SOCKET);
// - at every liveness check it writes its own heartbeat, sets and clears
//   the `down` flag of every process, removes entries that do not belong in
//   the generation directory and logs every change; the first check of a
//   new generation also logs each process that has not attached; from the
//   same loop it feeds the service manager's watchdog four times in its
//   interval (WATCHDOG_USEC);
// - on SIGTERM or SIGINT it marks itself as stopping, leaves the generation
//   in place and returns EXIT_OK;
// - it returns EXIT_ERROR if the tag-set file, the base directory or the
//   environment is not usable, or if another instance is running.
// Without a service manager it runs the same way and sends nothing. Log
// records go to the journal under a service manager and to the standard
// error stream otherwise.
//
// It blocks SIGTERM and SIGINT for the calling thread and leaves them
// blocked. Call it from the main thread before starting any other.
[[nodiscard]] int run(std::span<const std::string_view> args, std::ostream& out,
                      std::ostream& err);

}  // namespace scada::tagd

#endif  // SCADA_TAGD_TAGD_H
