// Integration tests for tag read and write with real processes (contract,
// sections 6, 10.1 and 15): what a reader sees when an owner is killed in
// the middle of a write or stops cleanly, and that readers never see a torn
// record while owners write at the reference load and at the maximum rate.
//
// Every wait is on a condition with a limit: a line a process prints, its
// exit, or a read that gives what is waited for.

#include "tag_processes_fixture.h"
#include "tagd_fixture.h"

#include "scada/sim/layout.h"
#include "scada/sim/pattern.h"
#include "scada/tag/scada_tag.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal numbers are declared here, not in <csignal>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using namespace scada::test::tagd;
using scada::test::ChildProcess;
using Clock = std::chrono::steady_clock;

// How long after the kill of an owner a reader may still not see it down:
// its heartbeat timeout, and time for the reading thread to be scheduled
// on a busy machine.
constexpr std::chrono::milliseconds DOWN_LIMIT{IO_TIMEOUT_MS + 1000};

// The reference load of the contract (section 15, test 2): updates per
// second of all owners together.
constexpr std::uint64_t REFERENCE_RATE = 50'000;
// How long the reference load runs unless SCADA_CONFORMANCE_SECONDS says
// otherwise, and the longest it may be asked to run.
constexpr std::uint64_t DEFAULT_LOAD_SECONDS = 3;
constexpr std::uint64_t MAX_LOAD_SECONDS = 3600;
// The rate the simulator must reach, in percent of the rate asked for.
constexpr std::uint64_t RATE_PERCENT = 95;
constexpr std::uint64_t PERCENT = 100;
constexpr std::uint64_t MILLISECONDS_PER_SECOND = 1000;
// Updates of the run at the maximum rate: each tag is written 40 times.
constexpr std::uint64_t MAX_RATE_ROUNDS = 40;
constexpr std::size_t READER_THREADS = 4;
// Tags one reader reads with one call (section 15, test 27).
constexpr std::size_t READ_BATCH = 500;

// Reads the tag `handle` until it reads as BAD with OWNER_DOWN, and
// returns that value; nothing if `limit` passes first.
std::optional<scada_value> read_until_down(scada_ctx* ctx, std::uint32_t handle,
                                           std::chrono::milliseconds limit) {
    const Clock::time_point deadline = Clock::now() + limit;
    while (Clock::now() < deadline) {
        scada_value value{};
        if (scada_tag_read(ctx, handle, &value) == SCADA_OK &&
            (value.quality_flags & SCADA_FLAG_OWNER_DOWN) != 0) {
            return value;
        }
        std::this_thread::yield();
    }
    return std::nullopt;
}

// The number after `key` in `text`, or nothing.
std::optional<std::uint64_t> number_after(std::string_view text,
                                          std::string_view key) {
    const std::size_t position = text.find(key);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view rest = text.substr(position + key.size());
    std::uint64_t value = 0;
    const std::from_chars_result parsed =
        std::from_chars(rest.data(), rest.data() + rest.size(), value);
    return parsed.ec == std::errc{} ? std::optional(value) : std::nullopt;
}

// How long the reference load runs: SCADA_CONFORMANCE_SECONDS if it is set
// to a number from 1 to MAX_LOAD_SECONDS, otherwise DEFAULT_LOAD_SECONDS.
std::uint64_t load_seconds() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing changes the environment while the tests run
    const char* const text = std::getenv("SCADA_CONFORMANCE_SECONDS");
    if (text == nullptr) {
        return DEFAULT_LOAD_SECONDS;
    }
    const std::optional<std::uint64_t> seconds = number_after(text, "");
    if (!seconds.has_value() || *seconds == 0 || *seconds > MAX_LOAD_SECONDS) {
        ADD_FAILURE() << "SCADA_CONFORMANCE_SECONDS is not 1 to "
                      << MAX_LOAD_SECONDS;
        return DEFAULT_LOAD_SECONDS;
    }
    return *seconds;
}

