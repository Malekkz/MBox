// Unit tests for the side of scada-tagd in the tag database: generations,
// reuse, liveness and the sweep of the generation directory (contract,
// sections 3, 10.1 and 11.1). Other processes are stood in for by their
// `proc` regions, attached through the same code a process will use.

#include "scada/tag/generation_host.h"

#include "layout.h"
#include "proc_region.h"
#include "region_file.h"
#include "shared_memory.h"
#include "temp_database.h"

#include "scada/common/fd.h"
#include "scada/common/mapped_region.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/liveness.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal functions are declared here, not in <csignal>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace scada::tag;
using scada::Fd;
using scada::MapAccess;
using scada::MappedRegion;
using scada::Result;
using scada::StatusCode;
using scada::tag::testing::GROUP_WRITABLE_BASE_MODE;
using scada::tag::testing::inode_of;
using scada::tag::testing::mode_of;
using scada::tag::testing::names_in;
using scada::tag::testing::open_directory;
using scada::tag::testing::open_file;
using scada::tag::testing::touch;
using scada::tag::testing::UMASK_077;

constexpr std::uint32_t IO_TIMEOUT_MS = 500;
constexpr std::int64_t IO_TIMEOUT_NS = 500'000'000;
// Any time on the monotonic clock.
constexpr std::int64_t START_NS = 2'000'000'000'000;
// Generation numbers of directories a test leaves behind.
constexpr std::uint64_t LEFT_EMPTY = 5;
constexpr std::uint64_t LEFT_HALF_BUILT = 7;
constexpr std::uint64_t LEFT_DANGLING = 9;
// Queues that make a tag set too large for any file system: 64 queues of
// 2^31 event records of 256 bytes are 2^45 bytes, 32 TiB.
constexpr int HUGE_QUEUES = 64;
// A file size limit below the size of any header.
constexpr rlim_t TINY_FILE_LIMIT = 64;

class Host : public scada::tag::testing::TempDatabase {
protected:
    // scada-tagd, a process `io` with one owner and one tag, and a process
    // `web` without tags that the header gives to another user. In name
    // order: io 0, tagd 1, web 2.
    [[nodiscard]] static TagSet tag_set() {
        TagSet set;
        set.processes = {
            {.name = "tagd", .uid = uid()},
            {.name = "io", .uid = uid(), .heartbeat_timeout_ms = IO_TIMEOUT_MS},
            {.name = "web", .uid = other_uid()},
        };
        set.owners = {{.name = "io",
                       .process = "io",
                       .change_ring_capacity = MIN_CHANGE_RING_CAPACITY}};
        set.queues = {
            {.name = "events.io", .producer = "io", .consumer = "web"}};
        set.tags = {
            {.name = "plant.level", .owner = "io"},
            {.name = "plant.label", .owner = "io", .type = TagType::STRING}};
        return set;
    }

    [[nodiscard]] Result<GenerationHost> start(const TagSet& set = tag_set()) {
        detail_.clear();
        return GenerationHost::start(
            set, {.base_dir = base().string(), .tagd_uid = uid()}, detail_);
    }

    // Starts and expects success; null otherwise.
    [[nodiscard]] std::unique_ptr<GenerationHost> started(
        const TagSet& set = tag_set()) {
        Result<GenerationHost> host = start(set);
        EXPECT_TRUE(host.ok()) << host.status().message() << ": " << detail_;
        if (!host.ok()) {
            return nullptr;
        }
        return std::make_unique<GenerationHost>(std::move(host).value());
    }

    [[nodiscard]] const std::string& detail() const { return detail_; }

    [[nodiscard]] std::filesystem::path generation_dir(
        std::uint64_t generation) const {
        return base() / generation_dir_name(generation);
    }

    [[nodiscard]] std::string current_target() const {
        std::error_code error;
        return std::filesystem::read_symlink(base() / "current", error)
            .string();
    }

    // Attaches the `proc` region of the process `io` in `generation`, as
    // that process would.
    [[nodiscard]] OwnProcRegion attach_io(std::uint64_t generation) {
        const Fd directory = open_directory(generation_dir(generation));
        ProcRegionParameters wanted;
        wanted.process = "io";
        wanted.process_index = 0;
        wanted.generation = generation;
        wanted.queue_count = 1;
        wanted.uid = uid();
        std::string why;
        Result<OwnProcRegion> region =
            OwnProcRegion::attach(directory.get(), wanted, why);
        EXPECT_TRUE(region.ok()) << why;
        return region.ok() ? std::move(region).value() : OwnProcRegion();
    }

    // The header of `generation`, mapped read-write as scada-tagd has it.
    [[nodiscard]] MappedRegion map_header(std::uint64_t generation) const {
        const std::filesystem::path path =
            generation_dir(generation) / "header";
        const Fd file = open_file(path);
        Result<MappedRegion> memory =
            MappedRegion::map(file.get(), std::filesystem::file_size(path),
                              MapAccess::READ_WRITE);
        EXPECT_TRUE(memory.ok());
        return memory.ok() ? std::move(memory).value() : MappedRegion();
    }

