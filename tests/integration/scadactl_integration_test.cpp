// Integration tests of scadactl with real processes (reference 11.5, 11.6;
// issue #19): scada-tagd, the test peer and scada-sim run as
// processes, and scadactl is started as its own program against them, as
// an operator would start it. scadactl observes: it needs no entry in the
// tag set.
//
// scada-sim (issue P1-10) is the writer for `tag watch`, as the
// acceptance of #19 asks: its `check` pattern stores, at the n-th write of a
// tag, value n, quality n % 3 and flags n & 0x0FDF, so a watch sees GOOD,
// UNCERTAIN and BAD values and the STALE flag in turn.
//
// Every wait is on a condition with a limit.

#include "tag_processes_fixture.h"
#include "tagd_fixture.h"

#include "scada/sim/layout.h"
#include "scada/sim/pattern.h"
#include "scada/sim/sim.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal numbers are declared here, not in <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scada::test::tagd;
using scada::test::ChildProcess;

// Exit codes of scadactl.
constexpr int EXIT_OK = 0;
constexpr int EXIT_ERROR = 1;
constexpr int EXIT_USAGE = 2;

// The simulation of the watches: the process `sim`, which is its one owner,
// with one `check` tag `sim.t0`, written WRITER_RATE times a
// second until it is stopped.
constexpr std::uint64_t WRITER_RATE = 200;

scada::sim::Layout watch_layout() {
    return scada::sim::Layout{.prefix = "sim",
                              .processes = 1,
                              .owners = 1,
                              .tags = 1,
                              .patterns = {scada::sim::Pattern::CHECK}};
}
constexpr int CHANGES = 9;

// How often `text` holds `part`.
std::size_t count_of(std::string_view text, std::string_view part) {
    std::size_t count = 0;
    for (std::size_t at = text.find(part); at != std::string_view::npos;
         at = text.find(part, at + part.size())) {
        ++count;
    }
    return count;
}

// The lines of `text` that start with `prefix`.
std::vector<std::string> lines_starting(std::string_view text,
                                        std::string_view prefix) {
    std::vector<std::string> lines;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const std::string_view line = text.substr(0, end);
        if (line.starts_with(prefix)) {
            lines.emplace_back(line);
        }
        text = end == std::string_view::npos ? std::string_view{}
                                             : text.substr(end + 1);
    }
    return lines;
}

class Scadactl : public TagProcesses {
protected:
    // Starts scadactl with `arguments` on the database of the test.
    [[nodiscard]] std::optional<ChildProcess> start_scadactl(
        const std::vector<std::string>& arguments) const {
        std::vector<std::string> all{"--base-dir", base(), "--tagd-uid",
                                     std::to_string(::geteuid())};
        all.insert(all.end(), arguments.begin(), arguments.end());
        return start_helper("SCADACTL_COMMAND", all);
    }

    // Runs scadactl with `arguments` to its end. Its exit code, or -1 if it
    // did not end.
    int scadactl(const std::vector<std::string>& arguments) {
        std::optional<ChildProcess> run = start_scadactl(arguments);
        if (!run.has_value()) {
            return -1;
        }
        const std::optional<int> code = run->wait_for_exit(WAIT_LIMIT);
        out_ = run->out();
        err_ = run->err();
        return code.value_or(-1);
    }

    [[nodiscard]] const std::string& out() const { return out_; }
    [[nodiscard]] const std::string& err() const { return err_; }

    // The tag set of watch_layout().
    void write_watch_tag_set() const { write_sim_tag_set(watch_layout()); }

    // Starts scada-sim on watch_layout() and waits until it writes.
    [[nodiscard]] std::optional<ChildProcess> writing() const {
        std::optional<ChildProcess> sim = start_sim(
            watch_layout(),
            {"--rate", std::to_string(WRITER_RATE), "--notify", "each"});
        if (sim.has_value()) {
            EXPECT_TRUE(prints(*sim, "writing"));
        }
        return sim;
    }

