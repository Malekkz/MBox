// Integration tests for observer contexts with real processes (contract,
// section 11.4): scada-tagd and the writing processes run as processes of
// their own, and this process only observes. Everything this process maps
// from the database is then the observer's, so /proc/self/maps shows
// whether an observer ever maps a region writable.
//
// Every wait is on a condition with a limit.

#include "tag_processes_fixture.h"
#include "tagd_fixture.h"

#include "scada/sim/layout.h"
#include "scada/sim/pattern.h"
#include "scada/tag/scada_tag.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
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
// The writer, scada-sim: the process `load` with two owners of 10 tags
// each, every tag written WRITER_ROUNDS times at WRITER_RATE updates per
// second of both owners together (4,000 updates in 2 s).
constexpr std::uint64_t WRITER_RATE = 2'000;
constexpr std::uint64_t WRITER_ROUNDS = 200;
constexpr std::uint32_t WRITER_OWNERS = 2;
constexpr std::uint64_t WRITER_TAGS = 20;

scada::sim::Layout writer_layout() {
    return scada::sim::Layout{.prefix = "load",
                              .processes = 1,
                              .owners = WRITER_OWNERS,
                              .tags = WRITER_TAGS,
                              .patterns = {scada::sim::Pattern::CHECK}};
}
constexpr std::size_t OBSERVERS = 3;
// A subscription may first report the ring appearing.
constexpr int NEXT_ATTEMPTS = 3;

// The lines of /proc/self/maps that name a file under `dir`, except those
// that hold `except` (empty: none excepted).
std::vector<std::string> mappings_under(const std::string& dir,
                                        const std::string& except = {}) {
    std::ifstream maps("/proc/self/maps");
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find(dir + "/") != std::string::npos &&
            (except.empty() || line.find(except) == std::string::npos)) {
            lines.push_back(line);
        }
    }
    return lines;
}

// True if the permissions column of the line of /proc/self/maps `line`
// has no `w`.
bool read_only(const std::string& line) {
    std::istringstream fields(line);
    std::string range;
    std::string permissions;
    fields >> range >> permissions;
    return permissions.find('w') == std::string::npos;
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

// The next change of `sub`, within NEXT_ATTEMPTS calls, skipping the gap
// of a ring that appeared.
int next_change(scada_sub* sub, scada_change& change) {
    int code = SCADA_E_TIMEOUT;
    for (int attempt = 0; attempt < NEXT_ATTEMPTS && code != SCADA_OK;
         ++attempt) {
        code = scada_sub_next(sub, &change, WAIT_MS);
    }
    return code;
}

// What one observer of the writer's tag set sees: a change of load.w0, a
// tag it reads, and the process `load` up.
void sees_the_writer(scada_ctx* ctx, scada_sub* sub) {
    scada_change change{};
    std::uint32_t handle = SCADA_INVALID_HANDLE;
    scada_value value{};
    std::uint16_t load = 0;
    scada_proc_status_t proc{};
    // Each call in order (a braced list is evaluated left to right).
    const std::vector<int> codes{
        next_change(sub, change),
        scada_tag_resolve(ctx, "load.w0.t0", &handle),
        scada_tag_read(ctx, handle, &value),
        scada_proc_resolve(ctx, "load", &load),
        scada_proc_status(ctx, load, &proc),
    };
    EXPECT_EQ(codes, std::vector<int>(codes.size(), SCADA_OK));
    EXPECT_EQ(change.type, SCADA_TYPE_UINT64);
    EXPECT_EQ(proc.down, 0);
    EXPECT_NE(proc.pid, 0U);
}

// Observers, each with a subscription to load.w0, closed and detached
// when it goes out of scope.
class ObserversOfTheWriter {
public:
    ObserversOfTheWriter(const std::string& base, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            observers_.push_back(std::make_unique<Observed>(base));
            scada_sub* sub = nullptr;
            EXPECT_EQ(scada_sub_open(observers_.back()->ctx(), "load.w0", &sub),
                      SCADA_OK);
            subs_.push_back(sub);
        }
    }
    ~ObserversOfTheWriter() {
        for (scada_sub* sub : subs_) {
            scada_sub_close(sub);
        }
    }
    ObserversOfTheWriter(const ObserversOfTheWriter&) = delete;
    ObserversOfTheWriter& operator=(const ObserversOfTheWriter&) = delete;
    ObserversOfTheWriter(ObserversOfTheWriter&&) = delete;
    ObserversOfTheWriter& operator=(ObserversOfTheWriter&&) = delete;

    void each_sees_the_writer() const {
        for (std::size_t i = 0; i < subs_.size(); ++i) {
            SCOPED_TRACE(i);
            sees_the_writer(observers_[i]->ctx(), subs_[i]);
        }
    }

private:
    std::vector<std::unique_ptr<Observed>> observers_;
    std::vector<scada_sub*> subs_;
};