    [[nodiscard]] static std::uint32_t state_of(const MappedRegion& header) {
        return load_shared(
            shared_at<GlobalHeader>(header.bytes(), sizeof(RegionHeader)).state,
            std::memory_order_acquire);
    }

    // Attaches the process `io` in `generation`, RUNNING, with a heartbeat
    // taken at `beat_ns`.
    [[nodiscard]] OwnProcRegion running_io(std::uint64_t generation,
                                           std::int64_t beat_ns) {
        OwnProcRegion region = attach_io(generation);
        if (region.valid()) {
            region.set_state(ProcessState::RUNNING);
            region.heartbeat(beat_ns);
        }
        return region;
    }

    // Starts on the tag set of the fixture, changes the 32-bit field at
    // `offset` of the header to 3, and starts again. Expects that the
    // second start does not reuse the generation and gives `note` as the
    // reason.
    void expect_not_reused_after(std::size_t offset, std::string_view note) {
        { const std::unique_ptr<GenerationHost> first = started(); }
        const MappedRegion header = map_header(1);
        if (!header.valid()) {
            return;
        }
        store_shared(shared_at<std::uint32_t>(header.bytes(), offset),
                     CHANGED_FIELD, std::memory_order_release);
        const std::unique_ptr<GenerationHost> host = started();
        if (host == nullptr) {
            return;
        }
        EXPECT_FALSE(host->reused());
        EXPECT_EQ(host->generation(), 2U);
        EXPECT_EQ(current_target(), "gen-2");
        ASSERT_FALSE(host->notes().empty());
        EXPECT_EQ(host->notes().front(),
                  "gen-1 is not reused: " + std::string(note));
    }

    // The `down` field of every entry of the process table of `header`.
    [[nodiscard]] static std::vector<std::uint32_t> down_flags(
        const MappedRegion& header) {
        const GlobalHeader& global =
            shared_at<GlobalHeader>(header.bytes(), sizeof(RegionHeader));
        std::vector<std::uint32_t> flags;
        for (std::size_t i = 0; i < global.process_count; ++i) {
            flags.push_back(load_shared(
                shared_at<ProcessEntry>(
                    header.bytes(),
                    global.process_table_offset + (i * sizeof(ProcessEntry)))
                    .down,
                std::memory_order_acquire));
        }
        return flags;
    }

    // The names of what `report` says was removed, sorted. An entry that
    // could not be removed is a failed expectation.
    [[nodiscard]] static std::vector<std::string> removed_by(
        const CheckReport& report) {
        std::vector<std::string> names;
        names.reserve(report.swept.size());
        for (const SweptEntry& entry : report.swept) {
            EXPECT_TRUE(entry.removed) << entry.name;
            names.push_back(entry.name);
        }
        std::ranges::sort(names);
        return names;
    }

    // What `expect_not_reused_after` writes: RETIRING where the state is,
    // and a change anywhere else.
    static constexpr std::uint32_t CHANGED_FIELD = 3;

    // The process names of `changes`, each with its reason.
    [[nodiscard]] static std::vector<std::pair<std::string, DownReason>>
    changes_of(const CheckReport& report) {
        std::vector<std::pair<std::string, DownReason>> list;
        list.reserve(report.changes.size());
        for (const ProcessChange& change : report.changes) {
            list.emplace_back(change.process, change.reason);
        }
        return list;
    }

    using Changes = std::vector<std::pair<std::string, DownReason>>;

private:
    std::string detail_;
};

// Conformance test 11, first part, for what scada-tagd creates: the modes
// of section 3.1 under umask 077.
TEST_F(Host, Conformance11GenerationHasTheModesOfTheContractUnderUmask077) {
    const mode_t previous = ::umask(UMASK_077);
    const std::unique_ptr<GenerationHost> host = started();
    static_cast<void>(::umask(previous));
    ASSERT_NE(host, nullptr);

    EXPECT_EQ(mode_of(base()), BASE_DIR_MODE);
    EXPECT_EQ(mode_of(generation_dir(1)), GENERATION_DIR_MODE);
    EXPECT_EQ(mode_of(generation_dir(1) / "header"), REGION_FILE_MODE);
    EXPECT_EQ(mode_of(generation_dir(1) / "proc.tagd"), REGION_FILE_MODE);
}