    // Adds a tag to the small tag set file, so that scada-tagd started on
    // it creates a new generation.
    void add_a_tag() const {
        std::string text;
        {
            std::ifstream file(tag_set_path());
            std::getline(file, text, '\0');
        }
        const std::string first = R"({"name": "plant.level", )";
        const std::size_t position = text.find(first);
        ASSERT_NE(position, std::string::npos);
        text.insert(
            position,
            R"({"name": "plant.extra", "owner": "io", "type": "int64"}, )");
        std::ofstream file(tag_set_path(), std::ios::trunc);
        file << text;
    }

private:
    std::string out_;
    std::string err_;
};

TEST_F(Scadactl, HelpDocumentsEveryCommand) {
    EXPECT_EQ(scadactl({"--help"}), EXIT_OK);
    for (const std::string_view word :
         {"status", "tag list", "tag get", "tag watch", "--json", "--count"}) {
        EXPECT_NE(out().find(word), std::string::npos) << word;
    }
    EXPECT_EQ(scadactl({}), EXIT_USAGE);
    EXPECT_NE(err().find("--help"), std::string::npos);
    EXPECT_EQ(scadactl({"tag", "write", "plant.level", "1"}), EXIT_USAGE);
}

TEST_F(Scadactl, SaysSoWhenThereIsNoDatabase) {
    EXPECT_EQ(scadactl({"status"}), EXIT_ERROR);
    EXPECT_NE(err().find("no tag database"), std::string::npos) << err();
}

// `status` in both forms, with `io` up and `probe` never attached.
::testing::AssertionResult shows_io_up_and_probe_down(std::string_view text,
                                                      std::string_view json) {
    const bool good =
        text.find("generation 1 RUNNING") != std::string_view::npos &&
        text.find("scada-tagd up") != std::string_view::npos &&
        lines_starting(text, "  io ").size() == 2 &&
        lines_starting(text, "! probe ").size() == 2 &&
        json.find(
            R"({"name":"io","process":"io","tags":1,"owner_down":false})") !=
            std::string_view::npos &&
        json.find(
            R"({"name":"probe","process":"probe","tags":1,"owner_down":true})") !=
            std::string_view::npos;
    return good ? ::testing::AssertionSuccess()
                : ::testing::AssertionFailure() << text << json;
}

TEST_F(Scadactl, StatusShowsProcessesAndOwners) {
    write_small_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> feeder = attached_io();
    if (!tagd.has_value() || !feeder.has_value()) {
        return;
    }
    EXPECT_EQ(scadactl({"status"}), EXIT_OK) << err();
    const std::string text = out();
    EXPECT_EQ(scadactl({"status", "--json"}), EXIT_OK) << err();
    EXPECT_TRUE(shows_io_up_and_probe_down(text, out()));
    EXPECT_TRUE(stops_cleanly(*feeder));
    stop(*tagd);
}

// `tag list` of the small tag set, in which no tag was ever written, in
// both forms.
::testing::AssertionResult lists_both_as_bad(std::string_view text,
                                             std::string_view json) {
    const bool good =
        lines_starting(text, "! plant.level ").size() == 1 &&
        lines_starting(text, "! probe.out ").size() == 1 &&
        count_of(json, R"("name":)") == 1 &&
        json.find(R"("bad":true)") != std::string_view::npos &&
        json.find(R"("flags":["NOT_INIT"])") != std::string_view::npos;
    return good ? ::testing::AssertionSuccess()
                : ::testing::AssertionFailure() << text << json;
}

TEST_F(Scadactl, TagListMarksBadValues) {
    write_small_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> feeder = attached_io();
    if (!tagd.has_value() || !feeder.has_value()) {
        return;
    }
    EXPECT_EQ(scadactl({"tag", "list"}), EXIT_OK) << err();
    const std::string text = out();
    EXPECT_EQ(scadactl({"tag", "list", "plant.*", "--json"}), EXIT_OK);
    EXPECT_TRUE(lists_both_as_bad(text, out()));
    EXPECT_TRUE(stops_cleanly(*feeder));
    stop(*tagd);
}

