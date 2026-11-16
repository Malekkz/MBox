// scada-tag-rt-reader: the real-time readers of conformance test 26 of the
// tag database contract (section 15, false trips). Test code; not
// installed.
//
// Usage: scada-tag-rt-reader <base dir> <process> <prefix> <owners> <tags>
//            <readers> <max seconds>
//
// Attaches as <process>, which the tag set marks REALTIME, resolves the
// <tags> tags of the simulation <prefix> with <owners> owners in one
// process (scada-sim's names, pattern CHECK), and starts <readers> reader
// threads under SCHED_FIFO, reader i on the i-th processor it may use,
// modulo their number. Each is a control task: every CYCLE it reads the
// next SLICE tags with scada_tag_read_many and checks every value, then
// waits for the start of its next cycle. The main thread, also under
// SCHED_FIFO, writes the heartbeat every BEAT.
//
// It prints "running" once every reader has done a cycle. On SIGTERM it
// stops the readers, prints one line of counts and exits with code 0:
//   reads= busy= torn= backwards= down= errors= cycles= overruns=
//   longest_cycle_us= realtime= timeout_ms=
// Exit code 1: an error; 2: wrong arguments.
//
// It cannot hang the machine. RLIMIT_RTTIME stops it, with SIGXCPU and
// then SIGKILL, if a thread runs under SCHED_FIFO for RT_TIME_LIMIT without
// blocking. It ends by itself after <max seconds> even without SIGTERM.
// Without the privilege for SCHED_FIFO it exits with code 1.

#include "scada/sim/layout.h"
#include "scada/sim/pattern.h"
#include "scada/tag/scada_tag.h"
#include "scada/test/cpu_affinity.h"

#include <pthread.h>
#include <sched.h>
#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal functions are declared here, not in <csignal>
#include <sys/resource.h>
#include <time.h>  // NOLINT(modernize-deprecated-headers): clock_nanosleep is declared here, not in <ctime>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

constexpr int EXIT_FAILED = 1;
constexpr int EXIT_USAGE = 2;
constexpr std::size_t ARGUMENTS = 8;
// A fast task (reference 15.2: 10 ms).
constexpr std::int64_t CYCLE_NS = 10'000'000;
// Tags one reader reads in one cycle, READ_BATCH at a time: with 4 readers
// and 50,000 tags every tag is read about every 25 cycles.
constexpr std::size_t SLICE = 5000;
constexpr std::size_t READ_BATCH = 500;
// The heartbeat period: below a third of the 500 ms timeout of a real-time
// process (section 10.1).
constexpr std::int64_t BEAT_NS = 100'000'000;
// The main thread asks for the state of the attachment about once a second
// (section 13: a process that writes no tags).
constexpr std::uint64_t BEATS_PER_STATUS = 10;
// Priorities under SCHED_FIFO: the readers above the main thread.
constexpr int READER_PRIORITY = 20;
constexpr int MAIN_PRIORITY = 10;
// CPU time a thread may use under SCHED_FIFO without blocking, in us.
constexpr rlim_t RT_TIME_LIMIT_US = 500'000;
constexpr rlim_t RT_TIME_HARD_LIMIT_US = 1'000'000;
constexpr std::uint64_t MAX_SECONDS = 3600;
constexpr std::uint64_t MAX_READERS = 64;
constexpr std::uint64_t MAX_OWNERS = 4096;
constexpr std::uint64_t MAX_TAGS = 1'000'000;
constexpr std::int64_t NS_PER_S = 1'000'000'000;
constexpr std::int64_t NS_PER_US = 1000;

std::optional<std::uint64_t> parse_number(std::string_view text,
                                          std::uint64_t max) {
    std::uint64_t value = 0;
    const std::from_chars_result parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        value > max) {
        return std::nullopt;
    }
    return value;
}

std::int64_t monotonic_ns() {
    timespec now{};
    static_cast<void>(::clock_gettime(CLOCK_MONOTONIC, &now));
    return (now.tv_sec * NS_PER_S) + now.tv_nsec;
}

