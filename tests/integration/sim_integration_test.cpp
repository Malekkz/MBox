// Integration tests for scada-sim, the simulator writer (issue P1-10), with
// real processes: the tag-set file it writes is the one scada-tagd runs on;
// every pattern gives the values its definition promises; several simulator
// processes share one simulation; a run stops on a signal; and a tag set
// that does not fit the run is refused.
//
// Every wait is on a condition with a limit.

#include "tag_processes_fixture.h"
#include "tagd_fixture.h"

#include "scada/sim/layout.h"
#include "scada/sim/pattern.h"
#include "scada/sim/sim.h"
#include "scada/tag/scada_tag.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <bit>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace scada::test::tagd;
using scada::sim::Layout;
using scada::sim::Pattern;
using scada::test::ChildProcess;
using Clock = std::chrono::steady_clock;

constexpr std::uint64_t ROUNDS = 7;
constexpr double AMPLITUDE = 3.0;
constexpr std::uint64_t PERIOD = 5;
constexpr std::uint64_t SEED = 11;
// Not a multiple of the owners or patterns, so that the shares differ.
constexpr std::uint64_t ALL_PATTERN_TAGS = 23;
constexpr std::uint64_t SMALL_TAGS = 10;
// Tags that take far longer than SHORT_TIMEOUT_MS to resolve: about 0.5 s
// in a dev build.
constexpr std::uint64_t MANY_TAGS = 100'000;
constexpr const char* SHORT_TIMEOUT_MS = "100";
constexpr const char* SHORT_CHECK_MS = "20";

// Every pattern, on two owner threads.
Layout all_patterns() {
    return Layout{.prefix = "sim",
                  .processes = 1,
                  .owners = 2,
                  .tags = ALL_PATTERN_TAGS,
                  .patterns = {Pattern::CHECK, Pattern::RAMP, Pattern::SINE,
                               Pattern::STEP, Pattern::WALK}};
}

// The waveform options of every run here.
std::vector<std::string> waveform_options() {
    return {"--amplitude",          "3",      "--period",
            std::to_string(PERIOD), "--seed", std::to_string(SEED)};
}

bool done(std::string_view out) {
    return out.find("done updates=") != std::string_view::npos;
}

// What tag `tag` of any owner holds after `rounds` writes.
scada_value expected_value(const Layout& layout, std::uint64_t tag,
                           std::uint64_t rounds) {
    const Pattern pattern = scada::sim::pattern_of(layout, tag);
    if (pattern == Pattern::CHECK) {
        scada_value value = scada::sim::check_value(rounds);
        value.change_count = static_cast<std::uint32_t>(rounds);
        return value;
    }
    const scada::sim::Waveform waveform{
        .amplitude = AMPLITUDE, .period = PERIOD, .seed = SEED};
    double wave = 0.0;
    std::uint32_t changes = 0;
    for (std::uint64_t count = 1; count <= rounds; ++count) {
        const double next =
            scada::sim::wave_value(pattern, waveform, tag, count, wave);
        // The first write changes quality and flags of a new record.
        if (count == 1 || std::bit_cast<std::uint64_t>(next) !=
                              std::bit_cast<std::uint64_t>(wave)) {
            ++changes;
        }
        wave = next;
    }
    scada_value value{};
    value.type = SCADA_TYPE_FLOAT64;
    value.quality = SCADA_QUALITY_GOOD;
    // A write without a source time gets the update time and this flag.
    value.quality_flags = SCADA_FLAG_LOCAL_TIME;
    value.value = std::bit_cast<std::uint64_t>(wave);
    value.change_count = changes;
    return value;
}

// Every tag of the owners of `layout` holds what `rounds` writes give.
::testing::AssertionResult holds_rounds(const Attached& reader,
                                        const Layout& layout,
                                        std::uint64_t rounds) {
    for (std::uint32_t owner = 0; owner < layout.owners; ++owner) {
        for (std::uint64_t tag = 0;
             tag < scada::sim::tags_of_owner(layout, owner); ++tag) {
            const std::string name = scada::sim::tag_name(layout, owner, tag);
            scada_value value{};
            if (scada_tag_read(reader.ctx(), reader.resolve(name), &value) !=
                SCADA_OK) {
                return ::testing::AssertionFailure() << name << " not read";
            }
            const scada_value expected = expected_value(layout, tag, rounds);
            if (value.type != expected.type ||
                value.quality != expected.quality ||
                value.quality_flags != expected.quality_flags ||
                value.value != expected.value ||
                value.change_count != expected.change_count) {
                return ::testing::AssertionFailure()
                       << name << ": value " << value.value << " quality "
                       << int{value.quality} << " flags " << value.quality_flags
                       << " changes " << value.change_count << ", expected "
                       << expected.value << " " << int{expected.quality} << " "
                       << expected.quality_flags << " "
                       << expected.change_count;
            }
            if (expected.type == SCADA_TYPE_UINT64 &&
                expected.source_time_ns != value.source_time_ns) {
                return ::testing::AssertionFailure()
                       << name << ": source time " << value.source_time_ns;
            }
        }
    }
    return ::testing::AssertionSuccess();
}

