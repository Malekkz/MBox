// Retained values and the generation change with real processes (tag
// database contract, sections 11.3 and 12; decision D-45): scada-tagd and
// scada-tag-test-retainer, which writes a retained counter and follows a
// generation change.
//
// - Conformance test 21, the part of retained values: a value written
//   right before the change is restored in the new generation, within
//   5 s of the new scada-tagd being ready. The parts on queues are issue
//   #65.
// - Conformance test 25: a damaged retain file is ignored and reported;
//   attach succeeds. A RETAIN_NOW tag is on disk within 1 s.
// - The acceptance of issue #17: SIGKILL of every process at random points
//   never leaves an unreadable retain file. A kill keeps the page cache, so
//   this shows the atomic rename, not durability; the power-loss model of
//   the unit tests (retain_power_loss_test.cpp) covers the fsync order.

#include "retain_file.h"
#include "tagd_fixture.h"

#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using scada::test::ChildProcess;
using namespace scada::test::tagd;

constexpr mode_t BASE_DIR_MODE = 02750;
constexpr int CHECK_MS = 50;
// Long: only the write at a change, at a stop and the first one of an owner
// bring a value of a tag without retain_now to disk.
constexpr int RETAIN_INTERVAL_MS = 3'600'000;
constexpr std::chrono::seconds CHANGE_LIMIT{5};
constexpr std::chrono::milliseconds RETAIN_NOW_LIMIT{1000};
constexpr int KILL_ROUNDS = 30;
constexpr std::uint32_t KILL_SEED = 17;
constexpr double TAGD_FIRST_ODDS = 0.5;
constexpr double FEEDER_SURVIVES_ODDS = 0.5;
// Write periods of the retainer, in ms.
constexpr int FAST_PERIOD_MS = 10;
constexpr int SLOW_PERIOD_MS = 100;
constexpr int KILL_AFTER_MIN_MS = 20;
constexpr int KILL_AFTER_MAX_MS = 400;

// The number after `key` in the last line of `text` that has it.
std::optional<std::int64_t> last_number(std::string_view text,
                                        std::string_view key) {
    const std::size_t found = text.rfind(key);
    if (found == std::string_view::npos) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const std::string_view rest = text.substr(found + key.size());
    const auto [end, error] =
        std::from_chars(rest.data(), rest.data() + rest.size(), value);
    if (error != std::errc{}) {
        return std::nullopt;
    }
    return value;
}

class Retained : public ::testing::Test {
protected:
    void SetUp() override {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "scada-retain-it-XXXXXX")
                .string();
        ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
        root_ = pattern;
        base_ = root_ / "base";
        std::filesystem::create_directory(base_);
        ASSERT_EQ(::chmod(base_.c_str(), BASE_DIR_MODE), 0);
        retain_ = retain_dir_for(base_.string());
        write_tag_set(false, false);
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    // scada-tagd and a process `io` whose owner has plant.count (INT64,
    // retain, and retain_now if `now`). With `more` a second tag: another
    // tag set, which a restarted scada-tagd makes the next generation of.
    void write_tag_set(bool now, bool more) const {
        const std::string uid = std::to_string(::geteuid());
        std::ofstream file(root_ / "tag-set.json");
        file << R"({"format": 1, "heartbeat_check_ms": )" << CHECK_MS
             << R"(, "processes": [{"name": "tagd", "uid": )" << uid
             << R"(}, {"name": "io", "uid": )" << uid
             << R"(}], "owners": [{"name": "io", "process": "io", )"
             << R"("change_ring_capacity": 1024}], "queues": [], )"
             << R"("tags": [{"name": "plant.count", "owner": "io", )"
             << R"("type": "int64", "retain": true)"
             << (now ? R"(, "retain_now": true)" : "") << "}";
        if (more) {
            file
                << R"(, {"name": "plant.more", "owner": "io", "type": "bool"})";
        }
        file << "]}\n";
    }

    [[nodiscard]] std::optional<RunningTagd> tagd() const {
        return ready_tagd(
            (root_ / "tag-set.json").string(), base_.string(), QUIET_WATCHDOG,
            {"--retain-interval-ms", std::to_string(RETAIN_INTERVAL_MS)});
    }

    [[nodiscard]] std::optional<ChildProcess> retainer(int period_ms) const {
        const std::vector<std::string> command =
            command_from("SCADA_TAG_RETAINER_COMMAND",
                         {base_.string(), retain_, "io", "plant.count",
                          std::to_string(period_ms)});
        std::optional<ChildProcess> child = ChildProcess::start(command);
        EXPECT_TRUE(child.has_value());
        return child;
    }

