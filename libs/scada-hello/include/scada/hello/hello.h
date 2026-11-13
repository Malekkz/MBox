#ifndef SCADA_HELLO_HELLO_H
#define SCADA_HELLO_HELLO_H

#include <iosfwd>
#include <span>
#include <string_view>

namespace scada::hello {

// Process exit codes of `scada-hello`.
inline constexpr int EXIT_OK = 0;
inline constexpr int EXIT_ERROR = 1;
inline constexpr int EXIT_USAGE = 2;

// Status line reported to the service manager before the main loop hangs on
// request (SIGUSR1).
inline constexpr std::string_view HANG_STATUS = "Main loop hung on request";

// Returns the greeting line, without a trailing newline. The view refers to
// static storage and is valid for the life of the process.
[[nodiscard]] std::string_view greeting() noexcept;

// Runs the `scada-hello` command. `args` are the command-line arguments
// without the program name and are treated as untrusted. Normal output goes to
// `out`, diagnostics to `err`. Returns the process exit code.
//
// With `--help` it prints the usage text and returns EXIT_OK. With unknown or
// surplus arguments it returns EXIT_USAGE. With no arguments it runs as a
// service and blocks until it is told to stop:
// - it prints the greeting, reports readiness to the service manager named in
//   the environment (NOTIFY_SOCKET), and feeds the watchdog at half the
//   interval in WATCHDOG_USEC;
// - on SIGTERM or SIGINT it reports that it is stopping and returns EXIT_OK;
// - on SIGUSR1 it stops feeding the watchdog and never returns, which lets a
//   test show that the service manager restarts a hung service;
// - it returns EXIT_ERROR if the environment values are invalid or readiness
//   cannot be reported.
// Without a service manager it runs the same way and sends nothing.
//
// It blocks SIGTERM, SIGINT and SIGUSR1 for the calling thread and leaves
// them blocked. Call it from the main thread before starting any other.
[[nodiscard]] int run(std::span<const std::string_view> args, std::ostream& out,
                      std::ostream& err);

}  // namespace scada::hello

#endif  // SCADA_HELLO_HELLO_H
