// The driver process through real tags (driver interface contract 2.4,
// 2.6, 9.1, 10; conformance tests 1, 2, 16, 17; decision D-142): scada-tagd,
// the test protocol's simulator (scada-driver-test-sim) and the driver
// process scada-drv-test are real processes; the test observes the tags
// through the C interface. Every wait is on a condition with a time-out:
// a change of the observed owner, or the end of a process. Nothing sleeps.

#include "tagd_fixture.h"

#include "scada/common/fd.h"
#include "scada/tag/scada_tag.h"
#include "scada/test/child_process.h"
#include "scada/test/temp_directory.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal numbers are declared here, not in <csignal>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers): the test data

using scada::test::ChildEnding;
using scada::test::ChildProcess;
using scada::test::tagd::command_from;
using scada::test::tagd::RunningTagd;
using scada::test::tagd::stops_cleanly;
using scada::test::tagd::WAIT_LIMIT;
using std::chrono::milliseconds;

constexpr mode_t BASE_DIR_MODE = 02750;
constexpr int EXIT_STALL = 4;  // scada::driver::EXIT_STALL

// One channel of the tag set: its name and the tags `<device>.<point>`.
struct ChannelTags {
    std::string name;
    std::vector<std::string> tags;
};

// The port of the simulator's first line, `tcp <port>`; 0 if none.
std::uint16_t port_of(const ChildProcess& sim) {
    std::istringstream text(sim.out());
    std::string word;
    std::uint16_t port = 0;
    text >> word >> port;
    return word == "tcp" ? port : 0;
}

// The arguments of one channel and of one device of the driver.
std::vector<std::string> tcp_channel(const std::string& name,
                                     std::uint16_t port,
                                     const std::string& connect_ms = "1000") {
    return {"--channel",
            name,
            "tcp",
            "127.0.0.1",
            std::to_string(port),
            "--connect-timeout-ms",
            connect_ms,
            "--reconnect-ms",
            "100",
            "400"};
}

std::vector<std::string> device(const std::string& name, int unit,
                                const std::string& group) {
    return {"--device",
            name,
            std::to_string(unit),
            "--timeout-ms",
            "500",
            "--point",
            "v",
            "0",
            group};
}

template <class... Parts>
std::vector<std::string> joined(const Parts&... parts) {
    std::vector<std::string> out;
    (out.insert(out.end(), parts.begin(), parts.end()), ...);
    return out;
}

bool good_with(const scada_value& value, std::int64_t expected) {
    return value.quality == SCADA_QUALITY_GOOD &&
           static_cast<std::int64_t>(value.value) == expected &&
           (value.quality_flags & SCADA_FLAG_STALE) == 0;
}

bool comm_fail(const scada_value& value) {
    return value.quality == SCADA_QUALITY_BAD &&
           (value.quality_flags & SCADA_FLAG_COMM_FAIL) != 0;
}

bool stale(const scada_value& value) {
    return (value.quality_flags & SCADA_FLAG_STALE) != 0;
}

// A socket on 127.0.0.1 with an ephemeral port: bound only, so that a
// connect is refused, or listening with an accept queue held full, so that
// a connect waits for its deadline.
class LocalPort {
public:
    explicit LocalPort(bool full_queue)
        : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
        address_.sin_family = AF_INET;
        (void)::inet_pton(AF_INET, "127.0.0.1", &address_.sin_addr);
        socklen_t length = sizeof(address_);
        EXPECT_EQ(::bind(fd_.get(), generic(), sizeof(address_)), 0);
        EXPECT_EQ(::getsockname(fd_.get(), generic(), &length), 0);
        if (full_queue) {
            // A backlog of 0 holds one connection; three keep it full.
            EXPECT_EQ(::listen(fd_.get(), 0), 0);
            for (int index = 0; index < 3; ++index) {
                fillers_.emplace_back(::socket(
                    AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
                const int begun = ::connect(fillers_.back().get(), generic(),
                                            sizeof(address_));
                EXPECT_TRUE(begun == 0 || errno == EINPROGRESS);
            }
        }
    }

    [[nodiscard]] std::uint16_t port() const {
        return ntohs(address_.sin_port);
    }

private:
    sockaddr* generic() {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,scada-banned): the socket interface takes every address type through a sockaddr pointer
        return reinterpret_cast<sockaddr*>(&address_);
    }

    scada::Fd fd_;
    sockaddr_in address_{};
    std::vector<scada::Fd> fillers_;
};

