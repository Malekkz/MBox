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
//   attach succeeds. A RETAIN_NOW tag is on disk within 1 s, provided its
//   write, with the writes queued before it, takes at most 200 ms and no
//   check runs longer than its period (section 12, decision D-62).
// - The acceptance of issue #17: SIGKILL of every process at random points
//   never leaves an unreadable retain file. A kill keeps the page cache, so
//   this shows the atomic rename, not durability; the power-loss model of
//   the unit tests (retain_power_loss_test.cpp) covers the fsync order.

#include "retain_file.h"
#include "tagd_fixture.h"

#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/types.h"
#include "scada/tagd/tagd.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal numbers are declared here, not in <csignal>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
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

    // Decision D-59, point 4: processes tagd, io and web; plant.count
    // (INT64, retain) owned by io, or with `moved` by web, which then is
    // the only owner. Another tag set each way.
    void write_moved_tag_set(bool moved) const {
        const std::string uid = std::to_string(::geteuid());
        const std::string owner = moved ? "web" : "io";
        std::ofstream file(root_ / "tag-set.json");
        file << R"({"format": 1, "heartbeat_check_ms": )" << CHECK_MS
             << R"(, "processes": [{"name": "tagd", "uid": )" << uid
             << R"(}, {"name": "io", "uid": )" << uid
             << R"(}, {"name": "web", "uid": )" << uid
             << R"(}], "owners": [{"name": ")" << owner << R"(", "process": ")"
             << owner << R"(", "change_ring_capacity": 1024}], "queues": [], )"
             << R"("tags": [{"name": "plant.count", "owner": ")" << owner
             << R"(", "type": "int64", "retain": true}]})" << "\n";
    }

    // The retainer as the process `process`.
    [[nodiscard]] std::optional<ChildProcess> retainer_as(
        const std::string& process, int period_ms) const {
        const std::vector<std::string> command =
            command_from("SCADA_TAG_RETAINER_COMMAND",
                         {base_.string(), retain_, process, "plant.count",
                          std::to_string(period_ms)});
        std::optional<ChildProcess> child = ChildProcess::start(command);
        EXPECT_TRUE(child.has_value());
        return child;
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

    // Waits until plant.count holds at least `least` in shared memory, as
    // an observer context reads it (contract 11.4), within `limit`.
    [[nodiscard]] bool tag_reaches(std::int64_t least,
                                   std::chrono::milliseconds limit) const {
        const scada_attach_opts options{.base_dir = base_.c_str(),
                                        .retain_dir = nullptr,
                                        .tagd_uid = std::int64_t{::geteuid()}};
        scada_ctx* raw = nullptr;
        if (scada_tag_observe_opts(&options, &raw) != SCADA_OK) {
            return false;
        }
        const std::unique_ptr<scada_ctx, decltype(&scada_tag_detach)> ctx(
            raw, &scada_tag_detach);
        std::uint32_t handle = SCADA_INVALID_HANDLE;
        if (scada_tag_resolve(ctx.get(), "plant.count", &handle) != SCADA_OK) {
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            scada_value value{};
            // An INT64 value holds the bits of an int64_t.
            if (scada_tag_read(ctx.get(), handle, &value) == SCADA_OK &&
                static_cast<std::int64_t>(value.value) >= least) {
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
// RESTORED and the LOCAL_TIME the library set at its write, which a
// restored value keeps (contract, section 12; decision D-59), and the old
// generation is gone.
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
        !has(out.substr(out.rfind("attached")), "quality=1 flags=3072")) {
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
    // A value written after it, so that only the write right before the
    // change can give the restored value. On a loaded machine the 10 ms
    // period of the retainer can pass the change without one (issue #73).
    ASSERT_TRUE(tag_reaches(2, WAIT_LIMIT));
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
    // Its own record and MESSAGE_ID (decision D-59).
    EXPECT_TRUE(logs(*service, R"(retain file ignored SUBJECT="io.bin")") &&
                logs(*service, "586ec48106f748609721eef8957c56d5"));
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
    // The kill point counts from a valid file on disk. Later rounds find
    // the file of the round before; the first finds none until scada-tagd
    // has written the first value of the owner, which a loaded machine
    // can delay past the kill point (issue #73). Without it the round
    // would test nothing: no file was ever written to leave unreadable.
    if (!file_reaches(0, WAIT_LIMIT)) {
        ADD_FAILURE() << "no retain file within the wait limit: "
                      << feeder->out() << service->child.err();
        return std::nullopt;
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

namespace {

constexpr std::int64_t OLD_VALUE = 41;
constexpr std::int64_t OLD_WRITTEN_TIME = 1'234'567;
// The worst case is 12 s (a stop during the 5-s wait of the start, the 5-s
// bound of the stop, 2 s for the log); this test sees about 5 s. Below the
// 15 s of TimeoutStopSec, with a margin over the worst case.
constexpr std::chrono::seconds UNSAVED_EXIT_LIMIT{14};

// Decision D-59, point 5: a retain write that hangs in fsync at the stop.
class HungWrite : public Retained {
protected:
    // io.bin with plant.count = OLD_VALUE.
    [[nodiscard]] ::testing::AssertionResult old_file() const {
        scada::tag::RetainFile old;
        old.values = {{"plant.count", scada::tag::TagType::INT64,
                       static_cast<std::uint64_t>(OLD_VALUE), 0}};
        old.written_time_ns = OLD_WRITTEN_TIME;
        const scada::Fd directory(
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open has no other form
            ::open(retain().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        std::string detail;
        if (!scada::tag::write_retain_file(directory.get(), "io", old, detail)
                 .ok()) {
            return ::testing::AssertionFailure() << detail;
        }
        return ::testing::AssertionSuccess();
    }

    // scada-tagd whose retain writer hangs in fsync.
    [[nodiscard]] std::optional<RunningTagd> hung_tagd() const {
        return ready_tagd(
            (base().parent_path() / "tag-set.json").string(), base().string(),
            QUIET_WATCHDOG,
            {"--retain-interval-ms", std::to_string(RETAIN_INTERVAL_MS)},
            "SCADA_TAGD_HUNG_WRITER_COMMAND");
    }

    // Waits until the temporary file of `tagd` is there: its write reached
    // fsync.
    [[nodiscard]] ::testing::AssertionResult write_hangs(
        const RunningTagd& tagd) const {
        const std::vector<std::string> wanted{".tmp.io.bin." +
                                              std::to_string(tagd.child.pid())};
        const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
        while (temporaries_in(retain()) != wanted &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        if (temporaries_in(retain()) != wanted) {
            return ::testing::AssertionFailure() << "no temporary file";
        }
        return ::testing::AssertionSuccess();
    }

    // Stops `tagd`, which must exit with EXIT_UNSAVED within
    // UNSAVED_EXIT_LIMIT and log both ERROR records with their IDs.
    [[nodiscard]] static ::testing::AssertionResult ends_unsaved(
        RunningTagd& tagd) {
        const auto sent = std::chrono::steady_clock::now();
        if (!tagd.child.send_signal(SIGTERM)) {
            return ::testing::AssertionFailure() << "no signal";
        }
        const std::optional<int> code = tagd.child.wait_for_exit(WAIT_LIMIT);
        const auto took = std::chrono::steady_clock::now() - sent;
        const std::string& err = tagd.child.err();
        if (code != scada::tagd::EXIT_UNSAVED || took >= UNSAVED_EXIT_LIMIT) {
            return ::testing::AssertionFailure()
                   << "exit " << code.value_or(-1) << " after "
                   << std::chrono::duration_cast<std::chrono::milliseconds>(
                          took)
                          .count()
                   << " ms: " << err;
        }
        // One record of unsaved values (review N7), naming io and the file
        // of it that stays.
        const std::size_t unsaved = err.find("values unsaved at exit");
        if (unsaved == std::string::npos ||
            err.find("values unsaved at exit", unsaved + 1) !=
                std::string::npos ||
            !has(err, R"(SUBJECT="io")") ||
            !has(err, R"(UNSAVED_OWNERS="io")") ||
            !has(err, R"(UNSAVED_COUNT="1")") ||
            // The file that stays, as scada-tagd knew it (review D4 of #93).
            !has(err, R"(KEPT_WRITTEN_TIME_NS="1234567")") ||
            !has(err, "7cf5c1c8535f44f6a297db538666d7a3") ||
            !has(err, "0031dfe3391142b0b9fa995b07115e10")) {
            return ::testing::AssertionFailure() << "records missing: " << err;
        }
        return ::testing::AssertionSuccess();
    }

    [[nodiscard]] ::testing::AssertionResult next_start_removes(
        const std::string& name) const;

    // io.bin still holds OLD_VALUE and `temporary` is the one temporary
    // file: the hung write changed nothing under the real name.
    [[nodiscard]] ::testing::AssertionResult left_as_it_was(
        const std::string& temporary) const {
        const std::optional<std::int64_t> value = on_file();
        const std::vector<std::string> temporaries = temporaries_in(retain());
        if (value != OLD_VALUE ||
            temporaries != std::vector<std::string>{temporary}) {
            return ::testing::AssertionFailure()
                   << "value " << value.value_or(-1) << ", "
                   << temporaries.size() << " temporary files";
        }
        return ::testing::AssertionSuccess();
    }
};

// A later scada-tagd removes the temporary file `name` the hung write
// left. Only that one: the new scada-tagd writes io's current values at
// its first check, through a temporary file of its own.
::testing::AssertionResult HungWrite::next_start_removes(
    const std::string& name) const {
    std::optional<RunningTagd> next = tagd();
    if (!next.has_value()) {
        return ::testing::AssertionFailure() << "no scada-tagd";
    }
    const std::vector<std::string> left = temporaries_in(retain());
    stop(*next);
    if (std::ranges::find(left, name) != left.end()) {
        return ::testing::AssertionFailure() << name << " is still there";
    }
    return ::testing::AssertionSuccess();
}

// scada-tagd waits 5 s, logs `values unsaved at exit`, and exits at once
// with code 3; the old file is intact, and the next start removes the
// temporary file the hung write left.
TEST_F(HungWrite, AtTheStopTagdEndsWithCode3AndTheOldFileStays) {
    ASSERT_TRUE(old_file());
    std::optional<RunningTagd> hung = hung_tagd();
    std::optional<ChildProcess> feeder = retainer(SLOW_PERIOD_MS);
    if (!hung.has_value() || !feeder.has_value()) {
        return;
    }
    // io restores the old value; its first write after the change hangs
    // in fsync, after the temporary file was written.
    ASSERT_TRUE(prints(*feeder, "attached value=41") && write_hangs(*hung))
        << feeder->out() << hung->child.err();
    EXPECT_TRUE(ends_unsaved(*hung));
    // The old file is intact; the temporary file is still there until the
    // next start.
    const std::string temporary =
        ".tmp.io.bin." + std::to_string(hung->child.pid());
    EXPECT_TRUE(left_as_it_was(temporary));
    EXPECT_TRUE(stops_cleanly(*feeder) && next_start_removes(temporary));
}

// Decision D-59, point 4: plant.count moves from io to web, and web
// attaches late. The first scada-tagd writes io's value F at its stop (the
// first write); io goes on counting in generation 1 while no scada-tagd
// runs. After the old generation is removed, web restores the last value io
// wrote there, from io's FINAL file (the second write); while io hangs in
// the old generation, web's wait runs out and it restores F.
class MovedTag : public Retained {
protected:
    void SetUp() override {
        Retained::SetUp();
        write_moved_tag_set(false);
    }

    // Waits until io has written a value above `value` (an observer reads
    // plant.count).
    [[nodiscard]] bool writes_beyond(std::int64_t value) const {
        const std::string base_dir = base().string();
        const scada_attach_opts opts{.base_dir = base_dir.c_str(),
                                     .retain_dir = nullptr,
                                     .tagd_uid = ::geteuid()};
        scada_ctx* ctx = nullptr;
        if (scada_tag_observe_opts(&opts, &ctx) != SCADA_OK) {
            return false;
        }
        std::uint32_t handle = SCADA_INVALID_HANDLE;
        bool beyond = false;
        if (scada_tag_resolve(ctx, "plant.count", &handle) == SCADA_OK) {
            const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
            while (!beyond && std::chrono::steady_clock::now() < deadline) {
                scada_value read{};
                beyond = scada_tag_read(ctx, handle, &read) == SCADA_OK &&
                         static_cast<std::int64_t>(read.value) > value;
                std::this_thread::yield();
            }
        }
        scada_tag_detach(ctx);
        return beyond;
    }

    // With `feeder` (io) counting under `first`: waits for io's first
    // write, stops `first`, which writes io's value, waits until io has
    // counted beyond it and writes the tag set after the move. The value on
    // file, or -1 on a failure.
    [[nodiscard]] std::int64_t counted_past_the_stop(ChildProcess& feeder,
                                                     RunningTagd& first) const {
        if (!prints(feeder, "attached value=0")) {
            return -1;
        }
        // io prints its attach before it writes: a stop before its first
        // write leaves nothing of io's to save (#143).
        if (!tag_reaches(1, WAIT_LIMIT)) {
            return -1;
        }
        stop(first);
        const std::int64_t flushed = on_file().value_or(-1);
        if (flushed < 1 || !writes_beyond(flushed)) {
            return -1;
        }
        write_moved_tag_set(true);
        return flushed;
    }

    // The last value io wrote before it left the old generation, once that
    // generation is removed by `second`; -1 on a failure.
    [[nodiscard]] static std::int64_t left_value(ChildProcess& feeder,
                                                 RunningTagd& second) {
        if (!feeder.wait_for_output(reattached, WAIT_LIMIT) ||
            !logs(second, "retired generation removed")) {
            return -1;
        }
        return last_number(feeder.out(), "leaving last=").value_or(-1);
    }

    // Starts web, which must restore `value`, and stops it.
    [[nodiscard]] ::testing::AssertionResult web_restores(
        std::int64_t value) const {
        std::optional<ChildProcess> web = retainer_as("web", SLOW_PERIOD_MS);
        if (!web.has_value()) {
            return ::testing::AssertionFailure() << "no web";
        }
        const ::testing::AssertionResult restored =
            prints(*web, "attached value=" + std::to_string(value) + " ");
        const ::testing::AssertionResult stopped = stops_cleanly(*web);
        return !restored ? restored : stopped;
    }
};

TEST_F(MovedTag, AfterTheOldGenerationIsRemovedTheLastValueIsRestored) {
    std::optional<RunningTagd> first = tagd();
    std::optional<ChildProcess> feeder = retainer_as("io", FAST_PERIOD_MS);
    if (!first.has_value() || !feeder.has_value()) {
        return;
    }
    const std::int64_t flushed = counted_past_the_stop(*feeder, *first);
    ASSERT_GE(flushed, 1);
    std::optional<RunningTagd> second = tagd();
    if (!second.has_value()) {
        return;
    }
    // io leaves generation 1 at once (it follows RETIRING); its FINAL file
    // is written and generation 1 removed.
    const std::int64_t last = left_value(*feeder, *second);
    ASSERT_GT(last, flushed) << feeder->out();
    EXPECT_TRUE(web_restores(last));
    EXPECT_TRUE(stops_cleanly(*feeder));
    stop(*second);
}

TEST_F(MovedTag,
       WhileTheFormerOwnerHangsTheWaitRunsOutAndTheFirstValueIsRestored) {
    std::optional<RunningTagd> first = tagd();
    std::optional<ChildProcess> feeder = retainer_as("io", FAST_PERIOD_MS);
    if (!first.has_value() || !feeder.has_value()) {
        return;
    }
    const std::int64_t flushed = counted_past_the_stop(*feeder, *first);
    ASSERT_GE(flushed, 1);
    // io hangs: it holds its lock and stays RUNNING in generation 1, with
    // a value newer than the file.
    ASSERT_TRUE(feeder->send_signal(SIGSTOP));
    std::optional<RunningTagd> second = tagd();
    if (!second.has_value()) {
        return;
    }
    // The FINAL wait of 2 s runs out; the file holds the first write.
    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(web_restores(flushed));
    EXPECT_GE(std::chrono::steady_clock::now() - start,
              std::chrono::seconds{2});
    static_cast<void>(kill(*feeder));
    stop(*second);
}

}  // namespace
