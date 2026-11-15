// Integration test of who may observe the tag database (contract, sections
// 3.1 and 11.4), with real users. It must run as root, which it needs to
// become other users: the CTest label `root` keeps it out of the presets
// that run unprivileged, and the pipeline runs it with sudo (`ctest
// --preset root`).
//
// A child process, user DB_UID in group DB_GID, plays scada-tagd and the
// process `io` and keeps `io` alive. Then:
// - root, which is not in group DB_GID, observes and reads `io`'s tag;
// - a user in no group of the database gets SCADA_E_PERMISSION;
// - a user that is in group DB_GID but in no entry of the process table
//   observes and reads, through read-only mappings only.

#include "scada/common/result.h"
#include "scada/tag/generation_host.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <grp.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

// The users of the test. None needs to exist in the user database.
constexpr uid_t DB_UID = 65534;        // scada-tagd and io
constexpr gid_t DB_GID = 65534;        // the group of the database
constexpr uid_t OUTSIDER_UID = 65533;  // in no group of the database
constexpr uid_t MEMBER_UID = 65532;    // in DB_GID, not in the process table

constexpr mode_t BASE_DIR_MODE = 02750;
constexpr mode_t TRAVERSABLE = 0755;
constexpr double LEVEL = 7.5;
constexpr int PERIOD_MS = 100;
constexpr int WAIT_MS = 30'000;
constexpr std::int64_t NS_PER_S = 1'000'000'000;

// Exit codes of the children.
constexpr int CHILD_OK = 0;
constexpr int CHILD_NO_USER = 100;
constexpr int CHILD_FAILED = 101;
constexpr int CHILD_WRONG_VALUE = 102;
constexpr int CHILD_WRITABLE_MAPPING = 103;

// Becomes `uid` with primary group `gid` and the supplementary `groups`.
bool become(uid_t uid, gid_t gid, const std::vector<gid_t>& groups) {
    return ::setgroups(groups.size(), groups.data()) == 0 &&
           ::setgid(gid) == 0 && ::setuid(uid) == 0 && ::geteuid() == uid;
}

scada_attach_opts options_for(const std::string& base) {
    return {.base_dir = base.c_str(),
            .retain_dir = nullptr,
            .tagd_uid = std::int64_t{DB_UID}};
}

// True if no mapping of this process under `dir` is writable.
bool only_read_only_mappings(const std::string& dir) {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find(dir + "/") == std::string::npos) {
            continue;
        }
        std::istringstream fields(line);
        std::string range;
        std::string permissions;
        fields >> range >> permissions;
        if (permissions.find('w') != std::string::npos) {
            return false;
        }
    }
    return true;
}

// Observes the database in `base` and reads plant.level: the code of
// observe, or a CHILD_* code. In a child, after `become`.
int observe_and_read(const std::string& base) {
    const scada_attach_opts options = options_for(base);
    scada_ctx* ctx = nullptr;
    const int code = scada_tag_observe_opts(&options, &ctx);
    if (code != SCADA_OK) {
        return code;
    }
    std::uint32_t level = SCADA_INVALID_HANDLE;
    scada_value value{};
    int result = CHILD_OK;
    if (scada_tag_resolve(ctx, "plant.level", &level) != SCADA_OK ||
        scada_tag_read(ctx, level, &value) != SCADA_OK) {
        result = CHILD_FAILED;
    } else if (value.quality != SCADA_QUALITY_GOOD ||
               std::bit_cast<double>(value.value) != LEVEL) {
        result = CHILD_WRONG_VALUE;
    } else if (!only_read_only_mappings(base)) {
        result = CHILD_WRITABLE_MAPPING;
    }
    scada_tag_detach(ctx);
    return result;
}

// Runs `body` in a child that has become `uid`, `gid`, `groups`, and
// returns its exit code, or nothing if it did not exit.
template <typename Body>
std::optional<int> run_as(uid_t uid, gid_t gid,
                          const std::vector<gid_t>& groups, Body body) {
    const pid_t child = ::fork();
    if (child < 0) {
        return std::nullopt;
    }
    if (child == 0) {
        ::_exit(become(uid, gid, groups) ? body() : CHILD_NO_USER);
    }
    int status = 0;
    if (::waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return std::nullopt;
    }
    return WEXITSTATUS(status);
}

scada::tag::TagSet tag_set() {
    scada::tag::TagSet set;
    set.processes = {{.name = "tagd", .uid = DB_UID},
                     {.name = "io", .uid = DB_UID}};
    set.owners = {
        {.name = "io",
         .process = "io",
         .change_ring_capacity = scada::tag::MIN_CHANGE_RING_CAPACITY}};
    set.tags = {{.name = "plant.level",
                 .owner = "io",
                 .type = scada::tag::TagType::FLOAT64}};
    return set;
}

