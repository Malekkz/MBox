// Unit tests for observer contexts (docs/contracts/tag-database.md, section
// 11.4): an observer reads like a process that owns nothing, refuses every
// call that would write, writes nothing to the generation directory, and
// reports a generation change. The side of scada-tagd and the processes
// are in the test process (AttachFixture); the tests with other processes
// and other users are in tests/integration.

#include "attach_fixture.h"

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
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace {

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

    bool operator==(const FileFacts&) const = default;
};

std::map<std::string, FileFacts> snapshot(const std::filesystem::path& dir) {
    std::map<std::string, FileFacts> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        struct stat info {};
        EXPECT_EQ(::lstat(entry.path().c_str(), &info), 0) << entry.path();
        FileFacts facts;
        facts.inode = info.st_ino;
        facts.size = info.st_size;
        facts.mtime_ns = (std::int64_t{info.st_mtim.tv_sec} * 1'000'000'000) +
                         info.st_mtim.tv_nsec;
        facts.mode = info.st_mode;
        facts.owner = info.st_uid;
        if (S_ISREG(info.st_mode)) {
            std::ifstream in(entry.path(), std::ios::binary);
            facts.bytes.assign(std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>());
        }
        files[entry.path().filename().string()] = facts;
    }
    return files;
}

// The lines of /proc/self/maps that name a file under `dir`.
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

// The permissions column of a line of /proc/self/maps, for example
// "r--s".
std::string permissions_of(const std::string& line) {
    std::istringstream fields(line);
    std::string range;
    std::string permissions;
    fields >> range >> permissions;
    return permissions;
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

    // Attaches `io` and writes LEVEL to plant.level, as a process does.
    void write_level(Context& io) const {
        ASSERT_EQ(attach("io", io), SCADA_OK);
        std::uint32_t level = SCADA_INVALID_HANDLE;
        ASSERT_EQ(scada_tag_resolve(io.get(), "plant.level", &level), SCADA_OK);
        scada_value value{};
        value.type = SCADA_TYPE_FLOAT64;
        value.quality = SCADA_QUALITY_GOOD;
        value.value = std::bit_cast<std::uint64_t>(LEVEL);
        ASSERT_EQ(scada_tag_write(io.get(), level, &value), SCADA_OK);
        ASSERT_EQ(scada_tag_notify(io.get()), SCADA_OK);
    }

    // A tag set with one tag more: another config_hash, so that starting
    // scada-tagd with it creates generation 2.
    [[nodiscard]] static scada::tag::TagSet larger_tag_set() {
        scada::tag::TagSet set = tag_set();
        set.tags.push_back({.name = "plant.extra",
                            .owner = "io",
                            .type = scada::tag::TagType::INT64});
        return set;
    }
};

TEST_F(Observer, OpensWithoutAProcessEntry) {
    start_host();
    host().heartbeat(scada::monotonic_ns());
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    ASSERT_NE(observer.get(), nullptr);

    scada_status status{};
    EXPECT_EQ(scada_tag_status(observer.get(), &status), SCADA_OK);
    EXPECT_EQ(status.generation, 1U);
    EXPECT_EQ(status.generation_state, SCADA_GENERATION_RUNNING);
    EXPECT_EQ(status.process_index, SCADA_NO_PROCESS);
    EXPECT_EQ(status.tagd_down, 0);
    EXPECT_EQ(status.regions_created, 0U);
    EXPECT_EQ(status.regions_reused, 0U);
    EXPECT_EQ(status.records_repaired, 0U);
    EXPECT_EQ(status.owner_count, 1U);
    EXPECT_EQ(status.process_count, 4U);
}

