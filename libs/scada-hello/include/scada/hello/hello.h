#ifndef SCADA_HELLO_HELLO_H
#define SCADA_HELLO_HELLO_H

#include <iosfwd>
#include <span>
#include <string_view>

namespace scada::hello {

// Process exit codes of `scada-hello`.
inline constexpr int EXIT_OK = 0;
inline constexpr int EXIT_USAGE = 2;

// Returns the greeting line, without a trailing newline. The view refers to
// static storage and is valid for the life of the process.
[[nodiscard]] std::string_view greeting() noexcept;

// Runs the `scada-hello` command. `args` are the command-line arguments
// without the program name and are treated as untrusted. Normal output goes to
// `out`, diagnostics to `err`. Returns the process exit code: EXIT_OK on
// success, EXIT_USAGE for unknown or surplus arguments.
[[nodiscard]] int run(std::span<const std::string_view> args, std::ostream& out,
                      std::ostream& err);

}  // namespace scada::hello

#endif  // SCADA_HELLO_HELLO_H
