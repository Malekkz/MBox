// Huge pages for the `tags` regions (contract section 16): is the 99.9th
// percentile of a read lower when the regions are backed by huge pages?
// (Issue P1-13, decision D-64.) It measures and reports; the decision is
// taken from the figures of both architectures, not by this test.
//
// A huge page is 2 MB. At the reference load an owner has 5,000 tags, and
// its `tags` region of 320 KB cannot hold one, so huge pages could not be
// used there at all. This test therefore uses one owner of 50,000 tags
// (3.2 MB), written by scada-sim at 50,000 updates per second, and reads
// them while it writes: once with the database on a tmpfs mounted
// huge=never, once on one mounted huge=always. In the second it checks
// that the region this process reads really is mapped with huge pages, so
// a figure that was not measured cannot pass as one.
//
// Mounting needs root: the CTest label is `root` (sudo ctest --preset
// root). As conformance test 11 does, the test moves into a mount namespace
// of its own whose mounts do not propagate, and mounts a tmpfs over its
// temporary directory; /dev/shm is not touched.

#include "tag_processes_fixture.h"
#include "tagd_fixture.h"

#include "scada/sim/layout.h"
#include "scada/sim/pattern.h"
#include "scada/tag/scada_tag.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>  // NOLINT(modernize-deprecated-headers): clock_gettime is declared here, not in <ctime>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace scada::test::tagd;
using scada::test::ChildProcess;

constexpr std::uint64_t RATE = 50'000;
constexpr std::uint64_t TAGS = 50'000;
// Long enough for both measurements; the simulator is stopped after them.
constexpr std::uint64_t ROUNDS = 600;
constexpr std::size_t READ_SAMPLES = 1'000'000;
constexpr std::size_t BATCH_SAMPLES = 20'000;
constexpr std::size_t BATCH = 500;
constexpr std::size_t CALLS_PER_BEAT = 2'000;
// The step through the tags between two reads, so that consecutive reads
// do not hit the same page.
constexpr std::size_t READ_STRIDE = 97;
constexpr std::int64_t NS_PER_S = 1'000'000'000;
constexpr std::size_t PER_MILLE = 1000;
constexpr std::size_t P999 = 999;

std::int64_t now_ns() {
    timespec now{};
    static_cast<void>(::clock_gettime(CLOCK_MONOTONIC, &now));
    return (std::int64_t{now.tv_sec} * NS_PER_S) + now.tv_nsec;
}

std::string last_error() {
    return std::error_code(errno, std::generic_category()).message();
}

// One owner of TAGS tags in one process.
scada::sim::Layout big_owner() {
    return scada::sim::Layout{.prefix = "big",
                              .processes = 1,
                              .owners = 1,
                              .tags = TAGS,
                              .patterns = {scada::sim::Pattern::CHECK}};
}

// The median and the 99.9th percentile of `times`, in ns.
struct Figures {
    std::int64_t median = 0;
    std::int64_t p999 = 0;
};

Figures figures_of(std::vector<std::int64_t>& times) {
    std::ranges::sort(times);
    return {.median = times.at(times.size() / 2),
            .p999 = times.at(times.size() * P999 / PER_MILLE)};
}

// The kB of the mappings of this process whose path holds `part` that are
// mapped with huge pages (ShmemPmdMapped and FilePmdMapped of
// /proc/self/smaps).
std::uint64_t huge_mapped_kb(std::string_view part) {
    std::ifstream smaps("/proc/self/smaps");
    std::string line;
    bool inside = false;
    std::uint64_t total = 0;
    while (std::getline(smaps, line)) {
        // A mapping starts with "<start>-<end> "; its fields follow.
        if (!line.empty() && line.find('-') < line.find(' ') &&
            line.find(':') > line.find(' ')) {
            inside = line.find(part) != std::string::npos;
            continue;
        }
        if (inside && (line.starts_with("ShmemPmdMapped:") ||
                       line.starts_with("FilePmdMapped:"))) {
            std::istringstream fields(line.substr(line.find(':') + 1));
            std::uint64_t value_kb = 0;
            fields >> value_kb;
            total += value_kb;
        }
    }
    return total;
}

