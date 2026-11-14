// scada-tag-test-writer: the writing side of conformance tests 2 and 3 of
// the tag database contract. It attaches as one process with several owner
// threads and writes the tags of each owner from a thread of its own, at a
// given rate, with values whose fields are derived from one another so that
// a reader can tell a torn read. Test code; not installed.
//
// Usage: scada-tag-test-writer <base directory> <process> <owners>
//            <tags per owner> <updates per second> <updates>
//
// The owners are `<process>.w<k>`, k from 0, and the tags of owner k are
// `<process>.w<k>.t<i>`, i from 0, of type UINT64. `updates per second` is
// the rate of all owners together; 0 writes as fast as it can. `updates` is
// how many updates all owners write together before they stop.
//
// The n-th write of a tag, n from 1, stores: value n, quality n % 3,
// quality flags n & 0x0FDF, source time 1,700,000,000,000,000,000 + n.
//
// It prints "attached" once it is RUNNING, "writing" when the owner threads
// start, and "done updates=<n> elapsed_ms=<n>" when they have written
// everything. It then keeps writing its heartbeat until SIGTERM, on which
// it detaches and exits with code 0. Exit code 1: an error; 2: wrong
// arguments.

#include "scada/tag/scada_tag.h"

#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal functions are declared here, not in <csignal>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int EXIT_FAILED = 1;
constexpr int EXIT_USAGE = 2;
constexpr std::size_t ARGUMENTS = 7;

constexpr std::uint64_t MAX_OWNERS = 64;
constexpr std::uint64_t MAX_TAGS_PER_OWNER = 100'000;
constexpr std::uint64_t MAX_RATE = 100'000'000;
constexpr std::uint64_t MAX_UPDATES = 1'000'000'000'000;

// The fields derived from the write count of a tag.
constexpr std::int64_t BASE_SOURCE_TIME = 1'700'000'000'000'000'000;
constexpr std::uint64_t FLAGS_MASK = 0x0FDF;
constexpr std::uint64_t QUALITIES = 3;

// A paced owner thread writes for one tick, then waits for the next.
constexpr std::chrono::milliseconds TICK{1};
constexpr std::uint64_t TICKS_PER_SECOND = 1000;
// The heartbeat period of the main thread, far below every timeout of the
// tests.
constexpr long HEARTBEAT_PERIOD_NS = 20'000'000;

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

// What one owner thread writes.
struct OwnerPlan {
    std::vector<std::uint32_t> handles;  // its tags
    std::uint64_t updates = 0;           // how many it writes in all
    std::uint64_t per_second = 0;        // its rate; 0: as fast as it can
};

// The value of the `count`-th write of a tag.
scada_value derived_value(std::uint64_t count) noexcept {
    scada_value value{};
    value.type = SCADA_TYPE_UINT64;
    value.value = count;
    // In range: below QUALITIES and within FLAGS_MASK.
    value.quality = static_cast<std::uint8_t>(count % QUALITIES);
    value.quality_flags = static_cast<std::uint16_t>(count & FLAGS_MASK);
    // In range: a count of writes stays far below 2^63.
    value.source_time_ns = BASE_SOURCE_TIME + static_cast<std::int64_t>(count);
    return value;
}

// The owner thread: writes `plan.updates` updates, going round its tags.
// With a rate, it writes the share of each tick and then waits for the
// next one; a thread that fell behind writes on without waiting.
void write_as_owner(scada_ctx* ctx, const OwnerPlan& plan,
                    std::atomic<std::uint64_t>& failures) {
    const Clock::time_point start = Clock::now();
    const std::size_t tags = plan.handles.size();
    for (std::uint64_t written = 0; written < plan.updates; ++written) {
        // The tag gets its next count: every tag is written in turn.
        const std::size_t tag = written % tags;
        const std::uint64_t count = (written / tags) + 1;
        const scada_value value = derived_value(count);
        if (scada_tag_write(ctx, plan.handles[tag], &value) != SCADA_OK) {
            failures.fetch_add(1, std::memory_order_relaxed);
        }
        if (plan.per_second != 0) {
            // The time at which `written + 1` updates are due, rounded
            // down to a tick.
            const std::uint64_t due_ticks =
                ((written + 1) * TICKS_PER_SECOND) / plan.per_second;
            // In range: ticks of a run of the tests.
            std::this_thread::sleep_until(
                start + TICK * static_cast<std::int64_t>(due_ticks));
        }
    }
}

// Resolves the tags of every owner and shares rate and updates among them.
std::optional<std::vector<OwnerPlan>> make_plans(scada_ctx* ctx,
                                                 const std::string& process,
                                                 std::uint64_t owners,
                                                 std::uint64_t tags_per_owner,
                                                 std::uint64_t per_second,
                                                 std::uint64_t updates) {
    std::vector<OwnerPlan> plans(owners);
    for (std::uint64_t owner = 0; owner < owners; ++owner) {
        OwnerPlan& plan = plans[owner];
        plan.updates = updates / owners + (owner < updates % owners ? 1 : 0);
        plan.per_second = per_second == 0 ? 0 : per_second / owners;
        plan.handles.reserve(tags_per_owner);
        for (std::uint64_t tag = 0; tag < tags_per_owner; ++tag) {
            const std::string name = process + ".w" + std::to_string(owner) +
                                     ".t" + std::to_string(tag);
            std::uint32_t handle = SCADA_INVALID_HANDLE;
            if (scada_tag_resolve(ctx, name.c_str(), &handle) != SCADA_OK) {
                std::cerr << "scada-tag-test-writer: no tag " << name << "\n";
                return std::nullopt;
            }
            plan.handles.push_back(handle);
        }
    }
    return plans;
}