// What one reader thread found.
struct ReaderCounts {
    std::uint64_t reads = 0;
    std::uint64_t busy = 0;
    std::uint64_t errors = 0;     // neither SCADA_OK nor SCADA_E_BUSY
    std::uint64_t torn = 0;       // fields that do not belong to one write
    std::uint64_t down = 0;       // read as OWNER_DOWN while the owner ran
    std::uint64_t backwards = 0;  // a value older than one read before
    std::uint64_t passes = 0;     // times it read every tag
};

// True if the fields of `value` belong to one write of the simulator, or
// the tag was never written.
bool is_one_write(const scada_value& value) {
    return scada::sim::is_check_value(value);
}

// Judges one read of a tag whose value was `last` at the read before, and
// sets `last` to its value now.
void judge(int status, const scada_value& value, std::uint64_t& last,
           ReaderCounts& counts) {
    ++counts.reads;
    if (status != SCADA_OK) {
        ++(status == SCADA_E_BUSY ? counts.busy : counts.errors);
        return;
    }
    if ((value.quality_flags & SCADA_FLAG_OWNER_DOWN) != 0) {
        ++counts.down;
        return;
    }
    if (!is_one_write(value)) {
        ++counts.torn;
    }
    if (value.value < last) {
        ++counts.backwards;
    }
    last = value.value;
}

// One reader: reads all of `handles`, READ_BATCH at a time, again and
// again until `stop` is set, and checks every value.
void read_all_until(scada_ctx* ctx, std::span<const std::uint32_t> handles,
                    const std::atomic<bool>& stop, ReaderCounts& counts) {
    std::vector<std::uint64_t> last(handles.size(), 0);
    std::array<scada_value, READ_BATCH> values{};
    std::array<int, READ_BATCH> status{};
    // Acquire: pairs with the release store of the test.
    while (!stop.load(std::memory_order_acquire)) {
        for (std::size_t first = 0; first < handles.size();
             first += READ_BATCH) {
            const std::size_t count =
                std::min(READ_BATCH, handles.size() - first);
            static_cast<void>(
                scada_tag_read_many(ctx, handles.subspan(first, count).data(),
                                    count, values.data(), status.data()));
            for (std::size_t i = 0; i < count; ++i) {
                judge(status.at(i), values.at(i), last[first + i], counts);
            }
        }
        ++counts.passes;
    }
}

// No reader saw anything but whole writes of a running owner, and each
// read every tag at least once. Records the reads of all readers.
void expect_whole_writes(std::span<const ReaderCounts> counts) {
    ReaderCounts all;
    all.passes = counts.empty() ? 0 : counts.front().passes;
    for (const ReaderCounts& count : counts) {
        all.reads += count.reads;
        all.busy += count.busy;
        all.errors += count.errors;
        all.torn += count.torn;
        all.down += count.down;
        all.backwards += count.backwards;
        all.passes = std::min(all.passes, count.passes);
    }
    EXPECT_EQ(all.torn, 0U);
    EXPECT_EQ(all.backwards, 0U);
    EXPECT_EQ(all.errors, 0U);
    EXPECT_EQ(all.down, 0U);
    EXPECT_GE(all.passes, 1U) << "a reader did not read every tag once";
    ::testing::Test::RecordProperty("reads", std::to_string(all.reads));
    ::testing::Test::RecordProperty("busy", std::to_string(all.busy));
}

// How many of the tags `handles` do not hold write number `rounds`.
std::uint64_t tags_without_last_write(scada_ctx* ctx,
                                      std::span<const std::uint32_t> handles,
                                      std::uint64_t rounds) {
    std::uint64_t wrong = 0;
    for (const std::uint32_t handle : handles) {
        scada_value value{};
        if (scada_tag_read(ctx, handle, &value) != SCADA_OK ||
            value.value != rounds || !is_one_write(value)) {
            ++wrong;
        }
    }
    return wrong;
}

