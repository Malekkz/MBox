// Conformance test 27 of the tag database contract (section 15): the
// latency of `scada_tag_read`, `scada_tag_read_many` of 500 tags,
// `scada_tag_write` and `scada_tag_write_many` of 500 tags, median and
// 99.9th percentile, measured while scada-sim writes the reference load
// (50,000 tags, 10 owners, 50,000 updates per second). It also measures
// the scan of contract section 16 (`scada_tag_write_many` of 5,000 tags),
// the memory of each phase 1 process during the load, and the size of the
// database.
//
// Each call is timed alone with CLOCK_MONOTONIC before and after it; the
// time of one clock reading is printed beside the figures, so a reader can
// tell the floor. The figures are printed as `key=value` lines and
// recorded as properties of the test.
//
// It fails on a target, each with its source (decision D-57):
// - the scan of 5,000 tags: median below 0.5 ms (contract section 16, the
//   cost of clock reads in `write`). The median gates on shared machines;
//   the 99.9th percentile is printed beside it, and the target board (issue
//   #41) decides the worst case;
// - the database below 64 MB: the size half of contract section 16, "default
//   change ring capacity". Its other half, no gap for a consumer that keeps
//   up with the reference load, is not gated here: conformance tests 13 and
//   14 check it at smaller loads, and the reference load itself is not
//   checked for gaps yet (decision D-57);
// - scada-tagd, scada-sim and this process together below 1.5 GB: a sanity
//   bound chosen in D-57, not a requirement. Reference 15.2 sets 1.5 GB for
//   all services, of which these are only a part;
// - a median more than SCADA_PERF_TOLERANCE times (2.0) the baseline of
//   this architecture in SCADA_PERF_BASELINE, for each call the baseline
//   names. The 99.9th percentiles are reported only: on shared machines
//   they are the scheduling of the host.

#include "tag_processes_fixture.h"
#include "tagd_fixture.h"

#include "scada/common/result.h"
#include "scada/sim/layout.h"
#include "scada/sim/tag_set.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <time.h>  // NOLINT(modernize-deprecated-headers): clock_gettime is declared here, not in <ctime>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace scada::test::tagd;
using scada::test::ChildProcess;

constexpr std::uint64_t REFERENCE_RATE = 50'000;
// Long enough for every measurement; the simulator is stopped after them.
constexpr std::uint64_t LOAD_ROUNDS = 600;
constexpr const char* BENCH = "bench";
constexpr std::size_t BENCH_TAGS = 5000;
constexpr std::size_t BATCH = 500;
constexpr std::size_t SCAN = 5000;
constexpr std::size_t SINGLE_SAMPLES = 200'000;
constexpr std::size_t BATCH_SAMPLES = 20'000;
constexpr std::size_t SCAN_SAMPLES = 2'000;
constexpr std::size_t CLOCK_SAMPLES = 100'000;
// Calls between two heartbeats of this process, outside the timing.
constexpr std::size_t CALLS_PER_BEAT = 2'000;
// Contract section 16, the cost of clock reads in `write`.
constexpr std::int64_t SCAN_LIMIT_NS = 500'000;
// The size half of contract section 16, "default change ring capacity".
constexpr std::uint64_t DATABASE_LIMIT_BYTES = 64ULL << 20U;
// A sanity bound chosen in D-57, not a requirement (see above).
constexpr std::uint64_t SANITY_MEMORY_BOUND_KB = 1536ULL << 10U;
constexpr double DEFAULT_TOLERANCE = 2.0;
constexpr std::int64_t NS_PER_S = 1'000'000'000;
constexpr std::size_t PER_MILLE = 1000;
constexpr std::size_t P999 = 999;
// The round of a bench write above its slot, so that each write changes the
// value.
constexpr unsigned ROUND_SHIFT = 16;
// The step through the load tags between two reads, so that consecutive
// reads do not hit the same cache line.
constexpr std::size_t READ_STRIDE = 7;

std::int64_t now_ns() {
    timespec now{};
    static_cast<void>(::clock_gettime(CLOCK_MONOTONIC, &now));
    return (std::int64_t{now.tv_sec} * NS_PER_S) + now.tv_nsec;
}

// The value of the environment variable `name`, or empty.
std::string environment(const char* name) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing changes the environment while the tests run
    const char* const text = std::getenv(name);
    return text == nullptr ? std::string() : std::string(text);
}

// Median, 99.9th percentile and maximum of a set of times, in ns.
struct Figures {
    std::size_t samples = 0;
    std::int64_t median = 0;
    std::int64_t p999 = 0;
    std::int64_t max = 0;
};