TEST_F(Observer, RefusesEveryCallThatWouldWrite) {
    start_host();
    Context io;
    write_level(io);
    io.detach();
    const auto before = snapshot(generation_dir());

    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    std::uint32_t level = SCADA_INVALID_HANDLE;
    std::uint32_t label = SCADA_INVALID_HANDLE;
    ASSERT_EQ(scada_tag_resolve(observer.get(), "plant.level", &level),
              SCADA_OK);
    ASSERT_EQ(scada_tag_resolve(observer.get(), "plant.label", &label),
              SCADA_OK);
    scada_value value{};
    value.type = SCADA_TYPE_FLOAT64;
    value.quality = SCADA_QUALITY_GOOD;
    EXPECT_EQ(scada_tag_write(observer.get(), level, &value),
              SCADA_E_NOT_OWNER);
    EXPECT_EQ(scada_tag_write(observer.get(), SCADA_INVALID_HANDLE, &value),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_write_many(observer.get(), &level, 1, &value),
              SCADA_E_NOT_OWNER);
    EXPECT_EQ(scada_tag_write_string(observer.get(), label, "x", 1,
                                     SCADA_QUALITY_GOOD, 0, 0),
              SCADA_E_NOT_OWNER);
    EXPECT_EQ(scada_tag_heartbeat(observer.get()), SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_notify(observer.get()), SCADA_E_INVALID);

    scada_tag_info_t info{};
    ASSERT_EQ(scada_tag_info(observer.get(), level, &info), SCADA_OK);
    EXPECT_EQ(info.owned, 0);
    observer.detach();
    EXPECT_EQ(snapshot(generation_dir()), before);
}

TEST_F(Observer, LeavesTheGenerationDirectoryAsItWas) {
    start_host();
    Context io;
    write_level(io);
    io.detach();
    const auto before = snapshot(generation_dir());
    const auto mapped_before = mappings_under(generation_dir());

    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    std::uint32_t level = SCADA_INVALID_HANDLE;
    ASSERT_EQ(scada_tag_resolve(observer.get(), "plant.level", &level),
              SCADA_OK);
    scada_value value{};
    EXPECT_EQ(scada_tag_read(observer.get(), level, &value), SCADA_OK);
    std::uint16_t index = 0;
    ASSERT_EQ(scada_proc_resolve(observer.get(), "io", &index), SCADA_OK);
    scada_proc_status_t proc{};
    EXPECT_EQ(scada_proc_status(observer.get(), index, &proc), SCADA_OK);
    scada_sub* sub = nullptr;
    ASSERT_EQ(scada_sub_open(observer.get(), "io", &sub), SCADA_OK);
    scada_waitset* set = nullptr;
    ASSERT_EQ(scada_waitset_create(observer.get(), &set), SCADA_OK);
    ASSERT_EQ(scada_waitset_add_sub(set, sub), SCADA_OK);
    ASSERT_EQ(scada_waitset_add_cycle(set, PROCESS_IO), SCADA_OK);
    // The owner stopped before the subscription opened: nothing comes.
    scada_change change{};
    EXPECT_EQ(scada_sub_next(sub, &change, SHORT_WAIT_MS), SCADA_E_TIMEOUT);
    EXPECT_EQ(scada_waitset_wait(set, SHORT_WAIT_MS), SCADA_E_TIMEOUT);
    scada_status status{};
    static_cast<void>(scada_tag_status(observer.get(), &status));

    // Every mapping the observer made is read-only.
    const auto mapped = mappings_under(generation_dir());
    std::vector<std::string> added;
    std::ranges::set_difference(mapped, mapped_before,
                                std::back_inserter(added));
    EXPECT_FALSE(added.empty());
    for (const std::string& line : added) {
        EXPECT_EQ(permissions_of(line).find('w'), std::string::npos) << line;
    }

    scada_waitset_destroy(set);
    scada_sub_close(sub);
    observer.detach();
    // Names, inodes, sizes, modes, owners, times and bytes: all as before.
    EXPECT_EQ(snapshot(generation_dir()), before);
    // And the observer's mappings are gone.
    EXPECT_EQ(mappings_under(generation_dir()), mapped_before);
}

