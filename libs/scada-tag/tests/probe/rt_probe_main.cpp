// scada-tag-rt-probe: the program that conformance test 22 of the tag
// database contract traces. It attaches as one process, resolves tags, and
// then calls every function the contract marks real-time safe between two
// markers. The test that starts it records the system calls between the
// markers; this program counts the allocations there itself. Test code; not
// installed.
//
// Usage: scada-tag-rt-probe <base directory> <process> <own tag>
//            <tag of another process> [syscall|allocation]
//
// `own tag` is a FLOAT64 tag the process owns. The process produces the
// event queue `events.<process>`, whose consumer is attached, and consumes
// the request queue `cmd.<process>.feeder`, into which another process
// posts one request that expires 1 ms later. Before the markers it waits
// until the request is there and has expired; between them it looks at the
// queue without waiting, which answers the request EXPIRED in
// `result.<process>.feeder` (decision D-40). It also posts one request to
// `io` in `cmd.io.<process>` and one answer in `result.<process>.feeder`. The
// uid of scada-tagd is taken to be the user of the caller, as in the tests.
// With `syscall` or `allocation` it makes one system call or one allocation
// between the markers on purpose, to show that the test finds it.
//
// It prints one line:
//   calls=<n> failed=<n> allocations=<n> page_faults=<n> clock_syscall=<0|1>
//   expired=<n> dropped=<n>
// `calls` real-time safe calls were made, `failed` of them did not return
// SCADA_OK, `allocations` calls of the C++ allocation functions happened
// between the markers, and the thread took `page_faults` page faults there
// (-1 if the operating system did not tell).
// Exit code 0; 1: an error before the markers; 2: wrong arguments.
//
// Allocations are counted by replacing the global allocation functions of
// this program. A call of `malloc` itself would not be counted; the
// library has none, which the banned-function check enforces. The replacement
// hands out memory from a fixed block and never takes it back: the program
// allocates little and ends at once.

#include "scada/tag/scada_tag.h"
#include "scada/test/syscall_trace.h"

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int EXIT_FAILED = 1;
constexpr int EXIT_USAGE = 2;
constexpr std::size_t MIN_ARGUMENTS = 5;
constexpr std::size_t MAX_ARGUMENTS = 6;

// Tags read in one call of scada_tag_read_many: the own tag and the other
// one, several times over.
constexpr std::size_t BATCH = 64;
constexpr std::uint64_t WRITTEN_BITS = 0x4045'4000'0000'0000U;  // 42.5

// The memory the allocation functions hand out, and how much of it is
// used. 16 MiB is many times what attach needs for the tag sets of the
// tests; untouched pages cost nothing.
constexpr std::size_t ARENA_BYTES = std::size_t{16} * 1024 * 1024;
// The arena starts on a page boundary for every page size of the two
// architectures (4 KiB to 64 KiB), so that a position in it that is a
// multiple of the page size is the start of a page.
constexpr std::size_t LARGEST_PAGE_BYTES = std::size_t{64} * 1024;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): the memory behind the replaced allocation functions, which cannot hold state anywhere else
alignas(LARGEST_PAGE_BYTES) std::array<std::byte, ARENA_BYTES> arena;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): see `arena`
std::atomic<std::size_t> arena_used{0};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): see `arena`
std::atomic<std::uint64_t> allocation_count{0};

// `size` bytes aligned to `alignment`, a power of two, from the arena. Ends
// the process if the arena is used up.
void* allocate(std::size_t size, std::size_t alignment) noexcept {
    // Sequentially consistent throughout: this is not a fast path.
    allocation_count.fetch_add(1);
    std::size_t used = arena_used.load();
    for (;;) {
        const std::size_t start = (used + alignment - 1) & ~(alignment - 1);
        if (start > ARENA_BYTES || size > ARENA_BYTES - start) {
            std::abort();
        }
        if (arena_used.compare_exchange_weak(used, start + size)) {
            return std::span<std::byte>(arena).subspan(start).data();
        }
    }
}

// Moves the next allocation to the start of a page the arena has not yet
// handed out, so that the allocation of the fault `allocation` lands on a
// fresh page on every run, not only when the allocations of attach happen
// to end near a page boundary. Without `prefault_arena` it would then take
// a page fault on every run.
void start_next_allocation_on_new_page(std::size_t page_size) noexcept {
    const std::size_t used = arena_used.load();
    const std::size_t rest = (page_size - used % page_size) % page_size;
    if (rest != 0) {
        static_cast<void>(allocate(rest, 1));
    }
}

