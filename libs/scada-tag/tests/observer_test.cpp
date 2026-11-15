// Unit tests for observer contexts (docs/contracts/tag-database.md, section
// 11.4): an observer reads like a process that owns nothing, refuses every
// call that would write, writes nothing to the generation directory, and
// reports a generation change. The side of scada-tagd and the processes
// are in the test process (AttachFixture); the tests with other processes
// and other users are in tests/integration.

#include "attach_fixture.h"
#include "layout.h"
#include "temp_database.h"

#include "scada/common/clock.h"
#include "scada/tag/handle.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <grp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using scada::tag::GenerationState;
using scada::tag::testing::AttachFixture;
using scada::tag::testing::OWNER_IO;
using scada::tag::testing::PROCESS_GUEST;
using scada::tag::testing::PROCESS_IO;
using scada::tag::testing::PROCESS_TAGD;
using scada::tag::testing::SLOT_LABEL;
using scada::tag::testing::SLOT_LEVEL;

// How long a test waits for a condition that the library promises within
// a second, in ms.
constexpr int WAIT_LIMIT_MS = 5000;
constexpr int POLL_MS = 10;
// A wait that must time out, in ms.
constexpr int SHORT_WAIT_MS = 20;
// A level a process writes.
constexpr double LEVEL = 4.25;
// The user id of nobody on Linux, which is in no group of a test.
constexpr uid_t NOBODY = 65534;
constexpr std::int64_t NS_PER_S = 1'000'000'000;
// Processes of the fixture's tag set.
constexpr std::uint16_t PROCESS_COUNT = 4;

// A context of the C interface, detached when it goes out of scope.
class Context {
public:
    Context() = default;
    ~Context() { scada_tag_detach(ctx_); }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    [[nodiscard]] scada_ctx* get() const { return ctx_; }
    [[nodiscard]] scada_ctx** out() { return &ctx_; }
    void detach() {
        scada_tag_detach(ctx_);
        ctx_ = nullptr;
    }

private:
    scada_ctx* ctx_ = nullptr;
};

// What is known of one file of the generation directory: what must not
// change when an observer looks at it.
struct FileFacts {
    ino_t inode = 0;
    off_t size = 0;
    std::int64_t mtime_ns = 0;
    mode_t mode = 0;
    uid_t owner = 0;
    std::string bytes;
};

bool operator==(const FileFacts& left, const FileFacts& right) {
    return std::tie(left.inode, left.size, left.mtime_ns, left.mode, left.owner,
                    left.bytes) == std::tie(right.inode, right.size,
                                            right.mtime_ns, right.mode,
                                            right.owner, right.bytes);
}

std::string contents_of(const std::filesystem::path& path, off_t size) {
    std::string bytes;
    if (size <= 0) {
        return bytes;
    }
    std::ifstream file(path, std::ios::binary);
    bytes.resize(static_cast<std::size_t>(size));
    file.read(bytes.data(), size);
    EXPECT_TRUE(file.good()) << path;
    return bytes;
}

std::map<std::string, FileFacts> snapshot(const std::filesystem::path& dir) {
    std::map<std::string, FileFacts> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        struct stat info {};
        EXPECT_EQ(::lstat(entry.path().c_str(), &info), 0) << entry.path();
        FileFacts facts;
        facts.inode = info.st_ino;
        facts.size = info.st_size;
        facts.mtime_ns = (std::int64_t{info.st_mtim.tv_sec} * NS_PER_S) +
                         info.st_mtim.tv_nsec;
        facts.mode = info.st_mode;
        facts.owner = info.st_uid;
        if (S_ISREG(info.st_mode)) {
            facts.bytes = contents_of(entry.path(), info.st_size);
        }
        files[entry.path().filename().string()] = facts;
    }
    return files;
}