TEST_F(Host, FirstStartCreatesGenerationOneAndNothingElse) {
    const std::unique_ptr<GenerationHost> host = started();
    ASSERT_NE(host, nullptr);
    EXPECT_FALSE(host->reused());
    EXPECT_EQ(host->generation(), 1U);
    EXPECT_EQ(host->heartbeat_check_ms(), DEFAULT_HEARTBEAT_CHECK_MS);
    EXPECT_EQ(host->heartbeat_timeout_ms(), DEFAULT_HEARTBEAT_TIMEOUT_MS);
    EXPECT_TRUE(host->notes().empty());

    EXPECT_TRUE(std::filesystem::is_symlink(base() / "current"));
    EXPECT_EQ(current_target(), "gen-1");
    // The two regions and the link; no temporary name is left.
    EXPECT_EQ(names_in(base()), (std::vector<std::string>{"current", "gen-1"}));
    EXPECT_EQ(names_in(generation_dir(1)),
              (std::vector<std::string>{"header", "proc.tagd"}));
}

TEST_F(Host, NewHeaderIsReadyRunningAndHoldsTheTagSet) {
    const std::unique_ptr<GenerationHost> host = started();
    ASSERT_NE(host, nullptr);
    const MappedRegion header = map_header(1);
    ASSERT_TRUE(header.valid());

    const RegionHeader& region = shared_at<RegionHeader>(header.bytes(), 0);
    EXPECT_EQ(region.magic, REGION_MAGIC);
    EXPECT_EQ(region.kind, to_raw(RegionKind::HEADER));
    EXPECT_EQ(region.generation, 1U);
    EXPECT_EQ(region.size_bytes, header.bytes().size());
    EXPECT_EQ(region.ready, 1U);
    const GlobalHeader& global =
        shared_at<GlobalHeader>(header.bytes(), sizeof(RegionHeader));
    EXPECT_EQ(global.state, to_raw(GenerationState::RUNNING));
    EXPECT_EQ(global.process_count, 3U);
    EXPECT_EQ(global.tag_count, 2U);
    EXPECT_GT(global.created_time_ns, 0);
    std::string why;
    const Result<ConfigHash> hash = config_hash(tag_set(), why);
    ASSERT_TRUE(hash.ok()) << why;
    EXPECT_EQ(global.config_hash, hash.value());
}

TEST_F(Host, OwnProcRegionIsRunningWithAHeartbeat) {
    const std::unique_ptr<GenerationHost> host = started();
    ASSERT_NE(host, nullptr);
    const Fd file = open_file(generation_dir(1) / "proc.tagd");
    const Result<MappedRegion> memory = MappedRegion::map(
        file.get(), proc_region_size(1), MapAccess::READ_ONLY);
    ASSERT_TRUE(memory.ok());
    const ProcHeader& proc =
        shared_at<ProcHeader>(memory.value().bytes(), sizeof(RegionHeader));
    // Index 1: `tagd` sorts after `io`.
    EXPECT_EQ(shared_at<RegionHeader>(memory.value().bytes(), 0).owner_index,
              1U);
    EXPECT_EQ(proc.state, to_raw(ProcessState::RUNNING));
    EXPECT_EQ(proc.heartbeat, 1U);
    EXPECT_GT(proc.heartbeat_mono_ns, 0);

    // A time after the first heartbeat, which took the clock: an older one
    // would not be published (contract 10.1).
    const std::int64_t later = proc.heartbeat_mono_ns + 1;
    host->heartbeat(later);
    EXPECT_EQ(proc.heartbeat, 2U);
    EXPECT_EQ(proc.heartbeat_mono_ns, later);

    host->stop();
    EXPECT_EQ(proc.state, to_raw(ProcessState::STOPPING));
}

// Conformance test 20, the part of scada-tagd: a restart with an unchanged
// tag set reuses the generation and touches nothing another process holds.
TEST_F(Host, Conformance20RestartWithTheSameTagSetReusesTheGeneration) {
    OwnProcRegion peer;
    {
        const std::unique_ptr<GenerationHost> first = started();
        ASSERT_NE(first, nullptr);
        peer = running_io(1, START_NS);
        ASSERT_TRUE(peer.valid());
        static_cast<void>(first->check(START_NS));
        first->stop();
    }
    const ino_t header_inode = inode_of(generation_dir(1) / "header");
    const ino_t peer_inode = inode_of(generation_dir(1) / "proc.io");

    // The tag set in another order is the same tag set.
    TagSet same = tag_set();
    std::ranges::reverse(same.processes);
    std::ranges::reverse(same.tags);
    const std::unique_ptr<GenerationHost> host = started(same);
    ASSERT_NE(host, nullptr);
    EXPECT_TRUE(host->reused());
    EXPECT_EQ(host->generation(), 1U);
    EXPECT_TRUE(host->notes().empty());
    EXPECT_EQ(names_in(base()), (std::vector<std::string>{"current", "gen-1"}));
    EXPECT_EQ(inode_of(generation_dir(1) / "header"), header_inode);
    EXPECT_EQ(inode_of(generation_dir(1) / "proc.io"), peer_inode);
    EXPECT_EQ(state_of(map_header(1)), to_raw(GenerationState::RUNNING));
}