class ObserverProcesses : public TagProcesses {
protected:
    // Starts the writer, scada-sim on writer_layout(), and waits until it
    // attached.
    [[nodiscard]] std::optional<ChildProcess> attached_writer() const {
        return start_sim(writer_layout(),
                         {"--rate", std::to_string(WRITER_RATE), "--rounds",
                          std::to_string(WRITER_ROUNDS), "--notify", "each"});
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
};

TEST_F(ObserverProcesses, SeveralObserveALiveWriterBesideAnAttach) {
    write_sim_tag_set(writer_layout());
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> writer = attached_writer();
    if (!tagd.has_value() || !writer.has_value()) {
        return;
    }
    const ObserversOfTheWriter observers(base(), OBSERVERS);
    // A process of the tag set attaches while the observers are there.
    const Attached reader(base(), "reader");
    ASSERT_NE(reader.ctx(), nullptr);
    observers.each_sees_the_writer();
    // `reader` maps its own `proc` region read-write; everything else this
    // process maps from the database is the observers', and read-only.
    EXPECT_TRUE(
        std::ranges::all_of(mappings_under(base(), "/proc.reader"), read_only));
    EXPECT_TRUE(stops_cleanly(*writer));
    stop(*tagd);
}

// Opens everything an observer can open on the small tag set: a tag, a
// subscription with a wait set and its descriptor, a process, and a
// status, and checks that every mapping under `base` is read-only.
void maps_read_only(const std::string& base) {
    const Observed observer(base);
    scada_ctx* const ctx = observer.ctx();
    std::uint32_t level = SCADA_INVALID_HANDLE;
    scada_sub* sub = nullptr;
    scada_waitset* set = nullptr;
    int descriptor = -1;
    std::uint16_t index = 0;
    scada_status status{};
    // Each call in order (a braced list is evaluated left to right).
    const std::vector<int> codes{
        scada_tag_resolve(ctx, "plant.level", &level),
        scada_sub_open(ctx, "io", &sub),
        scada_waitset_create(ctx, &set),
        scada_waitset_add_sub(set, sub),
        scada_waitset_fd(set, &descriptor),
        scada_proc_resolve(ctx, "io", &index),
        scada_tag_status(ctx, &status),
    };
    EXPECT_EQ(codes, std::vector<int>(codes.size(), SCADA_OK));
    const std::vector<std::string> lines = mappings_under(base);
    EXPECT_FALSE(lines.empty());
    EXPECT_TRUE(std::ranges::all_of(lines, read_only));
    scada_waitset_destroy(set);
    scada_sub_close(sub);
}

TEST_F(ObserverProcesses, MapsOnlyReadOnlyAndReleasesEverything) {
    write_small_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    std::optional<ChildProcess> feeder = attached_io();
    if (!tagd.has_value() || !feeder.has_value()) {
        return;
    }
    maps_read_only(base());
    // Detached: nothing of the database is mapped any more.
    EXPECT_TRUE(mappings_under(base()).empty());
    EXPECT_TRUE(stops_cleanly(*feeder));
    stop(*tagd);
}

TEST_F(ObserverProcesses, AnObserverStartedBeforeScadaTagdGetsNoDatabase) {
    write_small_tag_set();
    const std::string base_dir = base();
    const scada_attach_opts options{.base_dir = base_dir.c_str(),
                                    .retain_dir = nullptr,
                                    .tagd_uid = std::int64_t{::geteuid()}};
    scada_ctx* ctx = nullptr;
    EXPECT_EQ(scada_tag_observe_opts(&options, &ctx), SCADA_E_IO);
    EXPECT_EQ(errno, ENOENT);
    EXPECT_EQ(ctx, nullptr);
}

// The handle of plant.level in `ctx`.
std::uint32_t resolve_level(scada_ctx* ctx) {
    std::uint32_t level = SCADA_INVALID_HANDLE;
    EXPECT_EQ(scada_tag_resolve(ctx, "plant.level", &level), SCADA_OK);
    return level;
}

// True if plant.level, read by `ctx`, has no OWNER_DOWN now.
bool owner_is_up(scada_ctx* ctx, std::uint32_t level) {
    scada_value value{};
    return scada_tag_read(ctx, level, &value) == SCADA_OK &&
           (value.quality_flags & SCADA_FLAG_OWNER_DOWN) == 0;
}

// True once plant.level, read by `ctx`, no longer has OWNER_DOWN, within
// WAIT_MS; scada_tag_status looks for the region again meanwhile.
bool owner_comes_up(scada_ctx* ctx, std::uint32_t level) {
    const Clock::time_point deadline =
        Clock::now() + std::chrono::milliseconds(WAIT_MS);
    while (Clock::now() < deadline) {
        scada_status status{};
        static_cast<void>(scada_tag_status(ctx, &status));
        if (owner_is_up(ctx, level)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
    }
    return false;
}

TEST_F(ObserverProcesses, FindsAnOwnerThatAttachesLater) {
    write_small_tag_set();
    std::optional<RunningTagd> tagd = ready_tagd();
    if (!tagd.has_value()) {
        return;
    }
    const Observed observer(base());
    // `io` has not attached: its tag reads as BAD with OWNER_DOWN.
    const std::uint32_t level = resolve_level(observer.ctx());
    EXPECT_FALSE(owner_is_up(observer.ctx(), level));
    std::optional<ChildProcess> feeder = attached_io();
    if (!feeder.has_value()) {
        return;
    }
    EXPECT_TRUE(owner_comes_up(observer.ctx(), level));
    EXPECT_TRUE(stops_cleanly(*feeder));
    stop(*tagd);
}

// Observes the database in `base` and expects generation `generation`,
// RUNNING, with scada-tagd up.
void observes_generation(const std::string& base, std::uint64_t generation) {
    const Observed observer(base);
    scada_status status{};
    EXPECT_TRUE(status_becomes(observer.ctx(), SCADA_OK, status));
    EXPECT_EQ(status.generation, generation);
}

TEST_F(ObserverProcesses, FollowsAGenerationChangeByObservingAgain) {
    write_small_tag_set();
    std::optional<RunningTagd> first = ready_tagd();
    if (!first.has_value()) {
        return;
    }
    auto observer = std::make_unique<Observed>(base());
    stop(*first);
    // Another tag set: scada-tagd creates generation 2 and retires 1.
    add_a_tag();
    std::optional<RunningTagd> second = ready_tagd();
    if (!second.has_value()) {
        return;
    }
    scada_status status{};
    EXPECT_TRUE(status_becomes(observer->ctx(), SCADA_E_RETIRING, status));
    EXPECT_EQ(status.generation, 1U);
    // Detach and observe again: generation 2, and no mapping of
    // generation 1 left.
    observer.reset();
    EXPECT_TRUE(mappings_under(base() + "/gen-1").empty());
    observes_generation(base(), 2);
    stop(*second);
}

}  // namespace