// The lines of /proc/self/maps that name a file under `dir`, sorted.
std::vector<std::string> mappings_under(const std::filesystem::path& dir) {
    std::ifstream maps("/proc/self/maps");
    std::vector<std::string> lines;
    std::string line;
    const std::string prefix = dir.string() + "/";
    while (std::getline(maps, line)) {
        if (line.find(prefix) != std::string::npos) {
            lines.push_back(line);
        }
    }
    std::ranges::sort(lines);
    return lines;
}

// True if the permissions column of the line of /proc/self/maps `line`,
// for example "r--s", has no `w`.
bool read_only(const std::string& line) {
    std::istringstream fields(line);
    std::string range;
    std::string permissions;
    fields >> range >> permissions;
    return permissions.find('w') == std::string::npos;
}

scada_value float_value(double number, std::uint8_t quality,
                        std::uint16_t flags) {
    scada_value value{};
    value.type = SCADA_TYPE_FLOAT64;
    value.quality = quality;
    value.quality_flags = flags;
    value.value = std::bit_cast<std::uint64_t>(number);
    return value;
}

class Observer : public AttachFixture {
protected:
    [[nodiscard]] scada_attach_opts opts() const {
        return {.base_dir = base().c_str(),
                .retain_dir = nullptr,
                .tagd_uid = std::int64_t{uid()}};
    }

    [[nodiscard]] int observe(Context& context) const {
        const scada_attach_opts options = opts();
        return scada_tag_observe_opts(&options, context.out());
    }

    [[nodiscard]] int attach(const char* process, Context& context) const {
        const scada_attach_opts options = opts();
        return scada_tag_attach_opts(process, &options, context.out());
    }

    // Attaches `io` as `feeder` and writes LEVEL to plant.level, as a
    // process does.
    void write_level(Context& feeder) const {
        ASSERT_EQ(attach("io", feeder), SCADA_OK);
        std::uint32_t level = SCADA_INVALID_HANDLE;
        ASSERT_EQ(scada_tag_resolve(feeder.get(), "plant.level", &level),
                  SCADA_OK);
        const scada_value value = float_value(LEVEL, SCADA_QUALITY_GOOD, 0);
        ASSERT_EQ(scada_tag_write(feeder.get(), level, &value), SCADA_OK);
        ASSERT_EQ(scada_tag_notify(feeder.get()), SCADA_OK);
    }

    // The value of plant.level as `ctx` reads it, resolved first.
    static scada_value read_level(scada_ctx* ctx) {
        std::uint32_t level = SCADA_INVALID_HANDLE;
        EXPECT_EQ(scada_tag_resolve(ctx, "plant.level", &level), SCADA_OK);
        scada_value value{};
        EXPECT_EQ(scada_tag_read(ctx, level, &value), SCADA_OK);
        return value;
    }

    // Starts the side of scada-tagd again with one tag more: another
    // config_hash, so it creates generation 2 and retires generation 1.
    void start_second_generation() {
        drop_host();
        scada::tag::TagSet set = tag_set();
        set.tags.push_back({.name = "plant.extra",
                            .owner = "io",
                            .type = scada::tag::TagType::INT64});
        start_host(set);
    }
};

TEST_F(Observer, OpensWithoutAProcessEntry) {
    start_host();
    host().heartbeat(scada::monotonic_ns());
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    scada_status status{};
    EXPECT_EQ(scada_tag_status(observer.get(), &status), SCADA_OK);
    EXPECT_EQ(status.generation, 1U);
    EXPECT_EQ(status.generation_state, SCADA_GENERATION_RUNNING);
    EXPECT_EQ(status.process_index, SCADA_NO_PROCESS);
    EXPECT_EQ(status.tagd_down, 0);
    EXPECT_EQ(status.regions_created + status.regions_reused, 0U);
    EXPECT_EQ(status.owner_count, 1U);
    EXPECT_EQ(status.process_count, PROCESS_COUNT);
}

