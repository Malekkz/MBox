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
#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal numbers are declared here, not in <csignal>
#include <sys/stat.h>
#include <unistd.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
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
constexpr int KILL_AFTER_MIN_MS = 20;
constexpr int KILL_AFTER_MAX_MS = 400;

// The number after `key` in the last line of `text` that has it.
std::optional<std::int64_t> last_number(std::string_view text,
                                        std::string_view key) {
    const std::size_t at = text.rfind(key);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const std::string_view rest = text.substr(at + key.size());
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

private:
    std::filesystem::path root_;
    std::filesystem::path base_;
    std::string retain_;
};

bool has(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

TEST_F(Retained, Conformance21AValueWrittenRightBeforeTheChangeIsRestored) {
    std::optional<RunningTagd> first = tagd();
    ASSERT_TRUE(first.has_value());
    std::optional<ChildProcess> io = retainer(10);
    ASSERT_TRUE(io.has_value());
    ASSERT_TRUE(io->wait_for_output(
        [](std::string_view out) { return has(out, "attached value=0"); },
        WAIT_LIMIT));
    // The first write of the owner is on disk; later ones wait for the hour.
    ASSERT_TRUE(file_reaches(1, WAIT_LIMIT));
    stop(*first);

    // The tag set changes; the retainer goes on counting meanwhile.
    write_tag_set(false, true);
    std::optional<RunningTagd> second = tagd();
    ASSERT_TRUE(second.has_value());
    const auto ready = std::chrono::steady_clock::now();
    ASSERT_TRUE(io->wait_for_output(
        [](std::string_view out) {
            const std::size_t left = out.find("leaving last=");
            return left != std::string_view::npos &&
                   out.find("attached", left) != std::string_view::npos &&
                   out.find('\n', out.find("attached", left)) !=
                       std::string_view::npos;
        },
        WAIT_LIMIT))
        << io->out() << second->child.err();
    EXPECT_LT(std::chrono::steady_clock::now() - ready, CHANGE_LIMIT);
    const std::optional<std::int64_t> last =
        last_number(io->out(), "leaving last=");
    const std::optional<std::int64_t> restored =
        last_number(io->out(), "attached value=");
    ASSERT_TRUE(last.has_value());
    EXPECT_GT(*last, 1);
    EXPECT_EQ(restored, last) << io->out();
    // UNCERTAIN with RESTORED (contract, section 12).
    EXPECT_TRUE(has(io->out().substr(io->out().rfind("attached")),
                    "quality=1 flags=1024"))
        << io->out();
    EXPECT_TRUE(logs(*second, "retired generation removed"));
    EXPECT_FALSE(std::filesystem::exists(base() / "gen-1"));
    EXPECT_TRUE(stops_cleanly(*io));
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
    ASSERT_TRUE(service.has_value());
    std::optional<ChildProcess> io = retainer(10);
    ASSERT_TRUE(io.has_value());
    ASSERT_TRUE(io->wait_for_output(
        [](std::string_view out) { return has(out, "\n"); }, WAIT_LIMIT));
    // BAD with NOT_INIT: nothing restored.
    EXPECT_TRUE(has(io->out(), "attached value=0 quality=2 flags=16"))
        << io->out();
    EXPECT_TRUE(logs(*service, R"(retained values SUBJECT="io.bin")"));
    // The next write replaces it with a valid file.
    EXPECT_TRUE(file_reaches(1, WAIT_LIMIT));
    EXPECT_TRUE(stops_cleanly(*io));
    stop(*service);
}

TEST_F(Retained, Conformance25ARetainNowTagIsOnDiskWithinOneSecond) {
    write_tag_set(true, false);
    std::optional<RunningTagd> service = tagd();
    ASSERT_TRUE(service.has_value());
    std::optional<ChildProcess> io = retainer(100);
    ASSERT_TRUE(io.has_value());
    ASSERT_TRUE(file_reaches(3, WAIT_LIMIT));
    // The last write of the retainer comes at most one period before this.
    ASSERT_TRUE(stops_cleanly(*io));
    const auto stopped = std::chrono::steady_clock::now();
    const std::optional<std::int64_t> last =
        last_number(io->out(), "stopped last=");
    ASSERT_TRUE(last.has_value());
    EXPECT_TRUE(file_reaches(*last, RETAIN_NOW_LIMIT));
    EXPECT_LT(std::chrono::steady_clock::now() - stopped, RETAIN_NOW_LIMIT);
    EXPECT_EQ(on_file(), last);
    stop(*service);
}

TEST_F(Retained,
       SigkillOfEveryProcessAtRandomPointsNeverLeavesAnUnreadableFile) {
    write_tag_set(true, false);
    std::mt19937 random(KILL_SEED);
    std::uniform_int_distribution<int> after(KILL_AFTER_MIN_MS,
                                             KILL_AFTER_MAX_MS);
    std::bernoulli_distribution tagd_first(0.5);
    std::int64_t previous = -1;
    for (int round = 0; round < KILL_ROUNDS; ++round) {
        // Temporary files a killed scada-tagd left are gone once the next
        // one is ready. Only those: the new one may be writing a file of
        // its own at that moment.
        std::vector<std::string> stale;
        for (const auto& entry :
             std::filesystem::directory_iterator(retain())) {
            if (entry.path().filename().string().starts_with(".tmp.")) {
                stale.push_back(entry.path().filename().string());
            }
        }
        std::optional<RunningTagd> service = tagd();
        ASSERT_TRUE(service.has_value());
        for (const std::string& name : stale) {
            EXPECT_FALSE(
                std::filesystem::exists(std::filesystem::path(retain()) / name))
                << name << ", round " << round;
        }
        std::optional<ChildProcess> io = retainer(1);
        ASSERT_TRUE(io.has_value());
        ASSERT_TRUE(io->wait_for_output(
            [](std::string_view out) { return has(out, "attached"); },
            WAIT_LIMIT));
        std::this_thread::sleep_for(std::chrono::milliseconds(after(
            random)));  // NOLINT(readability-magic-numbers): a random point to kill at, not a wait for a condition
        if (tagd_first(random)) {
            kill(service->child);
            kill(*io);
        } else {
            kill(*io);
            kill(service->child);
        }
        const std::optional<std::int64_t> value = on_file();
        ASSERT_TRUE(value.has_value()) << "round " << round;
        // Every value comes back, and the counter never goes back.
        EXPECT_GE(*value, previous) << "round " << round;
        previous = *value;
    }
}

}  // namespace