TEST_F(Scadactl, TagGetMarksABadValueAndRefusesAnUnknownTag) {
    write_small_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    EXPECT_EQ(scadactl({"tag", "get", "plant.level"}), EXIT_OK) << err();
    EXPECT_NE(out().find("quality ! BAD"), std::string::npos) << out();
    EXPECT_EQ(scadactl({"tag", "get", "plant.nothing"}), EXIT_ERROR);
    EXPECT_NE(err().find("there is no tag plant.nothing"), std::string::npos);
    stop(*tagd);
}

// A watch of sim.t0 in text: one line of the value now, CHANGES
// change lines, among them BAD ones marked '!' and UNCERTAIN or STALE ones
// marked '~', each with its age.
::testing::AssertionResult watched_in_text(std::string_view text) {
    const bool good = count_of(text, " now    sim.t0  ") == 1 &&
                      count_of(text, " change sim.t0  ") ==
                          static_cast<std::size_t>(CHANGES) &&
                      !lines_starting(text, "! change ").empty() &&
                      !lines_starting(text, "~ change ").empty() &&
                      text.find("STALE") != std::string_view::npos &&
                      text.find(" age 0.") != std::string_view::npos;
    return good ? ::testing::AssertionSuccess()
                : ::testing::AssertionFailure() << text;
}

// The same watch in JSON: one document per line, BAD and STALE marked.
::testing::AssertionResult watched_in_json(std::string_view json) {
    const bool good =
        count_of(json, R"({"schema":1,"event":"change",)") ==
            static_cast<std::size_t>(CHANGES) &&
        json.find(R"("bad":true)") != std::string_view::npos &&
        json.find(R"("stale":true)") != std::string_view::npos &&
        json.find(R"("quality":"UNCERTAIN")") != std::string_view::npos &&
        json.find(R"("age_ns":)") != std::string_view::npos &&
        count_of(json, "\n") == count_of(json, R"({"schema":1,"event":)");
    return good ? ::testing::AssertionSuccess()
                : ::testing::AssertionFailure() << json;
}

TEST_F(Scadactl, TagWatchShowsChangesWithQualityFlagsAndAge) {
    write_watch_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> writer = writing();
    if (!tagd.has_value() || !writer.has_value()) {
        return;
    }
    EXPECT_EQ(scadactl({"tag", "watch", "sim.t0", "--count",
                        std::to_string(CHANGES)}),
              EXIT_OK)
        << err();
    EXPECT_TRUE(watched_in_text(out()));
    EXPECT_EQ(scadactl({"tag", "watch", "sim.*", "--json", "--count",
                        std::to_string(CHANGES)}),
              EXIT_OK)
        << err();
    EXPECT_TRUE(watched_in_json(out()));
    EXPECT_TRUE(stops_cleanly(*writer));
    stop(*tagd);
}

TEST_F(Scadactl, TagWatchStopsOnSigint) {
    write_watch_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> writer = writing();
    if (!tagd.has_value() || !writer.has_value()) {
        return;
    }
    std::optional<ChildProcess> watch =
        start_scadactl({"tag", "watch", "sim.*"});
    if (!watch.has_value()) {
        return;
    }
    EXPECT_TRUE(prints(*watch, " change sim.t0"));
    EXPECT_TRUE(watch->send_signal(SIGINT));
    EXPECT_EQ(watch->wait_for_exit(WAIT_LIMIT), EXIT_OK) << watch->err();
    EXPECT_TRUE(stops_cleanly(*writer));
    stop(*tagd);
}

TEST_F(Scadactl, TagWatchFollowsAGenerationChange) {
    write_small_tag_set();
    std::optional<RunningTagd> first = ready_tagd();
    if (!first.has_value()) {
        return;
    }
    std::optional<ChildProcess> watch =
        start_scadactl({"tag", "watch", "plant.*"});
    if (!watch.has_value()) {
        return;
    }
    EXPECT_TRUE(prints(*watch, " now    plant.level"));
    stop(*first);
    add_a_tag();
    std::optional<RunningTagd> second = ready_tagd();
    if (!second.has_value()) {
        return;
    }
    EXPECT_TRUE(prints(*watch, "generation changed: now 2"));
    EXPECT_TRUE(prints(*watch, " now    plant.extra"));
    EXPECT_TRUE(watch->send_signal(SIGTERM));
    EXPECT_EQ(watch->wait_for_exit(WAIT_LIMIT), EXIT_OK) << watch->err();
    stop(*second);
}