TEST_F(Observer, RefusesWrites) {
    start_host();
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    std::uint32_t level = SCADA_INVALID_HANDLE;
    std::uint32_t label = SCADA_INVALID_HANDLE;
    ASSERT_EQ(scada_tag_resolve(observer.get(), "plant.level", &level),
              SCADA_OK);
    ASSERT_EQ(scada_tag_resolve(observer.get(), "plant.label", &label),
              SCADA_OK);
    const scada_value value = float_value(LEVEL, SCADA_QUALITY_GOOD, 0);
    EXPECT_EQ(scada_tag_write(observer.get(), level, &value),
              SCADA_E_NOT_OWNER);
    EXPECT_EQ(scada_tag_write(observer.get(), SCADA_INVALID_HANDLE, &value),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_write_many(observer.get(), &level, 1, &value),
              SCADA_E_NOT_OWNER);
    EXPECT_EQ(scada_tag_write_string(observer.get(), label, "x", 1,
                                     SCADA_QUALITY_GOOD, 0, 0),
              SCADA_E_NOT_OWNER);
    scada_tag_info_t info{};
    ASSERT_EQ(scada_tag_info(observer.get(), level, &info), SCADA_OK);
    EXPECT_EQ(info.owned, 0);
}

TEST_F(Observer, HasNoHeartbeatAndWakesNobody) {
    start_host();
    Context feeder;
    write_level(feeder);
    feeder.detach();
    const auto before = snapshot(generation_dir());
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    EXPECT_EQ(scada_tag_heartbeat(observer.get()), SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_notify(observer.get()), SCADA_E_INVALID);
    observer.detach();
    EXPECT_EQ(snapshot(generation_dir()), before);
}

// Everything an observer does that reads: resolve, read, process status,
// a subscription and a wait set, status.
void read_everything(scada_ctx* ctx) {
    std::uint32_t level = SCADA_INVALID_HANDLE;
    scada_value value{};
    std::uint16_t index = 0;
    scada_proc_status_t proc{};
    scada_sub* sub = nullptr;
    scada_waitset* set = nullptr;
    scada_change change{};
    scada_status status{};
    // Each call in order (a braced list is evaluated left to right), with
    // the code it gives. The owner stopped before the subscription opened,
    // so nothing comes; scada-tagd started a moment ago, so it is up.
    const std::vector<int> codes{
        scada_tag_resolve(ctx, "plant.level", &level),
        scada_tag_read(ctx, level, &value),
        scada_proc_resolve(ctx, "io", &index),
        scada_proc_status(ctx, index, &proc),
        scada_sub_open(ctx, "io", &sub),
        scada_waitset_create(ctx, &set),
        scada_waitset_add_sub(set, sub),
        scada_waitset_add_cycle(set, PROCESS_IO),
        scada_sub_next(sub, &change, SHORT_WAIT_MS),
        scada_waitset_wait(set, SHORT_WAIT_MS),
        scada_tag_status(ctx, &status),
    };
    EXPECT_EQ(codes,
              (std::vector<int>{SCADA_OK, SCADA_OK, SCADA_OK, SCADA_OK,
                                SCADA_OK, SCADA_OK, SCADA_OK, SCADA_OK,
                                SCADA_E_TIMEOUT, SCADA_E_TIMEOUT, SCADA_OK}));
    scada_waitset_destroy(set);
    scada_sub_close(sub);
}

TEST_F(Observer, LeavesTheGenerationDirectoryAsItWas) {
    start_host();
    Context feeder;
    write_level(feeder);
    feeder.detach();
    const auto before = snapshot(generation_dir());
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    read_everything(observer.get());
    observer.detach();
    // Names, inodes, sizes, modes, owners, times and bytes: all as before.
    EXPECT_EQ(snapshot(generation_dir()), before);
}

TEST_F(Observer, MapsOnlyReadOnly) {
    start_host();
    Context feeder;
    write_level(feeder);
    feeder.detach();
    const auto mapped_before = mappings_under(generation_dir());
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    read_everything(observer.get());
    // The host of this test maps the header read-write; every mapping the
    // observer added is read-only.
    std::vector<std::string> added;
    std::ranges::set_difference(mappings_under(generation_dir()), mapped_before,
                                std::back_inserter(added));
    EXPECT_FALSE(added.empty());
    EXPECT_TRUE(std::ranges::all_of(added, read_only));
    observer.detach();
    EXPECT_EQ(mappings_under(generation_dir()), mapped_before);
}

