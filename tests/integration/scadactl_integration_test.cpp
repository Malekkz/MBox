// Integration tests of scadactl with real processes (reference 11.5, 11.6;
// issue #19): scada-tagd, the test peer and the test writer run as
// processes, and scadactl is started as its own program against them, as
// an operator would start it. scadactl observes: it needs no entry in the
// tag set.
//
// The acceptance of #19 names scada-sim as the writer for `tag watch`;
// until scada-sim (#18) is on main, the test writer of libscada-tag plays
// it. Its n-th write of a tag stores value n, quality n % 3 and flags
// n & 0x0FDF, so a watch sees GOOD, UNCERTAIN and BAD values and the STALE
// flag in turn.
//
// Every wait is on a condition with a limit.

#include "tag_processes_fixture.h"
#include "tagd_fixture.h"

#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal numbers are declared here, not in <csignal>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
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

// The writer: one owner of the large tag set with one tag, written at
// WRITER_RATE per second, long enough for every watch below.
constexpr std::uint64_t WRITER_RATE = 200;
constexpr std::uint64_t WRITER_UPDATES = 20'000;
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

    // Starts the writer on the large tag set: owner load.w0 with the tag
    // load.w0.t0. Waits until it writes.
    [[nodiscard]] std::optional<ChildProcess> writing() const {
        std::optional<ChildProcess> writer =
            start_helper("SCADA_TAG_WRITER_COMMAND",
                         {base(), "load", "1", "1", std::to_string(WRITER_RATE),
                          std::to_string(WRITER_UPDATES), "notify"});
        if (writer.has_value()) {
            EXPECT_TRUE(prints(*writer, "writing"));
        }
        return writer;
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

// A watch of load.w0.t0 in text: one line of the value now, CHANGES
// change lines, among them BAD ones marked '!' and UNCERTAIN or STALE ones
// marked '~', each with its age.
::testing::AssertionResult watched_in_text(std::string_view text) {
    const bool good = count_of(text, " now    load.w0.t0  ") == 1 &&
                      count_of(text, " change load.w0.t0  ") ==
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
        count_of(json, R"({"event":"change",)") ==
            static_cast<std::size_t>(CHANGES) &&
        json.find(R"("bad":true)") != std::string_view::npos &&
        json.find(R"("stale":true)") != std::string_view::npos &&
        json.find(R"("quality":"UNCERTAIN")") != std::string_view::npos &&
        json.find(R"("age_ns":)") != std::string_view::npos &&
        count_of(json, "\n") == count_of(json, R"({"event":)");
    return good ? ::testing::AssertionSuccess()
                : ::testing::AssertionFailure() << json;
}

TEST_F(Scadactl, TagWatchShowsChangesWithQualityFlagsAndAge) {
    write_load_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> writer = writing();
    if (!tagd.has_value() || !writer.has_value()) {
        return;
    }
    EXPECT_EQ(scadactl({"tag", "watch", "load.w0.t0", "--count",
                        std::to_string(CHANGES)}),
              EXIT_OK)
        << err();
    EXPECT_TRUE(watched_in_text(out()));
    EXPECT_EQ(scadactl({"tag", "watch", "load.*", "--json", "--count",
                        std::to_string(CHANGES)}),
              EXIT_OK)
        << err();
    EXPECT_TRUE(watched_in_json(out()));
    kill(*writer);  // it takes SIGTERM only once it has written everything
    stop(*tagd);
}

TEST_F(Scadactl, TagWatchStopsOnSigint) {
    write_load_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> writer = writing();
    if (!tagd.has_value() || !writer.has_value()) {
        return;
    }
    std::optional<ChildProcess> watch =
        start_scadactl({"tag", "watch", "load.w0.*"});
    if (!watch.has_value()) {
        return;
    }
    EXPECT_TRUE(prints(*watch, " change load.w0.t0"));
    EXPECT_TRUE(watch->send_signal(SIGINT));
    EXPECT_EQ(watch->wait_for_exit(WAIT_LIMIT), EXIT_OK) << watch->err();
    kill(*writer);  // it takes SIGTERM only once it has written everything
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

}  // namespace
