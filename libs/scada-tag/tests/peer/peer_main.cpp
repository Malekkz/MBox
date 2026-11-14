// scada-tag-test-peer: stands in for a service in the integration tests of
// scada-tagd, until the library can attach a process (issue P1-05). It
// attaches the `proc` region of one process and writes its heartbeat until
// it is stopped or killed. Test code; not installed.
//
// Usage: scada-tag-test-peer <generation directory> <process>
//            <process index> <generation> <queue count> <period in ms>
//
// It prints "attached" once the region is RUNNING. On SIGTERM it sets the
// state STOPPING and exits with code 0. Exit code 1: an error; 2: wrong
// arguments; 3: the process name is already attached.

#include "layout.h"
#include "proc_region.h"

#include "scada/common/clock.h"
#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"

#include <fcntl.h>
#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal functions are declared here, not in <csignal>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

constexpr int EXIT_FAILED = 1;
constexpr int EXIT_USAGE = 2;
constexpr int EXIT_BUSY = 3;

constexpr std::size_t ARGUMENT_COUNT = 7;
constexpr std::uint64_t MAX_PERIOD_MS = 60'000;
constexpr std::int64_t MILLISECONDS_PER_SECOND = 1000;
constexpr std::int64_t NANOSECONDS_PER_MILLISECOND = 1'000'000;

// The whole of `text` as a decimal number of at most `limit`.
std::optional<std::uint64_t> parse_number(std::string_view text,
                                          std::uint64_t limit) {
    std::uint64_t value = 0;
    const std::from_chars_result parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        value > limit) {
        return std::nullopt;
    }
    return value;
}

int run(std::span<const std::string_view> args) {
    using scada::tag::OwnProcRegion;
    using scada::tag::ProcessState;

    const std::optional<std::uint64_t> index =
        parse_number(args[3], UINT16_MAX);
    const std::optional<std::uint64_t> generation =
        parse_number(args[4], UINT64_MAX);
    const std::optional<std::uint64_t> queues =
        parse_number(args[5], UINT16_MAX);
    const std::optional<std::uint64_t> period =
        parse_number(args[6], MAX_PERIOD_MS);
    if (!index || !generation || !queues || !period || *period == 0) {
        std::cerr << "scada-tag-test-peer: bad number\n";
        return EXIT_USAGE;
    }

    sigset_t signals{};
    if (::sigemptyset(&signals) != 0 || ::sigaddset(&signals, SIGTERM) != 0 ||
        ::pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::cerr << "scada-tag-test-peer: cannot block signals\n";
        return EXIT_FAILED;
    }

    const std::string path(args[1]);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open has no other form
    const int raw = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const scada::Fd directory(raw);
    scada::tag::ProcRegionParameters parameters;
    parameters.process = args[2];
    // In range: checked against the limits above.
    parameters.process_index = static_cast<std::uint16_t>(*index);
    parameters.generation = *generation;
    parameters.queue_count = static_cast<std::size_t>(*queues);
    parameters.uid = ::geteuid();
    std::string detail;
    scada::Result<OwnProcRegion> attached =
        OwnProcRegion::attach(directory.get(), parameters, detail);
    if (!attached.ok()) {
        std::cerr << "scada-tag-test-peer: " << attached.status().message()
                  << " (" << detail << ")\n";
        return attached.status().code() == scada::StatusCode::ALREADY_EXISTS
                   ? EXIT_BUSY
                   : EXIT_FAILED;
    }
    OwnProcRegion region = std::move(attached).value();
    region.set_state(ProcessState::RUNNING);
    region.heartbeat(scada::monotonic_ns());
    std::cout << "attached\n" << std::flush;

    timespec wait{};
    // In range: the period is at most MAX_PERIOD_MS.
    const auto period_ms = static_cast<std::int64_t>(*period);
    wait.tv_sec = static_cast<std::time_t>(period_ms / MILLISECONDS_PER_SECOND);
    wait.tv_nsec = static_cast<long>((period_ms % MILLISECONDS_PER_SECOND) *
                                     NANOSECONDS_PER_MILLISECOND);
    for (;;) {
        if (::sigtimedwait(&signals, nullptr, &wait) >= 0) {
            region.set_state(ProcessState::STOPPING);
            return 0;
        }
        if (errno != EAGAIN && errno != EINTR) {
            std::cerr << "scada-tag-test-peer: waiting failed\n";
            return EXIT_FAILED;
        }
        region.heartbeat(scada::monotonic_ns());
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    // argv is the one C array the language hands us; wrap it with its length
    // at once so that nothing below indexes a raw pointer.
    const std::span<char*> raw_args(
        argv, argc > 0 ? static_cast<std::size_t>(argc) : 0);
    if (raw_args.size() != ARGUMENT_COUNT) {
        std::cerr << "scada-tag-test-peer: expected 6 arguments\n";
        return EXIT_USAGE;
    }
    std::array<std::string_view, ARGUMENT_COUNT> args;
    for (std::size_t i = 0; i < ARGUMENT_COUNT; ++i) {
        args.at(i) = raw_args[i];
    }
    return run(args);
}