TEST_F(Observer, ReadsWhatAProcessWrites) {
    start_host();
    Context feeder;
    write_level(feeder);
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    scada_value value = read_level(observer.get());
    EXPECT_EQ(value.quality, SCADA_QUALITY_GOOD);
    EXPECT_EQ(std::bit_cast<double>(value.value), LEVEL);
    // A clean stop of the owner reads as down at once (section 10.1).
    feeder.detach();
    value = read_level(observer.get());
    EXPECT_EQ(value.quality, SCADA_QUALITY_BAD);
    EXPECT_NE(value.quality_flags & SCADA_FLAG_OWNER_DOWN, 0);
}

TEST_F(Observer, WatchesWhatAProcessWrites) {
    start_host();
    Context feeder;
    write_level(feeder);
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    scada_sub* sub = nullptr;
    ASSERT_EQ(scada_sub_open(observer.get(), "io", &sub), SCADA_OK);
    std::uint32_t level = SCADA_INVALID_HANDLE;
    ASSERT_EQ(scada_tag_resolve(feeder.get(), "plant.level", &level), SCADA_OK);
    const scada_value next =
        float_value(LEVEL + 1, SCADA_QUALITY_UNCERTAIN, SCADA_FLAG_STALE);
    ASSERT_EQ(scada_tag_write(feeder.get(), level, &next), SCADA_OK);
    ASSERT_EQ(scada_tag_notify(feeder.get()), SCADA_OK);
    scada_change change{};
    EXPECT_EQ(scada_sub_next(sub, &change, WAIT_LIMIT_MS), SCADA_OK);
    EXPECT_EQ(change.handle, level);
    EXPECT_EQ(change.quality, SCADA_QUALITY_UNCERTAIN);
    EXPECT_NE(change.quality_flags & SCADA_FLAG_STALE, 0);
    EXPECT_EQ(std::bit_cast<double>(change.value), LEVEL + 1);
    scada_sub_close(sub);
}

TEST_F(Observer, SeveralRunBesideALiveAttachOfTheSameTagSet) {
    start_host();
    Context feeder;
    write_level(feeder);
    std::array<Context, 3> observers;
    for (Context& observer : observers) {
        ASSERT_EQ(observe(observer), SCADA_OK);
        EXPECT_EQ(read_level(observer.get()).quality, SCADA_QUALITY_GOOD);
    }
    // The lock of the process is still the process's alone, and it keeps
    // working.
    Context again;
    EXPECT_EQ(attach("io", again), SCADA_E_BUSY);
    EXPECT_EQ(scada_tag_heartbeat(feeder.get()), SCADA_OK);
    feeder.detach();
    EXPECT_EQ(attach("io", again), SCADA_OK);
}

TEST_F(Observer, ResolvesProcesses) {
    start_host();
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    std::uint16_t index = 0;
    EXPECT_EQ(scada_proc_resolve(observer.get(), "nobody", &index),
              SCADA_E_NOT_FOUND);
    EXPECT_EQ(scada_proc_resolve(observer.get(), "No Name", &index),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_proc_resolve(observer.get(), "io", &index), SCADA_OK);
    EXPECT_EQ(index, PROCESS_IO);
    scada_proc_status_t status{};
    EXPECT_EQ(scada_proc_status(observer.get(), PROCESS_COUNT, &status),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_proc_status(observer.get(), SCADA_NO_PROCESS, &status),
              SCADA_E_INVALID);
}