Figures figures_of(std::vector<std::int64_t>& times) {
    if (times.empty()) {
        return {};
    }
    std::ranges::sort(times);
    return {.samples = times.size(),
            .median = times.at(times.size() / 2),
            .p999 = times.at(times.size() * P999 / PER_MILLE),
            .max = times.back()};
}

// Prints and records `figures` of the call `name`.
void report(const std::string& name, const Figures& figures) {
    std::cout << "latency call=" << name << " samples=" << figures.samples
              << " median_ns=" << figures.median << " p999_ns=" << figures.p999
              << " max_ns=" << figures.max << "\n";
    ::testing::Test::RecordProperty(name + "_median_ns",
                                    std::to_string(figures.median));
    ::testing::Test::RecordProperty(name + "_p999_ns",
                                    std::to_string(figures.p999));
}

// Times `samples` calls of `call`, which is given the number of the call
// and returns its code, and calls `beat` every CALLS_PER_BEAT calls,
// outside the timing. Counts the calls that did not return SCADA_OK.
Figures time_calls(std::size_t samples,
                   const std::function<int(std::size_t)>& call,
                   const std::function<void()>& beat, std::size_t& failed) {
    std::vector<std::int64_t> times(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        if (i % CALLS_PER_BEAT == 0) {
            beat();
        }
        const std::int64_t start = now_ns();
        const int code = call(i);
        times[i] = now_ns() - start;
        failed += code == SCADA_OK ? 0 : 1;
    }
    return figures_of(times);
}

// The time of one clock reading, as the timing above sees it.
Figures clock_floor() {
    std::vector<std::int64_t> times(CLOCK_SAMPLES);
    for (std::int64_t& time : times) {
        const std::int64_t start = now_ns();
        time = now_ns() - start;
    }
    return figures_of(times);
}

// A value of tag `slot` for write number `round`.
scada_value bench_value(std::size_t round, std::size_t slot) {
    scada_value value{};
    value.type = SCADA_TYPE_UINT64;
    value.value = (std::uint64_t{round} << ROUND_SHIFT) + slot;
    return value;
}

// One field of /proc/<pid>/smaps_rollup or /proc/<pid>/status, in kB;
// nothing, with a failed expectation, if the file or the field is missing,
// so that a missing figure cannot pass as 0.
std::optional<std::uint64_t> proc_kb(int pid, const char* file,
                                     std::string_view key) {
    const std::string path = "/proc/" + std::to_string(pid) + "/" + file;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (line.starts_with(key)) {
            std::istringstream fields(line.substr(key.size()));
            std::uint64_t value_kb = 0;
            if (fields >> value_kb) {
                return value_kb;
            }
        }
    }
    ADD_FAILURE() << "no " << key << " in " << path;
    return std::nullopt;
}

// Prints and records the memory of the process `pid`, named `name`, and
// returns its proportional set size in kB; nothing if a figure is missing.
std::optional<std::uint64_t> report_memory(const std::string& name, int pid) {
    const std::optional<std::uint64_t> rss =
        proc_kb(pid, "smaps_rollup", "Rss:");
    const std::optional<std::uint64_t> pss =
        proc_kb(pid, "smaps_rollup", "Pss:");
    const std::optional<std::uint64_t> hwm = proc_kb(pid, "status", "VmHWM:");
    if (!rss.has_value() || !pss.has_value() || !hwm.has_value()) {
        return std::nullopt;
    }
    std::cout << "memory process=" << name << " rss_kb=" << *rss
              << " pss_kb=" << *pss << " hwm_kb=" << *hwm << "\n";
    ::testing::Test::RecordProperty(name + "_pss_kb", std::to_string(*pss));
    ::testing::Test::RecordProperty(name + "_hwm_kb", std::to_string(*hwm));
    return pss;
}

// The bytes of every file under `directory`; nothing, with a failed
// expectation, if it cannot be walked.
std::optional<std::uint64_t> bytes_under(
    const std::filesystem::path& directory) {
    std::uint64_t bytes = 0;
    std::error_code error;
    std::filesystem::recursive_directory_iterator entries(directory, error);
    for (; !error && entries != std::filesystem::recursive_directory_iterator();
         entries.increment(error)) {
        if (entries->is_regular_file(error)) {
            bytes += entries->file_size(error);
        }
    }
    if (error) {
        ADD_FAILURE() << "cannot measure " << directory << ": "
                      << error.message();
        return std::nullopt;
    }
    return bytes;
}