TEST_F(Observer, ReadsAndWatchesWhatAProcessWrites) {
    start_host();
    Context io;
    write_level(io);
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);

    std::uint32_t level = SCADA_INVALID_HANDLE;
    ASSERT_EQ(scada_tag_resolve(observer.get(), "plant.level", &level),
              SCADA_OK);
    scada_value value{};
    ASSERT_EQ(scada_tag_read(observer.get(), level, &value), SCADA_OK);
    EXPECT_EQ(value.quality, SCADA_QUALITY_GOOD);
    EXPECT_EQ(std::bit_cast<double>(value.value), LEVEL);

    scada_sub* sub = nullptr;
    ASSERT_EQ(scada_sub_open(observer.get(), "io", &sub), SCADA_OK);
    std::uint32_t io_level = SCADA_INVALID_HANDLE;
    ASSERT_EQ(scada_tag_resolve(io.get(), "plant.level", &io_level), SCADA_OK);
    scada_value next{};
    next.type = SCADA_TYPE_FLOAT64;
    next.quality = SCADA_QUALITY_UNCERTAIN;
    next.quality_flags = SCADA_FLAG_STALE;
    next.value = std::bit_cast<std::uint64_t>(LEVEL + 1);
    ASSERT_EQ(scada_tag_write(io.get(), io_level, &next), SCADA_OK);
    ASSERT_EQ(scada_tag_notify(io.get()), SCADA_OK);
    scada_change change{};
    ASSERT_EQ(scada_sub_next(sub, &change, WAIT_LIMIT_MS), SCADA_OK);
    EXPECT_EQ(change.handle, level);
    EXPECT_EQ(change.quality, SCADA_QUALITY_UNCERTAIN);
    EXPECT_NE(change.quality_flags & SCADA_FLAG_STALE, 0);
    EXPECT_EQ(std::bit_cast<double>(change.value), LEVEL + 1);
    scada_sub_close(sub);

    // A clean stop of the owner reads as down at once (section 10.1).
    io.detach();
    ASSERT_EQ(scada_tag_read(observer.get(), level, &value), SCADA_OK);
    EXPECT_EQ(value.quality, SCADA_QUALITY_BAD);
    EXPECT_NE(value.quality_flags & SCADA_FLAG_OWNER_DOWN, 0);
}

TEST_F(Observer, SeveralRunBesideALiveAttachOfTheSameTagSet) {
    start_host();
    Context io;
    write_level(io);
    Context first;
    Context second;
    Context third;
    EXPECT_EQ(observe(first), SCADA_OK);
    EXPECT_EQ(observe(second), SCADA_OK);
    EXPECT_EQ(observe(third), SCADA_OK);
    // The process lock is still the process's alone.
    Context io_again;
    EXPECT_EQ(attach("io", io_again), SCADA_E_BUSY);
    for (Context* observer : {&first, &second, &third}) {
        std::uint32_t level = SCADA_INVALID_HANDLE;
        ASSERT_EQ(scada_tag_resolve(observer->get(), "plant.level", &level),
                  SCADA_OK);
        scada_value value{};
        ASSERT_EQ(scada_tag_read(observer->get(), level, &value), SCADA_OK);
        EXPECT_EQ(value.quality, SCADA_QUALITY_GOOD);
    }
    // The process keeps working, and can be attached again after it stops.
    EXPECT_EQ(scada_tag_heartbeat(io.get()), SCADA_OK);
    io.detach();
    EXPECT_EQ(attach("io", io_again), SCADA_OK);
}