TEST_F(Observer, ReportsARunningProcess) {
    start_host();
    Context feeder;
    write_level(feeder);
    ASSERT_EQ(scada_tag_heartbeat(feeder.get()), SCADA_OK);
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    std::uint16_t index = 0;
    ASSERT_EQ(scada_proc_resolve(observer.get(), "io", &index), SCADA_OK);
    scada_proc_status_t status{};
    ASSERT_EQ(scada_proc_status(observer.get(), index, &status), SCADA_OK);
    EXPECT_STREQ(std::data(status.name), "io");
    EXPECT_EQ(status.state, SCADA_PROC_RUNNING);
    EXPECT_EQ(status.down_reason, SCADA_DOWN_NONE);
    EXPECT_EQ(status.down, 0);
    EXPECT_GE(status.heartbeat, 2U);  // attach and the call above
    EXPECT_GE(status.cycle_seq, 1U);  // write_level called notify
    EXPECT_EQ(status.pid, static_cast<std::uint32_t>(::getpid()));
    EXPECT_EQ(status.uid, uid());
    EXPECT_EQ(status.heartbeat_timeout_ms,
              scada::tag::DEFAULT_HEARTBEAT_TIMEOUT_MS);
}

TEST_F(Observer, ReportsTheFieldsOfAProcessEntry) {
    start_host();
    Context feeder;
    write_level(feeder);
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    std::uint16_t index = 0;
    ASSERT_EQ(scada_proc_resolve(observer.get(), "io", &index), SCADA_OK);
    scada_proc_status_t status{};
    ASSERT_EQ(scada_proc_status(observer.get(), index, &status), SCADA_OK);
    EXPECT_EQ(status.process_index, PROCESS_IO);
    EXPECT_EQ(status.realtime, 0);
    EXPECT_GT(status.heartbeat_mono_ns, 0);
    EXPECT_GE(status.heartbeat_age_ns, 0);
    EXPECT_TRUE(std::ranges::all_of(
        status.reserved, [](std::uint8_t byte) { return byte == 0; }));
}

TEST_F(Observer, ReportsAStoppedProcessBeforeScadaTagdDoes) {
    start_host();
    Context feeder;
    write_level(feeder);
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    std::uint16_t index = 0;
    ASSERT_EQ(scada_proc_resolve(observer.get(), "io", &index), SCADA_OK);
    feeder.detach();
    scada_proc_status_t status{};
    ASSERT_EQ(scada_proc_status(observer.get(), index, &status), SCADA_OK);
    EXPECT_EQ(status.down, 1);
    EXPECT_EQ(status.down_reason, SCADA_DOWN_NOT_RUNNING);
    EXPECT_EQ(status.state, SCADA_PROC_STOPPING);
    EXPECT_EQ(status.marked_down, 0);
    static_cast<void>(host().check(scada::monotonic_ns()));
    ASSERT_EQ(scada_proc_status(observer.get(), index, &status), SCADA_OK);
    EXPECT_EQ(status.marked_down, 1);
}

TEST_F(Observer, RefusesTheStatusOfAProcessThatWasNotResolved) {
    start_host();
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    // Not down: not judged, with a code that says so, and the name to
    // resolve it by.
    scada_proc_status_t status{};
    EXPECT_EQ(scada_proc_status(observer.get(), PROCESS_GUEST, &status),
              SCADA_E_INVALID);
    EXPECT_STREQ(std::data(status.name), "guest");
    EXPECT_EQ(status.down_reason, SCADA_DOWN_NOT_RESOLVED);
    EXPECT_EQ(status.down, 1);
    EXPECT_EQ(status.heartbeat + status.state, 0U);
    std::uint16_t index = 0;
    ASSERT_EQ(scada_proc_resolve(observer.get(), "guest", &index), SCADA_OK);
    EXPECT_EQ(scada_proc_status(observer.get(), index, &status), SCADA_OK);
    EXPECT_EQ(status.down_reason, SCADA_DOWN_REGION_MISSING);
}