timespec to_timespec(std::int64_t time_ns) {
    return {.tv_sec = time_ns / NS_PER_S, .tv_nsec = time_ns % NS_PER_S};
}

// What one reader found. Written by its thread only; read by the main
// thread once the thread is joined, but for `cycles`.
struct Counts {
    std::uint64_t reads = 0;
    std::uint64_t busy = 0;
    std::uint64_t torn = 0;
    std::uint64_t backwards = 0;
    std::uint64_t down = 0;
    std::uint64_t errors = 0;
    std::uint64_t overruns = 0;
    std::int64_t longest_cycle_ns = 0;
    std::atomic<std::uint64_t> cycles{0};
};

// Puts the calling thread under SCHED_FIFO at `priority`, on `cpu` if it is
// not negative.
bool make_real_time(int priority, int cpu) {
    const std::array<int, 1> only{cpu};
    if (cpu >= 0 && !scada::test::pin_this_thread(only)) {
        return false;
    }
    const sched_param parameter{.sched_priority = priority};
    return ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &parameter) ==
           0;
}

// Judges one read of a tag whose value was `last` at the read before, and
// sets `last` to its value now.
void judge(int status, const scada_value& value, std::uint64_t& last,
           Counts& counts) {
    ++counts.reads;
    if (status != SCADA_OK) {
        ++(status == SCADA_E_BUSY ? counts.busy : counts.errors);
        return;
    }
    if ((value.quality_flags & SCADA_FLAG_OWNER_DOWN) != 0) {
        ++counts.down;
        return;
    }
    if (!scada::sim::is_check_value(value)) {
        ++counts.torn;
    }
    if (value.value < last) {
        ++counts.backwards;
    }
    last = value.value;
}

// Reads SLICE tags of `handles` from `next` on, wrapping at the end, and
// judges every value. Returns where the next slice begins.
std::size_t read_slice(scada_ctx* ctx, std::span<const std::uint32_t> handles,
                       std::size_t next, std::span<std::uint64_t> last,
                       Counts& counts) {
    std::array<scada_value, READ_BATCH> values{};
    std::array<int, READ_BATCH> status{};
    for (std::size_t done = 0; done < SLICE;) {
        const std::size_t count =
            std::min({READ_BATCH, SLICE - done, handles.size() - next});
        static_cast<void>(
            scada_tag_read_many(ctx, handles.subspan(next, count).data(), count,
                                values.data(), status.data()));
        for (std::size_t i = 0; i < count; ++i) {
            judge(status.at(i), values.at(i), last[next + i], counts);
        }
        done += count;
        next = (next + count) % handles.size();
    }
    return next;
}

// One reader: a control task that reads SLICE tags every CYCLE_NS, from
// tag `first` on, until `stop` is set or `end_ns` passes.
void read_cycles(scada_ctx* ctx, std::span<const std::uint32_t> handles,
                 std::size_t first, std::int64_t end_ns,
                 const std::atomic<bool>& stop, Counts& counts) {
    std::vector<std::uint64_t> last(handles.size(), 0);
    std::size_t next = first % handles.size();
    std::int64_t cycle_start = monotonic_ns();
    // Acquire: pairs with the release store of the main thread.
    while (!stop.load(std::memory_order_acquire) && cycle_start < end_ns) {
        next = read_slice(ctx, handles, next, last, counts);
        const std::int64_t took = monotonic_ns() - cycle_start;
        counts.longest_cycle_ns = std::max(counts.longest_cycle_ns, took);
        // Relaxed: the main thread only waits for the count to grow.
        counts.cycles.fetch_add(1, std::memory_order_relaxed);
        cycle_start += CYCLE_NS;
        if (monotonic_ns() > cycle_start) {
            // The cycle overran its period: start the next one now.
            ++counts.overruns;
            cycle_start = monotonic_ns();
            continue;
        }
        // The wait of a control task for its next cycle, not a wait for
        // something to happen.
        const timespec wake = to_timespec(cycle_start);
        while (::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake,
                                 nullptr) == EINTR) {
        }
    }
}

