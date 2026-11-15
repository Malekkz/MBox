#ifndef SCADA_TESTS_INTEGRATION_TAG_PROCESSES_FIXTURE_H
#define SCADA_TESTS_INTEGRATION_TAG_PROCESSES_FIXTURE_H

// A tag database in a temporary directory for the integration tests of tag
// read and write: scada-tagd and helper processes are real processes, and
// the test process itself attaches as a reader through the C interface.

#include "tagd_fixture.h"

#include "scada/common/result.h"
#include "scada/sim/layout.h"
#include "scada/sim/pattern.h"
#include "scada/sim/tag_set.h"
#include "scada/tag/scada_tag.h"
#include "scada/tag/tag_set.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace scada::test::tagd {

// Times of the small tag set, in ms. The heartbeat timeout of `io` is long
// against its heartbeat period, so that a busy machine does not make it
// look dead.
inline constexpr int CHECK_MS = 50;
inline constexpr int IO_TIMEOUT_MS = 1000;
inline constexpr int IO_PERIOD_MS = 50;

// The large tag set: the process `load` with 10 owner threads `load.w<k>`
// and 50,000 CHECK tags `load.w<k>.t<i>` of type UINT64, the reference load
// of the contract (section 15, test 2).
inline constexpr std::size_t LOAD_OWNERS = 10;
inline constexpr std::size_t LOAD_TAGS_PER_OWNER = 5000;
// The heartbeat timeout of `load` and the readers of the large tag set, in
// ms: as long as the longest wait of a test (WAIT_LIMIT). A loaded machine
// can leave a process, or a reader inside one read_many, without the CPU
// for more than the default 3 s; that owner is then down by the rules of
// section 10.1 (decision D-34), which is not what the load tests look at
// (issue #62).
inline constexpr std::uint32_t LOAD_HEARTBEAT_TIMEOUT_MS = 30'000;
static_assert(std::chrono::milliseconds(LOAD_HEARTBEAT_TIMEOUT_MS) ==
              WAIT_LIMIT);

inline scada::sim::Layout load_layout() {
    return scada::sim::Layout{.prefix = "load",
                              .processes = 1,
                              .owners = LOAD_OWNERS,
                              .tags = LOAD_OWNERS * LOAD_TAGS_PER_OWNER,
                              .patterns = {scada::sim::Pattern::CHECK}};
}

// The arguments of `scada-sim <command>` that give `layout`.
inline std::vector<std::string> sim_arguments(
    const std::string& command, const scada::sim::Layout& layout) {
    std::string patterns;
    for (const scada::sim::Pattern pattern : layout.patterns) {
        patterns += (patterns.empty() ? "" : ",");
        patterns += scada::sim::to_string(pattern);
    }
    return {command,
            "--prefix",
            layout.prefix,
            "--processes",
            std::to_string(layout.processes),
            "--owners",
            std::to_string(layout.owners),
            "--tags",
            std::to_string(layout.tags),
            "--pattern",
            patterns};
}

// Mode of the base directory (contract, section 3.1).
inline constexpr mode_t BASE_DIR_MODE = 02750;

// The test process attached as one process of the tag set; detached when
// it goes out of scope.
class Attached {
public:
    // Attaches as `process` to the database in `base_dir`, whose scada-tagd
    // runs as the user of the test. A failure is a failed expectation, and
    // `ctx()` is null.
    Attached(const std::string& base_dir, const char* process) {
        const scada_attach_opts options{.base_dir = base_dir.c_str(),
                                        .retain_dir = nullptr,
                                        .tagd_uid = std::int64_t{::geteuid()}};
        EXPECT_EQ(scada_tag_attach_opts(process, &options, &ctx_), SCADA_OK);
    }
    ~Attached() { scada_tag_detach(ctx_); }
    Attached(const Attached&) = delete;
    Attached& operator=(const Attached&) = delete;
    Attached(Attached&&) = delete;
    Attached& operator=(Attached&&) = delete;

    [[nodiscard]] scada_ctx* ctx() const { return ctx_; }