// What a watch printed after the line that says the generation changed.
std::string_view after_generation_change(std::string_view out) {
    const std::size_t position = out.find("generation changed");
    return position == std::string_view::npos ? std::string_view{}
                                              : out.substr(position);
}

// True if the output of a watch, stopped after a generation change, has
// lines after the change and none about a tag of the old generation.
::testing::AssertionResult nothing_of_the_old_generation(std::string_view out) {
    const std::string_view later = after_generation_change(out);
    if (!later.empty() && later.find("sim.") == std::string_view::npos) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << out;
}

// Waits until the watch `watch` shows generation 2 with its tag
// probe.out, then stops it with SIGTERM.
::testing::AssertionResult moves_to_generation_two_and_stops(
    ChildProcess& watch) {
    if (!prints(watch, "generation changed: now 2") ||
        !prints(watch, " now    probe.out")) {
        return ::testing::AssertionFailure() << watch.out() << watch.err();
    }
    if (!watch.send_signal(SIGTERM) ||
        watch.wait_for_exit(WAIT_LIMIT) != EXIT_OK) {
        return ::testing::AssertionFailure()
               << "no clean stop: " << watch.err();
    }
    return ::testing::AssertionSuccess();
}

TEST_F(Scadactl, TagWatchNeverShowsTheTagsOfARetiredGeneration) {
    // Generation 1: the simulation's tag set, with scada-sim writing.
    write_watch_tag_set();
    std::optional<RunningTagd> first = ready_tagd();
    std::optional<ChildProcess> writer = writing();
    std::optional<ChildProcess> watch = start_scadactl({"tag", "watch", "*"});
    if (!first.has_value() || !writer.has_value() || !watch.has_value()) {
        return;
    }
    EXPECT_TRUE(prints(*watch, " change sim.t0"));
    // Generation 2: another tag set, without the simulation's tags.
    // scada-sim may go on writing generation 1 for a while; it stops by
    // itself once it sees the generation retire.
    stop(*first);
    write_small_tag_set();
    std::optional<RunningTagd> second = ready_tagd();
    if (!second.has_value()) {
        return;
    }
    EXPECT_TRUE(moves_to_generation_two_and_stops(*watch));
    EXPECT_TRUE(nothing_of_the_old_generation(watch->out()));
    // scada-sim stops by itself with code 1 once it sees generation 1
    // retire (D-41); a kill could find it gone already.
    EXPECT_EQ(writer->wait_for_exit(WAIT_LIMIT), scada::sim::EXIT_ERROR)
        << writer->err();
    stop(*second);
}

TEST_F(Scadactl, TagListSaysWhenScadaTagdIsDown) {
    write_small_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    // The generation stays when scada-tagd stops; its liveness check does
    // not run any more.
    stop(*tagd);
    EXPECT_EQ(scadactl({"tag", "list"}), EXIT_OK) << err();
    EXPECT_TRUE(
        out().starts_with("! scada-tagd down: liveness is still "
                          "checked; TAGD FLAG is not updated\n"))
        << out();
    EXPECT_EQ(scadactl({"tag", "get", "plant.level", "--json"}), EXIT_OK);
    EXPECT_NE(out().find(R"("tagd_down":true)"), std::string::npos) << out();
}

TEST_F(Scadactl, TagWatchOfNothingIsAnError) {
    write_small_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    EXPECT_EQ(scadactl({"tag", "watch", "nothing.*"}), EXIT_ERROR);
    EXPECT_NE(err().find("no tag matches nothing.*"), std::string::npos);
    stop(*tagd);
}

// How a run of scadactl whose standard output fails ended.
struct FailedOutput {
    std::optional<int> code;  // nothing: it did not end within WAIT_LIMIT
    std::string err;
};

// The ways the standard output of scadactl fails.
enum class Output : std::uint8_t {
    CLOSED_PIPE,  // a pipe whose reader goes away after one line
    FULL,         // /dev/full: every write fails with ENOSPC
    CLOSED,       // no descriptor 1 at all
};

