// Integration tests for observer contexts with real processes (contract,
// section 11.4): scada-tagd and the writing processes run as processes of
// their own, and this process only observes. Everything this process maps
// from the database is then the observer's, so /proc/self/maps shows
// whether an observer ever maps a region writable.
//
// Every wait is on a condition with a limit.

#include "tag_processes_fixture.h"
#include "tagd_fixture.h"

#include "scada/tag/scada_tag.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace scada::test::tagd;
using scada::test::ChildProcess;
using Clock = std::chrono::steady_clock;

constexpr int WAIT_MS = 10'000;
constexpr int POLL_MS = 10;
// The writer: two owners of the large tag set, 10 tags each.
constexpr std::uint64_t WRITER_RATE = 2'000;
constexpr std::uint64_t WRITER_UPDATES = 4'000;
constexpr std::size_t OBSERVERS = 3;

// The lines of /proc/self/maps that name a file under `dir`.
std::vector<std::string> mappings_under(const std::string& dir) {
    std::ifstream maps("/proc/self/maps");
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find(dir + "/") != std::string::npos) {
            lines.push_back(line);
        }
    }
    return lines;
}

std::string permissions_of(const std::string& line) {
    std::istringstream fields(line);
    std::string range;
    std::string permissions;
    fields >> range >> permissions;
    return permissions;
}

// Calls scada_tag_status on `ctx` until it returns `wanted`, within
// WAIT_MS.
::testing::AssertionResult status_becomes(scada_ctx* ctx, int wanted,
                                          scada_status& status) {
    const Clock::time_point deadline =
        Clock::now() + std::chrono::milliseconds(WAIT_MS);
    int code = SCADA_OK;
    while (Clock::now() < deadline) {
        code = scada_tag_status(ctx, &status);
        if (code == wanted) {
            return ::testing::AssertionSuccess();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
    }
    return ::testing::AssertionFailure()
           << "scada_tag_status gave " << code << ", not " << wanted;
}

class ObserverProcesses : public TagProcesses {};

TEST_F(ObserverProcesses, SeveralObserveALiveWriterThroughReadOnlyMappings) {
    write_load_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    ASSERT_TRUE(tagd.has_value());
    std::optional<ChildProcess> writer = start_helper(
        "SCADA_TAG_WRITER_COMMAND",
        {base(), "load", "2", "10", std::to_string(WRITER_RATE),
         std::to_string(WRITER_UPDATES), "notify"});
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer->wait_for_output(attached, WAIT_LIMIT)) << writer->err();

    std::vector<std::unique_ptr<Observed>> observers;
    std::vector<scada_sub*> subs;
    for (std::size_t i = 0; i < OBSERVERS; ++i) {
        observers.push_back(std::make_unique<Observed>(base()));
        ASSERT_NE(observers.back()->ctx(), nullptr);
        scada_sub* sub = nullptr;
        ASSERT_EQ(scada_sub_open(observers.back()->ctx(), "load.w0", &sub),
                  SCADA_OK);
        subs.push_back(sub);
    }
    // A process of the tag set attaches while the observers are there.
    const Attached reader(base(), "reader");
    ASSERT_NE(reader.ctx(), nullptr);

    for (std::size_t i = 0; i < OBSERVERS; ++i) {
        scada_change change{};
        int code = SCADA_E_TIMEOUT;
        for (int attempt = 0; attempt < 3 && code != SCADA_OK; ++attempt) {
            code = scada_sub_next(subs[i], &change, WAIT_MS);
        }
        ASSERT_EQ(code, SCADA_OK) << "observer " << i;
        EXPECT_EQ(change.type, SCADA_TYPE_UINT64);
        std::uint32_t handle = SCADA_INVALID_HANDLE;
        ASSERT_EQ(scada_tag_resolve(observers[i]->ctx(), "load.w0.t0", &handle),
                  SCADA_OK);
        scada_value value{};
        EXPECT_EQ(scada_tag_read(observers[i]->ctx(), handle, &value),
                  SCADA_OK);
        std::uint16_t load = 0;
        ASSERT_EQ(scada_proc_resolve(observers[i]->ctx(), "load", &load),
                  SCADA_OK);
        scada_proc_status_t proc{};
        ASSERT_EQ(scada_proc_status(observers[i]->ctx(), load, &proc),
                  SCADA_OK);
        EXPECT_EQ(proc.down, 0);
        EXPECT_NE(proc.pid, 0U);
    }

    // This process attached `reader` too, so it maps that process's own
    // regions read-write; everything else under the base directory is the
    // observers', and is read-only.
    for (const std::string& line : mappings_under(base())) {
        if (line.find("/proc.reader") != std::string::npos) {
            continue;
        }
        EXPECT_EQ(permissions_of(line).find('w'), std::string::npos) << line;
    }

    for (scada_sub* sub : subs) {
        scada_sub_close(sub);
    }
    EXPECT_TRUE(stops_cleanly(*writer));
    stop(*tagd);
}

