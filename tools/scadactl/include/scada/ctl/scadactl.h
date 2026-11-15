#ifndef SCADA_CTL_SCADACTL_H
#define SCADA_CTL_SCADACTL_H

// scadactl: the local command-line tool (reference section 11.5). In phase
// 1 it reads the shared-memory tag database directly, read-only, through an
// observer context of libscada-tag (tag database contract, section 11.4):
// `status`, `tag list`, `tag get` and `tag watch`. It has no command that
// writes.

#include <iosfwd>
#include <span>
#include <string_view>

namespace scada::ctl {

// Process exit codes of scadactl.
inline constexpr int EXIT_OK = 0;
inline constexpr int EXIT_ERROR = 1;
inline constexpr int EXIT_USAGE = 2;

// Runs the scadactl command `args` (the arguments without the program
// name, untrusted). Output goes to `out`, messages to `err`. Returns the
// process exit code: EXIT_USAGE for arguments that are not valid (with the
// reason and a pointer to --help on `err`), EXIT_ERROR if the database
// cannot be read or a named tag does not exist, EXIT_OK otherwise.
//
// `tag watch` runs until SIGINT or SIGTERM, or until --count changes were
// shown. It blocks both signals in the calling thread while it runs and
// restores the signal mask before it returns; call it from the main thread
// before other threads start. Every line it writes is flushed at once.
[[nodiscard]] int run(std::span<const std::string_view> args, std::ostream& out,
                      std::ostream& err);

}  // namespace scada::ctl

#endif  // SCADA_CTL_SCADACTL_H