// The second half of conformance test 20 for scada-tagd: a process that
// kept running through the restart is never reported down.
TEST_F(Host, Conformance20RunningProcessStaysUpAcrossTheRestart) {
    OwnProcRegion peer;
    {
        const std::unique_ptr<GenerationHost> first = started();
        ASSERT_NE(first, nullptr);
        peer = running_io(1, START_NS);
        ASSERT_TRUE(peer.valid());
        EXPECT_EQ(changes_of(first->check(START_NS)),
                  (Changes{{"web", DownReason::REGION_MISSING}}));
    }
    const std::unique_ptr<GenerationHost> host = started();
    ASSERT_NE(host, nullptr);
    // The flags are as the first instance left them.
    EXPECT_EQ(host->is_down("io"), false);
    EXPECT_EQ(host->is_down("web"), true);
    EXPECT_TRUE(host->check(START_NS).changes.empty());
    peer.heartbeat(START_NS + 1);
    EXPECT_TRUE(host->check(START_NS + IO_TIMEOUT_NS).changes.empty());
}

// Issue #42, point 4 (D-36): a heartbeat gap longer than the timeout that
// lies between two checks is never seen as down by scada-tagd, but readers
// saw the process down. The process counts it, and the check reports it.
TEST_F(Host, AHeartbeatGapBetweenTwoChecksIsReportedAsLate) {
    const std::unique_ptr<GenerationHost> host = started();
    ASSERT_NE(host, nullptr);
    OwnProcRegion peer = attach_io(1);
    ASSERT_TRUE(peer.valid());
    peer.count_late_after(IO_TIMEOUT_MS);
    peer.first_heartbeat(START_NS);
    peer.set_state(ProcessState::RUNNING);
    EXPECT_TRUE(host->check(START_NS).late.empty());

    // The gap ends before the next check: the check finds a fresh heartbeat.
    const std::int64_t late = START_NS + IO_TIMEOUT_NS + 1;
    peer.heartbeat(late);
    const CheckReport report = host->check(late);
    EXPECT_TRUE(changes_of(report).empty());
    ASSERT_EQ(report.late.size(), 1U);
    EXPECT_EQ(report.late[0].process, "io");
    EXPECT_EQ(report.late[0].added, 1U);
    EXPECT_EQ(report.late[0].total, 1U);
    EXPECT_EQ(report.late[0].longest_gap_ns, IO_TIMEOUT_NS + 1);
    EXPECT_EQ(report.late[0].last_late_mono_ns, late);
    EXPECT_FALSE(report.late[0].baseline);

    // Reported once.
    peer.heartbeat(late + 1);
    EXPECT_TRUE(host->check(late + 1).late.empty());
}

// A scada-tagd that starts finds the count of earlier late heartbeats: it is
// reported once as a baseline, not as new.
TEST_F(Host, LateHeartbeatsFoundAtStartAreABaseline) {
    OwnProcRegion peer;
    const std::int64_t late = START_NS + IO_TIMEOUT_NS + 1;
    {
        const std::unique_ptr<GenerationHost> first = started();
        ASSERT_NE(first, nullptr);
        peer = attach_io(1);
        ASSERT_TRUE(peer.valid());
        peer.count_late_after(IO_TIMEOUT_MS);
        peer.first_heartbeat(START_NS);
        peer.set_state(ProcessState::RUNNING);
        peer.heartbeat(late);
    }
    const std::unique_ptr<GenerationHost> host = started();
    ASSERT_NE(host, nullptr);
    const CheckReport report = host->check(late);
    ASSERT_EQ(report.late.size(), 1U);
    EXPECT_TRUE(report.late[0].baseline);
    EXPECT_EQ(report.late[0].added, 1U);
    EXPECT_TRUE(host->check(late).late.empty());
}

TEST_F(Host, ChangedTagSetCreatesTheNextGenerationAndRetiresTheOld) {
    { const std::unique_ptr<GenerationHost> first = started(); }
    TagSet changed = tag_set();
    changed.tags.push_back({.name = "plant.flow", .owner = "io"});

    const std::unique_ptr<GenerationHost> host = started(changed);
    ASSERT_NE(host, nullptr);
    EXPECT_FALSE(host->reused());
    EXPECT_EQ(host->generation(), 2U);
    EXPECT_EQ(current_target(), "gen-2");
    EXPECT_EQ(host->notes(),
              (std::vector<std::string>{
                  "gen-1 is not reused: header does not have the expected size",
                  "gen-1 is set to RETIRING"}));
    EXPECT_EQ(state_of(map_header(1)), to_raw(GenerationState::RETIRING));
    EXPECT_EQ(state_of(map_header(2)), to_raw(GenerationState::RUNNING));
}