TEST_F(Observer, ProcessStatus) {
    start_host();
    Context io;
    write_level(io);
    ASSERT_EQ(scada_tag_heartbeat(io.get()), SCADA_OK);
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);

    std::uint16_t index = 0;
    EXPECT_EQ(scada_proc_resolve(observer.get(), "nobody", &index),
              SCADA_E_NOT_FOUND);
    EXPECT_EQ(scada_proc_resolve(observer.get(), "No Name", &index),
              SCADA_E_INVALID);
    ASSERT_EQ(scada_proc_resolve(observer.get(), "io", &index), SCADA_OK);
    EXPECT_EQ(index, PROCESS_IO);

    scada_proc_status_t status{};
    ASSERT_EQ(scada_proc_status(observer.get(), index, &status), SCADA_OK);
    EXPECT_STREQ(status.name, "io");
    EXPECT_EQ(status.process_index, PROCESS_IO);
    EXPECT_EQ(status.state, SCADA_PROC_RUNNING);
    EXPECT_EQ(status.down, 0);
    EXPECT_EQ(status.down_reason, SCADA_DOWN_NONE);
    EXPECT_GE(status.heartbeat, 2U);  // attach and the call above
    EXPECT_GT(status.heartbeat_mono_ns, 0);
    EXPECT_GE(status.heartbeat_age_ns, 0);
    EXPECT_EQ(status.pid, static_cast<std::uint32_t>(::getpid()));
    EXPECT_EQ(status.uid, uid());
    EXPECT_GE(status.cycle_seq, 1U);  // write_level called notify
    EXPECT_EQ(status.heartbeat_timeout_ms,
              scada::tag::DEFAULT_HEARTBEAT_TIMEOUT_MS);
    EXPECT_EQ(status.realtime, 0);
    for (const std::uint8_t byte : status.reserved) {
        EXPECT_EQ(byte, 0);
    }

    // The `down` flag of scada-tagd follows its check, the judgement of
    // the library does not wait for it.
    io.detach();
    ASSERT_EQ(scada_proc_status(observer.get(), index, &status), SCADA_OK);
    EXPECT_EQ(status.down, 1);
    EXPECT_EQ(status.down_reason, SCADA_DOWN_NOT_RUNNING);
    EXPECT_EQ(status.state, SCADA_PROC_STOPPING);
    EXPECT_EQ(status.marked_down, 0);
    static_cast<void>(host().check(scada::monotonic_ns()));
    ASSERT_EQ(scada_proc_status(observer.get(), index, &status), SCADA_OK);
    EXPECT_EQ(status.marked_down, 1);

    // A process that was never resolved reads as missing; scada-tagd was
    // asked for by observe itself.
    ASSERT_EQ(scada_proc_status(observer.get(), PROCESS_GUEST, &status),
              SCADA_OK);
    EXPECT_STREQ(status.name, "guest");
    EXPECT_EQ(status.down_reason, SCADA_DOWN_REGION_MISSING);
    EXPECT_EQ(status.heartbeat, 0U);
    EXPECT_EQ(status.state, 0U);
    host().heartbeat(scada::monotonic_ns());
    ASSERT_EQ(scada_proc_status(observer.get(), PROCESS_TAGD, &status),
              SCADA_OK);
    EXPECT_EQ(status.down, 0);

    EXPECT_EQ(scada_proc_status(observer.get(), 4, &status), SCADA_E_INVALID);
    EXPECT_EQ(scada_proc_status(observer.get(), SCADA_NO_PROCESS, &status),
              SCADA_E_INVALID);
}

