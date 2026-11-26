#ifndef SCADA_TESTS_INTEGRATION_DRIVER_FIXTURE_H
#define SCADA_TESTS_INTEGRATION_DRIVER_FIXTURE_H

// The fixture of the driver process tests (driver_integration_test.cpp,
// driver_stop_integration_test.cpp): scada-tagd, the test protocol's
// simulator (scada-driver-test-sim) and the driver process scada-drv-test
// as real processes, a listener standing in for the service manager of
// the driver, and an observer of the tags through the C interface. Every
// wait is on a condition with a time-out: a change of the observed owner,
// a line of output, a notification, or the end of a process.

#include "tagd_fixture.h"

#include "scada/common/fd.h"
#include "scada/service/notify_listener.h"
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

#include <algorithm>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::test::driver_it {

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers): the test data

using scada::service::NotifyListener;
using scada::test::ChildEnding;
using scada::test::ChildProcess;
using scada::test::tagd::command_from;
using scada::test::tagd::receives;
using scada::test::tagd::RunningTagd;
using scada::test::tagd::stops_cleanly;
using scada::test::tagd::WAIT_LIMIT;
using std::chrono::milliseconds;
using SteadyClock = std::chrono::steady_clock;

inline constexpr mode_t BASE_DIR_MODE = 02750;
// scada::driver's exit statuses, and that of a failed thread check.
inline constexpr int EXIT_OK = 0;
inline constexpr int EXIT_ERROR = 1;
inline constexpr int EXIT_USAGE = 2;
inline constexpr int EXIT_THREAD_CHECK = 3;
inline constexpr int EXIT_STALL = 4;
// The driver's heartbeat timeout in the tag set.
inline constexpr milliseconds HEARTBEAT_TIMEOUT{1500};

// One channel of the tag set: its name and its INT64 tags
// `<device>.<point>`. The owner is `drv-test` for a single channel and
// `drv-test.<channel>` for several (TD 1), unless `owner` names another.
// `retain` gives every tag RETAIN.
struct ChannelTags {
    std::string name;
    std::vector<std::string> tags;
    // NOLINTNEXTLINE(readability-redundant-member-init): GCC's -Wmissing-field-initializers needs it where a designated initializer leaves the member out
    std::string owner{};
    bool retain = false;
};

// The port of the simulator's first line, `tcp <port>`; 0 if none.
inline std::uint16_t port_of(const ChildProcess& sim) {
    std::istringstream text(sim.out());
    std::string word;
    std::uint16_t port = 0;
    text >> word >> port;
    return word == "tcp" ? port : 0;
}