    // The value of plant.count in io.bin, if the file is there and valid.
    [[nodiscard]] std::optional<std::int64_t> on_file() const {
        const scada::Fd directory(
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open has no other form
            ::open(retain_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        std::string detail;
        const scada::Result<scada::tag::RetainFile> file =
            scada::tag::read_retain_file(directory.get(), "io.bin", ::geteuid(),
                                         detail);
        if (!file.ok()) {
            return std::nullopt;
        }
        const scada::tag::RetainedValue* value =
            scada::tag::find_retained(file.value(), "plant.count");
        if (value == nullptr) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(value->value);
    }

    // Waits until io.bin holds at least `least`, within `limit`.
    [[nodiscard]] bool file_reaches(std::int64_t least,
                                    std::chrono::milliseconds limit) const {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            if (on_file().value_or(-1) >= least) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    }

    [[nodiscard]] const std::filesystem::path& base() const { return base_; }
    [[nodiscard]] const std::string& retain() const { return retain_; }

    [[nodiscard]] ::testing::AssertionResult restored_after_change(
        ChildProcess& feeder, RunningTagd& second) const;

    [[nodiscard]] std::optional<std::int64_t> kill_round(
        std::chrono::milliseconds after, bool tagd_first, bool feeder_survives,
        std::optional<ChildProcess>& feeder);

private:
    std::filesystem::path root_;
    std::filesystem::path base_;
    std::string retain_;
};

bool has(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

// True once the retainer has left the old generation and attached again.
bool reattached(std::string_view out) {
    const std::size_t left = out.find("leaving last=");
    if (left == std::string_view::npos) {
        return false;
    }
    const std::size_t again = out.find("attached", left);
    return again != std::string_view::npos &&
           out.find('\n', again) != std::string_view::npos;
}

// After a generation change that `second` made: the retainer reattached
// within CHANGE_LIMIT, restored the last value it wrote, UNCERTAIN with
// RESTORED (contract, section 12), and the old generation is gone.
::testing::AssertionResult Retained::restored_after_change(
    ChildProcess& feeder, RunningTagd& second) const {
    const auto ready = std::chrono::steady_clock::now();
    if (!feeder.wait_for_output(reattached, WAIT_LIMIT)) {
        return ::testing::AssertionFailure()
               << "no reattach: " << feeder.out() << second.child.err();
    }
    if (std::chrono::steady_clock::now() - ready >= CHANGE_LIMIT) {
        return ::testing::AssertionFailure() << "reattach took too long";
    }
    const std::string& out = feeder.out();
    const std::int64_t last = last_number(out, "leaving last=").value_or(-1);
    const std::int64_t restored =
        last_number(out, "attached value=").value_or(-1);
    if (last <= 1 || restored != last ||
        !has(out.substr(out.rfind("attached")), "quality=1 flags=1024")) {
        return ::testing::AssertionFailure() << "not restored: " << out;
    }
    if (!logs(second, "retired generation removed") ||
        std::filesystem::exists(base() / "gen-1")) {
        return ::testing::AssertionFailure() << "gen-1 was not removed";
    }
    return ::testing::AssertionSuccess();
}

TEST_F(Retained, Conformance21AValueWrittenRightBeforeTheChangeIsRestored) {
    std::optional<RunningTagd> first = tagd();
    std::optional<ChildProcess> feeder = retainer(FAST_PERIOD_MS);
    if (!first.has_value() || !feeder.has_value()) {
        return;  // the helpers reported it
    }
    ASSERT_TRUE(feeder->wait_for_output(
        [](std::string_view out) { return has(out, "attached value=0"); },
        WAIT_LIMIT));
    // The first write of the owner is on disk; later ones wait for the hour.
    ASSERT_TRUE(file_reaches(1, WAIT_LIMIT));
    stop(*first);

    // The tag set changes; the retainer goes on counting meanwhile.
    write_tag_set(false, true);
    std::optional<RunningTagd> second = tagd();
    if (!second.has_value()) {
        return;
    }
    EXPECT_TRUE(restored_after_change(*feeder, *second));
    EXPECT_TRUE(stops_cleanly(*feeder));
    stop(*second);
}

TEST_F(Retained,
       Conformance25ADamagedFileIsIgnoredAndReportedAndAttachSucceeds) {
    {
        std::ofstream damaged(std::filesystem::path(retain()) / "io.bin",
                              std::ios::binary);
        damaged << "SCADARTN and nothing a retain file holds";
    }
    std::optional<RunningTagd> service = tagd();
    std::optional<ChildProcess> feeder = retainer(FAST_PERIOD_MS);
    if (!service.has_value() || !feeder.has_value()) {
        return;
    }
    ASSERT_TRUE(feeder->wait_for_output(
        [](std::string_view out) { return has(out, "\n"); }, WAIT_LIMIT));
    // BAD with NOT_INIT: nothing restored.
    EXPECT_TRUE(has(feeder->out(), "attached value=0 quality=2 flags=16"))
        << feeder->out();
    EXPECT_TRUE(logs(*service, R"(retained values SUBJECT="io.bin")"));
    // The next write replaces it with a valid file.
    EXPECT_TRUE(file_reaches(1, WAIT_LIMIT));
    EXPECT_TRUE(stops_cleanly(*feeder));
    stop(*service);
}

TEST_F(Retained, Conformance25ARetainNowTagIsOnDiskWithinOneSecond) {
    write_tag_set(true, false);
    std::optional<RunningTagd> service = tagd();
    std::optional<ChildProcess> feeder = retainer(SLOW_PERIOD_MS);
    if (!service.has_value() || !feeder.has_value()) {
        return;
    }
    ASSERT_TRUE(file_reaches(3, WAIT_LIMIT));
    // The last write of the retainer comes at most one period before this.
    ASSERT_TRUE(stops_cleanly(*feeder));
    const auto stopped = std::chrono::steady_clock::now();
    const std::int64_t last =
        last_number(feeder->out(), "stopped last=").value_or(-1);
    ASSERT_GT(last, 0);
    // On disk within the limit, counted from the stop.
    EXPECT_TRUE(file_reaches(
        last,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            RETAIN_NOW_LIMIT - (std::chrono::steady_clock::now() - stopped))));
    EXPECT_EQ(on_file(), last);
    stop(*service);
}

// The temporary files in the retain directory `retain`.
std::vector<std::string> temporaries_in(const std::string& retain) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(retain)) {
        if (entry.path().filename().string().starts_with(".tmp.")) {
            names.push_back(entry.path().filename().string());
        }
    }
    return names;
}