// The size of one read of the standard error stream, in bytes.
constexpr std::size_t CHUNK_BYTES = 256;

// Waits up to WAIT_LIMIT for `descriptor` to be readable.
bool readable(int descriptor) {
    pollfd wait{.fd = descriptor, .events = POLLIN, .revents = 0};
    const auto limit = std::chrono::milliseconds(WAIT_LIMIT).count();
    return ::poll(&wait, 1, static_cast<int>(limit)) == 1;
}

// Reads from `descriptor` until the end of a line or of the stream.
void read_a_line(int descriptor) {
    char byte = 0;
    while (readable(descriptor) && ::read(descriptor, &byte, 1) == 1 &&
           byte != '\n') {
    }
}

// Collects `descriptor` until its end; false if it did not end in time.
bool read_to_end(int descriptor, std::string& text) {
    std::array<char, CHUNK_BYTES> chunk{};
    for (;;) {
        if (!readable(descriptor)) {
            return false;
        }
        const ssize_t got = ::read(descriptor, chunk.data(), chunk.size());
        if (got <= 0) {
            return got == 0;
        }
        text.append(chunk.data(), static_cast<std::size_t>(got));
    }
}

// The exit code of a child whose program could not be executed.
constexpr int NOT_EXECUTED = 127;

// In the child: SIGPIPE blocked, which a program inherits over exec as it
// inherits an ignored one (a shell or service manager may leave either):
// a write to a pipe without reader then fails with EPIPE and ends nothing.
// Then the standard output as `output` asks, and `argv` executed.
[[noreturn]] void exec_with_output(std::vector<char*>& argv, Output output,
                                   int pipe_writer, int err_writer) {
    sigset_t pipe_signal{};
    sigemptyset(&pipe_signal);
    sigaddset(&pipe_signal, SIGPIPE);
    static_cast<void>(::pthread_sigmask(SIG_BLOCK, &pipe_signal, nullptr));
    ::dup2(err_writer, STDERR_FILENO);
    if (output == Output::CLOSED_PIPE) {
        ::dup2(pipe_writer, STDOUT_FILENO);
    } else if (output == Output::FULL) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open has no other form
        const int full = ::open("/dev/full", O_WRONLY | O_CLOEXEC);
        ::dup2(full, STDOUT_FILENO);
    } else {
        ::close(STDOUT_FILENO);
    }
    ::execvp(argv.front(), argv.data());
    ::_exit(NOT_EXECUTED);
}

// Runs `command` with its standard output failing as `output` says.
FailedOutput run_with_failing_output(std::vector<std::string> command,
                                     Output output) {
    FailedOutput result;
    std::array<int, 2> out_pipe{-1, -1};
    std::array<int, 2> err_pipe{-1, -1};
    if (command.empty() || ::pipe2(out_pipe.data(), O_CLOEXEC) != 0 ||
        ::pipe2(err_pipe.data(), O_CLOEXEC) != 0) {
        return result;
    }
    std::vector<char*> argv;
    argv.reserve(command.size() + 1);
    for (std::string& word : command) {
        argv.push_back(word.data());
    }
    argv.push_back(nullptr);
    const pid_t child = ::fork();
    if (child == 0) {
        exec_with_output(argv, output, out_pipe[1], err_pipe[1]);
    }
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    if (output == Output::CLOSED_PIPE) {
        read_a_line(out_pipe[0]);  // as `head -1` does
    }
    ::close(out_pipe[0]);
    const bool ended = child > 0 && read_to_end(err_pipe[0], result.err);
    ::close(err_pipe[0]);
    if (child <= 0) {
        return result;
    }
    if (!ended) {
        ::kill(child, SIGKILL);
    }
    int status = 0;
    if (::waitpid(child, &status, 0) == child && ended && WIFEXITED(status)) {
        result.code = WEXITSTATUS(status);
    }
    return result;
}

::testing::AssertionResult fails_with_an_output_error(const FailedOutput& run) {
    if (run.code == EXIT_ERROR &&
        run.err.find("writing the output failed") != std::string::npos) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << "code " << run.code.value_or(-1) << ": " << run.err;
}

