// Fuzz target for the parser of the service manager's environment variables.
//
// The input is split at line breaks into the values of NOTIFY_SOCKET,
// WATCHDOG_USEC and WATCHDOG_PID, in that order. A variable whose line is
// missing is unset. Bytes after the third line are ignored.

#include "scada/service/notify.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace {

// The process id the watchdog is checked against; the seed corpus uses it.
constexpr std::int64_t OWN_PID = 4242;

// Removes the first line from `rest` and returns it, or returns nothing if
// `rest` is exhausted.
std::optional<std::string> take_line(std::span<const std::uint8_t>& rest,
                                     bool& exhausted) {
    if (exhausted) {
        return std::nullopt;
    }
    std::string line;
    std::size_t used = 0;
    for (const std::uint8_t byte : rest) {
        ++used;
        if (byte == '\n') {
            rest = rest.subspan(used);
            return line;
        }
        line.push_back(static_cast<char>(byte));
    }
    exhausted = true;
    return line;
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    std::span<const std::uint8_t> rest(data, size);
    bool exhausted = rest.empty();
    scada::service::NotifyEnvironment environment;
    environment.notify_socket = take_line(rest, exhausted);
    environment.watchdog_usec = take_line(rest, exhausted);
    environment.watchdog_pid = take_line(rest, exhausted);
    // The result is not needed: rejecting input is correct behaviour.
    static_cast<void>(
        scada::service::parse_notify_environment(environment, OWN_PID));
    return 0;
}