// Kills `owner`, which was left in the middle of a write of the tag
// `level`, and expects the tag to read as down within DOWN_LIMIT, without a
// value.
void expect_down_after_kill(const Attached& reader, std::uint32_t level,
                            ChildProcess& owner) {
    const Clock::time_point killed = Clock::now();
    kill(owner);
    const std::optional<scada_value> down =
        read_until_down(reader.ctx(), level, WAIT_LIMIT);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - killed);
    if (!down.has_value()) {
        ADD_FAILURE() << "the tag of the killed owner never read as down";
        return;
    }
    EXPECT_LE(took, DOWN_LIMIT) << took.count() << " ms";
    ::testing::Test::RecordProperty("down_after_ms",
                                    std::to_string(took.count()));
    EXPECT_EQ(down->type, SCADA_TYPE_FLOAT64);
    EXPECT_EQ(down->quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(down->quality_flags, SCADA_FLAG_OWNER_DOWN);
    EXPECT_EQ(down->value, 0U);
}

// The tag `level` reads with the quality flags `flags`.
void expect_flags(const Attached& reader, std::uint32_t level, int flags) {
    scada_value value{};
    EXPECT_EQ(scada_tag_read(reader.ctx(), level, &value), SCADA_OK);
    EXPECT_EQ(value.quality, SCADA_QUALITY_BAD);
    EXPECT_EQ(value.quality_flags, flags);
}

class TagAccessProcesses : public TagProcesses {
protected:
    // The handles of every tag of the process `load`, as `reader` resolves
    // them.
    [[nodiscard]] static std::vector<std::uint32_t> load_handles(
        const Attached& reader) {
        std::vector<std::uint32_t> handles;
        handles.reserve(LOAD_OWNERS * LOAD_TAGS_PER_OWNER);
        const scada::sim::Layout layout = load_layout();
        for (std::uint32_t owner = 0; owner < layout.owners; ++owner) {
            for (std::uint64_t tag = 0;
                 tag < scada::sim::tags_of_owner(layout, owner); ++tag) {
                handles.push_back(
                    reader.resolve(scada::sim::tag_name(layout, owner, tag)));
            }
        }
        return handles;
    }

    // Runs scada-sim on the large tag set with `per_second` updates per
    // second (0: as fast as it can) until it has written every tag `rounds`
    // times, with READER_THREADS readers in this process, and checks that
    // no reader saw a torn record. Returns the time the simulator took in
    // ms; nothing if something failed before the end.
    [[nodiscard]] std::optional<std::uint64_t> run_load(
        std::uint64_t per_second, std::uint64_t rounds,
        std::chrono::seconds limit) {
        write_load_tag_set();
        std::optional<RunningTagd> tagd = ready_tagd();
        if (!tagd.has_value()) {
            return std::nullopt;
        }
        std::optional<ChildProcess> writer =
            start_sim(load_layout(), {"--rate", std::to_string(per_second),
                                      "--rounds", std::to_string(rounds)});
        if (!writer.has_value()) {
            return std::nullopt;
        }
        // After the simulator attached: its regions are there to be mapped.
        const Attached reader(base(), "reader");
        const std::vector<std::uint32_t> handles = load_handles(reader);

        std::atomic<bool> readers_stop{false};
        std::array<ReaderCounts, READER_THREADS> counts{};
        bool done = false;
        {
            std::vector<std::jthread> readers;
            readers.reserve(counts.size());
            for (ReaderCounts& count : counts) {
                readers.emplace_back([&reader, &handles, &readers_stop,
                                      &count] {
                    read_all_until(reader.ctx(), handles, readers_stop, count);
                });
            }
            done = writer->wait_for_output(
                [](std::string_view out) {
                    return out.find("done ") != std::string_view::npos;
                },
                limit);
            // Release: the readers load with acquire.
            readers_stop.store(true, std::memory_order_release);
        }
        EXPECT_TRUE(done) << writer->out() << writer->err();
        expect_whole_writes(counts);
        EXPECT_EQ(tags_without_last_write(reader.ctx(), handles, rounds), 0U);

        EXPECT_TRUE(stops_cleanly(*writer));
        stop(*tagd);
        return number_after(writer->out(), "elapsed_ms=");
    }
};