    // The handle of `name`, or SCADA_INVALID_HANDLE with a failed
    // expectation.
    [[nodiscard]] std::uint32_t resolve(const std::string& name) const {
        std::uint32_t handle = SCADA_INVALID_HANDLE;
        EXPECT_EQ(scada_tag_resolve(ctx_, name.c_str(), &handle), SCADA_OK)
            << name;
        return handle;
    }

private:
    scada_ctx* ctx_ = nullptr;
};

// An observer context of the test process (contract, section 11.4);
// detached when it goes out of scope.
class Observed {
public:
    // Observes the database in `base_dir`, whose scada-tagd runs as the
    // user of the test. A failure is a failed expectation, and `ctx()` is
    // null.
    explicit Observed(const std::string& base_dir) {
        const scada_attach_opts options{.base_dir = base_dir.c_str(),
                                        .retain_dir = nullptr,
                                        .tagd_uid = std::int64_t{::geteuid()}};
        EXPECT_EQ(scada_tag_observe_opts(&options, &ctx_), SCADA_OK);
    }
    ~Observed() { scada_tag_detach(ctx_); }
    Observed(const Observed&) = delete;
    Observed& operator=(const Observed&) = delete;
    Observed(Observed&&) = delete;
    Observed& operator=(Observed&&) = delete;

    [[nodiscard]] scada_ctx* ctx() const { return ctx_; }

private:
    scada_ctx* ctx_ = nullptr;
};

