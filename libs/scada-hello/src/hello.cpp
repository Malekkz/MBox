#include "scada/hello/hello.h"

#include "scada/common/result.h"
#include "scada/service/notify.h"

#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal functions are declared here, not in <csignal>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <optional>
#include <ostream>
#include <span>
#include <string_view>

namespace scada::hello {

namespace {

using scada::service::Notifier;
using scada::service::NotifySettings;

constexpr std::string_view USAGE =
    "Usage: scada-hello [--help | --service]\n"
    "\n"
    "Prints a greeting. Proves the build, the library-plus-thin-executable\n"
    "pattern and the service skeleton; it has no other function.\n"
    "\n"
    "Options:\n"
    "  --help     Show this text and exit.\n"
    "  --service  Run as a systemd notify service: print the greeting, report\n"
    "             readiness, and feed the watchdog until stopped.\n"
    "\n"
    "Signals (with --service):\n"
    "  SIGTERM, SIGINT  Stop and exit with code 0.\n"
    "  SIGUSR1          Hang the main loop on purpose, to test the watchdog.\n"
    "\n"
    "Environment (with --service, set by systemd):\n"
    "  NOTIFY_SOCKET    Socket for readiness and watchdog messages.\n"
    "  WATCHDOG_USEC    Watchdog interval in microseconds; the service feeds\n"
    "                   the watchdog at half this interval.\n"
    "  WATCHDOG_PID     Process the watchdog applies to.\n";

// What ended one wait of the main loop.
enum class Wake : std::uint8_t { FEED_DUE, STOP, HANG, ERROR };

// Waits for one of the signals in `signals`, which must be blocked, for at
// most `timeout`, or without limit if `timeout` is nullopt.
Wake wait_for_signal(
    const sigset_t& signals,
    std::optional<std::chrono::microseconds> timeout) noexcept {
    int signal_number = 0;
    if (timeout) {
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(*timeout);
        const std::chrono::nanoseconds rest = *timeout - seconds;
        // In range: the interval is at most MAX_WATCHDOG_INTERVAL (one day)
        // and `rest` is below one second.
        timespec limit{};
        limit.tv_sec = static_cast<std::time_t>(seconds.count());
        limit.tv_nsec = static_cast<long>(rest.count());
        signal_number = ::sigtimedwait(&signals, nullptr, &limit);
    } else {
        signal_number = ::sigwaitinfo(&signals, nullptr);
    }

    if (signal_number < 0) {
        // EAGAIN is the timeout. EINTR means an unrelated signal interrupted
        // the wait; feeding early is harmless, feeding late is not.
        return (errno == EAGAIN || errno == EINTR) ? Wake::FEED_DUE
                                                   : Wake::ERROR;
    }
    if (signal_number == SIGUSR1) {
        return Wake::HANG;
    }
    return Wake::STOP;
}

// Simulates a hung main loop. SIGTERM stays blocked, as it effectively is in
// a real hang; only a signal that cannot be blocked or is not blocked here
// (SIGKILL, the watchdog's SIGABRT) ends the process.
[[noreturn]] void hang() noexcept {
    for (;;) {
        static_cast<void>(::pause());
    }
}

int serve(std::ostream& out, std::ostream& err) {
    const Result<NotifySettings> parsed =
        scada::service::parse_notify_environment(
            scada::service::read_notify_environment(), ::getpid());
    if (!parsed.ok()) {
        err << "scada-hello: invalid environment: " << parsed.status().message()
            << '\n';
        return EXIT_ERROR;
    }
    const NotifySettings& settings = parsed.value();

    // The signals are blocked and then collected synchronously, so no signal
    // handler and no global flag is needed.
    sigset_t signals{};
    if (::sigemptyset(&signals) != 0 || ::sigaddset(&signals, SIGTERM) != 0 ||
        ::sigaddset(&signals, SIGINT) != 0 ||
        ::sigaddset(&signals, SIGUSR1) != 0 ||
        ::pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        err << "scada-hello: cannot block signals\n";
        return EXIT_ERROR;
    }

    Notifier notifier(settings);
    out << greeting() << '\n' << std::flush;
    if (!notifier.ready().ok()) {
        err << "scada-hello: cannot report readiness to the service manager\n";
        return EXIT_ERROR;
    }

    std::optional<std::chrono::microseconds> feed_period;
    if (settings.watchdog_interval) {
        feed_period = *settings.watchdog_interval / 2;
    }

    for (;;) {
        switch (wait_for_signal(signals, feed_period)) {
            case Wake::FEED_DUE:
                if (!notifier.watchdog().ok()) {
                    // Keep running: if feeding stays broken the service
                    // manager restarts the service, which is its job.
                    err << "scada-hello: cannot feed the watchdog\n"
                        << std::flush;
                }
                break;
            case Wake::STOP:
                // Best effort: the process exits either way, and the service
                // manager sees the exit.
                static_cast<void>(notifier.stopping());
                return EXIT_OK;
            case Wake::HANG:
                // Best effort: the status line only helps the person testing.
                static_cast<void>(notifier.set_status_text(HANG_STATUS));
                hang();
            case Wake::ERROR:
                err << "scada-hello: waiting for signals failed\n";
                return EXIT_ERROR;
        }
    }
}

}  // namespace

std::string_view greeting() noexcept {
    return "Hello from libscada-hello";
}

int run(std::span<const std::string_view> args, std::ostream& out,
        std::ostream& err) {
    if (args.empty()) {
        out << greeting() << '\n';
        return EXIT_OK;
    }
    if (args.size() == 1 && args.front() == "--service") {
        return serve(out, err);
    }
    if (args.size() == 1 && args.front() == "--help") {
        out << USAGE;
        return EXIT_OK;
    }
    err << "scada-hello: unexpected arguments\n" << USAGE;
    return EXIT_USAGE;
}

}  // namespace scada::hello
