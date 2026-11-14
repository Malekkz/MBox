// scada-tag-test-peer: stands in for a service in the integration tests. It
// attaches to the tag database as one process through the C interface and
// writes its heartbeat until it is stopped or killed. Test code; not
// installed.
//
// Usage: scada-tag-test-peer <base directory> <process> <period in ms>
//            [mid-write]
//
// The uid of scada-tagd is taken to be the user of the caller, as in the
// tests. It prints "attached" once the process is RUNNING, and then
// "regions created=<n> reused=<n> repaired=<n>". With `mid-write` it then
// leaves the first tag record of the owner named like the process as a
// write leaves it half way, with an odd sequence number, and prints
// "mid-write"; the test kills it in that state.
//
// On SIGTERM it detaches and exits with code 0. Exit code 1: an error; 2:
// wrong arguments; 3: the process name is already attached.

#include "layout.h"
#include "shared_memory.h"

#include "scada/common/fd.h"
#include "scada/common/mapped_region.h"
#include "scada/common/result.h"
#include "scada/tag/scada_tag.h"

#include <fcntl.h>
#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal functions are declared here, not in <csignal>
#include <unistd.h>

#include <array>
#include <atomic>
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

namespace {

constexpr int EXIT_FAILED = 1;
constexpr int EXIT_USAGE = 2;
constexpr int EXIT_BUSY = 3;

constexpr std::size_t MIN_ARGUMENTS = 4;
constexpr std::size_t MAX_ARGUMENTS = 5;
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

// Leaves the first record of `tags.<owner>` of the current generation with
// an odd sequence number, as an owner that dies in the middle of a write
// does. The file is this process's own.
bool leave_mid_write(const std::string& base, const std::string& owner) {
    const std::string path = base + "/current/tags." + owner;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open has no other form
    const scada::Fd file(::open(path.c_str(), O_RDWR | O_CLOEXEC));
    const std::size_t size =
        sizeof(scada::tag::RegionHeader) + sizeof(scada::tag::TagRecord);
    const scada::Result<scada::MappedRegion> memory = scada::MappedRegion::map(
        file.get(), size, scada::MapAccess::READ_WRITE);
    if (!memory.ok()) {
        return false;
    }
    auto& record = scada::tag::shared_at<scada::tag::TagRecord>(
        memory.value().bytes(), sizeof(scada::tag::RegionHeader));
    const std::uint32_t seq =
        scada::tag::load_shared(record.seq, std::memory_order_relaxed);
    scada::tag::store_shared(record.seq, seq | 1U, std::memory_order_release);
    return true;
}

int run(std::span<const std::string_view> args) {
    const std::optional<std::uint64_t> period =
        parse_number(args[3], MAX_PERIOD_MS);
    const bool mid_write = args.size() == MAX_ARGUMENTS;
    if (!period || *period == 0 || (mid_write && args[4] != "mid-write")) {
        std::cerr << "scada-tag-test-peer: bad argument\n";
        return EXIT_USAGE;
    }

    sigset_t signals{};
    if (::sigemptyset(&signals) != 0 || ::sigaddset(&signals, SIGTERM) != 0 ||
        ::pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::cerr << "scada-tag-test-peer: cannot block signals\n";
        return EXIT_FAILED;
    }

    const std::string base(args[1]);
    const std::string process(args[2]);
    const scada_attach_opts options{.base_dir = base.c_str(),
                                    .retain_dir = nullptr,
                                    .tagd_uid = std::int64_t{::geteuid()}};
    scada_ctx* context = nullptr;
    const int attached =
        scada_tag_attach_opts(process.c_str(), &options, &context);
    if (attached != SCADA_OK) {
        std::cerr << "scada-tag-test-peer: attach failed with code " << attached
                  << "\n";
        return attached == SCADA_E_BUSY ? EXIT_BUSY : EXIT_FAILED;
    }
    scada_status status{};
    static_cast<void>(scada_tag_status(context, &status));
    std::cout << "attached\nregions created=" << status.regions_created
              << " reused=" << status.regions_reused
              << " repaired=" << status.records_repaired << "\n"
              << std::flush;
    if (mid_write) {
        if (!leave_mid_write(base, process)) {
            std::cerr << "scada-tag-test-peer: cannot reach the tags region\n";
            scada_tag_detach(context);
            return EXIT_FAILED;
        }
        std::cout << "mid-write\n" << std::flush;
    }

    timespec wait{};
    // In range: the period is at most MAX_PERIOD_MS.
    const auto period_ms = static_cast<std::int64_t>(*period);
    wait.tv_sec = static_cast<std::time_t>(period_ms / MILLISECONDS_PER_SECOND);
    wait.tv_nsec = static_cast<long>((period_ms % MILLISECONDS_PER_SECOND) *
                                     NANOSECONDS_PER_MILLISECOND);
    for (;;) {
        if (::sigtimedwait(&signals, nullptr, &wait) >= 0) {
            scada_tag_detach(context);
            return 0;
        }
        if (errno != EAGAIN && errno != EINTR) {
            std::cerr << "scada-tag-test-peer: waiting failed\n";
            scada_tag_detach(context);
            return EXIT_FAILED;
        }
        // A retiring generation is no reason to stop here: the tests that
        // change the generation look at scada-tagd, not at this helper.
        static_cast<void>(scada_tag_heartbeat(context));
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    // argv is the one C array the language hands us; wrap it with its length
    // at once so that nothing below indexes a raw pointer.
    const std::span<char*> raw_args(
        argv, argc > 0 ? static_cast<std::size_t>(argc) : 0);
    if (raw_args.size() < MIN_ARGUMENTS || raw_args.size() > MAX_ARGUMENTS) {
        std::cerr << "scada-tag-test-peer: expected 3 or 4 arguments\n";
        return EXIT_USAGE;
    }
    std::array<std::string_view, MAX_ARGUMENTS> args;
    for (std::size_t i = 0; i < raw_args.size(); ++i) {
        args.at(i) = raw_args[i];
    }
    return run(std::span<const std::string_view>(args).first(raw_args.size()));
}