// The calls test 27 measures; a baseline may name only these.
const std::array<std::string_view, 5> CALLS = {
    "read", "read_many_500", "write", "write_many_500", "write_many_5000"};

// The medians of the baseline `path`: lines `<call> <median ns>`, `#`
// starts a comment, empty lines are skipped. Report only, with an empty
// result, when the baseline directory holds no baseline at all (until part
// three of issue #20 adds them). A failed expectation, so that gating
// cannot switch itself off unseen, when the directory holds baselines but
// none for this processor, or the file has a line that is not a known call
// with a positive median.
std::map<std::string, std::int64_t> read_baseline(const std::string& path) {
    std::map<std::string, std::int64_t> medians;
    const std::filesystem::path file(path);
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        bool others = false;
        for (const auto& entry :
             std::filesystem::directory_iterator(file.parent_path(), error)) {
            others = others || entry.path().extension() == ".txt";
        }
        EXPECT_FALSE(others) << "baselines in " << file.parent_path()
                             << ", but none for this processor: " << path;
        return medians;
    }
    std::ifstream input(file);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        std::istringstream fields(line);
        std::string call;
        std::int64_t median = 0;
        std::string rest;
        const bool parsed = static_cast<bool>(fields >> call >> median) &&
                            !(fields >> rest) && median > 0 &&
                            std::ranges::find(CALLS, call) != CALLS.end();
        EXPECT_TRUE(parsed) << path << ": not '<call> <median ns>': " << line;
        if (parsed) {
            medians[call] = median;
        }
    }
    return medians;
}

// SCADA_PERF_TOLERANCE if it is a number above 1, else DEFAULT_TOLERANCE.
double tolerance() {
    const std::string value_text = environment("SCADA_PERF_TOLERANCE");
    const std::string_view text = value_text;
    double value = 0;
    const std::from_chars_result parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && value > 1.0 ? value : DEFAULT_TOLERANCE;
}

// The median of `call` is at most the tolerance times its baseline, if the
// baseline names the call.
void expect_no_regression(const std::map<std::string, std::int64_t>& baseline,
                          const std::string& call, const Figures& figures) {
    const auto found = baseline.find(call);
    if (found == baseline.end()) {
        return;
    }
    const double limit = static_cast<double>(found->second) * tolerance();
    EXPECT_LE(static_cast<double>(figures.median), limit)
        << call << ": median " << figures.median << " ns, baseline "
        << found->second << " ns, tolerance " << tolerance();
}

class Latency : public TagProcesses {
protected:
    // The reference load, with `reader` and the process BENCH, which owns
    // BENCH_TAGS UINT64 tags.
    void write_tag_set() const {
        scada::sim::TagSetOptions options;
        options.layout = load_layout();
        options.uid = ::geteuid();
        options.tagd_uid = ::geteuid();
        options.readers = {"reader"};
        std::string detail;
        scada::Result<scada::tag::TagSet> made =
            scada::sim::make_tag_set(options, detail);
        if (!made.ok()) {
            ADD_FAILURE() << "no tag set: " << detail;
            return;
        }
        scada::tag::TagSet set = std::move(made).value();
        set.processes.push_back({.name = BENCH, .uid = ::geteuid()});
        set.owners.push_back({.name = BENCH, .process = BENCH});
        for (std::size_t tag = 0; tag < BENCH_TAGS; ++tag) {
            set.tags.push_back({.name = bench_tag(tag),
                                .owner = BENCH,
                                .type = scada::tag::TagType::UINT64});
        }
        std::ofstream file(tag_set_path());
        file << scada::sim::tag_set_json(set);
    }

    [[nodiscard]] static std::string bench_tag(std::size_t tag) {
        return std::string(BENCH) + ".t" + std::to_string(tag);
    }

    // The handles of the tags of the simulator and of BENCH, as `bench`
    // resolves them.
    void resolve(const Attached& bench) {
        const scada::sim::Layout layout = load_layout();
        for (std::uint32_t owner = 0; owner < layout.owners; ++owner) {
            for (std::uint64_t tag = 0;
                 tag < scada::sim::tags_of_owner(layout, owner); ++tag) {
                load_.push_back(
                    bench.resolve(scada::sim::tag_name(layout, owner, tag)));
            }
        }
        for (std::size_t tag = 0; tag < BENCH_TAGS; ++tag) {
            own_.push_back(bench.resolve(bench_tag(tag)));
        }
    }

