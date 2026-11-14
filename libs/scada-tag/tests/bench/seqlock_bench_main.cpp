// scada-tag-seqlock-bench: measures the sequence lock of a tag record
// (contract, sections 6.1, 6.2 and 7) on the machine it runs on, for issue
// #40 (decision D-31). It calls the library's own read and write functions
// on records in plain memory, so it measures the instructions the library
// runs, without processes, regions or scada-tagd. Test code; not installed.
//
// Usage:
//   scada-tag-seqlock-bench timings [<calls per batch>]
//   scada-tag-seqlock-bench load <scalar|string> <tags> <writes per second>
//       <readers> <seconds>
//
// `timings` prints the time of one call, the fastest of BATCHES batches:
// - attempts64_ns: a read of a record that is mid-write for good, that is
//   the 64 attempts with their pauses before a read gives up;
// - scalar_write_ns, string_write_ns: one write of a scalar tag and of a
//   string tag of 250 bytes, the whole call (an upper bound of the time the
//   record is odd);
// - scalar_read_ns, string_read_ns: one read of a record nobody writes.
//
// `load` runs one writer thread over `tags` records of one kind, at `writes
// per second` (0: as fast as it can), and `readers` reader threads that read
// every record in turn, for `seconds`, and prints how many reads gave up
// (what the library reports as SCADA_E_BUSY). Every value is derived from
// the write count, so a torn read is seen; one torn read is exit code 1.
//
// Every line it prints is `key=value` pairs. Exit code 2: wrong arguments.

#include "layout.h"
#include "shared_memory.h"
#include "tag_record.h"

#include "scada/common/cpu_relax.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using namespace scada::tag;
using Clock = std::chrono::steady_clock;

constexpr int EXIT_TORN = 1;
constexpr int EXIT_USAGE = 2;
constexpr std::size_t TIMINGS_ARGUMENTS = 3;
constexpr std::size_t LOAD_ARGUMENTS = 7;

constexpr int BATCHES = 7;
constexpr std::uint64_t DEFAULT_CALLS = 1'000'000;
constexpr std::uint64_t MAX_CALLS = 1'000'000'000;
constexpr std::uint64_t MAX_TAGS = 1'000'000;
constexpr std::uint64_t MAX_RATE = 100'000'000;
constexpr std::uint64_t MAX_READERS = 256;
constexpr std::uint64_t MAX_SECONDS = 86'400;

// An odd sequence number: a record that is mid-write.
constexpr std::uint32_t MID_WRITE = 1;
constexpr std::uint8_t OTHER_BYTE = 0x5A;

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

// What write number `n` of a tag stores. A reader checks that the times
// and the text belong to the same `n`.
RecordUpdate update_for(std::uint64_t n, std::uint64_t value) {
    // In range: the write count stays far below 2^63.
    const auto count = static_cast<std::int64_t>(n);
    return {.quality = 0,
            .quality_flags = 0,
            .value = value,
            .source_time_ns = count,
            .update_time_ns = count,
            .update_mono_ns = count};
}

// The byte every byte of the text of write number `n` holds.
std::uint8_t text_byte(std::uint64_t n) {
    return static_cast<std::uint8_t>(
        n % (std::numeric_limits<std::uint8_t>::max() + 1U));
}

// Nanoseconds per call of `call`, the fastest of BATCHES batches of `calls`.
template <typename Call>
double ns_per_call(std::uint64_t calls, const Call& call) {
    Clock::duration fastest = Clock::duration::max();
    for (int batch = 0; batch < BATCHES; ++batch) {
        const Clock::time_point begin = Clock::now();
        for (std::uint64_t i = 0; i < calls; ++i) {
            call(i);
        }
        fastest = std::min(fastest, Clock::now() - begin);
    }
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(fastest).count();
    return static_cast<double>(nanoseconds) / static_cast<double>(calls);
}