// Writes the heartbeat until SIGTERM arrives. False if waiting failed.
bool beat_until_stopped(scada_ctx* ctx, const sigset_t& signals,
                        const std::atomic<bool>& writers_done,
                        Clock::time_point start, std::uint64_t updates) {
    const timespec wait{.tv_sec = 0, .tv_nsec = HEARTBEAT_PERIOD_NS};
    bool reported = false;
    for (;;) {
        if (::sigtimedwait(&signals, nullptr, &wait) >= 0) {
            return true;
        }
        if (errno != EAGAIN && errno != EINTR) {
            return false;
        }
        static_cast<void>(scada_tag_heartbeat(ctx));
        // Acquire: pairs with the release store after the last write.
        if (!reported && writers_done.load(std::memory_order_acquire)) {
            reported = true;
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - start);
            std::cout << "done updates=" << updates
                      << " elapsed_ms=" << elapsed.count() << "\n"
                      << std::flush;
        }
    }
}

int run(std::span<const std::string_view> args) {
    const std::optional<std::uint64_t> owners =
        parse_number(args[3], MAX_OWNERS);
    const std::optional<std::uint64_t> tags_per_owner =
        parse_number(args[4], MAX_TAGS_PER_OWNER);
    const std::optional<std::uint64_t> per_second =
        parse_number(args[5], MAX_RATE);
    const std::optional<std::uint64_t> updates =
        parse_number(args[6], MAX_UPDATES);
    if (!owners || *owners == 0 || !tags_per_owner || *tags_per_owner == 0 ||
        !per_second || (*per_second != 0 && *per_second < *owners) ||
        !updates) {
        std::cerr << "scada-tag-test-writer: bad argument\n";
        return EXIT_USAGE;
    }

    sigset_t signals{};
    if (::sigemptyset(&signals) != 0 || ::sigaddset(&signals, SIGTERM) != 0 ||
        ::pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::cerr << "scada-tag-test-writer: cannot block signals\n";
        return EXIT_FAILED;
    }

    const std::string base(args[1]);
    const std::string process(args[2]);
    const scada_attach_opts options{.base_dir = base.c_str(),
                                    .retain_dir = nullptr,
                                    .tagd_uid = std::int64_t{::geteuid()}};
    scada_ctx* ctx = nullptr;
    const int attached = scada_tag_attach_opts(process.c_str(), &options, &ctx);
    if (attached != SCADA_OK) {
        std::cerr << "scada-tag-test-writer: attach failed with code "
                  << attached << "\n";
        return EXIT_FAILED;
    }
    std::cout << "attached\n" << std::flush;

    const std::optional<std::vector<OwnerPlan>> plans = make_plans(
        ctx, process, *owners, *tags_per_owner, *per_second, *updates);
    if (!plans.has_value()) {
        scada_tag_detach(ctx);
        return EXIT_FAILED;
    }

    std::atomic<std::uint64_t> failures{0};
    std::atomic<bool> writers_done{false};
    bool stopped = false;
    {
        std::cout << "writing\n" << std::flush;
        const Clock::time_point start = Clock::now();
        // Joins the owner threads, then tells the main thread.
        const std::jthread writers([&plans, &failures, &writers_done, ctx] {
            {
                std::vector<std::jthread> owner_threads;
                owner_threads.reserve(plans->size());
                for (const OwnerPlan& plan : *plans) {
                    owner_threads.emplace_back([&plan, &failures, ctx] {
                        write_as_owner(ctx, plan, failures);
                    });
                }
            }
            writers_done.store(true, std::memory_order_release);
        });
        stopped =
            beat_until_stopped(ctx, signals, writers_done, start, *updates);
    }
    scada_tag_detach(ctx);
    // Relaxed: every thread that wrote it has been joined.
    const std::uint64_t failed = failures.load(std::memory_order_relaxed);
    if (!stopped || failed != 0) {
        std::cerr << "scada-tag-test-writer: " << failed
                  << " writes failed, or waiting failed\n";
        return EXIT_FAILED;
    }
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    // argv is the one C array the language hands us; wrap it with its length
    // at once so that nothing below indexes a raw pointer.
    const std::span<char*> raw_args(
        argv, argc > 0 ? static_cast<std::size_t>(argc) : 0);
    if (raw_args.size() != ARGUMENTS) {
        std::cerr << "scada-tag-test-writer: expected 6 arguments\n";
        return EXIT_USAGE;
    }
    std::array<std::string_view, ARGUMENTS> args;
    for (std::size_t i = 0; i < raw_args.size(); ++i) {
        args.at(i) = raw_args[i];
    }
    return run(args);
}
