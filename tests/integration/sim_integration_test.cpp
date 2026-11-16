// Integration tests for scada-sim, the simulator writer (issue P1-10), with
// real processes: the tag-set file it writes is the one scada-tagd runs on;
// every pattern gives the values its definition promises; several simulator
// processes share one simulation; a run stops on a signal; a run started
// with --hold writes only once released; and a tag set that does not fit
// the run is refused.
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

#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal numbers are declared here, not in <csignal>
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
constexpr std::uint64_t FIRST_RUN_ROUNDS = 5;
constexpr std::uint64_t SECOND_RUN_ROUNDS = 2;
constexpr std::uint32_t TEN_OWNERS = 10;
// Tags that take far longer than SHORT_TIMEOUT_MS to resolve: about 0.2 s
// in a dev build, so that the test fails every time without the heartbeat
// while resolving (20,000 failed in one of three runs); scada-tagd builds
// their header well within its readiness limit under ThreadSanitizer.
constexpr std::uint64_t MANY_TAGS = 40'000;
constexpr const char* SHORT_TIMEOUT_MS = "100";
constexpr const char* SHORT_CHECK_MS = "20";
// An overloaded run: 2 owners of 1,000 tags at the largest rate, stopped
// once every tag of the first owner has been written OVERLOAD_ROUNDS times
// (about 0.3 s in a dev build, several seconds under ThreadSanitizer).
constexpr std::uint64_t OVERLOAD_TAGS = 2000;
constexpr std::uint64_t OVERLOAD_ROUNDS = 500;
// How long after SIGTERM an overloaded simulator may take to exit.
constexpr std::chrono::milliseconds STOP_LIMIT{1000};

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
// returns what it read last. The status call lets the library look again
// for the owner's regions if they were not there yet when it resolved.
scada_value read_once_written(const Attached& reader, std::uint32_t handle) {
    const Clock::time_point deadline = Clock::now() + WAIT_LIMIT;
    scada_value value{};
    while (Clock::now() < deadline &&
           (scada_tag_read(reader.ctx(), handle, &value) != SCADA_OK ||
            value.value == 0)) {
        scada_status status{};
        // Only the side effect is wanted; scada-tagd may be down.
        static_cast<void>(scada_tag_status(reader.ctx(), &status));
        std::this_thread::yield();
    }
    return value;
}

// Reads `handle` until its value is at least `count`, for at most
// WAIT_LIMIT.
::testing::AssertionResult reads_at_least(const Attached& reader,
                                          std::uint32_t handle,
                                          std::uint64_t count) {
    const Clock::time_point deadline = Clock::now() + WAIT_LIMIT;
    scada_value value{};
    while (Clock::now() < deadline) {
        if (scada_tag_read(reader.ctx(), handle, &value) == SCADA_OK &&
            value.value >= count) {
            return ::testing::AssertionSuccess();
        }
        std::this_thread::yield();
    }
    return ::testing::AssertionFailure() << "reached only " << value.value;
}

class SimProcesses : public TagProcesses {
protected:
    // Runs the simulator of `layout` for `rounds` rounds as fast as it can
    // and stops it.
    [[nodiscard]] ::testing::AssertionResult runs_to_the_end(
        const Layout& layout, std::uint64_t rounds) const {
        std::optional<ChildProcess> sim = start_sim(
            layout, {"--rate", "0", "--rounds", std::to_string(rounds)});
        if (!sim.has_value()) {
            return ::testing::AssertionFailure() << "not started";
        }
        if (!sim->wait_for_output(done, WAIT_LIMIT)) {
            return ::testing::AssertionFailure() << sim->err();
        }
        return stops_cleanly(*sim);
    }