class DriverProcess : public ::testing::Test {
protected:
    void SetUp() override {
        std::optional<scada::test::TempDirectory> temp =
            scada::test::TempDirectory::create("scada-driver-it-");
        if (!temp.has_value()) {
            FAIL() << "no temporary directory";
        }
        temp_.emplace(std::move(*temp));
        base_ = temp_->path() / "base";
        std::filesystem::create_directory(base_);
        ASSERT_EQ(::chmod(base_.c_str(), BASE_DIR_MODE), 0);
    }

    void TearDown() override {
        if (ctx_ != nullptr) {
            scada_sub_close(sub_);
            scada_tag_detach(ctx_);
        }
        tagd_.reset();
        temp_.reset();
    }

    // Writes the tag set and starts scada-tagd. With one channel its owner
    // is named like the process (TD 1), else `drv-test.<channel>`.
    void start_tagd(const std::vector<ChannelTags>& channels) {
        std::string owners;
        std::string tags;
        for (const ChannelTags& channel : channels) {
            std::string owner = "drv-test";
            if (channels.size() > 1) {
                owner += ".";
                owner += channel.name;
            }
            owners_.push_back(owner);
            owners += owners.empty() ? R"({"name": ")" : R"(, {"name": ")";
            owners += owner;
            owners +=
                R"(", "process": "drv-test", "change_ring_capacity": 1024})";
            for (const std::string& tag : channel.tags) {
                tags += tags.empty() ? R"({"name": ")" : R"(, {"name": ")";
                tags += tag;
                tags += R"(", "owner": ")";
                tags += owner;
                tags += R"(", "type": "int64"})";
            }
        }
        const std::string uid = std::to_string(::geteuid());
        std::ofstream file(tag_set_path());
        file << R"({"format": 1, "heartbeat_check_ms": 50, "processes": [)"
             << R"({"name": "tagd", "uid": )" << uid << "}, "
             << R"({"name": "drv-test", "uid": )" << uid
             << R"(, "heartbeat_timeout_ms": 1500}], "owners": [)" << owners
             << R"(], "queues": [], "tags": [)" << tags << "]}\n";
        file.close();
        std::optional<RunningTagd> started =
            scada::test::tagd::ready_tagd(tag_set_path(), base_.string());
        if (!started.has_value()) {
            FAIL() << "scada-tagd did not start";
        }
        tagd_.emplace(std::move(*started));
    }

    // The simulator serving units 1 and 2, registers 0 to 3, with `args`.
    static std::optional<ChildProcess> start_sim(
        const std::vector<std::string>& args) {
        std::vector<std::string> all{"--tcp", "--map", "1", "0", "0", "3",
                                     "--map", "2",     "0", "0", "3"};
        all.insert(all.end(), args.begin(), args.end());
        auto sim = ChildProcess::start(
            command_from("SCADA_DRIVER_TEST_SIM_COMMAND", all));
        if (!sim.has_value() || !sim->wait_for_output(
                                    [](std::string_view out) {
                                        return out.find('\n') !=
                                               std::string_view::npos;
                                    },
                                    WAIT_LIMIT)) {
            ADD_FAILURE() << "the simulator did not start";
            return std::nullopt;
        }
        return sim;
    }

    // scada-drv-test on this database, with `args` for its channels.
    [[nodiscard]] std::optional<ChildProcess> start_driver(
        const std::vector<std::string>& args) const {
        std::vector<std::string> all{
            "--base-dir",     base_.string(),
            "--tagd-uid",     std::to_string(::geteuid()),
            "--heartbeat-ms", "50"};
        all.insert(all.end(), args.begin(), args.end());
        auto driver =
            ChildProcess::start(command_from("SCADA_DRV_TEST_COMMAND", all));
        if (!driver.has_value()) {
            ADD_FAILURE() << "the driver did not start";
        }
        return driver;
    }

    // Observes the database and subscribes to the first owner, whose
    // changes are the waits of `eventually`.
    void observe() {
        if (ctx_ != nullptr) {
            return;
        }
        const scada_attach_opts options{.base_dir = base_.c_str(),
                                        .retain_dir = nullptr,
                                        .tagd_uid = std::int64_t{::geteuid()}};
        ASSERT_EQ(scada_tag_observe_opts(&options, &ctx_), SCADA_OK);
        ASSERT_EQ(scada_sub_open(ctx_, owners_.front().c_str(), &sub_),
                  SCADA_OK);
    }

    std::uint32_t handle(const std::string& name) {
        std::uint32_t found = SCADA_INVALID_HANDLE;
        EXPECT_EQ(scada_tag_resolve(ctx_, name.c_str(), &found), SCADA_OK)
            << name;
        return found;
    }

    scada_value read(std::uint32_t tag) {
        scada_value value{};
        EXPECT_EQ(scada_tag_read(ctx_, tag, &value), SCADA_OK);
        return value;
    }

    // Waits until `holds` is true of a read of `tag`, up to WAIT_LIMIT;
    // each wait between reads is for the next change of the observed
    // owner, at most 50 ms.
    bool eventually(std::uint32_t tag,
                    const std::function<bool(const scada_value&)>& holds) {
        const auto end = std::chrono::steady_clock::now() + WAIT_LIMIT;
        while (std::chrono::steady_clock::now() < end) {
            if (holds(read(tag))) {
                return true;
            }
            scada_change change{};
            (void)scada_sub_next(sub_, &change, 50);
        }
        return false;
    }

    // Runs the driver with `args` until `tag` holds `holds`, then stops it
    // with SIGTERM: a clean exit, and no thread check failed (test 1).
    ::testing::AssertionResult run_until(
        const std::vector<std::string>& args, const std::string& tag,
        const std::function<bool(const scada_value&)>& holds) {
        std::optional<ChildProcess> driver = start_driver(args);
        if (!driver.has_value()) {
            return ::testing::AssertionFailure() << "no driver";
        }
        observe();
        if (!eventually(handle(tag), holds)) {
            return ::testing::AssertionFailure()
                   << tag << " never held: " << driver->err();
        }
        const ::testing::AssertionResult stopped = stops_cleanly(*driver);
        if (stopped &&
            driver->err().find("thread check failed") != std::string::npos) {
            return ::testing::AssertionFailure() << driver->err();
        }
        return stopped;
    }

