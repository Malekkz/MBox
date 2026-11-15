// scada-tag-test-retainer: a process of the integration tests of retained
// values and the generation change (contract, sections 11.3 and 12). It
// attaches through the C interface and, every <period> ms, writes the next
// number to the INT64 tag <tag>, GOOD, then calls notify and writes its
// heartbeat. Test code; not installed.
//
// Usage: scada-tag-test-retainer <base directory> <retain directory>
//            <process> <tag> <period in ms>
//
// The uid of scada-tagd is taken to be the user of the caller. At attach it
// prints "attached value=<v> quality=<q> flags=<f>", what the tag held after
// attach, and goes on counting from <v> + 1. When its heartbeat reports
// RETIRING it stops writing, prints "leaving last=<v>" with the last value
// it wrote, detaches, attaches again to `current` (section 11.3, step 2),
// resolves the tag again and prints the "attached" line anew.
//
// On SIGTERM it detaches, prints "stopped last=<v>" and exits with code 0. Exit code 1: an error;
// 2: wrong arguments.

#include "scada/tag/scada_tag.h"

#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal functions are declared here, not in <csignal>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

constexpr int EXIT_FAILED = 1;
constexpr int EXIT_USAGE = 2;
constexpr std::size_t ARGUMENTS = 6;
constexpr std::uint64_t MAX_PERIOD_MS = 60'000;
constexpr long NANOSECONDS_PER_MILLISECOND = 1'000'000;
constexpr long MILLISECONDS_PER_SECOND = 1000;

bool parse_period(std::string_view text, std::uint64_t& value) {
    const std::from_chars_result parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
           value > 0 && value <= MAX_PERIOD_MS;
}

// Attaches, resolves the tag and prints what it holds. False on an error.
bool attach(const scada_attach_opts& opts, const std::string& process,
            const std::string& tag, scada_ctx*& ctx, std::uint32_t& handle,
            std::int64_t& value) {
    if (const int code = scada_tag_attach_opts(process.c_str(), &opts, &ctx);
        code != SCADA_OK) {
        std::cerr << "scada-tag-test-retainer: attach failed: " << code << '\n';
        return false;
    }
    scada_value read{};
    if (scada_tag_resolve(ctx, tag.c_str(), &handle) != SCADA_OK ||
        scada_tag_read(ctx, handle, &read) != SCADA_OK) {
        std::cerr << "scada-tag-test-retainer: cannot read " << tag << '\n';
        return false;
    }
    value = static_cast<std::int64_t>(read.value);
    std::cout << "attached value=" << value
              << " quality=" << unsigned{read.quality}
              << " flags=" << read.quality_flags << '\n'
              << std::flush;
    return true;
}

int run(std::span<const std::string_view> args) {
    std::uint64_t period = 0;
    if (!parse_period(args[5], period)) {
        std::cerr << "scada-tag-test-retainer: bad argument\n";
        return EXIT_USAGE;
    }
    sigset_t signals{};
    if (::sigemptyset(&signals) != 0 || ::sigaddset(&signals, SIGTERM) != 0 ||
        ::pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        return EXIT_FAILED;
    }
    const std::string base(args[1]);
    const std::string retain(args[2]);
    const std::string process(args[3]);
    const std::string tag(args[4]);
    const scada_attach_opts opts{.base_dir = base.c_str(),
                                 .retain_dir = retain.c_str(),
                                 .tagd_uid = ::geteuid()};
    scada_ctx* ctx = nullptr;
    std::uint32_t handle = SCADA_INVALID_HANDLE;
    std::int64_t value = 0;
    if (!attach(opts, process, tag, ctx, handle, value)) {
        return EXIT_FAILED;
    }
    timespec wait{};
    wait.tv_sec = static_cast<std::time_t>(period / MILLISECONDS_PER_SECOND);
    wait.tv_nsec = static_cast<long>(period % MILLISECONDS_PER_SECOND) *
                   NANOSECONDS_PER_MILLISECOND;
    for (;;) {
        if (scada_tag_heartbeat(ctx) == SCADA_E_RETIRING) {
            std::cout << "leaving last=" << value << '\n' << std::flush;
            scada_tag_detach(ctx);
            if (!attach(opts, process, tag, ctx, handle, value)) {
                return EXIT_FAILED;
            }
        } else {
            scada_value next{};
            next.type = SCADA_TYPE_INT64;
            next.quality = SCADA_QUALITY_GOOD;
            next.value = static_cast<std::uint64_t>(value + 1);
            if (scada_tag_write(ctx, handle, &next) == SCADA_OK) {
                ++value;
            }
            static_cast<void>(scada_tag_notify(ctx));
        }
        const int signal = ::sigtimedwait(&signals, nullptr, &wait);
        if (signal == SIGTERM) {
            scada_tag_detach(ctx);
            std::cout << "stopped last=" << value << '\n' << std::flush;
            return 0;
        }
        if (signal < 0 && errno != EAGAIN && errno != EINTR) {
            return EXIT_FAILED;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv, argv + argc);
    if (args.size() != ARGUMENTS) {
        std::cerr << "usage: scada-tag-test-retainer <base directory> "
                     "<retain directory> <process> <tag> <period in ms>\n";
        return EXIT_USAGE;
    }
    return run(args);
}