// Reads `handle` until it has been written once, or WAIT_LIMIT passed, and
// returns what it read last.
scada_value read_once_written(const Attached& reader, std::uint32_t handle) {
    const Clock::time_point deadline = Clock::now() + WAIT_LIMIT;
    scada_value value{};
    while (Clock::now() < deadline &&
           (scada_tag_read(reader.ctx(), handle, &value) != SCADA_OK ||
            value.value == 0)) {
        std::this_thread::yield();
    }
    return value;
}

class SimProcesses : public TagProcesses {
protected:
    // Runs `scada-sim <arguments>` to its end and returns its exit code.
    [[nodiscard]] static std::optional<int> sim_exit(
        const std::vector<std::string>& arguments, std::string* err = nullptr) {
        std::optional<ChildProcess> sim =
            start_helper("SCADA_SIM_COMMAND", arguments);
        if (!sim.has_value()) {
            return std::nullopt;
        }
        const std::optional<int> code = sim->wait_for_exit(WAIT_LIMIT);
        if (err != nullptr) {
            *err = sim->err();
        }
        return code;
    }
};

// The file of `scada-sim tag-set` is what scada-tagd runs on, and every
// pattern writes the values its definition gives.
TEST_F(SimProcesses, TheTagSetCommandAndEveryPattern) {
    const Layout layout = all_patterns();
    std::vector<std::string> arguments = sim_arguments("tag-set", layout);
    const std::string uid = std::to_string(::geteuid());
    arguments.insert(arguments.end(), {"--uid", uid, "--reader", "reader",
                                       "--output", tag_set_path()});
    EXPECT_EQ(sim_exit(arguments), scada::sim::EXIT_OK);
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    std::vector<std::string> extra{"--rate", "0", "--rounds",
                                   std::to_string(ROUNDS)};
    const std::vector<std::string> waveform = waveform_options();
    extra.insert(extra.end(), waveform.begin(), waveform.end());
    std::optional<ChildProcess> sim = start_sim(layout, extra);
    if (!sim.has_value()) {
        return;
    }
    ASSERT_TRUE(sim->wait_for_output(done, WAIT_LIMIT)) << sim->err();
    EXPECT_NE(sim->out().find(
                  "done updates=" + std::to_string(ROUNDS * layout.tags) + " "),
              std::string::npos)
        << sim->out();
    const Attached reader(base(), "reader");
    EXPECT_TRUE(holds_rounds(reader, layout, ROUNDS));
    EXPECT_TRUE(stops_cleanly(*sim));
    stop(*tagd);
}

// Several simulator processes of one layout, each with several owners.
TEST_F(SimProcesses, SeveralProcessesShareOneSimulation) {
    const Layout layout{.prefix = "sim",
                        .processes = 2,
                        .owners = 4,
                        .tags = 40,
                        .patterns = {Pattern::CHECK, Pattern::RAMP}};
    write_sim_tag_set(layout);
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    std::vector<std::string> extra{"--rate", "4000", "--rounds",
                                   std::to_string(ROUNDS)};
    const std::vector<std::string> waveform = waveform_options();
    extra.insert(extra.end(), waveform.begin(), waveform.end());
    std::optional<ChildProcess> first = start_sim(layout, extra, 0);
    std::optional<ChildProcess> second = start_sim(layout, extra, 1);
    if (!first.has_value() || !second.has_value()) {
        return;
    }
    EXPECT_TRUE(first->wait_for_output(done, WAIT_LIMIT)) << first->err();
    EXPECT_TRUE(second->wait_for_output(done, WAIT_LIMIT)) << second->err();
    const Attached reader(base(), "reader");
    EXPECT_TRUE(holds_rounds(reader, layout, ROUNDS));
    EXPECT_TRUE(stops_cleanly(*first));
    EXPECT_TRUE(stops_cleanly(*second));
    stop(*tagd);
}

// A run bounded by time writes the updates of that time at the rate.
TEST_F(SimProcesses, ARunOfOneSecondWritesTheRateOnce) {
    const Layout layout{.prefix = "sim",
                        .processes = 1,
                        .owners = 2,
                        .tags = 10,
                        .patterns = {Pattern::SINE}};
    write_sim_tag_set(layout);
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    std::optional<ChildProcess> sim =
        start_sim(layout, {"--rate", "1000", "--seconds", "1"});
    if (!sim.has_value()) {
        return;
    }
    EXPECT_TRUE(sim->wait_for_output(done, WAIT_LIMIT)) << sim->err();
    EXPECT_NE(sim->out().find("done updates=1000 "), std::string::npos)
        << sim->out();
    EXPECT_TRUE(stops_cleanly(*sim));
    stop(*tagd);
}