    // Starts the simulator of `layout` with --hold and `extra`, and waits
    // until it holds. Nothing, with a failed expectation, if it does not.
    [[nodiscard]] std::optional<ChildProcess> start_held_sim(
        const Layout& layout, std::vector<std::string> extra = {}) const {
        extra.insert(extra.begin(), "--hold");
        std::optional<ChildProcess> sim = start_sim(layout, extra);
        if (sim.has_value() && !prints(*sim, "ready\n")) {
            ADD_FAILURE() << "scada-sim did not hold";
            return std::nullopt;
        }
        return sim;
    }

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

// A simulator that attaches again to the regions it wrote goes on from the
// write count of each tag: after runs of 5, 2 and 1 rounds, every CHECK tag
// is a whole value of write 8, change count included.
TEST_F(SimProcesses, ARestartedSimulatorGoesOnCounting) {
    const Layout layout{.prefix = "sim",
                        .processes = 1,
                        .owners = 2,
                        .tags = SMALL_TAGS,
                        .patterns = {Pattern::CHECK}};
    write_sim_tag_set(layout);
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    EXPECT_TRUE(runs_to_the_end(layout, FIRST_RUN_ROUNDS));
    EXPECT_TRUE(runs_to_the_end(layout, SECOND_RUN_ROUNDS));
    std::optional<ChildProcess> last =
        start_sim(layout, {"--rate", "0", "--rounds", "1"});
    if (!last.has_value()) {
        return;
    }
    EXPECT_TRUE(last->wait_for_output(done, WAIT_LIMIT)) << last->err();
    const Attached reader(base(), "reader");
    EXPECT_TRUE(
        holds_rounds(reader, layout, FIRST_RUN_ROUNDS + SECOND_RUN_ROUNDS + 1));
    EXPECT_TRUE(stops_cleanly(*last));
    stop(*tagd);
}

// A run bounded by time shares the updates of the whole simulation among
// the owners without losing any to rounding: 1 update across 10 owners.
TEST_F(SimProcesses, ARunBoundedByTimeLosesNoUpdateToRounding) {
    const Layout layout{.prefix = "sim",
                        .processes = 1,
                        .owners = TEN_OWNERS,
                        .tags = TEN_OWNERS,
                        .patterns = {Pattern::CHECK}};
    write_sim_tag_set(layout);
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    std::optional<ChildProcess> sim =
        start_sim(layout, {"--rate", "1", "--seconds", "1"});
    if (!sim.has_value()) {
        return;
    }
    EXPECT_TRUE(sim->wait_for_output(done, WAIT_LIMIT)) << sim->err();
    EXPECT_NE(sim->out().find("done updates=1 "), std::string::npos)
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

// Asked for more than the machine can write, a paced simulator falls
// further behind with every tick. It still stops within STOP_LIMIT of
// SIGTERM, keeps its heartbeat, and is never seen down while it runs.
TEST_F(SimProcesses, AnOverloadedRunStopsPromptly) {
    const Layout layout{.prefix = "sim",
                        .processes = 1,
                        .owners = 2,
                        .tags = OVERLOAD_TAGS,
                        .patterns = {Pattern::CHECK}};
    write_sim_tag_set(layout);
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    std::optional<ChildProcess> sim =
        start_sim(layout, {"--rate", std::to_string(scada::sim::MAX_RATE)});
    if (!sim.has_value()) {
        return;
    }
    // Waits until the backlog is large: every tag written OVERLOAD_ROUNDS
    // times.
    const Attached reader(base(), "reader");
    const std::uint32_t handle =
        reader.resolve(scada::sim::tag_name(layout, 0, 0));
    EXPECT_TRUE(reads_at_least(reader, handle, OVERLOAD_ROUNDS));
    const Clock::time_point signalled = Clock::now();
    ASSERT_TRUE(sim->send_signal(SIGTERM));
    EXPECT_EQ(sim->wait_for_exit(WAIT_LIMIT), scada::sim::EXIT_OK)
        << sim->err();
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - signalled);
    EXPECT_LE(took, STOP_LIMIT) << took.count() << " ms";
    RecordProperty("stop_ms", std::to_string(took.count()));
    stop(*tagd);
    EXPECT_EQ(
        tagd->child.err().find(R"(PROCESS="sim" REASON="heartbeat stale")"),
        std::string::npos)
        << tagd->child.err();
}

// SIGHUP, as when the terminal of a run by hand closes, stops a simulator
// as SIGTERM does.
TEST_F(SimProcesses, SighupStopsCleanly) {
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
    ASSERT_TRUE(sim->send_signal(SIGHUP));
    EXPECT_EQ(sim->wait_for_exit(WAIT_LIMIT), scada::sim::EXIT_OK)
        << sim->err();
    EXPECT_NE(sim->out().find("stopped updates="), std::string::npos)
        << sim->out();
    stop(*tagd);
}

// A simulator left behind, its reader gone and scada-tagd stopped, does not
// write on until it is killed: writing to the closed standard output does
// not kill it (no SIGPIPE), and it exits with an error once scada-tagd has
// been down for 5 s.
TEST_F(SimProcesses, ALeftBehindSimulatorStopsWhenTagdStaysDown) {
    const Layout layout{.prefix = "sim",
                        .processes = 1,
                        .owners = 1,
                        .tags = SMALL_TAGS,
                        .patterns = {Pattern::CHECK}};
    std::vector<std::string> arguments = sim_arguments("tag-set", layout);
    arguments.insert(arguments.end(),
                     {"--heartbeat-timeout-ms", SHORT_TIMEOUT_MS, "--reader",
                      "reader", "--output", tag_set_path()});
    EXPECT_EQ(sim_exit(arguments), scada::sim::EXIT_OK);
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    // The shell runs the simulator with its standard output into a reader
    // that exits at once, and reports the simulator's exit code on the
    // standard error stream.
    std::vector<std::string> run = sim_arguments("run", layout);
    run.insert(run.end(), {"--rate", "1000", "--base-dir", base(), "--tagd-uid",
                           std::to_string(::geteuid())});
    std::vector<std::string> command{
        "sh", "-c", R"(( "$@"; echo "sim exit $?" >&2 ) | true)", "sh"};
    const std::vector<std::string> sim = command_from("SCADA_SIM_COMMAND", run);
    command.insert(command.end(), sim.begin(), sim.end());
    std::optional<ChildProcess> shell = ChildProcess::start(command);
    if (!shell.has_value()) {
        FAIL() << "could not start sh";
    }
    // Once the simulator writes, its output reader is long gone.
    const Attached reader(base(), "reader");
    const scada_value value = read_once_written(
        reader, reader.resolve(scada::sim::tag_name(layout, 0, 0)));
    EXPECT_GT(value.value, 0U);
    stop(*tagd);
    EXPECT_EQ(shell->wait_for_exit(WAIT_LIMIT), 0) << shell->err();
    EXPECT_NE(shell->err().find("sim exit 1"), std::string::npos)
        << shell->err();
    EXPECT_NE(shell->err().find("scada-tagd has been down"), std::string::npos)
        << shell->err();
}

// The heartbeat count of the process `index`, as `reader` sees it; 0 if it
// cannot be read.
std::uint64_t heartbeat_of(const Attached& reader, std::uint16_t index) {
    scada_proc_status_t status{};
    if (scada_proc_status(reader.ctx(), index, &status) != SCADA_OK) {
        return 0;
    }
    return status.heartbeat;
}

// Heartbeats a held simulator must write while the test looks.
constexpr std::uint64_t HELD_BEATS = 3;

// The process `index` writes `beats` more heartbeats within WAIT_LIMIT.
::testing::AssertionResult beats_on(const Attached& reader, std::uint16_t index,
                                    std::uint64_t beats) {
    const std::uint64_t first = heartbeat_of(reader, index);
    const Clock::time_point deadline = Clock::now() + WAIT_LIMIT;
    while (heartbeat_of(reader, index) < first + beats) {
        if (Clock::now() >= deadline) {
            return ::testing::AssertionFailure()
                   << "heartbeat " << heartbeat_of(reader, index) << ", wanted "
                   << first + beats;
        }
        std::this_thread::yield();
    }
    return ::testing::AssertionSuccess();
}

// No tag of the owners of `layout` has been written.
::testing::AssertionResult no_tag_written(const Attached& reader,
                                          const Layout& layout) {
    for (std::uint32_t owner = 0; owner < layout.owners; ++owner) {
        for (std::uint64_t tag = 0;
             tag < scada::sim::tags_of_owner(layout, owner); ++tag) {
            const std::string name = scada::sim::tag_name(layout, owner, tag);
            scada_value value{};
            if (scada_tag_read(reader.ctx(), reader.resolve(name), &value) !=
                    SCADA_OK ||
                value.change_count != 0 ||
                (value.quality_flags & SCADA_FLAG_NOT_INIT) == 0) {
                return ::testing::AssertionFailure()
                       << name << " was written: change count "
                       << value.change_count;
            }
        }
    }
    return ::testing::AssertionSuccess();
}

// The held simulator `sim`, the process "sim" of `layout`, writes
// HELD_BEATS heartbeats while not one of its tags is written and it has not
// printed "writing".
::testing::AssertionResult held_without_writing(const Attached& reader,
                                                const Layout& layout,
                                                const ChildProcess& sim) {
    std::uint16_t index = 0;
    if (scada_proc_resolve(reader.ctx(), "sim", &index) != SCADA_OK) {
        return ::testing::AssertionFailure() << "sim not resolved";
    }
    if (const ::testing::AssertionResult beats =
            beats_on(reader, index, HELD_BEATS);
        !beats) {
        return beats;
    }
    if (const ::testing::AssertionResult none = no_tag_written(reader, layout);
        !none) {
        return none;
    }
    if (sim.out().find("writing") != std::string::npos) {
        return ::testing::AssertionFailure() << sim.out();
    }
    return ::testing::AssertionSuccess();
}

// A simulator started with --hold writes nothing until SIGUSR1, while its
// heartbeat goes on; after SIGUSR1 it runs as one without the option.
TEST_F(SimProcesses, AHeldSimulatorWritesOnlyAfterSigusr1) {
    const Layout layout{.prefix = "sim",
                        .processes = 1,
                        .owners = 2,
                        .tags = SMALL_TAGS,
                        .patterns = {Pattern::CHECK}};
    write_sim_tag_set(layout);
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    std::optional<ChildProcess> sim = start_held_sim(
        layout, {"--rate", "0", "--rounds", std::to_string(ROUNDS)});
    if (!sim.has_value()) {
        return;
    }
    const Attached reader(base(), "reader");
    EXPECT_TRUE(held_without_writing(reader, layout, *sim));

    ASSERT_TRUE(sim->send_signal(SIGUSR1));
    EXPECT_TRUE(prints(*sim, "done "));
    EXPECT_TRUE(holds_rounds(reader, layout, ROUNDS));
    EXPECT_TRUE(stops_cleanly(*sim));
    stop(*tagd);
}

// A held simulator stops on SIGTERM as a running one does, having written
// nothing.
TEST_F(SimProcesses, AHeldSimulatorStopsOnASignal) {
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
    std::optional<ChildProcess> sim = start_held_sim(layout);
    if (!sim.has_value()) {
        return;
    }
    ASSERT_TRUE(sim->send_signal(SIGTERM));
    EXPECT_EQ(sim->wait_for_exit(WAIT_LIMIT), scada::sim::EXIT_OK)
        << sim->err();
    EXPECT_NE(sim->out().find("stopped updates=0 elapsed_ms=0\n"),
              std::string::npos)
        << sim->out();
    EXPECT_EQ(sim->out().find("writing"), std::string::npos) << sim->out();
    stop(*tagd);
}

// A held simulator whose test died does not wait for ever: the rule for
// one left behind applies while it holds (decision D-50).
TEST_F(SimProcesses, AHeldSimulatorStopsWhenTagdStaysDown) {
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
    std::optional<ChildProcess> sim = start_held_sim(layout);
    if (!sim.has_value()) {
        return;
    }
    stop(*tagd);
    EXPECT_EQ(sim->wait_for_exit(WAIT_LIMIT), scada::sim::EXIT_ERROR)
        << sim->err();
    EXPECT_NE(sim->err().find("scada-tagd has been down"), std::string::npos)
        << sim->err();
    EXPECT_EQ(sim->out().find("writing"), std::string::npos) << sim->out();
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
    EXPECT_EQ(
        tagd->child.err().find(R"(PROCESS="sim" REASON="heartbeat stale")"),
        std::string::npos)
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