private:
    [[nodiscard]] std::string tag_set_path() const {
        return temp_.has_value() ? (temp_->path() / "tag-set.json").string()
                                 : std::string();
    }

    std::optional<scada::test::TempDirectory> temp_;
    std::filesystem::path base_;
    std::vector<std::string> owners_;
    std::optional<RunningTagd> tagd_;
    scada_ctx* ctx_ = nullptr;
    scada_sub* sub_ = nullptr;
};

// --- 9.1, conformance test 1 ------------------------------------------------

TEST_F(DriverProcess, EveryTagIsNotInitialisedBeforeTheFirstRequest) {
    // 9.1: the value as stored, BAD with NOT_INIT, no source time. A
    // disabled device's tag gets only that write, so it shows what the
    // worker wrote before its first request; the first run gives it a value.
    // Every call of the protocol runs on its thread (conformance test 1):
    // `run_until` fails on the driver's thread check.
    start_tagd({{.name = "line1", .tags = {"d1.v", "d2.v"}}});
    auto sim =
        start_sim({"--set", "1", "0", "0", "11", "--set", "2", "0", "0", "22"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    const std::vector<std::string> channel =
        tcp_channel("line1", port_of(*sim));
    ASSERT_TRUE(run_until(
        joined(channel, device("d1", 1, "0"), device("d2", 2, "0")), "d2.v",
        [](const scada_value& value) { return good_with(value, 22); }));
    std::vector<std::string> disabled = device("d2", 2, "0");
    disabled.emplace_back("--disabled");
    ASSERT_TRUE(run_until(
        joined(channel, device("d1", 1, "0"), disabled), "d1.v",
        [](const scada_value& value) { return good_with(value, 11); }));

    const scada_value stored = read(handle("d2.v"));
    EXPECT_EQ(stored.quality, SCADA_QUALITY_BAD);
    // Source time 0 is "no device time": the library stores its own update
    // time with LOCAL_TIME (TD 13, scada_tag_write). The driver has stopped,
    // so the read adds OWNER_DOWN, which is never stored.
    EXPECT_EQ(stored.quality_flags & ~SCADA_FLAG_OWNER_DOWN,
              SCADA_FLAG_NOT_INIT | SCADA_FLAG_LOCAL_TIME);
    EXPECT_EQ(static_cast<std::int64_t>(stored.value), 22);
    EXPECT_EQ(stored.source_time_ns, stored.update_time_ns);
}

// --- Conformance tests 16 and 17 --------------------------------------------

// Kills `process` and waits for its end.
::testing::AssertionResult killed(ChildProcess& process) {
    if (!process.send_signal(SIGKILL) ||
        !process.wait_for_ending(WAIT_LIMIT).has_value()) {
        return ::testing::AssertionFailure() << "did not end on SIGKILL";
    }
    return ::testing::AssertionSuccess();
}

TEST_F(DriverProcess, ALostConnectionSetsEveryDeviceBadWithCommFail) {
    start_tagd({{.name = "line1", .tags = {"d1.v", "d2.v"}}});
    auto sim =
        start_sim({"--set", "1", "0", "0", "5", "--set", "2", "0", "0", "6"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    auto driver =
        start_driver(joined(tcp_channel("line1", port_of(*sim)),
                            device("d1", 1, "0"), device("d2", 2, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    const std::uint32_t one = handle("d1.v");
    const std::uint32_t two = handle("d2.v");
    ASSERT_TRUE(eventually(one, [&](const auto& value) {
        return good_with(value, 5) && good_with(read(two), 6);
    }));
    // The peer goes: every device of the channel BAD with COMM_FAIL; the
    // reconnects are refused and the tags stay so.
    ASSERT_TRUE(killed(*sim));
    EXPECT_TRUE(eventually(one, comm_fail));
    EXPECT_TRUE(eventually(two, comm_fail));
    EXPECT_TRUE(stops_cleanly(*driver));
}

TEST_F(DriverProcess, ARefusedConnectionIsCommFail) {
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    const LocalPort refusing(false);
    auto driver = start_driver(
        joined(tcp_channel("line1", refusing.port()), device("d1", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    EXPECT_TRUE(eventually(handle("d1.v"), comm_fail));
    EXPECT_TRUE(stops_cleanly(*driver));
}

TEST_F(DriverProcess, StaleAfterMaxAgeAndClearedByTheNextRead) {
    // 7.5: a point of the 1 s group with `max_age` 300 ms goes STALE
    // between its reads; the next read clears it.
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto sim = start_sim({"--set", "1", "0", "0", "9"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    std::vector<std::string> point = device("d1", 1, "1");
    point.insert(point.end(), {"--max-age-ms", "300"});
    auto driver =
        start_driver(joined(tcp_channel("line1", port_of(*sim)), point));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    observe();
    const std::uint32_t tag = handle("d1.v");
    const auto fresh = [](const auto& value) { return good_with(value, 9); };
    ASSERT_TRUE(eventually(tag, fresh));
    EXPECT_TRUE(eventually(tag, stale));
    EXPECT_TRUE(eventually(tag, fresh));
    EXPECT_TRUE(eventually(tag, stale));
    EXPECT_TRUE(stops_cleanly(*driver));
}

// --- Conformance test 2: the channel watchdog --------------------------------

TEST_F(DriverProcess, AStalledWorkerEndsTheProcessWithItsExitStatus) {
    // 2.6: the stall limit of the channel is three times its longest wait,
    // here `connect_timeout` 1 s, so 3 s; the process ends with its own
    // status, the stall logged with the channel's name.
    start_tagd({{.name = "line1", .tags = {"d1.v"}}});
    auto sim = start_sim({});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    std::vector<std::string> channel = tcp_channel("line1", port_of(*sim));
    channel.emplace_back("--hang");
    const auto started = std::chrono::steady_clock::now();
    auto driver = start_driver(joined(channel, device("d1", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    const std::optional<ChildEnding> ending =
        driver->wait_for_ending(WAIT_LIMIT);
    if (!ending.has_value()) {
        FAIL() << "the driver did not end: " << driver->err();
    }
    EXPECT_FALSE(ending->by_signal);
    EXPECT_EQ(ending->number, EXIT_STALL) << driver->err();
    EXPECT_GE(std::chrono::steady_clock::now() - started, milliseconds{3000});
    EXPECT_NE(
        driver->err().find(
            R"(channel stalled MESSAGE_ID="driver-stall" CHANNEL="line1")"),
        std::string::npos)
        << driver->err();
}

TEST_F(DriverProcess, AnIdleWorkerAndAConnectingOneDoNotStall) {
    // 2.6: channel `idle` polls every 10 s with a stall limit of 3 s;
    // channel `wait` is in connects of 2 s (a peer whose accept queue is
    // full) with a stall limit of 6 s. Neither ends the process in 12 s.
    start_tagd({{.name = "idle", .tags = {"d1.v"}},
                {.name = "wait", .tags = {"d2.v"}}});
    auto sim = start_sim({"--set", "1", "0", "0", "3"});
    if (!sim.has_value()) {
        FAIL() << "no simulator";
    }
    const LocalPort full(true);
    auto driver = start_driver(
        joined(tcp_channel("idle", port_of(*sim)), device("d1", 1, "2"),
               tcp_channel("wait", full.port(), "2000"), device("d2", 1, "0")));
    if (!driver.has_value()) {
        FAIL() << "no driver";
    }
    EXPECT_FALSE(driver->wait_for_ending(milliseconds{12'000}).has_value())
        << driver->err();
    EXPECT_TRUE(stops_cleanly(*driver));
    EXPECT_EQ(driver->err().find("stalled"), std::string::npos);
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

}  // namespace
