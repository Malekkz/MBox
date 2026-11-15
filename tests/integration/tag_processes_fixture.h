#ifndef SCADA_TESTS_INTEGRATION_TAG_PROCESSES_FIXTURE_H
#define SCADA_TESTS_INTEGRATION_TAG_PROCESSES_FIXTURE_H

// A tag database in a temporary directory for the integration tests of tag
// read and write: scada-tagd and helper processes are real processes, and
// the test process itself attaches as a reader through the C interface.

#include "tagd_fixture.h"

#include "scada/tag/scada_tag.h"
#include "scada/test/child_process.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

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

// The owners of the process `load` of the large tag set and the tags of
// each: 50,000 tags across 10 owners, the reference load of the contract
// (section 15, test 2).
inline constexpr std::size_t LOAD_OWNERS = 10;
inline constexpr std::size_t LOAD_TAGS_PER_OWNER = 5000;
// At least twice the tags of an owner (contract, section 14).
inline constexpr int LOAD_RING_CAPACITY = 16384;

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
    // without tags. All run as the
    // user of the test.
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

    // The large tag set: scada-tagd; `load` with LOAD_OWNERS owner threads
    // `load.w<k>`, each with LOAD_TAGS_PER_OWNER tags `load.w<k>.t<i>` of
    // type UINT64; and `reader` without tags.
    void write_load_tag_set() const {
        const std::string uid = std::to_string(::geteuid());
        std::ofstream file(tag_set_path());
        file << R"({"format": 1, "processes": [{"name": "tagd", "uid": )" << uid
             << R"(}, {"name": "load", "uid": )" << uid
             << R"(}, {"name": "reader", "uid": )" << uid
             << R"(}], "queues": [], "owners": [)";
        for (std::size_t owner = 0; owner < LOAD_OWNERS; ++owner) {
            file << (owner == 0 ? "" : ", ") << R"({"name": "load.w)" << owner
                 << R"(", "process": "load", "change_ring_capacity": )"
                 << LOAD_RING_CAPACITY << "}";
        }
        file << R"(], "tags": [)";
        for (std::size_t owner = 0; owner < LOAD_OWNERS; ++owner) {
            for (std::size_t tag = 0; tag < LOAD_TAGS_PER_OWNER; ++tag) {
                file << (owner + tag == 0 ? "" : ",\n") << R"({"name": "load.w)"
                     << owner << ".t" << tag << R"(", "owner": "load.w)"
                     << owner << R"(", "type": "uint64"})";
            }
        }
        file << "]}\n";
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