// The handles of every tag of the simulation `layout`, or nothing.
std::optional<std::vector<std::uint32_t>> resolve_all(
    scada_ctx* ctx, const scada::sim::Layout& layout) {
    std::vector<std::uint32_t> handles;
    handles.reserve(layout.tags);
    for (std::uint32_t owner = 0; owner < layout.owners; ++owner) {
        for (std::uint64_t tag = 0;
             tag < scada::sim::tags_of_owner(layout, owner); ++tag) {
            std::uint32_t handle = SCADA_INVALID_HANDLE;
            const std::string name = scada::sim::tag_name(layout, owner, tag);
            if (scada_tag_resolve(ctx, name.c_str(), &handle) != SCADA_OK) {
                std::cerr << "scada-tag-rt-reader: cannot resolve " << name
                          << "\n";
                return std::nullopt;
            }
            handles.push_back(handle);
        }
    }
    return handles;
}

struct Arguments {
    std::string base;
    std::string process;
    scada::sim::Layout layout;
    std::size_t readers = 0;
    std::uint64_t max_seconds = 0;
};

std::optional<Arguments> parse(std::span<const std::string_view> args) {
    if (args.size() != ARGUMENTS) {
        return std::nullopt;
    }
    const std::optional<std::uint64_t> owners =
        parse_number(args[4], MAX_OWNERS);
    const std::optional<std::uint64_t> tags = parse_number(args[5], MAX_TAGS);
    const std::optional<std::uint64_t> readers =
        parse_number(args[6], MAX_READERS);
    const std::optional<std::uint64_t> seconds =
        parse_number(args[7], MAX_SECONDS);
    if (!owners || !tags || !readers || !seconds || *owners == 0 ||
        *tags < *owners || *readers == 0 || *seconds == 0) {
        return std::nullopt;
    }
    Arguments parsed;
    parsed.base = std::string(args[1]);
    parsed.process = std::string(args[2]);
    // In range: at most MAX_OWNERS.
    parsed.layout = {.prefix = std::string(args[3]),
                     .processes = 1,
                     .owners = static_cast<std::uint32_t>(*owners),
                     .tags = *tags,
                     .patterns = {scada::sim::Pattern::CHECK}};
    parsed.readers = *readers;
    parsed.max_seconds = *seconds;
    return parsed;
}

// Writes the heartbeat every BEAT_NS until SIGTERM or `end_ns`, and tells
// once every reader has done a cycle. False if waiting failed.
bool beat_until_stopped(scada_ctx* ctx, const sigset_t& signals,
                        std::int64_t end_ns, std::span<const Counts> counts) {
    const timespec beat = to_timespec(BEAT_NS);
    bool running = false;
    for (std::uint64_t beats = 0; monotonic_ns() < end_ns; ++beats) {
        if (::sigtimedwait(&signals, nullptr, &beat) >= 0) {
            return true;
        }
        if (errno != EAGAIN && errno != EINTR) {
            std::cerr << "scada-tag-rt-reader: waiting failed\n";
            return false;
        }
        static_cast<void>(scada_tag_heartbeat(ctx));
        if (beats % BEATS_PER_STATUS == 0) {
            scada_status status{};
            static_cast<void>(scada_tag_status(ctx, &status));
        }
        if (!running && std::ranges::all_of(counts, [](const Counts& count) {
                return count.cycles.load(std::memory_order_relaxed) != 0;
            })) {
            running = true;
            std::cout << "running\n" << std::flush;
        }
    }
    return true;
}

void print_counts(scada_ctx* ctx, std::span<const Counts> counts) {
    Counts all;
    std::uint64_t cycles = 0;
    for (const Counts& count : counts) {
        all.reads += count.reads;
        all.busy += count.busy;
        all.torn += count.torn;
        all.backwards += count.backwards;
        all.down += count.down;
        all.errors += count.errors;
        all.overruns += count.overruns;
        all.longest_cycle_ns =
            std::max(all.longest_cycle_ns, count.longest_cycle_ns);
        cycles += count.cycles.load(std::memory_order_relaxed);
    }
    // How the process table has this process: REALTIME, and its timeout.
    scada_status status{};
    scada_proc_status_t own{};
    static_cast<void>(scada_tag_status(ctx, &status));
    static_cast<void>(scada_proc_status(ctx, status.process_index, &own));
    std::cout << "reads=" << all.reads << " busy=" << all.busy
              << " torn=" << all.torn << " backwards=" << all.backwards
              << " down=" << all.down << " errors=" << all.errors
              << " cycles=" << cycles << " overruns=" << all.overruns
              << " longest_cycle_us=" << all.longest_cycle_ns / NS_PER_US
              << " realtime=" << int{own.realtime}
              << " timeout_ms=" << own.heartbeat_timeout_ms << "\n"
              << std::flush;
}

