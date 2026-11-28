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
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
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
// `<device>.<point>`. The owner is `drv-test.<channel>` (CS 12.5, TD 1.1),
// unless `owner` names another; `drv-test` itself is the main thread's,
// with the `sys.proc.drv-test.*` tags (D-146).
// `retain` gives every tag RETAIN.
struct ChannelTags {
    std::string name;
    std::vector<std::string> tags;
    // NOLINTNEXTLINE(readability-redundant-member-init): GCC's -Wmissing-field-initializers needs it where a designated initializer leaves the member out
    std::string owner{};
    bool retain = false;
    // The system tags of the channel and its devices (11.1) are listed.
    bool system_tags = true;
    // The owner of those system tags when not the channel's: a tag set the
    // driver must refuse (review S3).
    // NOLINTNEXTLINE(readability-redundant-member-init): GCC's -Wmissing-field-initializers needs it where a designated initializer leaves the member out
    std::string system_owner{};
    // The devices with `identify`: they have the identity tags too (11.1).
    // NOLINTNEXTLINE(readability-redundant-member-init): GCC's -Wmissing-field-initializers needs it where a designated initializer leaves the member out
    std::vector<std::string> identified{};
};

// One system tag of driver interface contract 11.1: its signal and type.
struct SystemTag {
    std::string_view signal;
    std::string_view type;
};

// The system tags of 11.1 that slice 5b-1 writes, as CS 12.5 names them:
// written out here, not taken from the library, so that a wrong name or
// type in the driver fails the start (D-146).
inline constexpr std::array<SystemTag, 5> CHANNEL_SYSTEM_TAGS{{
    {.signal = "state", .type = "int64"},
    {.signal = "connects", .type = "uint64"},
    {.signal = "refused_connects", .type = "uint64"},
    {.signal = "secured", .type = "bool"},
    {.signal = "requests_per_s", .type = "float64"},
}};
inline constexpr std::array<SystemTag, 7> PROCESS_SYSTEM_TAGS{{
    {.signal = "realtime", .type = "bool"},
    {.signal = "config_id", .type = "uint64"},
    {.signal = "config_state", .type = "int64"},
    {.signal = "stale_outcomes", .type = "uint64"},
    {.signal = "answers_dropped", .type = "uint64"},
    {.signal = "safe_unjournalled", .type = "uint64"},
    {.signal = "log_dropped", .type = "uint64"},
}};
// scada-drv-test's poll groups.
inline constexpr std::array<std::string_view, 3> POLL_GROUPS{"fast", "normal",
                                                             "slow"};
inline constexpr std::array<SystemTag, 15> DEVICE_SYSTEM_TAGS{{
    {.signal = "state", .type = "int64"},
    {.signal = "last_ok_time", .type = "int64"},
    {.signal = "requests", .type = "uint64"},
    {.signal = "errors", .type = "uint64"},
    {.signal = "timeouts", .type = "uint64"},
    {.signal = "response_ms", .type = "float64"},
    {.signal = "requests_per_scan", .type = "uint64"},
    {.signal = "pacing_limited", .type = "bool"},
    {.signal = "last_status", .type = "int64"},
    {.signal = "exceptions", .type = "uint64"},
    {.signal = "refusals", .type = "uint64"},
    {.signal = "sentinels", .type = "uint64"},
    {.signal = "invalid", .type = "uint64"},
    {.signal = "points_absent", .type = "uint64"},
    {.signal = "restarts", .type = "uint64"},
}};

// The device tags the validator generates for every device (11.1) that no
// slice built yet writes: the driver resolves none of them, and they read
// BAD with NOT_INIT, from the walk of 9.1, for the driver's whole life
// (the command slices bring them; review S2). `refresh_failures` and the
// identity tags are only for some devices.
inline constexpr std::array<SystemTag, 2> UNWRITTEN_DEVICE_TAGS{{
    {.signal = "selections_lost", .type = "uint64"},
    {.signal = "last_command_stage", .type = "int64"},
}};