// A run without a bound writes until SIGTERM, reports what it wrote and
// stops cleanly; its tags then read as down at once.
TEST_F(SimProcesses, AnUnboundedRunStopsOnASignal) {
    const Layout layout{.prefix = "sim",
                        .processes = 1,
                        .owners = 1,
                        .tags = SMALL_TAGS,
                        .patterns = {Pattern::CHECK}};
    write_sim_tag_set(layout);
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    std::optional<ChildProcess> sim = start_sim(layout, {"--rate", "1000"});
    if (!sim.has_value()) {
        return;
    }
    const Attached reader(base(), "reader");
    const std::uint32_t handle =
        reader.resolve(scada::sim::tag_name(layout, 0, SMALL_TAGS - 1));
    const scada_value value = read_once_written(reader, handle);
    EXPECT_TRUE(value.value > 0 && scada::sim::is_check_value(value));
    EXPECT_TRUE(stops_cleanly(*sim));
    // It stopped before it was done.
    EXPECT_TRUE(sim->out().find("stopped updates=") != std::string::npos &&
                sim->out().find("done ") == std::string::npos)
        << sim->out();
    scada_value after{};
    EXPECT_EQ(scada_tag_read(reader.ctx(), handle, &after), SCADA_OK);
    EXPECT_NE(after.quality_flags & SCADA_FLAG_OWNER_DOWN, 0);
    stop(*tagd);
}

// Resolving many tags takes longer than a short heartbeat timeout; the
// simulator writes its heartbeat meanwhile, so scada-tagd never sees it
// down while it starts.
TEST_F(SimProcesses, ALargeLayoutDoesNotReadAsDownWhileItStarts) {
    const Layout layout{.prefix = "sim",
                        .processes = 1,
                        .owners = 1,
                        .tags = MANY_TAGS,
                        .patterns = {Pattern::CHECK}};
    std::vector<std::string> arguments = sim_arguments("tag-set", layout);
    arguments.insert(
        arguments.end(),
        {"--heartbeat-timeout-ms", SHORT_TIMEOUT_MS, "--heartbeat-check-ms",
         SHORT_CHECK_MS, "--reader", "reader", "--output", tag_set_path()});
    EXPECT_EQ(sim_exit(arguments), scada::sim::EXIT_OK);
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    std::optional<ChildProcess> sim = start_sim(layout, {"--rate", "1000"});
    if (!sim.has_value()) {
        return;
    }
    EXPECT_TRUE(prints(*sim, "writing\n"));
    EXPECT_TRUE(logs(*tagd, R"(process up PROCESS="sim")"));
    EXPECT_TRUE(stops_cleanly(*sim));
    stop(*tagd);
    EXPECT_EQ(tagd->child.err().find("heartbeat stale"), std::string::npos)
        << tagd->child.err();
}

// A run whose patterns do not give the types of the tag set is refused
// before anything is written; so is a process the tag set does not have.
TEST_F(SimProcesses, RefusesATagSetThatDoesNotFitTheRun) {
    Layout layout = all_patterns();
    layout.patterns = {Pattern::CHECK};
    write_sim_tag_set(layout);
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    std::vector<std::string> arguments = sim_arguments("run", all_patterns());
    arguments.insert(arguments.end(),
                     {"--base-dir", base(), "--tagd-uid",
                      std::to_string(::geteuid()), "--rounds", "1"});
    std::string err;
    EXPECT_EQ(sim_exit(arguments, &err), scada::sim::EXIT_ERROR);
    EXPECT_NE(err.find("not of the type of the pattern ramp"),
              std::string::npos)
        << err;

    layout.prefix = "other";
    arguments = sim_arguments("run", layout);
    arguments.insert(arguments.end(), {"--base-dir", base(), "--tagd-uid",
                                       std::to_string(::geteuid())});
    EXPECT_EQ(sim_exit(arguments, &err), scada::sim::EXIT_ERROR);
    EXPECT_NE(err.find("attach as other failed"), std::string::npos) << err;
    stop(*tagd);
}

TEST_F(SimProcesses, WrongArgumentsAreAUsageError) {
    std::string err;
    EXPECT_EQ(sim_exit({"run", "--rate", "fast"}, &err),
              scada::sim::EXIT_USAGE);
    EXPECT_NE(err.find("--rate"), std::string::npos) << err;
    EXPECT_EQ(sim_exit({"--help"}), scada::sim::EXIT_OK);
}

}  // namespace