// The child that plays scada-tagd and `io`: creates the generation, writes
// LEVEL, writes 'r' to `ready`, and keeps both heartbeats fresh until a
// byte arrives on `stop`.
int database_owner(const std::string& base, int ready, int stop) {
    std::string detail;
    scada::Result<scada::tag::GenerationHost> host =
        scada::tag::GenerationHost::start(
            tag_set(),
            {.base_dir = base,
             .tagd_uid = DB_UID,
             .retain_dir = {},
             .retain_interval_ms = scada::tag::DEFAULT_RETAIN_INTERVAL_MS},
            detail);
    if (!host.ok()) {
        return CHILD_FAILED;
    }
    const scada_attach_opts options = options_for(base);
    scada_ctx* feeder = nullptr;
    std::uint32_t level = SCADA_INVALID_HANDLE;
    if (scada_tag_attach_opts("io", &options, &feeder) != SCADA_OK ||
        scada_tag_resolve(feeder, "plant.level", &level) != SCADA_OK) {
        return CHILD_FAILED;
    }
    scada_value value{};
    value.type = SCADA_TYPE_FLOAT64;
    value.quality = SCADA_QUALITY_GOOD;
    value.value = std::bit_cast<std::uint64_t>(LEVEL);
    if (scada_tag_write(feeder, level, &value) != SCADA_OK ||
        ::write(ready, "r", 1) != 1) {
        return CHILD_FAILED;
    }
    pollfd wait{.fd = stop, .events = POLLIN, .revents = 0};
    while (::poll(&wait, 1, PERIOD_MS) == 0) {
        static_cast<void>(scada_tag_heartbeat(feeder));
        timespec now{};
        ::clock_gettime(CLOCK_MONOTONIC, &now);
        host.value().heartbeat((std::int64_t{now.tv_sec} * NS_PER_S) +
                               now.tv_nsec);
    }
    scada_tag_detach(feeder);
    return CHILD_OK;
}

// The database, hosted by a child that is DB_UID, for each test.
class ObserverUsers : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(::geteuid(), 0U)
            << "this test becomes other users and must run as root: "
               "sudo ctest --preset root";
        std::string pattern =
            (std::filesystem::temp_directory_path() / "scada-tag-users-XXXXXX")
                .string();
        ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
        root_ = pattern;
        ASSERT_EQ(::chmod(root_.c_str(), TRAVERSABLE), 0);
        base_ = (root_ / "base").string();
        ASSERT_EQ(::mkdir(base_.c_str(), TRAVERSABLE), 0);
        ASSERT_EQ(::chown(base_.c_str(), DB_UID, DB_GID), 0);
        ASSERT_EQ(::chmod(base_.c_str(), BASE_DIR_MODE), 0);
        start_owner();
    }

    void TearDown() override {
        stop_owner();
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] const std::string& base() const { return base_; }

private:
    void start_owner() {
        ASSERT_EQ(::pipe(ready_.data()), 0);
        ASSERT_EQ(::pipe(stop_.data()), 0);
        owner_ = ::fork();
        ASSERT_GE(owner_, 0);
        if (owner_ == 0) {
            ::_exit(become(DB_UID, DB_GID, {DB_GID})
                        ? database_owner(base_, ready_[1], stop_[0])
                        : CHILD_NO_USER);
        }
        // The ends the child uses are closed here, so that a child that
        // ends early is seen at once: the read gives 0, not a byte.
        close_end(ready_[1]);
        close_end(stop_[0]);
        pollfd wait{.fd = ready_[0], .events = POLLIN, .revents = 0};
        ASSERT_EQ(::poll(&wait, 1, WAIT_MS), 1) << "the database did not start";
        char byte = 0;
        ASSERT_EQ(::read(ready_[0], &byte, 1), 1)
            << "the database owner ended early (can it become user " << DB_UID
            << " and reach " << base_ << "?)";
    }

    static void close_end(int& descriptor) {
        if (descriptor >= 0) {
            ::close(descriptor);
            descriptor = -1;
        }
    }

    void stop_owner() {
        if (owner_ > 0) {
            EXPECT_EQ(owner_exit_code(), CHILD_OK)
                << "the database owner failed";
        }
        for (int& descriptor : ready_) {
            close_end(descriptor);
        }
        for (int& descriptor : stop_) {
            close_end(descriptor);
        }
    }

    // Tells the owner to stop, unless it has ended already (a write would
    // raise SIGPIPE then), and returns its exit code, or -1.
    int owner_exit_code() {
        int status = 0;
        if (::waitpid(owner_, &status, WNOHANG) == 0) {
            if (::write(stop_[1], "s", 1) != 1 ||
                ::waitpid(owner_, &status, 0) != owner_) {
                return -1;
            }
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    std::filesystem::path root_;
    std::string base_;
    pid_t owner_ = -1;
    std::array<int, 2> ready_{-1, -1};
    std::array<int, 2> stop_{-1, -1};
};

TEST_F(ObserverUsers, RootObservesWithoutBeingInTheGroup) {
    std::vector<gid_t> groups(
        static_cast<std::size_t>(::getgroups(0, nullptr)));
    ASSERT_GE(::getgroups(static_cast<int>(groups.size()), groups.data()), 0);
    EXPECT_EQ(std::ranges::find(groups, DB_GID), groups.end());
    EXPECT_NE(::getegid(), DB_GID);
    EXPECT_EQ(observe_and_read(base()), CHILD_OK);
}

TEST_F(ObserverUsers, AUserOutsideTheGroupIsRefused) {
    EXPECT_EQ(run_as(OUTSIDER_UID, OUTSIDER_UID, {},
                     [base = base()] { return observe_and_read(base); }),
              SCADA_E_PERMISSION);
    // Attach is refused in the same way.
    EXPECT_EQ(run_as(OUTSIDER_UID, OUTSIDER_UID, {},
                     [base = base()] {
                         const scada_attach_opts options = options_for(base);
                         scada_ctx* ctx = nullptr;
                         return scada_tag_attach_opts("io", &options, &ctx);
                     }),
              SCADA_E_PERMISSION);
}

TEST_F(ObserverUsers, AGroupMemberOutsideTheProcessTableObserves) {
    EXPECT_EQ(run_as(MEMBER_UID, MEMBER_UID, {DB_GID},
                     [base = base()] { return observe_and_read(base); }),
              CHILD_OK);
}

}  // namespace