// Conformance test 5, the part of the reader, with scada-tagd running.
TEST_F(TagAccessProcesses,
       Conformance05AKilledOwnerReadsAsDownWithinItsTimeout) {
    write_small_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> owner = attached_io({"mid-write"});
    if (!tagd.has_value() || !owner.has_value()) {
        return;
    }
    EXPECT_TRUE(prints(*owner, "mid-write\n"));
    const Attached reader(base(), "reader");
    const std::uint32_t level = reader.resolve("plant.level");

    // The owner is alive and in the middle of a write: no value, and no
    // false alarm.
    scada_value value{};
    EXPECT_EQ(scada_tag_read(reader.ctx(), level, &value), SCADA_E_BUSY);
    expect_down_after_kill(reader, level, *owner);

    // scada-tagd comes to the same conclusion, and the tag stays down.
    EXPECT_TRUE(
        logs(*tagd, R"(process down PROCESS="io" REASON="heartbeat stale")"));
    expect_flags(reader, level, SCADA_FLAG_OWNER_DOWN);
    stop(*tagd);
}

// Conformance test 7, the part of the reader: after the restart of a killed
// owner the reader gets values again without attaching or resolving again.
TEST_F(TagAccessProcesses, Conformance07AReaderRecoversWhenTheOwnerRestarts) {
    write_small_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> owner = attached_io({"mid-write"});
    if (!tagd.has_value() || !owner.has_value()) {
        return;
    }
    EXPECT_TRUE(prints(*owner, "mid-write\n"));
    const Attached reader(base(), "reader");
    const std::uint32_t level = reader.resolve("plant.level");
    kill(*owner);

    std::optional<ChildProcess> again = attached_io();
    if (!again.has_value()) {
        return;
    }
    // The record left in the middle of a write was repaired.
    EXPECT_TRUE(prints(*again, "repaired=1"));
    expect_flags(reader, level, SCADA_FLAG_NOT_INIT);
    EXPECT_TRUE(stops_cleanly(*again));
    stop(*tagd);
}

// Conformance test 5 with scada-tagd stopped: the reader judges the owner
// by itself.
TEST_F(TagAccessProcesses, Conformance05AKilledOwnerReadsAsDownWithoutTagd) {
    write_small_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> owner = attached_io({"mid-write"});
    if (!tagd.has_value() || !owner.has_value()) {
        return;
    }
    EXPECT_TRUE(prints(*owner, "mid-write\n"));
    const Attached reader(base(), "reader");
    const std::uint32_t level = reader.resolve("plant.level");
    stop(*tagd);
    scada_status status{};
    EXPECT_EQ(scada_tag_status(reader.ctx(), &status), SCADA_E_TAGD_DOWN);

    scada_value value{};
    EXPECT_EQ(scada_tag_read(reader.ctx(), level, &value), SCADA_E_BUSY);
    expect_down_after_kill(reader, level, *owner);
}

// Conformance test 6, the part of the reader.
TEST_F(TagAccessProcesses, Conformance06ACleanlyStoppedOwnerReadsAsDownAtOnce) {
    write_small_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> owner = attached_io();
    if (!tagd.has_value() || !owner.has_value()) {
        return;
    }
    const Attached reader(base(), "reader");
    const std::uint32_t level = reader.resolve("plant.level");
    expect_flags(reader, level, SCADA_FLAG_NOT_INIT);

    EXPECT_TRUE(stops_cleanly(*owner));
    // The first read after the owner is gone: no wait for a timeout.
    expect_flags(reader, level, SCADA_FLAG_OWNER_DOWN | SCADA_FLAG_NOT_INIT);

    std::optional<ChildProcess> again = attached_io();
    if (!again.has_value()) {
        return;
    }
    expect_flags(reader, level, SCADA_FLAG_NOT_INIT);
    EXPECT_TRUE(stops_cleanly(*again));
    stop(*tagd);
}