// Section 11.1, step 3: an old generation stays while a process is attached
// to it, and is removed by the next start that builds a generation once the
// process is gone.
TEST_F(Host, OldGenerationIsRemovedOnlyWhenNoProcessIsAttached) {
    OwnProcRegion peer;
    {
        const std::unique_ptr<GenerationHost> first = started();
        ASSERT_NE(first, nullptr);
        peer = attach_io(1);
        ASSERT_TRUE(peer.valid());
    }
    TagSet changed = tag_set();
    changed.tags.push_back({.name = "plant.flow", .owner = "io"});
    { const std::unique_ptr<GenerationHost> second = started(changed); }
    changed.tags.push_back({.name = "plant.temp", .owner = "io"});
    {
        const std::unique_ptr<GenerationHost> third = started(changed);
        ASSERT_NE(third, nullptr);
        EXPECT_EQ(third->generation(), 3U);
        // gen-1 has the process; gen-2 was `current` until now.
        EXPECT_EQ(names_in(base()), (std::vector<std::string>{
                                        "current", "gen-1", "gen-2", "gen-3"}));
        EXPECT_NE(std::ranges::find(third->notes(),
                                    "gen-1 stays: a process is attached to it"),
                  third->notes().end());
    }

    peer = OwnProcRegion();
    changed.tags.push_back({.name = "plant.valve", .owner = "io"});
    const std::unique_ptr<GenerationHost> host = started(changed);
    ASSERT_NE(host, nullptr);
    EXPECT_EQ(host->generation(), 4U);
    EXPECT_EQ(names_in(base()),
              (std::vector<std::string>{"current", "gen-3", "gen-4"}));
    EXPECT_EQ(current_target(), "gen-4");
    EXPECT_EQ(state_of(map_header(3)), to_raw(GenerationState::RETIRING));
}

TEST_F(Host, EqualSizeButOtherTagSetIsNotReused) {
    { const std::unique_ptr<GenerationHost> first = started(); }
    // Same names and counts, another heartbeat timeout: the header has the
    // same size and another hash.
    TagSet changed = tag_set();
    changed.processes.at(1).heartbeat_timeout_ms += 1;
    const std::unique_ptr<GenerationHost> host = started(changed);
    ASSERT_NE(host, nullptr);
    EXPECT_FALSE(host->reused());
    EXPECT_EQ(host->generation(), 2U);
    ASSERT_FALSE(host->notes().empty());
    EXPECT_EQ(host->notes().front(),
              "gen-1 is not reused: the tag set changed");
}

// Reuse needs more than an equal hash (review of decision D-26): the
// header must be ready, RUNNING and unchanged.
TEST_F(Host, GenerationThatIsNotReadyIsNotReused) {
    expect_not_reused_after(offsetof(RegionHeader, ready),
                            "header is not ready");
}

TEST_F(Host, GenerationThatIsNotRunningIsNotReused) {
    expect_not_reused_after(
        sizeof(RegionHeader) + offsetof(GlobalHeader, state),
        "its state is not RUNNING");
}

TEST_F(Host, GenerationWithADamagedHeaderIsNotReused) {
    // The first name field of the owner table.
    expect_not_reused_after(sizeof(RegionHeader) + sizeof(GlobalHeader),
                            "its header is not the one this tag set builds");
}

TEST_F(Host, DirectoriesLeftByACrashWhileBuildingAreRemoved) {
    // A generation without header, one with a half-built header, and a
    // stale link under its temporary name. Nothing points to them.
    std::filesystem::create_directory(generation_dir(LEFT_EMPTY));
    std::filesystem::create_directory(generation_dir(LEFT_HALF_BUILT));
    touch(generation_dir(LEFT_HALF_BUILT) / ".tmp.header.4242");
    std::filesystem::create_directory(generation_dir(LEFT_HALF_BUILT) / "sub");
    std::filesystem::create_symlink("gen-7", base() / ".tmp.current.4242");
    // Not a generation name: left alone.
    std::filesystem::create_directory(base() / "gen-08");

    const std::unique_ptr<GenerationHost> host = started();
    ASSERT_NE(host, nullptr);
    // Numbers are not used twice, even of generations that are gone.
    EXPECT_EQ(host->generation(), LEFT_HALF_BUILT + 1);
    EXPECT_EQ(current_target(), "gen-8");
    EXPECT_EQ(names_in(base()),
              (std::vector<std::string>{"current", "gen-08", "gen-8"}));
    EXPECT_EQ(host->notes(), (std::vector<std::string>{"gen-5 was removed",
                                                       "gen-7 was removed"}));
}

TEST_F(Host, CurrentThatPointsNowhereUsefulIsReplaced) {
    std::filesystem::create_symlink("elsewhere", base() / "current");
    {
        const std::unique_ptr<GenerationHost> first = started();
        ASSERT_NE(first, nullptr);
        EXPECT_EQ(first->generation(), 1U);
        EXPECT_EQ(current_target(), "gen-1");
        EXPECT_EQ(first->notes(),
                  (std::vector<std::string>{
                      "current does not point to a generation"}));
    }
    // A link to a generation that does not exist.
    std::filesystem::remove(base() / "current");
    std::filesystem::create_symlink(generation_dir_name(LEFT_DANGLING),
                                    base() / "current");
    const std::unique_ptr<GenerationHost> host = started();
    ASSERT_NE(host, nullptr);
    EXPECT_EQ(host->generation(), LEFT_DANGLING + 1);
    // gen-1 had no process attached any more.
    EXPECT_EQ(names_in(base()),
              (std::vector<std::string>{"current", "gen-10"}));
    EXPECT_EQ(current_target(), "gen-10");
}