int run_timings(std::uint64_t calls) {
    TagRecord record;
    StringSlot slot;
    RecordCopy copy;
    StringCopy text_copy;
    std::uint64_t failed = 0;

    // A record left mid-write: every read makes all of its attempts.
    store_shared(record.seq, MID_WRITE, std::memory_order_relaxed);
    const double attempts = ns_per_call(calls, [&](std::uint64_t) {
        failed += read_record(record, copy) ? 0U : 1U;
    });
    if (failed != calls * BATCHES) {
        std::cerr << "scada-tag-seqlock-bench: a read of a record mid-write "
                     "succeeded\n";
        return EXIT_TORN;
    }
    store_shared(record.seq, std::uint32_t{0}, std::memory_order_relaxed);

    const double scalar_write = ns_per_call(calls, [&](std::uint64_t call) {
        static_cast<void>(write_record(record, update_for(call, call)));
    });
    const double scalar_read = ns_per_call(calls, [&](std::uint64_t) {
        failed += read_record(record, copy) ? 0U : 1U;
    });

    // Two texts of the full length that differ in the first byte: the
    // comparison before the write stops at once, as for most real writes.
    std::array<std::uint8_t, STRING_SLOT_DATA_SIZE> first{};
    std::array<std::uint8_t, STRING_SLOT_DATA_SIZE> second{};
    first.fill(OTHER_BYTE);
    second.fill(OTHER_BYTE);
    second.front() = 0;
    const double string_write = ns_per_call(calls, [&](std::uint64_t call) {
        static_cast<void>(write_string_record(
            record, slot, call % 2 == 0 ? first : second, update_for(call, 0)));
    });
    const double string_read = ns_per_call(calls, [&](std::uint64_t) {
        failed += read_string_record(record, slot, copy, text_copy) ? 0U : 1U;
    });
    if (failed != calls * BATCHES) {
        std::cerr << "scada-tag-seqlock-bench: a read of a quiet record "
                     "failed\n";
        return EXIT_TORN;
    }

    std::cout << "pause=" << scada::CPU_RELAX_INSTRUCTION << " calls=" << calls
              << " attempts64_ns=" << attempts
              << " scalar_write_ns=" << scalar_write
              << " string_write_ns=" << string_write
              << " scalar_read_ns=" << scalar_read
              << " string_read_ns=" << string_read << "\n";
    return 0;
}

struct LoadPlan {
    bool strings = false;
    std::uint64_t tags = 0;
    std::uint64_t per_second = 0;  // 0: as fast as it can
    std::uint64_t readers = 0;
    std::uint64_t seconds = 0;
};

// The records of a load, one string slot for each in a string load.
struct Records {
    std::vector<TagRecord> records;
    std::vector<StringSlot> slots;
};

// What one reader thread found.
struct ReaderCounts {
    std::uint64_t reads = 0;
    std::uint64_t busy = 0;
    std::uint64_t torn = 0;
};

// True if `copy` (and `text` in a string load) belong to one write, or to
// none yet.
bool is_one_write(const RecordCopy& copy, const StringCopy* text) {
    const auto count = static_cast<std::uint64_t>(copy.source_time_ns);
    if (copy.update_time_ns != copy.source_time_ns ||
        copy.update_mono_ns != copy.source_time_ns) {
        return false;
    }
    if (text == nullptr) {
        return copy.value == count;
    }
    if (count == 0) {
        return text->len == 0;
    }
    return text->len == STRING_SLOT_DATA_SIZE &&
           std::ranges::all_of(text->data, [count](std::uint8_t byte) {
               return byte == text_byte(count);
           });
}

// The writer: write number n goes to record n % tags, at the planned rate,
// until `stop` is set. Returns the number of writes.
std::uint64_t write_until(const LoadPlan& plan, Records& records,
                          const std::atomic<bool>& stop) {
    std::array<std::uint8_t, STRING_SLOT_DATA_SIZE> text{};
    std::uint64_t count = 0;
    const Clock::time_point start = Clock::now();
    // Acquire: pairs with the release store of the main thread.
    while (!stop.load(std::memory_order_acquire)) {
        if (plan.per_second != 0) {
            // Paced by the clock, outside the lock: the writer is never
            // ahead of its rate.
            const std::chrono::duration<double> elapsed = Clock::now() - start;
            if (static_cast<double>(count) >=
                elapsed.count() * static_cast<double>(plan.per_second)) {
                continue;
            }
        }
        ++count;
        const std::size_t tag = count % plan.tags;
        if (plan.strings) {
            text.fill(text_byte(count));
            static_cast<void>(write_string_record(records.records[tag],
                                                  records.slots[tag], text,
                                                  update_for(count, tag)));
        } else {
            static_cast<void>(
                write_record(records.records[tag], update_for(count, count)));
        }
    }
    return count;
}

// A reader: reads every record in turn until `stop` is set.
void read_until(const LoadPlan& plan, Records& records,
                const std::atomic<bool>& stop, ReaderCounts& counts) {
    RecordCopy copy;
    StringCopy text;
    // Acquire: pairs with the release store of the main thread.
    while (!stop.load(std::memory_order_acquire)) {
        for (std::size_t tag = 0; tag < plan.tags; ++tag) {
            ++counts.reads;
            const bool read =
                plan.strings
                    ? read_string_record(records.records[tag],
                                         records.slots[tag], copy, text)
                    : read_record(records.records[tag], copy);
            if (!read) {
                ++counts.busy;
            } else if (!is_one_write(copy, plan.strings ? &text : nullptr)) {
                ++counts.torn;
            }
        }
    }
}