// The devices of a channel's point tags, in order of first appearance.
inline std::vector<std::string> devices_of(const ChannelTags& channel) {
    std::vector<std::string> devices;
    for (const std::string& tag : channel.tags) {
        std::string device = tag.substr(0, tag.find('.'));
        if (std::find(devices.begin(), devices.end(), device) ==
            devices.end()) {
            devices.push_back(std::move(device));
        }
    }
    return devices;
}

// The system tags of `channel` and of each device of its point tags
// (CS 12.5), with their types.
inline std::vector<std::pair<std::string, std::string_view>> system_tags_of(
    const ChannelTags& channel) {
    std::vector<std::pair<std::string, std::string_view>> out;
    const std::vector<std::string> devices = devices_of(channel);
    out.reserve(CHANNEL_SYSTEM_TAGS.size() + POLL_GROUPS.size() +
                (devices.size() *
                 (DEVICE_SYSTEM_TAGS.size() + UNWRITTEN_DEVICE_TAGS.size())));
    const std::string drv = "sys.drv." + channel.name + ".";
    for (const SystemTag& tag : CHANNEL_SYSTEM_TAGS) {
        out.emplace_back(drv + std::string(tag.signal), tag.type);
    }
    for (const std::string_view group : POLL_GROUPS) {
        out.emplace_back(drv + "period." + std::string(group), "float64");
    }
    for (const std::string& device : devices) {
        for (const SystemTag& tag : DEVICE_SYSTEM_TAGS) {
            out.emplace_back(
                "sys.dev." + device + "." + std::string(tag.signal), tag.type);
        }
        for (const SystemTag& tag : UNWRITTEN_DEVICE_TAGS) {
            out.emplace_back(
                "sys.dev." + device + "." + std::string(tag.signal), tag.type);
        }
        if (std::find(channel.identified.begin(), channel.identified.end(),
                      device) != channel.identified.end()) {
            out.emplace_back("sys.dev." + device + ".identity", "string");
            out.emplace_back("sys.dev." + device + ".identity_ok", "bool");
        }
    }
    return out;
}

// Appends the tag `name` of `owner`, of `type`, to the JSON list `tags`.
inline void append_tag(std::string& tags, const std::string& name,
                       const std::string& owner, std::string_view type,
                       bool retain) {
    tags += tags.empty() ? R"({"name": ")" : R"(, {"name": ")";
    tags += name;
    tags += R"(", "owner": ")";
    tags += owner;
    tags += R"(", "type": ")";
    tags += type;
    tags += retain ? R"(", "retain": true})" : R"("})";
}

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

// The sum of the COUNT fields of the `driver-device-restart` records in
// `err` (D-147).
inline std::uint64_t restart_counts(std::string_view err) {
    constexpr std::string_view RECORD = R"(MESSAGE_ID="driver-device-restart")";
    constexpr std::string_view COUNT = R"(COUNT=")";
    std::uint64_t sum = 0;
    for (std::size_t at = err.find(RECORD); at != std::string_view::npos;
         at = err.find(RECORD, at + RECORD.size())) {
        const std::string_view line = err.substr(at, err.find('\n', at) - at);
        const std::size_t count = line.find(COUNT);
        if (count != std::string_view::npos) {
            sum += std::stoull(std::string(line.substr(count + COUNT.size())));
        }
    }
    return sum;
}

