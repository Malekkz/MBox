// Unit tests for the `proc.<process>` region: the process that owns it and
// the process that watches it (contract, sections 10 and 11.2).

#include "proc_region.h"

#include "layout.h"
#include "region_file.h"
#include "shared_memory.h"
#include "temp_database.h"

#include "scada/common/fd.h"
#include "scada/common/mapped_region.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/liveness.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>

namespace {

using namespace scada::tag;
using scada::Fd;
using scada::MapAccess;
using scada::Result;
using scada::StatusCode;
using scada::tag::testing::inode_of;
using scada::tag::testing::mode_of;
using scada::tag::testing::open_directory;
using scada::tag::testing::touch;
using scada::tag::testing::UMASK_077;

constexpr std::uint32_t TIMEOUT_MS = 500;
constexpr std::int64_t TIMEOUT_NS = 500'000'000;
// Any time on the monotonic clock.
constexpr std::int64_t START_NS = 1'000'000'000'000;
// A value of ProcHeader.state that is no enumerator.
constexpr std::uint32_t NO_STATE = 77;
// A file size that is not the size of the region.
constexpr off_t OTHER_SIZE = 4096;

TEST(Liveness, EveryReasonHasAText) {
    EXPECT_EQ(to_string(DownReason::NONE), "not down");
    EXPECT_EQ(to_string(DownReason::REGION_MISSING), "region missing");
    EXPECT_EQ(to_string(DownReason::NOT_RUNNING), "not running");
    EXPECT_EQ(to_string(DownReason::HEARTBEAT_STALE), "heartbeat stale");
}

class ProcRegion : public scada::tag::testing::TempDatabase {
protected:
    void SetUp() override {
        TempDatabase::SetUp();
        directory_ = open_directory(base());
        ASSERT_TRUE(directory_.valid());
    }

    [[nodiscard]] int directory() const { return directory_.get(); }

    [[nodiscard]] static ProcRegionParameters parameters() {
        ProcRegionParameters wanted;
        wanted.process = "io";
        wanted.process_index = 1;
        wanted.generation = 4;
        wanted.queue_count = 3;
        wanted.uid = uid();
        return wanted;
    }

    [[nodiscard]] Result<OwnProcRegion> attach(
        const ProcRegionParameters& wanted = parameters()) {
        std::string detail;
        Result<OwnProcRegion> region =
            OwnProcRegion::attach(directory(), wanted, detail);
        if (!region.ok()) {
            EXPECT_FALSE(detail.empty());
        }
        return region;
    }

    // The region as another process sees it, or a failed assertion.
    [[nodiscard]] OpenedRegion open() {
        std::string detail;
        Result<OpenedRegion> region = open_region(
            directory(), "proc.io", proc_region_identity(parameters()),
            MapAccess::READ_WRITE, detail);
        EXPECT_TRUE(region.ok()) << detail;
        return region.ok() ? std::move(region).value() : OpenedRegion{};
    }

    [[nodiscard]] DownReason observe(ProcProbe& probe, std::int64_t now) {
        return probe.observe(directory(), parameters(), TIMEOUT_MS, now);
    }