TEST_F(Host, CurrentThatIsNotALinkStopsTheStart) {
    touch(base() / "current");
    EXPECT_EQ(start().status().code(), StatusCode::FAILED_PRECONDITION);
    EXPECT_EQ(names_in(base()), std::vector<std::string>{"current"});
}

// Conformance test 9, third part.
TEST_F(Host, Conformance09BaseDirectoryWithWrongOwnerOrModeIsRefused) {
    ASSERT_EQ(::chmod(base().c_str(), GROUP_WRITABLE_BASE_MODE), 0);
    EXPECT_EQ(start().status().code(), StatusCode::PERMISSION_DENIED);
    EXPECT_NE(detail().find("mode 2750"), std::string::npos) << detail();
    ASSERT_EQ(::chmod(base().c_str(), BASE_DIR_MODE), 0);

    // scada-tagd running as another user than the owner of the directory.
    TagSet set = tag_set();
    set.processes.at(0).uid = other_uid();
    std::string why;
    EXPECT_EQ(
        GenerationHost::start(
            set, {.base_dir = base().string(), .tagd_uid = other_uid()}, why)
            .status()
            .code(),
        StatusCode::PERMISSION_DENIED);
    EXPECT_TRUE(std::filesystem::is_empty(base()));
}

TEST_F(Host, MissingBaseDirectoryIsNotCreated) {
    std::string why;
    const std::filesystem::path missing = root() / "none";
    EXPECT_EQ(
        GenerationHost::start(
            tag_set(), {.base_dir = missing.string(), .tagd_uid = uid()}, why)
            .status()
            .code(),
        StatusCode::NOT_FOUND);
    EXPECT_FALSE(std::filesystem::exists(missing));
}

TEST_F(Host, TagSetMustHoldTheProcessTagdWithTheUidOfTheCaller) {
    TagSet set = tag_set();
    set.processes.erase(set.processes.begin());
    EXPECT_EQ(start(set).status().code(), StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(detail(), "the tag set has no process named tagd");

    set = tag_set();
    set.processes.at(0).uid = other_uid();
    EXPECT_EQ(start(set).status().code(), StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(detail().find("runs as user"), std::string::npos) << detail();

    // A tag set that is not valid is refused as `validate` refuses it.
    set = tag_set();
    set.tags.push_back(set.tags.front());
    EXPECT_EQ(start(set).status().code(), StatusCode::ALREADY_EXISTS);
    EXPECT_TRUE(std::filesystem::is_empty(base()));
}

TEST_F(Host, SecondInstanceOnTheSameBaseDirectoryIsRefused) {
    const std::unique_ptr<GenerationHost> first = started();
    ASSERT_NE(first, nullptr);
    const ino_t header = inode_of(generation_dir(1) / "header");

    EXPECT_EQ(start().status().code(), StatusCode::ALREADY_EXISTS);
    // Not even with another tag set does it build anything.
    TagSet changed = tag_set();
    changed.tags.clear();
    EXPECT_EQ(start(changed).status().code(), StatusCode::ALREADY_EXISTS);
    EXPECT_EQ(names_in(base()), (std::vector<std::string>{"current", "gen-1"}));
    EXPECT_EQ(current_target(), "gen-1");
    EXPECT_EQ(inode_of(generation_dir(1) / "header"), header);
}

// Regions the file system has no room for: the generation is not published.
TEST_F(Host, TagSetThatDoesNotFitTheFileSystemIsRefused) {
    TagSet set = tag_set();
    for (int i = 0; i < HUGE_QUEUES; ++i) {
        set.queues.push_back({.name = "events.big" + std::to_string(i),
                              .producer = "io",
                              .consumer = "web",
                              .capacity = MAX_QUEUE_CAPACITY});
    }
    EXPECT_EQ(start(set).status().code(), StatusCode::RESOURCE_EXHAUSTED);
    EXPECT_NE(detail().find("bytes free"), std::string::npos) << detail();
    EXPECT_TRUE(std::filesystem::is_empty(base()));
}

// Conformance test 11, second part, as far as it can be shown without a
// full file system: when the header file cannot get its size, the start
// fails with an error and leaves nothing behind. The limit on the file size
// stands in for the full file system. Exceeding it raises SIGXFSZ, which is
// blocked here and taken out of the queue afterwards.
TEST_F(Host, Conformance11HeaderThatCannotBeAllocatedFailsWithAnError) {
    sigset_t blocked{};
    sigset_t previous_mask{};
    ASSERT_EQ(::sigemptyset(&blocked), 0);
    ASSERT_EQ(::sigaddset(&blocked, SIGXFSZ), 0);
    ASSERT_EQ(::pthread_sigmask(SIG_BLOCK, &blocked, &previous_mask), 0);
    rlimit previous{};
    ASSERT_EQ(::getrlimit(RLIMIT_FSIZE, &previous), 0);
    const rlimit small{.rlim_cur = TINY_FILE_LIMIT,
                       .rlim_max = previous.rlim_max};
    ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &small), 0);

    const Result<GenerationHost> host = start();

    ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &previous), 0);
    const std::timespec no_wait{};
    static_cast<void>(::sigtimedwait(&blocked, nullptr, &no_wait));
    ASSERT_EQ(::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr), 0);

    EXPECT_EQ(host.status().code(), StatusCode::RESOURCE_EXHAUSTED);
    EXPECT_NE(detail().find(".tmp.header."), std::string::npos) << detail();
    EXPECT_TRUE(std::filesystem::is_empty(base()));
}