class ScadactlOutput : public Scadactl {
protected:
    // The command of scadactl with `arguments` on the database of the test.
    [[nodiscard]] std::vector<std::string> command(
        const std::vector<std::string>& arguments) const {
        std::vector<std::string> all = command_from(
            "SCADACTL_COMMAND",
            {"--base-dir", base(), "--tagd-uid", std::to_string(::geteuid())});
        all.insert(all.end(), arguments.begin(), arguments.end());
        return all;
    }

    // True if scadactl with `arguments` fails with an output error when
    // its output fails as `output` says.
    [[nodiscard]] ::testing::AssertionResult fails_writing(
        const std::vector<std::string>& arguments, Output output) const {
        return fails_with_an_output_error(
            run_with_failing_output(command(arguments), output));
    }
};

TEST_F(ScadactlOutput, TagWatchEndsWhenItsOutputCannotBeWritten) {
    write_watch_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> writer = writing();
    if (!tagd.has_value() || !writer.has_value()) {
        return;
    }
    // `tag watch | head -1`: the watch ends at its next line.
    EXPECT_TRUE(fails_writing({"tag", "watch", "sim.*"}, Output::CLOSED_PIPE));
    EXPECT_TRUE(fails_writing({"tag", "watch", "sim.*"}, Output::FULL));
    EXPECT_TRUE(stops_cleanly(*writer));
    stop(*tagd);
}

TEST_F(ScadactlOutput, ReadCommandsFailWhenTheirOutputCannotBeWritten) {
    write_small_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    for (const Output output : {Output::FULL, Output::CLOSED}) {
        EXPECT_TRUE(fails_writing({"tag", "list"}, output));
        EXPECT_TRUE(fails_writing({"status", "--json"}, output));
        EXPECT_TRUE(fails_writing({"tag", "get", "plant.level"}, output));
    }
    stop(*tagd);
}

// The offset of `capacity` in a change ring region: it follows the 64
// bytes of the region header (contract 8).
constexpr std::streamoff RING_CAPACITY_OFFSET = 64;
// The capacity of the ring of `io` in the small tag set.
constexpr std::uint32_t SMALL_RING_CAPACITY = 1024;

// Sets the capacity in the header of the change ring region `path`.
void set_ring_capacity(const std::filesystem::path& path,
                       std::uint32_t capacity) {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(RING_CAPACITY_OFFSET);
    // The layout is in the byte order of the unit (contract 4), as is
    // this process.
    const auto bytes =
        std::bit_cast<std::array<char, sizeof(capacity)>>(capacity);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(file.good()) << path;
}

TEST_F(Scadactl, TagWatchEndsWhenAChangeRingDoesNotMatch) {
    write_small_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> feeder = attached_io();
    if (!tagd.has_value() || !feeder.has_value()) {
        return;
    }
    // The ring `io` made, with a capacity the owner table does not have,
    // kept aside; then the ring is taken away, so that the watch starts
    // without it.
    EXPECT_TRUE(stops_cleanly(*feeder));
    const std::filesystem::path generation =
        std::filesystem::canonical(std::filesystem::path(base()) / "current");
    const std::filesystem::path ring = generation / "changes.io";
    const std::filesystem::path aside =
        std::filesystem::path(tag_set_path()).parent_path() / "changes.bad";
    ASSERT_TRUE(std::filesystem::exists(ring));
    std::filesystem::copy_file(ring, aside);
    set_ring_capacity(aside, 2 * SMALL_RING_CAPACITY);
    std::filesystem::remove(ring);
    std::optional<ChildProcess> watch =
        start_scadactl({"tag", "watch", "plant.level"});
    if (!watch.has_value()) {
        return;
    }
    EXPECT_TRUE(prints(*watch, " now    plant.level"));
    // The ring appears; every read of it gives SCADA_E_LAYOUT.
    std::filesystem::rename(aside, ring);
    EXPECT_EQ(watch->wait_for_exit(WAIT_LIMIT), EXIT_ERROR) << watch->out();
    EXPECT_NE(watch->err().find("the change ring of io does not match"),
              std::string::npos)
        << watch->err();
    stop(*tagd);
}

}  // namespace