// One round of the kill test: scada-tagd, and the retainer `feeder` (one
// is started if there is none), killed after `after` ms in the order
// `tagd_first` says. With `feeder_survives` the retainer is left running
// into the next round, whose scada-tagd has the other tag set: that one
// builds a new generation while the retainer is attached to the old one,
// so the next kill can come during the FINAL handshake. The value on file
// after the round, or nothing if there is no valid one.
std::optional<std::int64_t> Retained::kill_round(
    std::chrono::milliseconds after, bool tagd_first, bool feeder_survives,
    std::optional<ChildProcess>& feeder) {
    // Temporary files a killed scada-tagd left are gone once the next one
    // is ready. Only those: the new one may be writing a file of its own
    // at that moment.
    const std::vector<std::string> stale = temporaries_in(retain());
    std::optional<RunningTagd> service = tagd();
    if (!service.has_value()) {
        return std::nullopt;
    }
    for (const std::string& name : stale) {
        EXPECT_FALSE(
            std::filesystem::exists(std::filesystem::path(retain()) / name))
            << name;
    }
    // A retainer left from the round before may have ended by itself: its
    // attach fails while no scada-tagd runs.
    if (feeder.has_value() && !feeder->send_signal(0)) {
        feeder.reset();
    }
    if (!feeder.has_value()) {
        std::optional<ChildProcess> started = retainer(1);
        if (!started.has_value()) {
            return std::nullopt;
        }
        feeder.emplace(std::move(*started));
        if (!feeder->wait_for_output(
                [](std::string_view out) { return has(out, "attached"); },
                WAIT_LIMIT)) {
            return std::nullopt;
        }
    }
    // A random point to kill at, not a wait for a condition.
    std::this_thread::sleep_for(after);
    if (feeder_survives) {
        kill(service->child);
    } else if (tagd_first) {
        kill(service->child);
        kill(*feeder);
        feeder.reset();
    } else {
        kill(*feeder);
        feeder.reset();
        kill(service->child);
    }
    return on_file();
}

TEST_F(Retained,
       SigkillOfEveryProcessAtRandomPointsNeverLeavesAnUnreadableFile) {
    // A fixed seed: the same kill points at every run, so that a failure
    // can be repeated.
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp): a fixed seed keeps the test repeatable
    std::mt19937 random(KILL_SEED);
    std::uniform_int_distribution<int> after(KILL_AFTER_MIN_MS,
                                             KILL_AFTER_MAX_MS);
    std::bernoulli_distribution tagd_first(TAGD_FIRST_ODDS);
    std::bernoulli_distribution feeder_survives(FEEDER_SURVIVES_ODDS);
    std::optional<ChildProcess> feeder;
    std::int64_t previous = -1;
    for (int round = 0; round < KILL_ROUNDS; ++round) {
        // Another tag set at every round: each scada-tagd builds a new
        // generation, and the one before retires.
        write_tag_set(true, round % 2 == 1);
        const std::int64_t value =
            kill_round(std::chrono::milliseconds(after(random)),
                       tagd_first(random), feeder_survives(random), feeder)
                .value_or(-1);
        // Every value comes back, and the counter never goes back.
        ASSERT_GE(value, 0) << "no valid retain file, round " << round;
        EXPECT_GE(value, previous) << "round " << round;
        previous = value;
    }
}

}  // namespace