TEST_F(Observer, NeedsNoResolveForScadaTagd) {
    start_host();
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    host().heartbeat(scada::monotonic_ns());
    scada_proc_status_t status{};
    ASSERT_EQ(scada_proc_status(observer.get(), PROCESS_TAGD, &status),
              SCADA_OK);
    EXPECT_EQ(status.down, 0);
}

TEST_F(Observer, DescribesOwners) {
    start_host();
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    scada_owner_info_t owner{};
    ASSERT_EQ(scada_owner_info(observer.get(), OWNER_IO, &owner), SCADA_OK);
    EXPECT_STREQ(std::data(owner.name), "io");
    EXPECT_EQ(owner.tag_count, scada::tag::testing::IO_TAG_COUNT);
    EXPECT_EQ(owner.change_ring_capacity, scada::tag::MIN_CHANGE_RING_CAPACITY);
    EXPECT_EQ(owner.process_index, PROCESS_IO);
    EXPECT_EQ(owner.owned, 0);
    EXPECT_EQ(owner.reserved0 + owner.reserved1, 0U);
    EXPECT_EQ(scada_owner_info(observer.get(), 1, &owner), SCADA_E_INVALID);
}

TEST_F(Observer, ListsEveryTagThroughOwnersAndHandles) {
    start_host();
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    scada_owner_info_t owner{};
    ASSERT_EQ(scada_owner_info(observer.get(), OWNER_IO, &owner), SCADA_OK);
    std::vector<std::string> names;
    for (std::uint32_t slot = 0; slot < owner.tag_count; ++slot) {
        scada_tag_info_t info{};
        EXPECT_EQ(scada_tag_info(observer.get(),
                                 scada_tag_handle(OWNER_IO, slot), &info),
                  SCADA_OK);
        names.emplace_back(std::data(info.name));
    }
    EXPECT_EQ(names, (std::vector<std::string>{"plant.label", "plant.level",
                                               "plant.note", "plant.running"}));
    // The slot past the last is no tag.
    scada_tag_info_t info{};
    EXPECT_EQ(
        scada_tag_info(observer.get(),
                       scada_tag_handle(OWNER_IO, owner.tag_count), &info),
        SCADA_E_INVALID);
}

TEST(TagHandle, PacksOwnerAndSlot) {
    EXPECT_EQ(scada_tag_handle(0, 0), 0U);
    EXPECT_EQ(scada_tag_handle(1, SLOT_LEVEL),
              (std::uint32_t{1} << scada::tag::HANDLE_SLOT_BITS) | SLOT_LEVEL);
    EXPECT_EQ(scada_tag_handle(2, SLOT_LABEL),
              scada::tag::pack_handle(2, SLOT_LABEL).value_or(0));
    EXPECT_EQ(scada_tag_handle(scada::tag::MAX_OWNERS, 0),
              SCADA_INVALID_HANDLE);
    EXPECT_EQ(scada_tag_handle(0, scada::tag::MAX_TAGS_PER_OWNER),
              SCADA_INVALID_HANDLE);
    EXPECT_EQ(scada_tag_handle(SCADA_NO_PROCESS, 0), SCADA_INVALID_HANDLE);
}

TEST_F(Observer, ReportsARetiringGeneration) {
    start_host();
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    start_second_generation();
    scada_status status{};
    EXPECT_EQ(scada_tag_status(observer.get(), &status), SCADA_E_RETIRING);
    EXPECT_EQ(status.generation, 1U);
    EXPECT_EQ(status.generation_state, SCADA_GENERATION_RETIRING);
}

TEST_F(Observer, ObservesAgainAfterAGenerationChange) {
    start_host();
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    start_second_generation();
    // The old mappings go with the old context.
    observer.detach();
    EXPECT_TRUE(mappings_under(generation_dir(1)).empty());
    ASSERT_EQ(observe(observer), SCADA_OK);
    scada_status status{};
    EXPECT_EQ(scada_tag_status(observer.get(), &status), SCADA_OK);
    EXPECT_EQ(status.generation, 2U);
    std::uint32_t extra = SCADA_INVALID_HANDLE;
    EXPECT_EQ(scada_tag_resolve(observer.get(), "plant.extra", &extra),
              SCADA_OK);
}