    // Measures every call against the running load; returns the figures
    // by call name.
    [[nodiscard]] std::map<std::string, Figures> measure(scada_ctx* ctx) {
        std::map<std::string, Figures> all;
        std::size_t failed = 0;
        const auto beat = [ctx] {
            static_cast<void>(scada_tag_heartbeat(ctx));
            static_cast<void>(scada_tag_notify(ctx));
        };
        std::array<scada_value, SCAN> values{};
        std::array<int, BATCH> status{};
        all["read"] = time_calls(
            SINGLE_SAMPLES,
            [&](std::size_t call) {
                return scada_tag_read(
                    ctx, load_[(call * READ_STRIDE) % load_.size()],
                    values.data());
            },
            beat, failed);
        all["read_many_500"] = time_calls(
            BATCH_SAMPLES,
            [&](std::size_t call) {
                const std::size_t first = (call * BATCH) % load_.size();
                return scada_tag_read_many(ctx, &load_[first], BATCH,
                                           values.data(), status.data());
            },
            beat, failed);
        all["write"] = time_calls(
            SINGLE_SAMPLES,
            [&](std::size_t call) {
                const scada_value value = bench_value(call, call % own_.size());
                return scada_tag_write(ctx, own_[call % own_.size()], &value);
            },
            beat, failed);
        all["write_many_500"] = time_calls(
            BATCH_SAMPLES,
            [&](std::size_t call) {
                const std::size_t first = (call * BATCH) % own_.size();
                fill(values, call, first, BATCH);
                return scada_tag_write_many(ctx, &own_[first], BATCH,
                                            values.data());
            },
            beat, failed);
        all["write_many_5000"] = time_calls(
            SCAN_SAMPLES,
            [&](std::size_t call) {
                fill(values, call, 0, SCAN);
                return scada_tag_write_many(ctx, own_.data(), SCAN,
                                            values.data());
            },
            beat, failed);
        EXPECT_EQ(failed, 0U);
        return all;
    }

    // Values of write number `round` for the tags `first` to `first + n`.
    static void fill(std::span<scada_value> values, std::size_t round,
                     std::size_t first, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            values[i] = bench_value(round, first + i);
        }
    }

private:
    std::vector<std::uint32_t> load_;
    std::vector<std::uint32_t> own_;
};

// Prints the memory of every process of the run and the size of the
// database, and expects both below their targets.
void report_footprint(const RunningTagd& tagd, const ChildProcess& sim,
                      const std::filesystem::path& base) {
    const std::optional<std::uint64_t> tagd_kb =
        report_memory("scada-tagd", tagd.child.pid());
    const std::optional<std::uint64_t> sim_kb =
        report_memory("scada-sim", sim.pid());
    const std::optional<std::uint64_t> bench_kb =
        report_memory("bench", ::getpid());
    if (tagd_kb.has_value() && sim_kb.has_value() && bench_kb.has_value()) {
        const std::uint64_t pss = *tagd_kb + *sim_kb + *bench_kb;
        EXPECT_LT(pss, SANITY_MEMORY_BOUND_KB) << pss << " kB in all";
    }
    const std::optional<std::uint64_t> database = bytes_under(base);
    if (!database.has_value()) {
        return;
    }
    std::cout << "database bytes=" << *database << "\n";
    ::testing::Test::RecordProperty("database_bytes",
                                    std::to_string(*database));
    EXPECT_GT(*database, 0U);
    EXPECT_LT(*database, DATABASE_LIMIT_BYTES);
}

TEST_F(Latency, Conformance27ReadAndWriteLatencyUnderTheReferenceLoad) {
    write_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        FAIL() << "no tagd";
    }
    std::optional<ChildProcess> sim =
        start_sim(load_layout(), {"--rate", std::to_string(REFERENCE_RATE),
                                  "--rounds", std::to_string(LOAD_ROUNDS)});
    if (!sim.has_value()) {
        FAIL() << "no sim";
    }
    // Measured only once the owner threads write.
    ASSERT_TRUE(prints(*sim, "writing\n"));
    const Attached bench(base(), BENCH);
    resolve(bench);
    report("clock", clock_floor());
    std::map<std::string, Figures> all = measure(bench.ctx());
    report_footprint(*tagd, *sim, base());

    const std::map<std::string, std::int64_t> baseline =
        read_baseline(environment("SCADA_PERF_BASELINE"));
    for (const auto& [call, figures] : all) {
        report(call, figures);
        expect_no_regression(baseline, call, figures);
    }
    EXPECT_LT(all["write_many_5000"].median, SCAN_LIMIT_NS);
    EXPECT_TRUE(stops_cleanly(*sim));
    stop(*tagd);
}

}  // namespace