// The arguments of one channel and of one device of the driver.
inline std::vector<std::string> tcp_channel(
    const std::string& name, std::uint16_t port,
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

inline std::vector<std::string> device(const std::string& name, int unit,
                                       const std::string& group,
                                       const std::string& timeout_ms = "500") {
    return {"--device",
            name,
            std::to_string(unit),
            "--timeout-ms",
            timeout_ms,
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

inline bool good_with(const scada_value& value, std::int64_t expected) {
    return value.quality == SCADA_QUALITY_GOOD &&
           std::bit_cast<std::int64_t>(value.value) == expected &&
           (value.quality_flags & SCADA_FLAG_STALE) == 0;
}

inline bool comm_fail(const scada_value& value) {
    return value.quality == SCADA_QUALITY_BAD &&
           (value.quality_flags & SCADA_FLAG_COMM_FAIL) != 0;
}

inline bool stale(const scada_value& value) {
    return (value.quality_flags & SCADA_FLAG_STALE) != 0;
}

inline bool owner_down(const scada_value& value) {
    return (value.quality_flags & SCADA_FLAG_OWNER_DOWN) != 0;
}

// True when `value` has `quality`, exactly the flags `flags` and the INT64
// `number`.
inline ::testing::AssertionResult holds(const scada_value& value,
                                        std::uint8_t quality,
                                        std::uint16_t flags,
                                        std::int64_t number) {
    if (value.quality == quality && value.quality_flags == flags &&
        std::bit_cast<std::int64_t>(value.value) == number) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << "quality " << int{value.quality} << " flags "
           << value.quality_flags << " value "
           << std::bit_cast<std::int64_t>(value.value);
}

// The same, whatever the flags but those in `without`, which it must not
// have.
inline ::testing::AssertionResult holds_value(const scada_value& value,
                                              std::uint8_t quality,
                                              std::int64_t number,
                                              std::uint16_t without = 0) {
    return holds(value, quality,
                 static_cast<std::uint16_t>(value.quality_flags & ~without),
                 number);
}

// True when `out` holds `line` (a log record or a simulator line).
inline bool has(const std::string& out, std::string_view line) {
    return out.find(line) != std::string::npos;
}

// Waits until `process` has printed `line` on standard error.
inline ::testing::AssertionResult logs(ChildProcess& process,
                                       std::string_view line) {
    if (process.wait_for_error(
            [&](std::string_view err) { return has(std::string(err), line); },
            WAIT_LIMIT)) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << "no '" << line << "' in: " << process.err();
}

// The times `line` is in `out`.
inline std::size_t count_of(std::string_view out, std::string_view line) {
    std::size_t count = 0;
    for (std::size_t at = out.find(line); at != std::string_view::npos;
         at = out.find(line, at + line.size())) {
        ++count;
    }
    return count;
}

// Waits until `process` has printed `line` `times` times on standard
// output.
inline ::testing::AssertionResult prints_times(
    ChildProcess& process, std::string_view line, std::size_t times,
    milliseconds limit = WAIT_LIMIT) {
    if (process.wait_for_output(
            [&](std::string_view out) { return count_of(out, line) >= times; },
            limit)) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "fewer than " << times << " '"
                                         << line << "' in: " << process.out();
}

// Waits until `process` has printed `line` on standard output.
inline ::testing::AssertionResult prints(ChildProcess& process,
                                         std::string_view line) {
    if (process.wait_for_output(
            [&](std::string_view out) { return has(std::string(out), line); },
            WAIT_LIMIT)) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << "no '" << line << "' in: " << process.out();
}

// Waits, at most `limit`, for the end of `process`, and checks that it
// exited with `status`.
inline ::testing::AssertionResult ends_with(ChildProcess& process, int status,
                                            milliseconds limit = WAIT_LIMIT) {
    const std::optional<ChildEnding> ending = process.wait_for_ending(limit);
    if (!ending.has_value()) {
        return ::testing::AssertionFailure()
               << "did not end: " << process.err();
    }
    if (ending->by_signal || ending->number != status) {
        return ::testing::AssertionFailure()
               << scada::test::tagd::describe(*ending) << ": " << process.err();
    }
    return ::testing::AssertionSuccess();
}

// The start of `process` failed: exit status 1, `message` on its standard
// error.
inline ::testing::AssertionResult fails_start(ChildProcess& process,
                                              std::string_view message) {
    const ::testing::AssertionResult ended = ends_with(process, EXIT_ERROR);
    if (ended && !has(process.err(), message)) {
        return ::testing::AssertionFailure()
               << "no '" << message << "' in: " << process.err();
    }
    return ended;
}

// Sends `signal_number` and waits for the end of `process`.
inline std::optional<ChildEnding> end_with(ChildProcess& process,
                                           int signal_number) {
    if (!process.send_signal(signal_number)) {
        return std::nullopt;
    }
    return process.wait_for_ending(WAIT_LIMIT);
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

// How the driver's service manager environment is set.
struct NotifyEnv {
    // The listener's socket when empty; else this value of NOTIFY_SOCKET.
    // NOLINTNEXTLINE(readability-redundant-member-init): as for ChannelTags::owner
    std::string socket{};
    // WATCHDOG_USEC, or none.
    // NOLINTNEXTLINE(readability-redundant-member-init): as for ChannelTags::owner
    std::optional<std::chrono::microseconds> watchdog{};
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
        scada::Result<NotifyListener> listener = NotifyListener::create();
        ASSERT_TRUE(listener.ok());
        listener_ =
            std::make_unique<NotifyListener>(std::move(listener).value());
    }

    void TearDown() override {
        close_observer();
        tagd_.reset();
        temp_.reset();
    }

    void close_observer() {
        if (ctx_ != nullptr) {
            scada_sub_close(sub_);
            scada_tag_detach(ctx_);
            ctx_ = nullptr;
            sub_ = nullptr;
        }
    }

    // Writes the tag set and starts scada-tagd.
    void start_tagd(const std::vector<ChannelTags>& channels,
                    const std::vector<std::string>& extra_tags = {}) {
        write_tag_set(channels, extra_tags);
        std::optional<RunningTagd> started =
            scada::test::tagd::ready_tagd(tag_set_path(), base_.string());
        if (!started.has_value()) {
            FAIL() << "scada-tagd did not start";
        }
        tagd_.emplace(std::move(*started));
    }

    // Stops scada-tagd in order (its retained values are written) and
    // starts it again: on the same tag set, or on `channels` and
    // `extra_tags`, a changed tag set and so a new generation (TD 11).
    void restart_tagd(const std::vector<ChannelTags>* channels = nullptr,
                      const std::vector<std::string>& extra_tags = {}) {
        close_observer();
        if (tagd_.has_value()) {
            scada::test::tagd::stop(*tagd_);
            tagd_.reset();
        }
        if (channels != nullptr) {
            write_tag_set(*channels, extra_tags);
        }
        std::optional<RunningTagd> started =
            scada::test::tagd::ready_tagd(tag_set_path(), base_.string());
        if (!started.has_value()) {
            FAIL() << "scada-tagd did not start again";
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

    // scada-drv-test on this database, with `args` for its channels, its
    // service manager the fixture's listener (or as `env` says).
    [[nodiscard]] std::optional<ChildProcess> start_driver(
        const std::vector<std::string>& args, const NotifyEnv& env = {}) {
        // What an earlier driver sent is not this one's.
        while (listener_->receive(milliseconds{0}).has_value()) {
        }
        std::vector<std::string> command{
            "env", "-u",           "WATCHDOG_PID", "-u", "JOURNAL_STREAM",
            "-u",  "WATCHDOG_USEC"};
        command.push_back("NOTIFY_SOCKET=" + (env.socket.empty()
                                                  ? listener_->socket_name()
                                                  : env.socket));
        if (env.watchdog.has_value()) {
            command.push_back("WATCHDOG_USEC=" +
                              std::to_string(env.watchdog->count()));
        }
        std::vector<std::string> all{
            "--base-dir",     base_.string(),
            "--retain-dir",   scada::test::tagd::retain_dir_for(base_.string()),
            "--tagd-uid",     std::to_string(::geteuid()),
            "--heartbeat-ms", "50"};
        all.insert(all.end(), args.begin(), args.end());
        const std::vector<std::string> drv =
            command_from("SCADA_DRV_TEST_COMMAND", all);
        command.insert(command.end(), drv.begin(), drv.end());
        auto driver = ChildProcess::start(command);
        if (!driver.has_value()) {
            ADD_FAILURE() << "the driver did not start";
        }
        return driver;
    }

    NotifyListener& listener() { return *listener_; }

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

    // The next change of the observed owner within `limit`, gaps skipped.
    // A change is an entry of its ring: a write that changed value,
    // quality or flags (TD 8). The status call makes the observer look
    // again for a ring its owner made anew (scada_tag_notify).
    std::optional<scada_change> next_change(milliseconds limit = WAIT_LIMIT) {
        const auto end = SteadyClock::now() + limit;
        while (SteadyClock::now() < end) {
            scada_status status{};
            (void)scada_tag_status(ctx_, &status);
            scada_change change{};
            const int code = scada_sub_next(sub_, &change, 50);
            if (code == SCADA_OK) {
                return change;
            }
        }
        return std::nullopt;
    }

    // The next change of the observed owner for which `wanted` holds.
    std::optional<scada_change> next_change_where(
        const std::function<bool(const scada_change&)>& wanted) {
        for (;;) {
            std::optional<scada_change> change = next_change();
            if (!change.has_value() || wanted(change.value())) {
                return change;
            }
        }
    }

    // Waits for the end of `process`, and tells in `seen_down` whether the
    // library saw drv-test down meanwhile.
    std::optional<ChildEnding> end_watching(ChildProcess& process,
                                            bool& seen_down) {
        const auto end = SteadyClock::now() + WAIT_LIMIT;
        seen_down = false;
        std::optional<ChildEnding> ending;
        while (
            !(ending = process.wait_for_ending(milliseconds{20})).has_value() &&
            SteadyClock::now() < end) {
            seen_down = seen_down || driver_status().down != 0;
        }
        return ending;
    }

    // Stops `process` with SIGSTOP until `tag` reads OWNER_DOWN (its
    // heartbeat timeout), then continues it.
    ::testing::AssertionResult pause_until_down(ChildProcess& process,
                                                std::uint32_t tag) {
        if (!process.send_signal(SIGSTOP)) {
            return ::testing::AssertionFailure() << "no SIGSTOP";
        }
        const bool down = eventually(tag, owner_down);
        if (!process.send_signal(SIGCONT) || !down) {
            return ::testing::AssertionFailure() << "never down, or no SIGCONT";
        }
        return ::testing::AssertionSuccess();
    }

    // True when `count` more changes of the observed owner come.
    bool changes(int count) {
        for (int seen = 0; seen < count; ++seen) {
            if (!next_change().has_value()) {
                return false;
            }
        }
        return true;
    }

    // Takes every change already in the ring.
    void drain() {
        scada_change change{};
        while (scada_sub_next(sub_, &change, 0) == SCADA_OK) {
        }
    }

    // What the library says of the process drv-test now.
    scada_proc_status_t driver_status() {
        std::uint16_t index = 0;
        EXPECT_EQ(scada_proc_resolve(ctx_, "drv-test", &index), SCADA_OK);
        scada_proc_status_t status{};
        EXPECT_EQ(scada_proc_status(ctx_, index, &status), SCADA_OK);
        return status;
    }

    // Waits until `holds` is true of a read of `tag`, up to `limit`;
    // each wait between reads is for the next change of the observed
    // owner, at most 50 ms.
    bool eventually(std::uint32_t tag,
                    const std::function<bool(const scada_value&)>& holds,
                    milliseconds limit = WAIT_LIMIT) {
        const auto end = SteadyClock::now() + limit;
        while (SteadyClock::now() < end) {
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
        if (stopped && has(driver->err(), "thread check failed")) {
            return ::testing::AssertionFailure() << driver->err();
        }
        return stopped;
    }

    [[nodiscard]] const std::filesystem::path& base() const { return base_; }

private:
    [[nodiscard]] std::string tag_set_path() const {
        return temp_.has_value() ? (temp_->path() / "tag-set.json").string()
                                 : std::string();
    }

    void write_tag_set(const std::vector<ChannelTags>& channels,
                       const std::vector<std::string>& extra_tags) {
        std::string owners;
        std::string tags;
        owners_.clear();
        const auto add_tag = [&](const std::string& name,
                                 const std::string& owner, bool retain) {
            tags += tags.empty() ? R"({"name": ")" : R"(, {"name": ")";
            tags += name;
            tags += R"(", "owner": ")";
            tags += owner;
            tags += retain ? R"(", "type": "int64", "retain": true})"
                           : R"(", "type": "int64"})";
        };
        for (const ChannelTags& channel : channels) {
            std::string owner = channel.owner;
            if (owner.empty()) {
                owner = "drv-test";
                if (channels.size() > 1) {
                    owner += ".";
                    owner += channel.name;
                }
            }
            if (std::find(owners_.begin(), owners_.end(), owner) ==
                owners_.end()) {
                owners_.push_back(owner);
                owners += owners.empty() ? R"({"name": ")" : R"(, {"name": ")";
                owners += owner;
                owners +=
                    R"(", "process": "drv-test", "change_ring_capacity": 1024})";
            }
            for (const std::string& tag : channel.tags) {
                add_tag(tag, owner, channel.retain);
            }
        }
        for (const std::string& tag : extra_tags) {
            add_tag(tag, owners_.front(), false);
        }
        const std::string uid = std::to_string(::geteuid());
        std::ofstream file(tag_set_path());
        file << R"({"format": 1, "heartbeat_check_ms": 50, "processes": [)"
             << R"({"name": "tagd", "uid": )" << uid << "}, "
             << R"({"name": "drv-test", "uid": )" << uid
             << R"(, "heartbeat_timeout_ms": )" << HEARTBEAT_TIMEOUT.count()
             << R"(}], "owners": [)" << owners
             << R"(], "queues": [], "tags": [)" << tags << "]}\n";
    }

    std::optional<scada::test::TempDirectory> temp_;
    std::filesystem::path base_;
    std::vector<std::string> owners_;
    std::optional<RunningTagd> tagd_;
    std::unique_ptr<NotifyListener> listener_;
    scada_ctx* ctx_ = nullptr;
    scada_sub* sub_ = nullptr;
};

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

}  // namespace scada::test::driver_it

#endif  // SCADA_TESTS_INTEGRATION_DRIVER_FIXTURE_H