int run_load(const LoadPlan& plan) {
    Records records;
    records.records.resize(plan.tags);
    if (plan.strings) {
        records.slots.resize(plan.tags);
    }
    std::vector<ReaderCounts> counts(plan.readers);
    std::atomic<bool> stop{false};
    std::uint64_t writes = 0;
    Clock::duration took{};
    {
        std::vector<std::jthread> threads;
        threads.reserve(plan.readers + 1);
        for (ReaderCounts& count : counts) {
            threads.emplace_back([&plan, &records, &stop, &count] {
                read_until(plan, records, stop, count);
            });
        }
        const Clock::time_point start = Clock::now();
        threads.emplace_back([&plan, &records, &stop, &writes] {
            writes = write_until(plan, records, stop);
        });
        // The run lasts a fixed time; nothing is waited for.
        std::this_thread::sleep_for(
            std::chrono::seconds(static_cast<std::int64_t>(plan.seconds)));
        // Release: the threads load with acquire.
        stop.store(true, std::memory_order_release);
        threads.clear();
        took = Clock::now() - start;
    }

    ReaderCounts all;
    for (const ReaderCounts& count : counts) {
        all.reads += count.reads;
        all.busy += count.busy;
        all.torn += count.torn;
    }
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(took).count();
    std::cout << "pause=" << scada::CPU_RELAX_INSTRUCTION
              << " kind=" << (plan.strings ? "string" : "scalar")
              << " tags=" << plan.tags << " rate=" << plan.per_second
              << " readers=" << plan.readers << " elapsed_ms=" << elapsed_ms
              << " writes=" << writes << " reads=" << all.reads
              << " busy=" << all.busy << " torn=" << all.torn;
    if (all.busy != 0) {
        std::cout << " reads_per_busy=" << all.reads / all.busy;
    }
    std::cout << "\n";
    return all.torn == 0 ? 0 : EXIT_TORN;
}

int usage() {
    std::cerr << "usage: scada-tag-seqlock-bench timings [<calls per batch>]\n"
                 "       scada-tag-seqlock-bench load <scalar|string> <tags> "
                 "<writes per second> <readers> <seconds>\n";
    return EXIT_USAGE;
}

int run(std::span<const std::string_view> args) {
    if (args.size() >= 2 && args[1] == "timings" &&
        args.size() <= TIMINGS_ARGUMENTS) {
        const std::optional<std::uint64_t> calls =
            args.size() == TIMINGS_ARGUMENTS ? parse_number(args[2], MAX_CALLS)
                                             : std::optional(DEFAULT_CALLS);
        if (!calls.has_value() || *calls == 0) {
            return usage();
        }
        return run_timings(*calls);
    }
    if (args.size() != LOAD_ARGUMENTS || args[1] != "load" ||
        (args[2] != "scalar" && args[2] != "string")) {
        return usage();
    }
    const std::optional<std::uint64_t> tags = parse_number(args[3], MAX_TAGS);
    const std::optional<std::uint64_t> rate = parse_number(args[4], MAX_RATE);
    const std::optional<std::uint64_t> readers =
        parse_number(args[5], MAX_READERS);
    const std::optional<std::uint64_t> seconds =
        parse_number(args[6], MAX_SECONDS);
    if (!tags || *tags == 0 || !rate || !readers || !seconds || *seconds == 0) {
        return usage();
    }
    return run_load({.strings = args[2] == "string",
                     .tags = *tags,
                     .per_second = *rate,
                     .readers = *readers,
                     .seconds = *seconds});
}

}  // namespace

int main(int argc, char* argv[]) {
    // argv is the one C array the language hands us; wrap it with its length
    // at once so that nothing below indexes a raw pointer.
    const std::span<char*> raw_args(
        argv, argc > 0 ? static_cast<std::size_t>(argc) : 0);
    if (raw_args.size() > LOAD_ARGUMENTS) {
        return usage();
    }
    std::array<std::string_view, LOAD_ARGUMENTS> args;
    for (std::size_t i = 0; i < raw_args.size(); ++i) {
        args.at(i) = raw_args[i];
    }
    return run(std::span<const std::string_view>(args).first(raw_args.size()));
}