// The times of day, in ms since midnight UTC, of the lines of `err` that
// hold `record`, in order: from the stream sink's leading
// "YYYY-MM-DDTHH:MM:SS.mmmZ" (log.h, `make_stream_sink`).
inline std::vector<std::int64_t> record_times_ms(std::string_view err,
                                                 std::string_view record) {
    constexpr std::size_t TIME_AT = 11;      // after "YYYY-MM-DDT"
    constexpr std::size_t TIME_LENGTH = 12;  // "HH:MM:SS.mmm"
    std::vector<std::int64_t> times;
    for (std::size_t at = err.find(record); at != std::string_view::npos;
         at = err.find(record, at + record.size())) {
        const std::size_t start = err.rfind('\n', at);
        const std::size_t line =
            start == std::string_view::npos ? 0 : start + 1;
        if (line + TIME_AT + TIME_LENGTH > err.size()) {
            continue;
        }
        const std::string time(err.substr(line + TIME_AT, TIME_LENGTH));
        const std::int64_t hours = std::stoll(time.substr(0, 2));
        const std::int64_t minutes = std::stoll(time.substr(3, 2));
        const std::int64_t seconds = std::stoll(time.substr(6, 2));
        const std::int64_t millis = std::stoll(time.substr(9, 3));
        times.push_back((((hours * 60) + minutes) * 60 + seconds) * 1000 +
                        millis);
    }
    return times;
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
    // The next tag set has no owner `drv-test` and no `sys.proc` tags.
    void without_process_tags() { process_tags_ = false; }
    // The next tag sets lack the tag `name`, or give it `type` (review S4).
    void omit_tag(const std::string& name) { omitted_.push_back(name); }
    void retype_tag(const std::string& name, const std::string& type) {
        retyped_.emplace_back(name, type);
    }
    // The next tag sets give the owner `drv-test` the INT64 tag `name` too,
    // which no code of the driver writes (review M8).
    void add_process_tag(const std::string& name) {
        process_extra_tags_.push_back(name);
    }
    // Writes `value` GOOD to the tag `name` of process drv-test, attached as
    // that process, then detaches: what a previous life of the driver
    // would have left. False if any call is refused.
    bool write_as_driver(const std::string& name, std::int64_t value) {
        const scada_attach_opts options{.base_dir = base_.c_str(),
                                        .retain_dir = nullptr,
                                        .tagd_uid = std::int64_t{::geteuid()}};
        scada_ctx* ctx = nullptr;
        if (scada_tag_attach_opts("drv-test", &options, &ctx) != SCADA_OK) {
            return false;
        }
        std::uint32_t tag = SCADA_INVALID_HANDLE;
        scada_value written{};
        written.type = SCADA_TYPE_INT64;
        written.value = std::bit_cast<std::uint64_t>(value);
        written.quality = SCADA_QUALITY_GOOD;
        const bool done =
            scada_tag_resolve(ctx, name.c_str(), &tag) == SCADA_OK &&
            scada_tag_write(ctx, tag, &written) == SCADA_OK &&
            scada_tag_notify(ctx) == SCADA_OK;
        scada_tag_detach(ctx);
        return done;
    }
    // The next tag sets are whole again.
    void whole_tag_sets() {
        omitted_.clear();
        retyped_.clear();
    }

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

    // The text of the STRING tag `tag`, and its quality in `quality`.
    std::string read_text(std::uint32_t tag, std::uint8_t& quality) {
        std::array<char, SCADA_STRING_MAX> buffer{};
        std::size_t length = 0;
        scada_value value{};
        if (scada_tag_read_string(ctx_, tag, buffer.data(), buffer.size(),
                                  &length, &value) != SCADA_OK) {
            quality = SCADA_QUALITY_BAD;
            return {};
        }
        quality = value.quality;
        return {buffer.data(), length};
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

    // True when drv-test's cycle counter (`scada_tag_notify`) reaches
    // `before` + `count` within WAIT_LIMIT.
    bool cycles_since(std::uint32_t before, std::uint32_t count) {
        const auto end = SteadyClock::now() + WAIT_LIMIT;
        while (SteadyClock::now() < end) {
            if (driver_status().cycle_seq - before >= count) {
                return true;
            }
            scada_change change{};
            (void)scada_sub_next(sub_, &change, 50);
        }
        return false;
    }

    // True when, within `limit`, a read of `tag` right after a notify of
    // drv-test (its cycle counter, TD 10) holds `holds`: a value written
    // and its readers woken, not only written (review S4).
    bool notified_while(std::uint32_t tag,
                        const std::function<bool(const scada_value&)>& holds,
                        milliseconds limit = WAIT_LIMIT) {
        std::uint16_t index = 0;
        scada_waitset* set = nullptr;
        if (scada_proc_resolve(ctx_, "drv-test", &index) != SCADA_OK ||
            scada_waitset_create(ctx_, &set) != SCADA_OK) {
            return false;
        }
        bool seen = false;
        if (scada_waitset_add_cycle(set, index) == SCADA_OK) {
            const auto end = SteadyClock::now() + limit;
            while (!seen && SteadyClock::now() < end) {
                if (scada_waitset_wait(set, 50) == SCADA_OK) {
                    seen = holds(read(tag));
                }
            }
        }
        scada_waitset_destroy(set);
        return seen;
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
        for (const ChannelTags& channel : channels) {
            const std::string owner = channel.owner.empty()
                                          ? "drv-test." + channel.name
                                          : channel.owner;
            // The owner's change ring at least twice its tags (CF-1003).
            const std::size_t owned =
                channel.tags.size() +
                (channel.system_tags && channel.system_owner.empty()
                     ? system_tags_of(channel).size()
                     : 0);
            add_owner(owners, owner, true, ring_for(owned));
            for (const std::string& tag : channel.tags) {
                add_tag(tags, tag, owner, "int64", channel.retain);
            }
            // With the channel's owner (CS 12.5), or the one the test gives.
            if (channel.system_tags) {
                const std::string& system =
                    channel.system_owner.empty() ? owner : channel.system_owner;
                add_owner(owners, system, false);
                for (const auto& [name, type] : system_tags_of(channel)) {
                    add_tag(tags, name, system, type, false);
                }
            }
        }
        for (const std::string& tag : extra_tags) {
            add_tag(tags, tag, owners_.front(), "int64", false);
        }
        // The main thread's owner, unless a channel already took the name,
        // and its tags (11.1).
        if (process_tags_) {
            add_owner(owners, "drv-test", false);
            for (const std::string& extra : process_extra_tags_) {
                add_tag(tags, extra, "drv-test", "int64", false);
            }
            for (const SystemTag& tag : PROCESS_SYSTEM_TAGS) {
                add_tag(tags, "sys.proc.drv-test." + std::string(tag.signal),
                        "drv-test", tag.type, false);
            }
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

    // Appends the tag to `tags` as `append_tag` does, unless `omit_tag`
    // named it, with the type `retype_tag` gave it.
    void add_tag(std::string& tags, const std::string& name,
                 const std::string& owner, std::string_view type, bool retain) {
        if (std::find(omitted_.begin(), omitted_.end(), name) !=
            omitted_.end()) {
            return;
        }
        for (const auto& [retyped, other] : retyped_) {
            if (retyped == name) {
                type = other;
            }
        }
        append_tag(tags, name, owner, type, retain);
    }

    // Adds `owner` of process drv-test to the JSON list `owners` once;
    // `observed`: it is one `observe` may subscribe to (a channel's).
    // The least change ring of the tag set's rules for `tags` tags: a power
    // of two, at least 1,024 and twice the tags (CF-1003).
    static std::size_t ring_for(std::size_t tags) {
        constexpr std::size_t LEAST = 1024;
        std::size_t ring = LEAST;
        while (ring < 2 * tags) {
            ring *= 2;
        }
        return ring;
    }

    void add_owner(std::string& owners, const std::string& owner,
                   bool observed = true, std::size_t ring = 1024) {
        if (std::find(owners_.begin(), owners_.end(), owner) != owners_.end() ||
            owners.find(R"("name": ")" + owner + "\"") != std::string::npos) {
            return;
        }
        if (observed) {
            owners_.push_back(owner);
        }
        owners += owners.empty() ? R"({"name": ")" : R"(, {"name": ")";
        owners += owner;
        owners += R"(", "process": "drv-test", "change_ring_capacity": )";
        owners += std::to_string(ring);
        owners += "}";
    }

    std::optional<scada::test::TempDirectory> temp_;
    std::filesystem::path base_;
    std::vector<std::string> owners_;
    std::vector<std::string> omitted_;
    std::vector<std::string> process_extra_tags_;
    std::vector<std::pair<std::string, std::string>> retyped_;
    bool process_tags_ = true;  // the owner `drv-test` and its tags
    std::optional<RunningTagd> tagd_;
    std::unique_ptr<NotifyListener> listener_;
    scada_ctx* ctx_ = nullptr;
    scada_sub* sub_ = nullptr;
};

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

}  // namespace scada::test::driver_it

#endif  // SCADA_TESTS_INTEGRATION_DRIVER_FIXTURE_H