    [[nodiscard]] std::filesystem::path path() const {
        return base() / "proc.io";
    }

private:
    Fd directory_;
};

TEST_F(ProcRegion, AttachCreatesTheRegionInStateStarting) {
    const mode_t previous = ::umask(UMASK_077);
    const Result<OwnProcRegion> region = attach();
    static_cast<void>(::umask(previous));
    ASSERT_TRUE(region.ok());
    EXPECT_FALSE(region.value().reused());

    EXPECT_EQ(mode_of(path()), REGION_FILE_MODE);
    EXPECT_EQ(std::filesystem::file_size(path()), 128U + (3U * 8U));
    const OpenedRegion seen = open();
    ASSERT_TRUE(seen.memory.valid());
    const RegionHeader& region_header =
        shared_at<RegionHeader>(seen.memory.bytes(), 0);
    EXPECT_EQ(region_header.kind, to_raw(RegionKind::PROC));
    EXPECT_EQ(region_header.owner_index, 1U);
    EXPECT_EQ(region_header.queue_id, NO_QUEUE_ID);
    const ProcHeader& proc =
        shared_at<ProcHeader>(seen.memory.bytes(), sizeof(RegionHeader));
    EXPECT_EQ(proc.pid, static_cast<std::uint32_t>(::getpid()));
    EXPECT_EQ(proc.state, to_raw(ProcessState::STARTING));
    EXPECT_EQ(proc.heartbeat, 0U);
    EXPECT_EQ(proc.heartbeat_mono_ns, 0);
    EXPECT_EQ(proc.cycle_seq, 0U);
}

// Conformance test 10, second part: a second process with the same name is
// refused, whether the first created or reused the region.
TEST_F(ProcRegion, Conformance10SecondAttachWithTheSameNameIsRefused) {
    Result<OwnProcRegion> first = attach();
    ASSERT_TRUE(first.ok());
    const auto inode = inode_of(path());

    EXPECT_EQ(attach().status().code(), StatusCode::ALREADY_EXISTS);
    // The region of the first is untouched and still its own.
    EXPECT_EQ(inode_of(path()), inode);
    first.value().set_state(ProcessState::RUNNING);

    // Once the first is gone the name is free again.
    first = Result<OwnProcRegion>(OwnProcRegion());
    const Result<OwnProcRegion> second = attach();
    ASSERT_TRUE(second.ok());
    EXPECT_TRUE(second.value().reused());
    EXPECT_EQ(attach().status().code(), StatusCode::ALREADY_EXISTS);
}

TEST_F(ProcRegion, ReuseKeepsTheRegionAndStartsAgainInStateStarting) {
    {
        Result<OwnProcRegion> first = attach();
        ASSERT_TRUE(first.ok());
        first.value().set_state(ProcessState::RUNNING);
        first.value().heartbeat(START_NS);
        first.value().heartbeat(START_NS + 1);
    }
    const auto inode = inode_of(path());

    const Result<OwnProcRegion> second = attach();
    ASSERT_TRUE(second.ok());
    EXPECT_TRUE(second.value().reused());
    EXPECT_EQ(inode_of(path()), inode);
    const OpenedRegion seen = open();
    ASSERT_TRUE(seen.memory.valid());
    const ProcHeader& proc =
        shared_at<ProcHeader>(seen.memory.bytes(), sizeof(RegionHeader));
    EXPECT_EQ(proc.state, to_raw(ProcessState::STARTING));
    // The heartbeat counter goes on from where it was.
    EXPECT_EQ(proc.heartbeat, 2U);
    EXPECT_EQ(proc.heartbeat_mono_ns, START_NS + 1);
}

// Conformance test 10, first part: a region left unfinished or unusable
// under its real name by the same user is rebuilt.
TEST_F(ProcRegion, Conformance10OwnUnusableRegionIsRebuilt) {
    {
        const Result<OwnProcRegion> first = attach();
        ASSERT_TRUE(first.ok());
    }
    {
        const OpenedRegion seen = open();
        ASSERT_TRUE(seen.memory.valid());
        store_shared(shared_at<RegionHeader>(seen.memory.bytes(), 0).ready, 0U,
                     std::memory_order_release);
    }
    {
        const Result<OwnProcRegion> second = attach();
        ASSERT_TRUE(second.ok());
        EXPECT_FALSE(second.value().reused());
        const OpenedRegion seen = open();  // ready again
        EXPECT_TRUE(seen.memory.valid());
    }

    // A file of the wrong size, and one of another generation.
    ASSERT_EQ(::truncate(path().c_str(), 64), 0);
    {
        const Result<OwnProcRegion> third = attach();
        ASSERT_TRUE(third.ok());
        EXPECT_FALSE(third.value().reused());
        EXPECT_EQ(std::filesystem::file_size(path()), 152U);
    }
    ProcRegionParameters next = parameters();
    next.generation += 1;
    const Result<OwnProcRegion> fourth = attach(next);
    ASSERT_TRUE(fourth.ok());
    EXPECT_FALSE(fourth.value().reused());
}

// Conformance test 10, first part: temporary files of a process killed
// while it created the region do not stop the next attach, and are removed.
TEST_F(ProcRegion, Conformance10AttachesAfterAnInterruptedCreation) {
    const std::filesystem::path stale = base() / ".tmp.proc.io.999999";
    touch(stale);
    const Result<OwnProcRegion> region = attach();
    ASSERT_TRUE(region.ok());
    EXPECT_FALSE(region.value().reused());
    EXPECT_FALSE(std::filesystem::exists(stale));
}

TEST_F(ProcRegion, EntryOfAnotherUserIsLeftAndAttachFails) {
    {
        const Result<OwnProcRegion> first = attach();
        ASSERT_TRUE(first.ok());
    }
    const auto inode = inode_of(path());
    // The header says the process runs as another user than the one that
    // created the file.
    ProcRegionParameters wanted = parameters();
    wanted.uid = other_uid();
    EXPECT_EQ(attach(wanted).status().code(), StatusCode::PERMISSION_DENIED);
    EXPECT_EQ(inode_of(path()), inode);
}

TEST_F(ProcRegion, SymlinkOfTheSameUserIsReplacedNotFollowed) {
    const std::filesystem::path target = root() / "target";
    touch(target);
    std::filesystem::create_symlink(target, path());

    const Result<OwnProcRegion> region = attach();
    ASSERT_TRUE(region.ok());
    EXPECT_FALSE(std::filesystem::is_symlink(path()));
    EXPECT_EQ(std::filesystem::file_size(target), 0U);
}

TEST_F(ProcRegion, DirectoryUnderTheRegionNameFailsTheAttach) {
    std::filesystem::create_directory(path());
    EXPECT_EQ(attach().status().code(), StatusCode::DATA_CORRUPT);
}

// Section 10.1, rule by rule, with the times given so that the limits are
// exact.
TEST_F(ProcRegion, ProbeAppliesTheLivenessRulesInOrder) {
    ProcProbe probe;
    EXPECT_EQ(observe(probe, START_NS), DownReason::REGION_MISSING);

    Result<OwnProcRegion> region = attach();
    ASSERT_TRUE(region.ok());
    // Attached but not yet running.
    EXPECT_EQ(observe(probe, START_NS), DownReason::NOT_RUNNING);

    // RUNNING with a heartbeat that was never written.
    region.value().set_state(ProcessState::RUNNING);
    EXPECT_EQ(observe(probe, START_NS), DownReason::HEARTBEAT_STALE);

    region.value().heartbeat(START_NS);
    EXPECT_EQ(observe(probe, START_NS), DownReason::NONE);
    // Exactly the timeout old is not yet older than the timeout.
    EXPECT_EQ(observe(probe, START_NS + TIMEOUT_NS), DownReason::NONE);
    EXPECT_EQ(observe(probe, START_NS + TIMEOUT_NS + 1),
              DownReason::HEARTBEAT_STALE);
    // A heartbeat written after the checker read its clock is fresh, up to
    // the timeout ahead of the checker's clock.
    EXPECT_EQ(observe(probe, START_NS - 1), DownReason::NONE);
    EXPECT_EQ(observe(probe, START_NS - TIMEOUT_NS), DownReason::NONE);
    EXPECT_EQ(observe(probe, START_NS - TIMEOUT_NS - 1),
              DownReason::HEARTBEAT_STALE);

    region.value().heartbeat(START_NS + TIMEOUT_NS + 1);
    EXPECT_EQ(observe(probe, START_NS + TIMEOUT_NS + 1), DownReason::NONE);

    // Conformance test 6: a clean stop is seen at once, whatever the
    // heartbeat.
    region.value().set_state(ProcessState::STOPPING);
    EXPECT_EQ(observe(probe, START_NS + TIMEOUT_NS + 1),
              DownReason::NOT_RUNNING);
}

TEST_F(ProcRegion, ProbeToleratesNonsenseInTheHeartbeatTime) {
    ProcProbe probe;
    Result<OwnProcRegion> region = attach();
    ASSERT_TRUE(region.ok());
    region.value().set_state(ProcessState::RUNNING);

    region.value().heartbeat(std::numeric_limits<std::int64_t>::min());
    EXPECT_EQ(observe(probe, std::numeric_limits<std::int64_t>::max()),
              DownReason::HEARTBEAT_STALE);
    // A heartbeat time far in the future, from a corrupt region or another
    // clock, does not keep the process alive.
    region.value().heartbeat(std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(observe(probe, START_NS), DownReason::HEARTBEAT_STALE);
    EXPECT_EQ(observe(probe, std::numeric_limits<std::int64_t>::min()),
              DownReason::HEARTBEAT_STALE);

    // A state that is no enumerator is not RUNNING.
    const OpenedRegion seen = open();
    ASSERT_TRUE(seen.memory.valid());
    region.value().heartbeat(START_NS);
    store_shared(
        shared_at<ProcHeader>(seen.memory.bytes(), sizeof(RegionHeader)).state,
        NO_STATE, std::memory_order_release);
    EXPECT_EQ(observe(probe, START_NS), DownReason::NOT_RUNNING);
}

TEST_F(ProcRegion, ProbeFollowsARegionThatIsRemovedAndCreatedAgain) {
    ProcProbe probe;
    {
        Result<OwnProcRegion> region = attach();
        ASSERT_TRUE(region.ok());
        region.value().set_state(ProcessState::RUNNING);
        region.value().heartbeat(START_NS);
        EXPECT_EQ(observe(probe, START_NS), DownReason::NONE);
    }
    ASSERT_EQ(::unlink(path().c_str()), 0);
    EXPECT_EQ(observe(probe, START_NS), DownReason::REGION_MISSING);

    // A new file under the name: the probe must not look at the old
    // mapping, whose heartbeat is fresh.
    Result<OwnProcRegion> region = attach();
    ASSERT_TRUE(region.ok());
    EXPECT_FALSE(region.value().reused());
    EXPECT_EQ(observe(probe, START_NS), DownReason::NOT_RUNNING);
    region.value().set_state(ProcessState::RUNNING);
    region.value().heartbeat(START_NS);
    EXPECT_EQ(observe(probe, START_NS), DownReason::NONE);
}

TEST_F(ProcRegion, ProbeTreatsAnUnusableRegionAsMissing) {
    ProcProbe probe;
    Result<OwnProcRegion> region = attach();
    ASSERT_TRUE(region.ok());
    region.value().set_state(ProcessState::RUNNING);
    region.value().heartbeat(START_NS);
    EXPECT_EQ(observe(probe, START_NS), DownReason::NONE);

    // The header of the generation names another user for the process.
    ProcRegionParameters wanted = parameters();
    wanted.uid = other_uid();
    ProcProbe strict;
    EXPECT_EQ(strict.observe(directory(), wanted, TIMEOUT_MS, START_NS),
              DownReason::REGION_MISSING);

    // The owner made its file larger: the size no longer matches, and the
    // mapping the probe holds is dropped (contract, section 3.3).
    ASSERT_EQ(::truncate(path().c_str(), OTHER_SIZE), 0);
    EXPECT_EQ(observe(probe, START_NS), DownReason::REGION_MISSING);
}

}  // namespace