class HugePages : public TagProcesses {
protected:
    void SetUp() override {
        ASSERT_EQ(::geteuid(), 0U)
            << "this test mounts a file system; run it as root (sudo ctest "
               "--preset root)";
        TagProcesses::SetUp();
        if (HasFatalFailure()) {
            return;
        }
        // A mount namespace for this thread and the processes it starts,
        // whose mounts are not seen outside it.
        ASSERT_EQ(::unshare(CLONE_NEWNS), 0) << last_error();
        ASSERT_EQ(::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr),
                  0)
            << last_error();
    }

    void TearDown() override {
        if (mounted_) {
            EXPECT_EQ(::umount2(root().c_str(), MNT_DETACH), 0) << last_error();
        }
        TagProcesses::TearDown();
    }

    // Mounts a tmpfs with the huge page mode `huge` ("never" or "always")
    // over root() and makes the base directory again on it.
    void mount_tmpfs(const std::string& huge) {
        const std::string options = "size=256m,mode=0700,huge=" + huge;
        ASSERT_EQ(::mount("tmpfs", root().c_str(), "tmpfs",
                          MS_NOSUID | MS_NODEV | MS_NOEXEC, options.c_str()),
                  0)
            << options << ": " << last_error();
        mounted_ = true;
        std::error_code error;
        std::filesystem::create_directory(base(), error);
        ASSERT_FALSE(error) << error.message();
        ASSERT_EQ(::chmod(base().c_str(), BASE_DIR_MODE), 0);
    }

    // Measures `scada_tag_read` and `scada_tag_read_many` of BATCH tags on
    // the owner of big_owner() while scada-sim writes it, and prints and
    // records the figures under `mode`. In mode "always" the region read
    // must be mapped with huge pages.
    void measure(const std::string& mode) {
        mount_tmpfs(mode);
        if (HasFatalFailure()) {
            return;
        }
        const scada::sim::Layout layout = big_owner();
        write_sim_tag_set(layout);
        std::optional<RunningTagd> tagd = ready_tagd();
        if (!tagd.has_value()) {
            FAIL() << "no tagd";
        }
        std::optional<ChildProcess> sim =
            start_sim(layout, {"--rate", std::to_string(RATE), "--rounds",
                               std::to_string(ROUNDS)});
        if (!sim.has_value()) {
            FAIL() << "no sim";
        }
        ASSERT_TRUE(prints(*sim, "writing\n"));
        const Attached reader(base(), "reader");
        std::vector<std::uint32_t> handles;
        handles.reserve(TAGS);
        for (std::uint64_t tag = 0; tag < TAGS; ++tag) {
            handles.push_back(
                reader.resolve(scada::sim::tag_name(layout, 0, tag)));
        }
        Figures single{};
        Figures batch{};
        time_reads(reader.ctx(), handles, single, batch);
        const std::uint64_t huge_kb = huge_mapped_kb("tags.");
        std::cout << "huge pages: mode=" << mode
                  << " huge_mapped_kb=" << huge_kb
                  << " read_median_ns=" << single.median
                  << " read_p999_ns=" << single.p999
                  << " read_many_500_median_ns=" << batch.median
                  << " read_many_500_p999_ns=" << batch.p999 << "\n";
        RecordProperty(mode + "_read_p999_ns", std::to_string(single.p999));
        RecordProperty(mode + "_huge_mapped_kb", std::to_string(huge_kb));
        // In mode "always" a region not mapped with huge pages means that
        // nothing was measured.
        EXPECT_EQ(huge_kb > 0, mode == "always") << huge_kb << " kB";
        EXPECT_TRUE(stops_cleanly(*sim));
        stop(*tagd);
    }

    // Times READ_SAMPLES reads and BATCH_SAMPLES reads of BATCH tags,
    // each call alone.
    static void time_reads(scada_ctx* ctx,
                           const std::vector<std::uint32_t>& handles,
                           Figures& single, Figures& batch) {
        std::vector<std::int64_t> times(READ_SAMPLES);
        std::array<scada_value, BATCH> values{};
        std::array<int, BATCH> status{};
        std::size_t failed = 0;
        for (std::size_t call = 0; call < READ_SAMPLES; ++call) {
            if (call % CALLS_PER_BEAT == 0) {
                static_cast<void>(scada_tag_heartbeat(ctx));
            }
            const std::uint32_t handle =
                handles[(call * READ_STRIDE) % handles.size()];
            const std::int64_t start = now_ns();
            const int code = scada_tag_read(ctx, handle, values.data());
            times[call] = now_ns() - start;
            failed += code == SCADA_OK ? 0 : 1;
        }
        single = figures_of(times);
        times.assign(BATCH_SAMPLES, 0);
        for (std::size_t call = 0; call < BATCH_SAMPLES; ++call) {
            if (call % CALLS_PER_BEAT == 0) {
                static_cast<void>(scada_tag_heartbeat(ctx));
            }
            const std::size_t first = (call * BATCH) % handles.size();
            const std::int64_t start = now_ns();
            const int code = scada_tag_read_many(ctx, &handles[first], BATCH,
                                                 values.data(), status.data());
            times[call] = now_ns() - start;
            failed += code == SCADA_OK ? 0 : 1;
        }
        batch = figures_of(times);
        // SCADA_E_BUSY is possible against a writer and is not a failure of
        // this measurement; it shows in the 99.9th percentile.
        ::testing::Test::RecordProperty("reads_not_ok", std::to_string(failed));
    }

private:
    bool mounted_ = false;
};

TEST_F(HugePages, ReadsOnANormalTmpfs) {
    measure("never");
}

TEST_F(HugePages, ReadsOnATmpfsWithHugePages) {
    measure("always");
}

}  // namespace