TEST_F(ObserverProcesses, OnlyReadOnlyMappingsWithoutAnAttach) {
    write_small_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    ASSERT_TRUE(tagd.has_value());
    std::optional<ChildProcess> io = attached_io();
    ASSERT_TRUE(io.has_value());
    {
        const Observed observer(base());
        ASSERT_NE(observer.ctx(), nullptr);
        std::uint32_t level = SCADA_INVALID_HANDLE;
        ASSERT_EQ(scada_tag_resolve(observer.ctx(), "plant.level", &level),
                  SCADA_OK);
        scada_sub* sub = nullptr;
        ASSERT_EQ(scada_sub_open(observer.ctx(), "io", &sub), SCADA_OK);
        scada_waitset* set = nullptr;
        ASSERT_EQ(scada_waitset_create(observer.ctx(), &set), SCADA_OK);
        ASSERT_EQ(scada_waitset_add_sub(set, sub), SCADA_OK);
        int descriptor = -1;
        ASSERT_EQ(scada_waitset_fd(set, &descriptor), SCADA_OK);
        std::uint16_t index = 0;
        ASSERT_EQ(scada_proc_resolve(observer.ctx(), "io", &index), SCADA_OK);
        scada_status status{};
        EXPECT_EQ(scada_tag_status(observer.ctx(), &status), SCADA_OK);

        const std::vector<std::string> lines = mappings_under(base());
        EXPECT_FALSE(lines.empty());
        for (const std::string& line : lines) {
            EXPECT_EQ(permissions_of(line).find('w'), std::string::npos)
                << line;
        }
        scada_waitset_destroy(set);
        scada_sub_close(sub);
    }
    // Detached: nothing of the database is mapped any more.
    EXPECT_TRUE(mappings_under(base()).empty());
    EXPECT_TRUE(stops_cleanly(*io));
    stop(*tagd);
}

TEST_F(ObserverProcesses, AnObserverStartedBeforeScadaTagdGetsNoDatabase) {
    write_small_tag_set();
    {
        scada_ctx* ctx = nullptr;
        const scada_attach_opts options{.base_dir = base().c_str(),
                                        .retain_dir = nullptr,
                                        .tagd_uid = std::int64_t{::geteuid()}};
        EXPECT_EQ(scada_tag_observe_opts(&options, &ctx), SCADA_E_IO);
        EXPECT_EQ(errno, ENOENT);
        EXPECT_EQ(ctx, nullptr);
    }
    std::optional<RunningTagd> tagd = ready_tagd();
    ASSERT_TRUE(tagd.has_value());
    const Observed observer(base());
    ASSERT_NE(observer.ctx(), nullptr);
    // `io` has not attached: its tag reads as BAD with OWNER_DOWN, with no
    // value.
    std::uint32_t level = SCADA_INVALID_HANDLE;
    ASSERT_EQ(scada_tag_resolve(observer.ctx(), "plant.level", &level),
              SCADA_OK);
    scada_value value{};
    ASSERT_EQ(scada_tag_read(observer.ctx(), level, &value), SCADA_OK);
    EXPECT_EQ(value.quality, SCADA_QUALITY_BAD);
    EXPECT_NE(value.quality_flags & SCADA_FLAG_OWNER_DOWN, 0);
    EXPECT_EQ(value.update_mono_ns, 0);
    // When it attaches, the observer finds its region within about a
    // second, from scada_tag_status.
    std::optional<ChildProcess> io = attached_io();
    ASSERT_TRUE(io.has_value());
    const Clock::time_point deadline =
        Clock::now() + std::chrono::milliseconds(WAIT_MS);
    while (Clock::now() < deadline) {
        scada_status status{};
        static_cast<void>(scada_tag_status(observer.ctx(), &status));
        ASSERT_EQ(scada_tag_read(observer.ctx(), level, &value), SCADA_OK);
        if ((value.quality_flags & SCADA_FLAG_OWNER_DOWN) == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
    }
    EXPECT_EQ(value.quality_flags & SCADA_FLAG_OWNER_DOWN, 0);
    EXPECT_TRUE(stops_cleanly(*io));
    stop(*tagd);
}

TEST_F(ObserverProcesses, FollowsAGenerationChangeByObservingAgain) {
    write_small_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    ASSERT_TRUE(tagd.has_value());
    auto observer = std::make_unique<Observed>(base());
    ASSERT_NE(observer->ctx(), nullptr);
    scada_status status{};
    EXPECT_TRUE(status_becomes(observer->ctx(), SCADA_OK, status));
    EXPECT_EQ(status.generation, 1U);
    stop(*tagd);

    // Another tag set: scada-tagd creates generation 2 and retires 1.
    std::string text;
    {
        std::ifstream in(tag_set_path());
        std::getline(in, text, '\0');
    }
    const std::string tag = R"({"name": "plant.level", "owner": "io", )";
    const std::size_t at = text.find(tag);
    ASSERT_NE(at, std::string::npos);
    text.insert(at, R"({"name": "plant.extra", "owner": "io", "type": "int64"}, )");
    {
        std::ofstream out(tag_set_path(), std::ios::trunc);
        out << text;
    }
    std::optional<RunningTagd> second = ready_tagd();
    ASSERT_TRUE(second.has_value());
    EXPECT_TRUE(status_becomes(observer->ctx(), SCADA_E_RETIRING, status));
    EXPECT_EQ(status.generation, 1U);

    // Detach and observe again: generation 2, with the new tag, and no
    // mapping of generation 1 left.
    observer.reset();
    EXPECT_TRUE(mappings_under(base() + "/gen-1").empty());
    observer = std::make_unique<Observed>(base());
    ASSERT_NE(observer->ctx(), nullptr);
    EXPECT_TRUE(status_becomes(observer->ctx(), SCADA_OK, status));
    EXPECT_EQ(status.generation, 2U);
    std::uint32_t extra = SCADA_INVALID_HANDLE;
    EXPECT_EQ(scada_tag_resolve(observer->ctx(), "plant.extra", &extra),
              SCADA_OK);
    observer.reset();
    stop(*second);
}

}  // namespace
