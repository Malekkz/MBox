// Integration test of the queue part of conformance test 21 (contract,
// sections 9.5 and 11.3; decision D-54): across a generation change made by
// a restart of scada-tagd with another tag set, no event record whose post
// returned SCADA_OK is lost, and no request of the old generation is
// executed in the new one. The processes are the helper
// scada-tag-test-crosser, which reattaches on SCADA_E_RETIRING.

#include "tagd_fixture.h"

#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using scada::test::ChildProcess;
using namespace scada::test::tagd;

constexpr mode_t BASE_DIR_MODE = 02750;
constexpr int CHECK_MS = 50;
constexpr int EVENTS = 3000;
constexpr int REQUESTS = 400;
// Posted before the generation changes.
constexpr std::string_view EVENTS_BEFORE = "ok 500\n";
// Requests posted before the generation changes.
constexpr std::size_t REQUESTS_BEFORE = 100;
constexpr std::chrono::seconds RUN_LIMIT{60};

class RetiringQueues : public ::testing::Test {
protected:
    void SetUp() override {
        std::string pattern = (std::filesystem::temp_directory_path() /
                               "scada-retiring-it-XXXXXX")
                                  .string();
        ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
        root_ = pattern;
        base_ = root_ / "base";
        std::filesystem::create_directory(base_);
        ASSERT_EQ(::chmod(base_.c_str(), BASE_DIR_MODE), 0);
        write_tag_set(false);
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    // scada-tagd; `feed` posts events to `hist`; `web` posts requests to
    // the driver `io`, which answers in `result.io.web`. With `spare`, one
    // more process: another tag set, which a restarted scada-tagd makes the
    // next generation of.
    void write_tag_set(bool spare) const {
        const std::string uid = std::to_string(::geteuid());
        std::ofstream file(root_ / "tag-set.json");
        file << R"({"format": 1, "heartbeat_check_ms": )" << CHECK_MS
             << R"(, "processes": [{"name": "tagd", "uid": )" << uid
             << R"(}, {"name": "feed", "uid": )" << uid
             << R"(}, {"name": "hist", "uid": )" << uid
             << R"(}, {"name": "io", "uid": )" << uid
             << R"(}, {"name": "web", "uid": )" << uid << "}";
        if (spare) {
            file << R"(, {"name": "spare", "uid": )" << uid << "}";
        }
        file << R"(], "owners": [], "tags": [], "queues": [)"
             << R"({"name": "events.feed", "producer": "feed", )"
             << R"("consumer": "hist"}, )"
             << R"({"name": "cmd.io.web", "producer": "web", )"
             << R"("consumer": "io"}, )"
             << R"({"name": "result.io.web", "producer": "io", )"
             << R"("consumer": "web"}]})" << "\n";
    }

    [[nodiscard]] std::optional<RunningTagd> tagd() const {
        return ready_tagd((root_ / "tag-set.json").string(), base_.string(),
                          QUIET_WATCHDOG, {});
    }

    // The helper as `process` in `mode`, once it has attached.
    [[nodiscard]] std::optional<ChildProcess> crosser(
        const std::string& process, const std::string& mode,
        const std::string& queue, const std::string& last) const {
        std::vector<std::string> arguments{base_.string(), process, mode,
                                           queue};
        if (!last.empty()) {
            arguments.push_back(last);
        }
        std::optional<ChildProcess> child = ChildProcess::start(
            command_from("SCADA_TAG_CROSSER_COMMAND", arguments));
        if (!child.has_value() || !child->wait_for_output(
                                      [](std::string_view out) {
                                          return out.find("attached gen") !=
                                                 std::string_view::npos;
                                      },
                                      WAIT_LIMIT)) {
            ADD_FAILURE() << process << " did not attach";
            return std::nullopt;
        }
        return child;
    }

    // Stops `first` and starts scada-tagd again with the other tag set: the
    // generation changes.
    [[nodiscard]] std::optional<RunningTagd> change_generation(
        RunningTagd& first) const {
        stop(first);
        write_tag_set(true);
        return tagd();
    }

private:
    std::filesystem::path root_;
    std::filesystem::path base_;
};

bool has(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

// The numbers after `key` on the lines of `text` that start with it.
std::set<std::string> after(std::string_view text, std::string_view key) {
    std::set<std::string> found;
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line)) {
        if (line.starts_with(key)) {
            found.insert(line.substr(key.size()));
        }
    }
    return found;
}