class TagProcesses : public ::testing::Test {
protected:
    void SetUp() override {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "scada-tag-it-XXXXXX")
                .string();
        ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
        root_ = pattern;
        base_ = root_ / "base";
        std::filesystem::create_directory(base_);
        // The owner and mode the platform gives /dev/shm/scada.
        ASSERT_EQ(::chmod(base_.c_str(), BASE_DIR_MODE), 0);
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] std::string base() const { return base_.string(); }

    [[nodiscard]] std::string tag_set_path() const {
        return (root_ / "tag-set.json").string();
    }

    // The small tag set: scada-tagd; `io` with the tag `plant.level`;
    // `probe` with the tag `probe.out`, the event queue `events.probe` to
    // `io`, the requests of `feeder` in `cmd.probe.feeder`, answered in
    // `result.probe.feeder`, and its own requests to `io` in
    // `cmd.io.probe`, answered in `result.io.probe`; `reader` and `feeder`
    // without tags. All run as the user of the test.
    void write_small_tag_set() const {
        const std::string uid = std::to_string(::geteuid());
        std::ofstream file(tag_set_path());
        file << R"({"format": 1, "heartbeat_check_ms": )" << CHECK_MS
             << R"(, "processes": [{"name": "tagd", "uid": )" << uid
             << R"(}, {"name": "io", "uid": )" << uid
             << R"(, "heartbeat_timeout_ms": )" << IO_TIMEOUT_MS
             << R"(}, {"name": "probe", "uid": )" << uid
             << R"(}, {"name": "reader", "uid": )" << uid
             << R"(}, {"name": "feeder", "uid": )" << uid
             << R"(}], "owners": [)" << R"({"name": "io", "process": "io", )"
             << R"("change_ring_capacity": 1024}, )"
             << R"({"name": "probe", "process": "probe", )"
             << R"("change_ring_capacity": 1024}], "queues": [)"
             << R"({"name": "events.probe", "producer": "probe", )"
             << R"("consumer": "io"}, )"
             << R"({"name": "cmd.probe.feeder", "producer": "feeder", )"
             << R"("consumer": "probe"}, )"
             << R"({"name": "result.probe.feeder", "producer": "probe", )"
             << R"("consumer": "feeder"}, )"
             << R"({"name": "cmd.io.probe", "producer": "probe", )"
             << R"("consumer": "io"}, )"
             << R"({"name": "result.io.probe", "producer": "io", )"
             << R"("consumer": "probe"}], "tags": [)"
             << R"({"name": "plant.level", "owner": "io", "type": "float64"}, )"
             << R"({"name": "probe.out", "owner": "probe", )"
             << R"("type": "float64"}]})" << "\n";
    }

    // The tag set of the simulation `layout` (scada-sim, issue P1-10):
    // scada-tagd, the simulator processes of the layout and `reader`
    // without tags, all running as the user of the test, with the
    // heartbeat timeout `heartbeat_timeout_ms` for all but scada-tagd.
    void write_sim_tag_set(const scada::sim::Layout& layout,
                           std::uint32_t heartbeat_timeout_ms =
                               scada::tag::DEFAULT_HEARTBEAT_TIMEOUT_MS) const {
        scada::sim::TagSetOptions options;
        options.layout = layout;
        options.heartbeat_timeout_ms = heartbeat_timeout_ms;
        options.uid = ::geteuid();
        options.tagd_uid = ::geteuid();
        options.readers = {"reader"};
        std::string detail;
        const Result<scada::tag::TagSet> tag_set =
            scada::sim::make_tag_set(options, detail);
        if (!tag_set.ok()) {
            ADD_FAILURE() << "no tag set: " << detail;
            return;
        }
        std::ofstream file(tag_set_path());
        file << scada::sim::tag_set_json(tag_set.value());
    }

    // The large tag set: the simulation `load_layout()`, with the heartbeat
    // timeout LOAD_HEARTBEAT_TIMEOUT_MS.
    void write_load_tag_set() const {
        write_sim_tag_set(load_layout(), LOAD_HEARTBEAT_TIMEOUT_MS);
    }

    // Starts the simulator process `instance` of `layout` with the options
    // `extra` of `scada-sim run`, and waits until it has attached. Nothing,
    // with a failed expectation, if it did not attach.
    [[nodiscard]] std::optional<ChildProcess> start_sim(
        const scada::sim::Layout& layout, const std::vector<std::string>& extra,
        std::uint32_t instance = 0) const {
        std::vector<std::string> arguments = sim_arguments("run", layout);
        arguments.insert(arguments.end(),
                         {"--instance", std::to_string(instance), "--base-dir",
                          base(), "--tagd-uid", std::to_string(::geteuid())});
        arguments.insert(arguments.end(), extra.begin(), extra.end());
        std::optional<ChildProcess> sim =
            start_helper("SCADA_SIM_COMMAND", arguments);
        if (sim.has_value() && !sim->wait_for_output(attached, WAIT_LIMIT)) {
            ADD_FAILURE() << "scada-sim did not attach: " << sim->err();
            return std::nullopt;
        }
        return sim;
    }

    // Starts scada-tagd on the tag set written before and waits until it
    // has reported readiness.
    [[nodiscard]] std::optional<RunningTagd> ready_tagd() const {
        return scada::test::tagd::ready_tagd(tag_set_path(), base());
    }

    // Starts the helper whose command CTest put into the environment
    // variable `variable`, with `arguments`. Nothing, with a failed
    // expectation, if it could not be started.
    [[nodiscard]] static std::optional<ChildProcess> start_helper(
        const char* variable, const std::vector<std::string>& arguments) {
        const std::vector<std::string> command =
            command_from(variable, arguments);
        if (command.empty()) {
            return std::nullopt;
        }
        std::optional<ChildProcess> helper = ChildProcess::start(command);
        if (!helper.has_value()) {
            ADD_FAILURE() << "could not start " << command.front();
        }
        return helper;
    }

    // Starts the helper that attaches as `io` and writes its heartbeat,
    // with `extra` as further arguments, and waits until it has attached.
    [[nodiscard]] std::optional<ChildProcess> attached_io(
        const std::vector<std::string>& extra = {}) const {
        std::vector<std::string> arguments{base(), "io",
                                           std::to_string(IO_PERIOD_MS)};
        arguments.insert(arguments.end(), extra.begin(), extra.end());
        std::optional<ChildProcess> helper =
            start_helper("SCADA_TAG_PEER_COMMAND", arguments);
        if (helper.has_value()) {
            EXPECT_TRUE(helper->wait_for_output(attached, WAIT_LIMIT))
                << helper->err();
        }
        return helper;
    }

private:
    std::filesystem::path root_;
    std::filesystem::path base_;
};

}  // namespace scada::test::tagd

#endif  // SCADA_TESTS_INTEGRATION_TAG_PROCESSES_FIXTURE_H