TEST_F(Observer, OwnerInfoAndHandlesListEveryTag) {
    start_host();
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    scada_status status{};
    static_cast<void>(scada_tag_status(observer.get(), &status));
    ASSERT_EQ(status.owner_count, 1U);

    scada_owner_info_t owner{};
    ASSERT_EQ(scada_owner_info(observer.get(), OWNER_IO, &owner), SCADA_OK);
    EXPECT_STREQ(owner.name, "io");
    EXPECT_EQ(owner.tag_count, scada::tag::testing::IO_TAG_COUNT);
    EXPECT_EQ(owner.change_ring_capacity,
              scada::tag::MIN_CHANGE_RING_CAPACITY);
    EXPECT_EQ(owner.process_index, PROCESS_IO);
    EXPECT_EQ(owner.owned, 0);
    EXPECT_EQ(owner.reserved0, 0);
    EXPECT_EQ(owner.reserved1, 0U);
    EXPECT_EQ(scada_owner_info(observer.get(), 1, &owner), SCADA_E_INVALID);

    std::vector<std::string> names;
    for (std::uint32_t slot = 0; slot < owner.tag_count; ++slot) {
        const std::uint32_t handle = scada_tag_handle(OWNER_IO, slot);
        scada_tag_info_t info{};
        ASSERT_EQ(scada_tag_info(observer.get(), handle, &info), SCADA_OK);
        EXPECT_EQ(info.owner_index, OWNER_IO);
        names.emplace_back(info.name);
    }
    EXPECT_EQ(names, (std::vector<std::string>{"plant.label", "plant.level",
                                               "plant.note", "plant.running"}));
    // The slot past the last is no tag.
    scada_tag_info_t info{};
    EXPECT_EQ(scada_tag_info(observer.get(),
                             scada_tag_handle(OWNER_IO, owner.tag_count),
                             &info),
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
    drop_host();
    start_host(larger_tag_set());  // generation 2; generation 1 retires
    scada_status status{};
    EXPECT_EQ(scada_tag_status(observer.get(), &status), SCADA_E_RETIRING);
    EXPECT_EQ(status.generation, 1U);
    EXPECT_EQ(status.generation_state, SCADA_GENERATION_RETIRING);

    // Observing again finds generation 2, and the old mappings go with the
    // old context.
    observer.detach();
    EXPECT_TRUE(mappings_under(generation_dir(1)).empty());
    ASSERT_EQ(observe(observer), SCADA_OK);
    EXPECT_EQ(scada_tag_status(observer.get(), &status), SCADA_OK);
    EXPECT_EQ(status.generation, 2U);
    std::uint32_t extra = SCADA_INVALID_HANDLE;
    EXPECT_EQ(scada_tag_resolve(observer.get(), "plant.extra", &extra),
              SCADA_OK);
}

TEST_F(Observer, ReportsACurrentThatNamesAnotherGeneration) {
    start_host();
    Context observer;
    ASSERT_EQ(observe(observer), SCADA_OK);
    drop_host();
    start_host(larger_tag_set());
    // Retiring is best effort (section 11.3): here generation 1 never got
    // the state, so only `current` tells.
    set_generation_state(scada::tag::GenerationState::RUNNING);
    scada_status status{};
    int code = SCADA_OK;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(WAIT_LIMIT_MS);
    // `current` is read again at most once a second.
    while (std::chrono::steady_clock::now() < deadline) {
        code = scada_tag_status(observer.get(), &status);
        if (code == SCADA_E_RETIRING) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
    }
    EXPECT_EQ(code, SCADA_E_RETIRING);
    EXPECT_EQ(status.generation, 1U);
    EXPECT_EQ(status.generation_state, SCADA_GENERATION_RUNNING);
    // It stays reported.
    EXPECT_EQ(scada_tag_status(observer.get(), &status), SCADA_E_RETIRING);
}

TEST_F(Observer, WithoutADatabaseOrWithAMissingRegion) {
    Context observer;
    // No generation yet: scada-tagd has not started.
    EXPECT_EQ(observe(observer), SCADA_E_IO);
    EXPECT_EQ(errno, ENOENT);
    EXPECT_EQ(observer.get(), nullptr);
    // No base directory at all.
    const scada_attach_opts nowhere{.base_dir = "/nonexistent/scada",
                                    .retain_dir = nullptr,
                                    .tagd_uid = std::int64_t{uid()}};
    EXPECT_EQ(scada_tag_observe_opts(&nowhere, observer.out()), SCADA_E_IO);
    EXPECT_EQ(errno, ENOENT);

    // A generation whose owner never attached: its tags read as BAD with
    // OWNER_DOWN, its ring as missing.
    start_host();
    ASSERT_EQ(observe(observer), SCADA_OK);
    std::uint32_t level = SCADA_INVALID_HANDLE;
    ASSERT_EQ(scada_tag_resolve(observer.get(), "plant.level", &level),
              SCADA_OK);
    scada_value value{};
    ASSERT_EQ(scada_tag_read(observer.get(), level, &value), SCADA_OK);
    EXPECT_EQ(value.quality, SCADA_QUALITY_BAD);
    EXPECT_NE(value.quality_flags & SCADA_FLAG_OWNER_DOWN, 0);
    EXPECT_EQ(value.update_mono_ns, 0);
    scada_sub* sub = nullptr;
    ASSERT_EQ(scada_sub_open(observer.get(), "io", &sub), SCADA_OK);
    scada_change change{};
    EXPECT_EQ(scada_sub_next(sub, &change, 0), SCADA_E_TIMEOUT);
    scada_sub_close(sub);
}

TEST_F(Observer, AGenerationThatIsNotRunning) {
    start_host();
    set_generation_state(scada::tag::GenerationState::BUILDING);
    Context observer;
    EXPECT_EQ(observe(observer), SCADA_E_IO);
    EXPECT_EQ(errno, EAGAIN);
}

TEST_F(Observer, ACallerWhoMayNotReadTheBaseDirectoryGetsPermission) {
    start_host();
    if (::geteuid() == 0) {
        // Root reads anything: the caller is a child that is nobody, in no
        // group of the database.
        const pid_t child = ::fork();
        ASSERT_GE(child, 0);
        if (child == 0) {
            const gid_t groups[] = {NOBODY};
            int code = -1;
            if (::setgroups(1, groups) == 0 && ::setgid(NOBODY) == 0 &&
                ::setuid(NOBODY) == 0) {
                Context observer;
                code = observe(observer);
            }
            ::_exit(code);
        }
        int status = 0;
        ASSERT_EQ(::waitpid(child, &status, 0), child);
        ASSERT_TRUE(WIFEXITED(status));
        EXPECT_EQ(WEXITSTATUS(status), SCADA_E_PERMISSION);
    } else {
        // Without privilege, the same refusal by the kernel: no search
        // permission on the base directory.
        ASSERT_EQ(::chmod(base().c_str(), 0), 0);
        Context observer;
        EXPECT_EQ(observe(observer), SCADA_E_PERMISSION);
        // Attach gets the same code (contract 3.1).
        Context io;
        EXPECT_EQ(attach("io", io), SCADA_E_PERMISSION);
        ASSERT_EQ(::chmod(base().c_str(), BASE_MODE), 0);
    }
}

TEST_F(Observer, NullArgumentsAndBadOptions) {
    start_host();
    EXPECT_EQ(scada_tag_observe(nullptr), SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_observe_opts(nullptr, nullptr), SCADA_E_INVALID);
    const scada_attach_opts bad_uid{.base_dir = base().c_str(),
                                    .retain_dir = nullptr,
                                    .tagd_uid = -2};
    Context observer;
    EXPECT_EQ(scada_tag_observe_opts(&bad_uid, observer.out()),
              SCADA_E_INVALID);
    EXPECT_EQ(observer.get(), nullptr);
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
    Context io;
    write_level(io);
    scada_owner_info_t owner{};
    ASSERT_EQ(scada_owner_info(io.get(), OWNER_IO, &owner), SCADA_OK);
    EXPECT_EQ(owner.owned, 1);
    std::uint16_t index = 0;
    ASSERT_EQ(scada_proc_resolve(io.get(), "io", &index), SCADA_OK);
    scada_proc_status_t status{};
    ASSERT_EQ(scada_proc_status(io.get(), index, &status), SCADA_OK);
    EXPECT_EQ(status.down, 0);
    EXPECT_EQ(status.state, SCADA_PROC_RUNNING);
}

}  // namespace