// The events `feed` printed as accepted all reach `hist`, in the old
// generation's drain or in the new one.
::testing::AssertionResult every_accepted_event_delivered(ChildProcess& feed,
                                                          ChildProcess& hist) {
    if (!feed.wait_for_output(
            [](std::string_view out) { return has(out, "done\n"); },
            RUN_LIMIT)) {
        return ::testing::AssertionFailure()
               << "the poster did not finish: " << feed.err();
    }
    const std::set<std::string> accepted = after(feed.out(), "ok ");
    if (accepted.size() != std::size_t{EVENTS}) {
        return ::testing::AssertionFailure()
               << accepted.size() << " events accepted, not " << EVENTS;
    }
    static_cast<void>(hist.wait_for_output(
        [&accepted](std::string_view out) {
            return std::ranges::includes(after(out, "got "), accepted);
        },
        RUN_LIMIT));
    std::vector<std::string> missing;
    std::ranges::set_difference(accepted, after(hist.out(), "got "),
                                std::back_inserter(missing));
    if (!missing.empty()) {
        std::string listed;
        for (const std::string& number : missing) {
            listed += number + " ";
        }
        return ::testing::AssertionFailure()
               << missing.size()
               << " events accepted but not delivered: " << listed
               << hist.err();
    }
    return ::testing::AssertionSuccess();
}

// Every request `driver` executed was posted by `web` in the generation the
// driver took it in.
::testing::AssertionResult no_request_of_another_generation(
    ChildProcess& web, ChildProcess& driver) {
    if (!web.wait_for_output(
            [](std::string_view out) { return has(out, "done\n"); },
            RUN_LIMIT)) {
        return ::testing::AssertionFailure()
               << "the requester did not finish: " << web.err();
    }
    const std::set<std::string> posted = after(web.out(), "posted ");
    if (posted.empty()) {
        return ::testing::AssertionFailure() << "nothing posted";
    }
    // The driver has had time to take what was posted last.
    const std::string last = "took " + *posted.rbegin();
    static_cast<void>(driver.wait_for_output(
        [&last](std::string_view out) { return has(out, last); }, WAIT_LIMIT));
    const std::set<std::string> taken = after(driver.out(), "took ");
    if (taken.empty()) {
        return ::testing::AssertionFailure() << "nothing taken";
    }
    for (const std::string& request : taken) {
        if (!posted.contains(request)) {
            return ::testing::AssertionFailure() << "took " << request;
        }
    }
    return ::testing::AssertionSuccess();
}

// Both processes reached generation 2 and stop cleanly, and then `tagd`.
void end_in_generation_2(ChildProcess& one, ChildProcess& other,
                         RunningTagd& tagd) {
    EXPECT_TRUE(has(one.out(), "attached gen 2")) << one.out();
    EXPECT_TRUE(has(other.out(), "attached gen 2")) << other.out();
    EXPECT_TRUE(stops_cleanly(one));
    EXPECT_TRUE(stops_cleanly(other));
    stop(tagd);
}

TEST_F(RetiringQueues,
       Conformance21NoEventRecordIsLostAcrossAGenerationChange) {
    std::optional<RunningTagd> first = tagd();
    std::optional<ChildProcess> hist =
        crosser("hist", "consume", "events.feed", "");
    std::optional<ChildProcess> feed =
        crosser("feed", "post", "events.feed", std::to_string(EVENTS));
    if (!first.has_value() || !hist.has_value() || !feed.has_value()) {
        ADD_FAILURE() << "the processes did not start";
        return;
    }
    ASSERT_TRUE(prints(*feed, EVENTS_BEFORE));
    std::optional<RunningTagd> second = change_generation(*first);
    if (!second.has_value()) {
        ADD_FAILURE() << "the second scada-tagd did not start";
        return;
    }
    EXPECT_TRUE(every_accepted_event_delivered(*feed, *hist));
    end_in_generation_2(*feed, *hist, *second);
}

TEST_F(RetiringQueues,
       Conformance21NoRequestOfTheOldGenerationIsExecutedInTheNewOne) {
    std::optional<RunningTagd> first = tagd();
    std::optional<ChildProcess> driver =
        crosser("io", "drive", "cmd.io.web", "result.io.web");
    std::optional<ChildProcess> web =
        crosser("web", "request", "cmd.io.web", std::to_string(REQUESTS));
    if (!first.has_value() || !driver.has_value() || !web.has_value()) {
        ADD_FAILURE() << "the processes did not start";
        return;
    }
    ASSERT_TRUE(web->wait_for_output(
        [](std::string_view out) {
            return after(out, "posted ").size() >= REQUESTS_BEFORE;
        },
        WAIT_LIMIT))
        << web->err();
    std::optional<RunningTagd> second = change_generation(*first);
    if (!second.has_value()) {
        ADD_FAILURE() << "the second scada-tagd did not start";
        return;
    }
    // Ids restart in each generation: a request is told by its stamp and
    // generation.
    EXPECT_TRUE(no_request_of_another_generation(*web, *driver));
    end_in_generation_2(*web, *driver, *second);
}

}  // namespace