// Writes the `bytes` bytes of the arena after the part already handed out.
// The first write to a page of the arena is a page fault; the allocation of
// the fault `allocation` must be found as an allocation, not also as a page
// fault, which test 22 counts on its own. The stores are atomic so that the
// compiler keeps them although nothing reads the bytes.
void prefault_arena(std::size_t bytes) noexcept {
    const std::size_t used = arena_used.load();
    const std::size_t end = used + std::min(bytes, ARENA_BYTES - used);
    for (std::size_t i = used; i < end; ++i) {
        std::atomic_ref<std::byte>(arena.at(i))
            .store(std::byte{0}, std::memory_order_relaxed);
    }
}

}  // namespace

// The replaced allocation functions. The array forms and the forms that do
// not throw forward to these by default.
void* operator new(std::size_t size) {
    return allocate(size, alignof(std::max_align_t));
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* /*memory*/) noexcept {}
void operator delete(void* /*memory*/, std::size_t /*size*/) noexcept {}
void operator delete(void* /*memory*/,
                     std::align_val_t /*alignment*/) noexcept {}
void operator delete(void* /*memory*/, std::size_t /*size*/,
                     std::align_val_t /*alignment*/) noexcept {}

namespace {

// What the marked part did.
struct Outcome {
    int calls = 0;
    int failed = 0;
    std::uint64_t allocations = 0;
    long page_faults = -1;
};

// The page faults this thread has taken so far, minor and major; -1 if the
// operating system does not tell.
long page_faults_so_far() noexcept {
    rusage usage{};
    if (::getrusage(RUSAGE_THREAD, &usage) != 0) {
        return -1;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): the C library declares these fields of rusage inside unions
    return usage.ru_minflt + usage.ru_majflt;
}

// Everything the marked part uses, prepared before it.
struct Prepared {
    scada_ctx* ctx = nullptr;
    std::uint32_t own = SCADA_INVALID_HANDLE;
    std::uint32_t other = SCADA_INVALID_HANDLE;
    std::vector<std::uint32_t> handles;
    std::vector<scada_value> values;
    std::vector<int> status;
    std::vector<std::uint32_t> own_handles;
    std::vector<scada_value> own_values;
    scada_queue* queue = nullptr;
    scada_event_record event{};
    scada_queue* requests = nullptr;
    scada_write_request request{};
    // A request it posts to `io`, and the answer it posts to the feeder.
    scada_queue* commands = nullptr;
    scada_write_request command{};
    scada_queue* answers = nullptr;
    scada_write_result answer{};
    // What the fault `allocation` allocates. Kept, so that the compiler
    // cannot remove the allocation.
    std::unique_ptr<int> wasted;
};

void count(Outcome& outcome, int code, int expected = SCADA_OK) noexcept {
    ++outcome.calls;
    if (code != expected) {
        ++outcome.failed;
    }
}

// The marked part: every function the contract marks real-time safe that
// exists so far, the first read of each tag after its resolve among them.
Outcome run_marked(Prepared& prepared, std::string_view fault) {
    Outcome outcome;
    scada_value value{};
    const long faults_before = page_faults_so_far();
    const std::uint64_t before = allocation_count.load();
    scada::test::mark_trace_begin();

    count(outcome, scada_tag_read(prepared.ctx, prepared.other, &value));
    count(outcome, scada_tag_read(prepared.ctx, prepared.own, &value));
    count(outcome, scada_tag_heartbeat(prepared.ctx));
    count(outcome, scada_tag_write(prepared.ctx, prepared.own,
                                   prepared.own_values.data()));
    count(outcome,
          scada_tag_write_many(prepared.ctx, prepared.own_handles.data(),
                               prepared.own_handles.size(),
                               prepared.own_values.data()));
    count(outcome,
          scada_tag_read_many(prepared.ctx, prepared.handles.data(),
                              prepared.handles.size(), prepared.values.data(),
                              prepared.status.data()));
    count(outcome, scada_tag_read(prepared.ctx, prepared.own, &value));
    count(outcome, scada_queue_post(prepared.queue, &prepared.event,
                                    sizeof(prepared.event)));
    // Not marked real-time safe, but with timeout 0 it makes no system call
    // (decision D-40).
    count(outcome,
          scada_queue_next(prepared.requests, &prepared.request,
                           sizeof(prepared.request), 0),
          SCADA_E_TIMEOUT);
    count(outcome, scada_queue_post(prepared.commands, &prepared.command,
                                    sizeof(prepared.command)));
    count(outcome, scada_queue_post(prepared.answers, &prepared.answer,
                                    sizeof(prepared.answer)));
    if (fault == "syscall") {
        static_cast<void>(::getppid());
    }
    if (fault == "allocation") {
        prepared.wasted = std::make_unique<int>(0);
    }

    scada::test::mark_trace_end();
    outcome.allocations = allocation_count.load() - before;
    const long faults_after = page_faults_so_far();
    if (faults_before >= 0 && faults_after >= 0) {
        outcome.page_faults = faults_after - faults_before;
    }
    return outcome;
}

// The time of CLOCK_MONOTONIC, in ns.
std::int64_t monotonic_now() noexcept {
    timespec now{};
    static_cast<void>(::clock_gettime(CLOCK_MONOTONIC, &now));
    constexpr std::int64_t NANOSECONDS_PER_SECOND = 1'000'000'000;
    return (std::int64_t{now.tv_sec} * NANOSECONDS_PER_SECOND) + now.tv_nsec;
}

// Waits until the request queue holds a record, without taking it, and
// then until the 1 ms lifetime of that record has passed: it was posted
// before the wait returned.
bool wait_for_expired_request(Prepared& prepared) {
    constexpr int WAIT_LIMIT_MS = 30'000;
    constexpr std::int64_t LIFETIME_NS = 1'000'000;
    scada_waitset* set = nullptr;
    if (scada_waitset_create(prepared.ctx, &set) != SCADA_OK) {
        return false;
    }
    const bool arrived =
        scada_waitset_add_queue(set, prepared.requests) == SCADA_OK &&
        scada_waitset_wait(set, WAIT_LIMIT_MS) == SCADA_OK;
    scada_waitset_destroy(set);
    const std::int64_t expired_after = monotonic_now() + LIFETIME_NS;
    while (monotonic_now() <= expired_after) {
        // The clock is the condition: a lifetime is a time.
    }
    return arrived;
}

int run(std::span<const std::string_view> args) {
    const std::string_view fault =
        args.size() == MAX_ARGUMENTS ? args[MAX_ARGUMENTS - 1] : "";
    if (!fault.empty() && fault != "syscall" && fault != "allocation") {
        std::cerr << "scada-tag-rt-probe: bad argument\n";
        return EXIT_USAGE;
    }
    const std::string base(args[1]);
    const std::string process(args[2]);
    const std::string own_name(args[3]);
    const std::string other_name(args[4]);

    const scada_attach_opts options{.base_dir = base.c_str(),
                                    .retain_dir = nullptr,
                                    .tagd_uid = std::int64_t{::geteuid()}};
    Prepared prepared;
    const int attached =
        scada_tag_attach_opts(process.c_str(), &options, &prepared.ctx);
    if (attached != SCADA_OK) {
        std::cerr << "scada-tag-rt-probe: attach failed with code " << attached
                  << "\n";
        return EXIT_FAILED;
    }
    if (scada_tag_resolve(prepared.ctx, own_name.c_str(), &prepared.own) !=
            SCADA_OK ||
        scada_tag_resolve(prepared.ctx, other_name.c_str(), &prepared.other) !=
            SCADA_OK) {
        std::cerr << "scada-tag-rt-probe: cannot resolve the tags\n";
        scada_tag_detach(prepared.ctx);
        return EXIT_FAILED;
    }
    for (std::size_t i = 0; i < BATCH; ++i) {
        prepared.handles.push_back(i % 2 == 0 ? prepared.other : prepared.own);
    }
    prepared.values.resize(BATCH);
    prepared.status.resize(BATCH);
    scada_value written{};
    written.type = SCADA_TYPE_FLOAT64;
    written.quality = SCADA_QUALITY_GOOD;
    written.value = WRITTEN_BITS;
    prepared.own_handles.assign(BATCH, prepared.own);
    prepared.own_values.assign(BATCH, written);
    const std::string queue_name = "events." + process;
    if (scada_queue_open(prepared.ctx, queue_name.c_str(), &prepared.queue) !=
        SCADA_OK) {
        std::cerr << "scada-tag-rt-probe: cannot open " << queue_name << "\n";
        scada_tag_detach(prepared.ctx);
        return EXIT_FAILED;
    }
    const std::string requests_name = "cmd." + process + ".feeder";
    if (scada_queue_open(prepared.ctx, requests_name.c_str(),
                         &prepared.requests) != SCADA_OK) {
        std::cerr << "scada-tag-rt-probe: cannot open " << requests_name
                  << "\n";
        scada_queue_close(prepared.queue);
        scada_tag_detach(prepared.ctx);
        return EXIT_FAILED;
    }
    const std::string commands_name = "cmd.io." + process;
    const std::string answers_name = "result." + process + ".feeder";
    if (scada_queue_open(prepared.ctx, commands_name.c_str(),
                         &prepared.commands) != SCADA_OK ||
        scada_queue_open(prepared.ctx, answers_name.c_str(),
                         &prepared.answers) != SCADA_OK) {
        std::cerr << "scada-tag-rt-probe: cannot open the queues of "
                     "requests and answers\n";
        scada_queue_close(prepared.commands);
        scada_queue_close(prepared.requests);
        scada_queue_close(prepared.queue);
        scada_tag_detach(prepared.ctx);
        return EXIT_FAILED;
    }
    constexpr std::int64_t COMMAND_LIFETIME_NS = 30'000'000'000;
    prepared.command.target_handle = prepared.own;
    prepared.command.operation = SCADA_OP_READ_NOW;
    prepared.command.source = SCADA_SOURCE_AUTO;
    prepared.command.expiry_mono_ns = monotonic_now() + COMMAND_LIFETIME_NS;
    prepared.answer.request_id = 1;
    prepared.answer.status = SCADA_RESULT_OK;
    if (!wait_for_expired_request(prepared)) {
        std::cerr << "scada-tag-rt-probe: no request arrived\n";
        scada_queue_close(prepared.requests);
        scada_queue_close(prepared.queue);
        scada_tag_detach(prepared.ctx);
        return EXIT_FAILED;
    }
    prepared.event.kind = SCADA_EVENT_SYSTEM;
    prepared.event.source_handle = SCADA_INVALID_HANDLE;
    scada_status status{};
    static_cast<void>(scada_tag_status(prepared.ctx, &status));

    // The pages the next allocations will use, written before the markers.
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0 ||
        static_cast<std::size_t>(page_size) > LARGEST_PAGE_BYTES) {
        std::cerr << "scada-tag-rt-probe: page size not 4 KiB to 64 KiB\n";
        scada_tag_detach(prepared.ctx);
        return EXIT_FAILED;
    }
    const auto page = static_cast<std::size_t>(page_size);
    if (fault == "allocation") {
        start_next_allocation_on_new_page(page);
    }
    prefault_arena(2 * page);