// Calls scada_tag_status on `ctx` until it gives SCADA_E_RETIRING, within
// WAIT_LIMIT_MS.
bool becomes_retiring(scada_ctx* ctx, scada_status& status) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(WAIT_LIMIT_MS);
    while (std::chrono::steady_clock::now() < deadline) {
        if (scada_tag_status(ctx, &status) == SCADA_E_RETIRING) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
    }
    return false;
}

TEST_F(Observer, ReportsACurrentThatNamesAnotherGeneration) {
    start_host();
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    start_second_generation();
    // Retiring is best effort (section 11.3): here generation 1 never got
    // the state, so only `current`, read again at most once a second,
    // tells.
    set_generation_state(GenerationState::RUNNING);
    scada_status status{};
    EXPECT_TRUE(becomes_retiring(observer.get(), status));
    EXPECT_EQ(status.generation, 1U);
    EXPECT_EQ(status.generation_state, SCADA_GENERATION_RUNNING);
    // It stays reported.
    EXPECT_EQ(scada_tag_status(observer.get(), &status), SCADA_E_RETIRING);
}

TEST_F(Observer, ReportsACurrentThatStaysUnreadable) {
    start_host();
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    // The database taken down under the observer: no `current` any more.
    std::filesystem::remove(base() / "current");
    scada_status status{};
    EXPECT_TRUE(becomes_retiring(observer.get(), status));
    EXPECT_EQ(status.generation, 1U);
}

TEST_F(Observer, ReportsABaseDirectoryThatWasSetUpAgain) {
    start_host();
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    drop_host();
    // The directory removed and created anew, as at a reboot: the old one
    // still holds generation 1 for the observer's descriptor.
    std::filesystem::rename(base(), root() / "old-base");
    ASSERT_EQ(::mkdir(base().c_str(), scada::tag::testing::PRIVATE_DIR_MODE),
              0);
    ASSERT_EQ(::chmod(base().c_str(), BASE_MODE), 0);
    start_host();
    scada_status status{};
    EXPECT_TRUE(becomes_retiring(observer.get(), status));
    observer.detach();
    ASSERT_EQ(observe(observer), SCADA_OK);
    EXPECT_EQ(scada_tag_status(observer.get(), &status), SCADA_OK);
}

TEST_F(Observer, NeedsADatabase) {
    Context observer;
    // No generation yet: scada-tagd has not started.
    EXPECT_EQ(observe(observer), SCADA_E_IO);
    EXPECT_EQ(errno, ENOENT);
    EXPECT_EQ(observer.get(), nullptr);
    const scada_attach_opts nowhere{.base_dir = "/nonexistent/scada",
                                    .retain_dir = nullptr,
                                    .tagd_uid = std::int64_t{uid()}};
    EXPECT_EQ(scada_tag_observe_opts(&nowhere, observer.out()), SCADA_E_IO);
    EXPECT_EQ(errno, ENOENT);
}

TEST_F(Observer, NeedsARunningGeneration) {
    start_host();
    set_generation_state(GenerationState::BUILDING);
    Context observer;
    EXPECT_EQ(observe(observer), SCADA_E_IO);
    EXPECT_EQ(errno, EAGAIN);
}

TEST_F(Observer, ReadsATagWhoseOwnerNeverAttachedAsOwnerDown) {
    start_host();
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    const scada_value value = read_level(observer.get());
    EXPECT_EQ(value.quality, SCADA_QUALITY_BAD);
    EXPECT_NE(value.quality_flags & SCADA_FLAG_OWNER_DOWN, 0);
    EXPECT_EQ(value.update_mono_ns, 0);
    scada_sub* sub = nullptr;
    ASSERT_EQ(scada_sub_open(observer.get(), "io", &sub), SCADA_OK);
    scada_change change{};
    EXPECT_EQ(scada_sub_next(sub, &change, 0), SCADA_E_TIMEOUT);
    scada_sub_close(sub);
}