TEST_F(Host, StartAfterAFailedStartBeginsAtGenerationOne) {
    TagSet set = tag_set();
    set.tags.push_back(set.tags.front());
    ASSERT_FALSE(start(set).ok());
    const std::unique_ptr<GenerationHost> host = started();
    ASSERT_NE(host, nullptr);
    EXPECT_EQ(host->generation(), 1U);
}

// Conformance test 5, the part of scada-tagd, with the times given so that
// the limits are exact. The integration test does the same with real
// processes and SIGKILL.
TEST_F(Host, Conformance05EveryProcessIsDownUntilItAttaches) {
    const std::unique_ptr<GenerationHost> host = started();
    ASSERT_NE(host, nullptr);
    EXPECT_EQ(host->is_down("io"), false);
    EXPECT_EQ(host->is_down("nobody"), std::nullopt);

    // Nobody has attached yet. scada-tagd itself is never down.
    EXPECT_EQ(changes_of(host->check(START_NS)),
              (Changes{{"io", DownReason::REGION_MISSING},
                       {"web", DownReason::REGION_MISSING}}));
    EXPECT_EQ(host->is_down("io"), true);
    EXPECT_EQ(host->is_down("tagd"), false);
    // A state that does not change is not reported again.
    EXPECT_TRUE(host->check(START_NS).changes.empty());

    // Attached but still STARTING: down until it is RUNNING and beats.
    OwnProcRegion peer = attach_io(1);
    ASSERT_TRUE(peer.valid());
    EXPECT_TRUE(host->check(START_NS).changes.empty());
    peer.set_state(ProcessState::RUNNING);
    peer.heartbeat(START_NS);
    EXPECT_EQ(changes_of(host->check(START_NS)),
              (Changes{{"io", DownReason::NONE}}));
    EXPECT_EQ(host->is_down("io"), false);
}

TEST_F(Host, Conformance05ProcessIsDownOnceItsHeartbeatIsOlderThanItsTimeout) {
    const std::unique_ptr<GenerationHost> host = started();
    ASSERT_NE(host, nullptr);
    const OwnProcRegion peer = running_io(1, START_NS);
    ASSERT_TRUE(peer.valid());
    static_cast<void>(host->check(START_NS));
    ASSERT_EQ(host->is_down("io"), false);

    // The heartbeat stops, as after SIGKILL. Exactly the timeout old is
    // not yet older than the timeout.
    EXPECT_TRUE(host->check(START_NS + IO_TIMEOUT_NS).changes.empty());
    EXPECT_EQ(host->is_down("io"), false);
    EXPECT_EQ(changes_of(host->check(START_NS + IO_TIMEOUT_NS + 1)),
              (Changes{{"io", DownReason::HEARTBEAT_STALE}}));
    EXPECT_EQ(host->is_down("io"), true);
}

TEST_F(Host, Conformance05RestartedProcessIsUpAgain) {
    const std::unique_ptr<GenerationHost> host = started();
    ASSERT_NE(host, nullptr);
    { const OwnProcRegion killed = running_io(1, START_NS); }
    const std::int64_t later = START_NS + IO_TIMEOUT_NS + 1;
    static_cast<void>(host->check(later));
    ASSERT_EQ(host->is_down("io"), true);

    // The process is restarted: it reuses its region and beats again.
    const OwnProcRegion peer = running_io(1, later);
    ASSERT_TRUE(peer.valid());
    EXPECT_TRUE(peer.reused());
    EXPECT_EQ(changes_of(host->check(later)),
              (Changes{{"io", DownReason::NONE}}));
    EXPECT_EQ(host->is_down("io"), false);
}