// Conformance test 2: 50,000 tags across 10 owners, 50,000 updates per
// second, 4 concurrent readers, no torn read. It runs for 3 s here; set
// SCADA_CONFORMANCE_SECONDS=600 for the 10 minutes of the contract.
TEST_F(TagAccessProcesses, Conformance02ReferenceLoadWithFourReaders) {
    const std::uint64_t seconds = load_seconds();
    // In range: at most MAX_LOAD_SECONDS.
    const std::chrono::seconds limit(static_cast<std::int64_t>(seconds * 4) +
                                     WAIT_LIMIT.count());
    const std::optional<std::uint64_t> elapsed_ms =
        run_load(REFERENCE_RATE, seconds, limit);
    if (!elapsed_ms.has_value() || *elapsed_ms == 0) {
        FAIL() << "the simulator did not report its time";
    }
    const std::uint64_t updates = seconds * REFERENCE_RATE;
    const std::uint64_t per_second =
        updates * MILLISECONDS_PER_SECOND / *elapsed_ms;
    RecordProperty("updates_per_second", std::to_string(per_second));
    RecordProperty("seconds", std::to_string(seconds));
    EXPECT_GE(per_second * PERCENT, REFERENCE_RATE * RATE_PERCENT)
        << updates << " updates in " << *elapsed_ms << " ms";
}

// Conformance test 3 with real processes: the simulator at its maximum rate.
TEST_F(TagAccessProcesses, Conformance03MaximumWriteRateWithFourReaders) {
    const std::optional<std::uint64_t> elapsed_ms =
        run_load(0, MAX_RATE_ROUNDS, WAIT_LIMIT * 4);
    if (!elapsed_ms.has_value()) {
        FAIL() << "the writer did not report its time";
    }
    RecordProperty("elapsed_ms", std::to_string(*elapsed_ms));
}

// True if CTest says that the library of this build checks the thread of an
// owner.
bool thread_check_is_built_in() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing changes the environment while the tests run
    const char* const checked = std::getenv("SCADA_TAG_THREAD_CHECK");
    return checked != nullptr && std::string_view(checked) == "1";
}

// The library stopped `writer` when its second thread wrote.
void expect_stopped_at_second_thread(ChildProcess& writer) {
    EXPECT_EQ(writer.wait_for_signal_exit(WAIT_LIMIT), SIGABRT)
        << writer.out() << writer.err();
    EXPECT_NE(writer.err().find("written from two threads"), std::string::npos)
        << writer.err();
    EXPECT_EQ(writer.out().find("second-thread written"), std::string::npos);
}

// Contract 13.2: all writes to the tags of one owner come from one thread.
// A library built without NDEBUG stops a process that breaks the rule; one
// built with NDEBUG does not check. CTest says which this build is.
TEST_F(TagAccessProcesses, ASecondWritingThreadStopsADebugBuild) {
    write_load_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    std::optional<ChildProcess> writer = start_helper(
        "SCADA_TAG_WRITER_COMMAND", {base(), "load", "load.w0.t0"});
    if (!writer.has_value()) {
        return;
    }
    if (thread_check_is_built_in()) {
        expect_stopped_at_second_thread(*writer);
    } else {
        EXPECT_EQ(writer->wait_for_exit(WAIT_LIMIT), 0) << writer->err();
        EXPECT_NE(writer->out().find("second-thread written"),
                  std::string::npos);
    }
    stop(*tagd);
}

}  // namespace