int run(std::span<const std::string_view> args) {
    const std::optional<Arguments> parsed = parse(args);
    if (!parsed.has_value()) {
        std::cerr << "usage: scada-tag-rt-reader <base dir> <process> <prefix> "
                     "<owners> <tags> <readers> <max seconds>\n";
        return EXIT_USAGE;
    }
    // The watchdog of a real-time thread that does not block.
    const rlimit limit{.rlim_cur = RT_TIME_LIMIT_US,
                       .rlim_max = RT_TIME_HARD_LIMIT_US};
    if (::setrlimit(RLIMIT_RTTIME, &limit) != 0) {
        std::cerr << "scada-tag-rt-reader: cannot limit the real-time CPU "
                     "time\n";
        return EXIT_FAILED;
    }
    sigset_t signals{};
    if (::sigemptyset(&signals) != 0 || ::sigaddset(&signals, SIGTERM) != 0 ||
        ::pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::cerr << "scada-tag-rt-reader: cannot block signals\n";
        return EXIT_FAILED;
    }
    const scada_attach_opts options{.base_dir = parsed->base.c_str(),
                                    .retain_dir = nullptr,
                                    .tagd_uid = std::int64_t{::geteuid()}};
    scada_ctx* ctx = nullptr;
    const int attached =
        scada_tag_attach_opts(parsed->process.c_str(), &options, &ctx);
    if (attached != SCADA_OK) {
        std::cerr << "scada-tag-rt-reader: attach failed with code " << attached
                  << "\n";
        return EXIT_FAILED;
    }
    std::cout << "attached\n" << std::flush;
    const std::optional<std::vector<std::uint32_t>> handles =
        resolve_all(ctx, parsed->layout);
    const std::vector<int> cpus = scada::test::allowed_cpus();
    if (!handles.has_value() || cpus.empty() ||
        !make_real_time(MAIN_PRIORITY, -1)) {
        std::cerr << "scada-tag-rt-reader: cannot resolve or cannot run under "
                     "SCHED_FIFO\n";
        scada_tag_detach(ctx);
        return EXIT_FAILED;
    }

    // In range: at most MAX_SECONDS.
    const std::int64_t end_ns =
        monotonic_ns() +
        (static_cast<std::int64_t>(parsed->max_seconds) * NS_PER_S);
    std::vector<Counts> counts(parsed->readers);
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> failed{0};
    bool good = true;
    {
        std::vector<std::jthread> readers;
        readers.reserve(parsed->readers);
        for (std::size_t i = 0; i < parsed->readers; ++i) {
            const int cpu = cpus.at(i % cpus.size());
            const std::size_t first = i * (handles->size() / parsed->readers);
            readers.emplace_back([ctx, &handles, first, end_ns, cpu, &stop,
                                  &failed, &count = counts.at(i)] {
                if (!make_real_time(READER_PRIORITY, cpu)) {
                    failed.fetch_add(1);
                    return;
                }
                read_cycles(ctx, *handles, first, end_ns, stop, count);
            });
        }
        good = beat_until_stopped(ctx, signals, end_ns, counts);
        // Release: the readers load with acquire.
        stop.store(true, std::memory_order_release);
    }
    if (failed.load() != 0) {
        std::cerr << "scada-tag-rt-reader: a reader could not run under "
                     "SCHED_FIFO\n";
        good = false;
    }
    print_counts(ctx, counts);
    scada_tag_detach(ctx);
    return good ? 0 : EXIT_FAILED;
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
    std::vector<std::string_view> args(raw.begin(), raw.end());
    return run(args);
}