// Conformance test 6, the part of scada-tagd: a clean stop is marked at
// the next check, whatever the heartbeat.
TEST_F(Host, Conformance06CleanStopIsMarkedAtTheNextCheck) {
    const std::unique_ptr<GenerationHost> host = started();
    ASSERT_NE(host, nullptr);
    OwnProcRegion peer = running_io(1, START_NS);
    ASSERT_TRUE(peer.valid());
    static_cast<void>(host->check(START_NS));
    ASSERT_EQ(host->is_down("io"), false);

    peer.set_state(ProcessState::STOPPING);
    EXPECT_EQ(changes_of(host->check(START_NS)),
              (Changes{{"io", DownReason::NOT_RUNNING}}));
    EXPECT_EQ(host->is_down("io"), true);
}

TEST_F(Host, DownFlagIsWhatOtherProcessesSeeInTheHeader) {
    const std::unique_ptr<GenerationHost> host = started();
    ASSERT_NE(host, nullptr);
    const MappedRegion header = map_header(1);
    ASSERT_TRUE(header.valid());
    // In process index order: io, tagd, web.
    EXPECT_EQ(down_flags(header), (std::vector<std::uint32_t>{0, 0, 0}));
    static_cast<void>(host->check(START_NS));
    EXPECT_EQ(down_flags(header), (std::vector<std::uint32_t>{1, 0, 1}));

    const OwnProcRegion peer = running_io(1, START_NS);
    ASSERT_TRUE(peer.valid());
    static_cast<void>(host->check(START_NS));
    EXPECT_EQ(down_flags(header), (std::vector<std::uint32_t>{0, 0, 1}));
}

// Conformance test 9, first part: what does not belong in the generation
// directory is removed at the liveness check.
TEST_F(Host, Conformance09SweepRemovesUnlistedEntriesAndThoseOfTheWrongUser) {
    const std::unique_ptr<GenerationHost> host = started();
    ASSERT_NE(host, nullptr);
    const std::filesystem::path dir = generation_dir(1);
    const OwnProcRegion peer = attach_io(1);
    ASSERT_TRUE(peer.valid());

    // Listed and of the right user: stay. `strings.io` is listed because
    // the owner has a string tag.
    touch(dir / "tags.io");
    touch(dir / "changes.io");
    touch(dir / "strings.io");
    touch(dir / "events.io");
    touch(dir / ".tmp.tags.io.4242");  // a region being created
    // Listed, but the header gives the process `web` to another user than
    // the one that created the file.
    touch(dir / "proc.web");
    touch(dir / ".tmp.proc.web.4242");
    // Not listed.
    touch(dir / "proc.nobody");
    touch(dir / "tags.web");
    touch(dir / "core");
    touch(dir / ".tmp.tags.io.x1");
    touch(dir / ".tmp.unknown.1");
    std::filesystem::create_symlink("header", dir / "link");
    std::filesystem::create_directory(dir / "empty-dir");

    EXPECT_EQ(
        removed_by(host->check(START_NS)),
        (std::vector<std::string>{".tmp.proc.web.4242", ".tmp.tags.io.x1",
                                  ".tmp.unknown.1", "core", "empty-dir", "link",
                                  "proc.nobody", "proc.web", "tags.web"}));
    EXPECT_EQ(names_in(dir),
              (std::vector<std::string>{".tmp.tags.io.4242", "changes.io",
                                        "events.io", "header", "proc.io",
                                        "proc.tagd", "strings.io", "tags.io"}));
    EXPECT_TRUE(host->check(START_NS).swept.empty());
}

TEST_F(Host, EntryThatCannotBeRemovedIsReportedOnce) {
    const std::unique_ptr<GenerationHost> host = started();
    ASSERT_NE(host, nullptr);
    const std::filesystem::path stuck = generation_dir(1) / "stuck";
    std::filesystem::create_directory(stuck);
    touch(stuck / "file");

    const CheckReport first = host->check(START_NS);
    ASSERT_EQ(first.swept.size(), 1U);
    EXPECT_EQ(first.swept.front().name, "stuck");
    EXPECT_FALSE(first.swept.front().removed);
    EXPECT_TRUE(host->check(START_NS).swept.empty());

    // Once it can be removed it is, and that is reported.
    std::filesystem::remove(stuck / "file");
    EXPECT_EQ(removed_by(host->check(START_NS)),
              std::vector<std::string>{"stuck"});
    EXPECT_FALSE(std::filesystem::exists(stuck));
}

TEST_F(Host, ReuseSweepsBeforeItAttachesItsOwnRegion) {
    { const std::unique_ptr<GenerationHost> first = started(); }
    // `proc.web` stands for any file of a user the header does not expect;
    // for `proc.tagd` itself the wrong user cannot be made without one.
    touch(generation_dir(1) / "proc.web");
    const std::unique_ptr<GenerationHost> host = started();
    ASSERT_NE(host, nullptr);
    EXPECT_TRUE(host->reused());
    EXPECT_FALSE(std::filesystem::exists(generation_dir(1) / "proc.web"));
}

}  // namespace