// The return code of an observe by a child that has become nobody, in no
// group of the database, or -1 if the child could not become nobody.
int observe_as_nobody(const scada_attach_opts& options) {
    const pid_t child = ::fork();
    if (child == 0) {
        const std::array<gid_t, 1> groups{NOBODY};
        int code = -1;
        if (::setgroups(groups.size(), groups.data()) == 0 &&
            ::setgid(NOBODY) == 0 && ::setuid(NOBODY) == 0) {
            scada_ctx* ctx = nullptr;
            code = scada_tag_observe_opts(&options, &ctx);
            scada_tag_detach(ctx);
        }
        ::_exit(code);
    }
    int status = 0;
    if (child < 0 || ::waitpid(child, &status, 0) != child ||
        !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

TEST_F(Observer, ACallerWhoMayNotReadTheBaseDirectoryGetsPermission) {
    start_host();
    if (::geteuid() == 0) {
        // Root reads anything: the caller is a child that is nobody.
        EXPECT_EQ(observe_as_nobody(opts()), SCADA_E_PERMISSION);
        return;
    }
    // Without privilege, the same refusal by the kernel: no search
    // permission on the base directory. Attach gets the same code.
    ASSERT_EQ(::chmod(base().c_str(), 0), 0);
    Context observer;
    EXPECT_EQ(observe(observer), SCADA_E_PERMISSION);
    Context feeder;
    EXPECT_EQ(attach("io", feeder), SCADA_E_PERMISSION);
    ASSERT_EQ(::chmod(base().c_str(), BASE_MODE), 0);
}

TEST_F(Observer, RefusesBadOptions) {
    start_host();
    EXPECT_EQ(scada_tag_observe(nullptr), SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_observe_opts(nullptr, nullptr), SCADA_E_INVALID);
    const scada_attach_opts bad_uid{
        .base_dir = base().c_str(), .retain_dir = nullptr, .tagd_uid = -2};
    Context observer;
    EXPECT_EQ(scada_tag_observe_opts(&bad_uid, observer.out()),
              SCADA_E_INVALID);
    EXPECT_EQ(observer.get(), nullptr);
}

TEST_F(Observer, RefusesNullArguments) {
    start_host();
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    scada_owner_info_t owner{};
    EXPECT_EQ(scada_owner_info(nullptr, 0, &owner), SCADA_E_INVALID);
    EXPECT_EQ(scada_owner_info(observer.get(), 0, nullptr), SCADA_E_INVALID);
    std::uint16_t index = 0;
    EXPECT_EQ(scada_proc_resolve(nullptr, "io", &index), SCADA_E_INVALID);
    EXPECT_EQ(scada_proc_resolve(observer.get(), nullptr, &index),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_proc_resolve(observer.get(), "io", nullptr),
              SCADA_E_INVALID);
    scada_proc_status_t status{};
    EXPECT_EQ(scada_proc_status(nullptr, 0, &status), SCADA_E_INVALID);
    EXPECT_EQ(scada_proc_status(observer.get(), 0, nullptr), SCADA_E_INVALID);
}

TEST_F(Observer, AProcessSeesItsOwnOwnerAndProcess) {
    start_host();
    Context feeder;
    write_level(feeder);
    scada_owner_info_t owner{};
    ASSERT_EQ(scada_owner_info(feeder.get(), OWNER_IO, &owner), SCADA_OK);
    EXPECT_EQ(owner.owned, 1);
    std::uint16_t index = 0;
    ASSERT_EQ(scada_proc_resolve(feeder.get(), "io", &index), SCADA_OK);
    scada_proc_status_t status{};
    ASSERT_EQ(scada_proc_status(feeder.get(), index, &status), SCADA_OK);
    EXPECT_EQ(status.down, 0);
    EXPECT_EQ(status.state, SCADA_PROC_RUNNING);
}

}  // namespace