    const Outcome outcome = run_marked(prepared, fault);
    scada_queue_stats_t stats{};
    static_cast<void>(scada_queue_stats(prepared.requests, &stats));

    std::cout << "calls=" << outcome.calls << " failed=" << outcome.failed
              << " allocations=" << outcome.allocations
              << " page_faults=" << outcome.page_faults
              << " clock_syscall=" << int{status.clock_syscall}
              << " expired=" << stats.expired
              << " dropped=" << stats.answers_dropped << "\n"
              << std::flush;
    scada_queue_close(prepared.answers);
    scada_queue_close(prepared.commands);
    scada_queue_close(prepared.requests);
    scada_queue_close(prepared.queue);
    scada_tag_detach(prepared.ctx);
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    // argv is the one C array the language hands us; wrap it with its length
    // at once so that nothing below indexes a raw pointer.
    const std::span<char*> raw_args(
        argv, argc > 0 ? static_cast<std::size_t>(argc) : 0);
    if (raw_args.size() < MIN_ARGUMENTS || raw_args.size() > MAX_ARGUMENTS) {
        std::cerr << "scada-tag-rt-probe: expected 4 or 5 arguments\n";
        return EXIT_USAGE;
    }
    std::array<std::string_view, MAX_ARGUMENTS> args;
    for (std::size_t i = 0; i < raw_args.size(); ++i) {
        args.at(i) = raw_args[i];
    }
    return run(std::span<const std::string_view>(args).first(raw_args.size()));
}
