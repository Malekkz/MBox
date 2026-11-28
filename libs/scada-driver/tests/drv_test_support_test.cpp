// The command line of scada-drv-test (drv_test_support.h): every number
// unsigned and in range, `--help`, a disabled channel and its faults.

#include "drv_test_support.h"
#include "scada/driver/compiled.h"
#include "scada/driver/identity.h"

#include <gtest/gtest.h>

#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace scada::driver::test {
namespace {

std::optional<DrvTestOptions> parse(const std::vector<const char*>& args,
                                    std::string& err) {
    // One by one: GCC's release build sees a copy of the vector's range as
    // a possible overflow (-Wstringop-overflow), a false positive.
    std::vector<const char*> all{"scada-drv-test"};
    for (const char* arg : args) {
        all.push_back(arg);
    }
    std::ostringstream stream;
    std::optional<DrvTestOptions> options = parse_drv_test_args(all, stream);
    err = stream.str();
    return options;
}

TEST(DrvTestArgs, HelpIsTakenAlone) {
    std::string err;
    const auto options = parse({"--help"}, err);
    if (!options.has_value()) {
        FAIL() << err;
    }
    EXPECT_TRUE(options->help);
    EXPECT_NE(drv_test_usage().find("usage: scada-drv-test"),
              std::string_view::npos);
}

TEST(DrvTestArgs, AChannelWithADeviceAndAPoint) {
    std::string err;
    const auto options =
        parse({"--base-dir", "/b", "--retain-dir", "/r", "--channel", "c1",
               "tcp", "127.0.0.1", "502", "--hang", "--device", "d1", "7",
               "--point", "v", "3", "1"},
              err);
    if (!options.has_value()) {
        FAIL() << err;
    }
    const DrvTestOptions& parsed = *options;
    EXPECT_EQ(parsed.settings.retain_dir, "/r");
    ASSERT_EQ(parsed.config.channels.size(), 1U);
    EXPECT_TRUE(parsed.config.channels[0].enabled);
    EXPECT_EQ(parsed.endpoints[0].fault, Fault::HANG);
    EXPECT_EQ(parsed.config.channels[0].devices[0].unit, 7);
}

// One device d1 on a channel, with points v (0) and model (1) and `extra`.
std::optional<DrvTestOptions> with_points(const std::vector<const char*>& extra,
                                          std::string& err) {
    std::vector<const char*> args{
        "--base-dir", "/b",       "--channel", "c1",    "tcp",     "127.0.0.1",
        "502",        "--device", "d1",        "1",     "--point", "v",
        "0",          "0",        "--point",   "model", "1",       "0"};
    args.insert(args.end(), extra.begin(), extra.end());
    return parse(args, err);
}

TEST(DrvTestArgs, AnIdentityCheckOnAPointTheDeviceHas) {
    // D-147: `--on-mismatch-warn` runs a device whose check fails.
    std::string err;
    const auto options =
        with_points({"--identify-equals", "1", "7", "--on-mismatch-warn"}, err);
    if (!options.has_value()) {
        FAIL() << err;
    }
    const CompiledDevice& device = options->config.channels[0].devices[0];
    ASSERT_EQ(device.identify.size(), 1U);
    EXPECT_EQ(device.identify[0].point, 1U);
    ASSERT_EQ(device.identify[0].values.size(), 1U);
    EXPECT_EQ(device.identify[0].values[0].i, 7);
    EXPECT_FALSE(device.refuse_on_mismatch);
}

TEST(DrvTestArgs, ARestartRuleOnAPointTheDeviceHas) {
    std::string err;
    const auto options = with_points({"--restart-decreases", "0"}, err);
    if (!options.has_value()) {
        FAIL() << err;
    }
    const CompiledDevice& device = options->config.channels[0].devices[0];
    ASSERT_EQ(device.restart_indication.size(), 1U);
    EXPECT_EQ(device.restart_indication[0].point, 0U);
    EXPECT_EQ(device.restart_indication[0].kind, RestartRule::Kind::DECREASES);
    EXPECT_TRUE(device.refuse_on_mismatch);
}

TEST(DrvTestArgs, AnIdentityCheckOnAPointNotYetListedIsRefused) {
    std::string err;
    EXPECT_FALSE(parse({"--base-dir", "/b", "--channel", "c1", "tcp",
                        "127.0.0.1", "502", "--device", "d1", "1", "--point",
                        "v", "0", "0", "--identify-equals", "1", "7"},
                       err)
                     .has_value());
}

TEST(DrvTestArgs, ADisabledChannelAndASpinningOne) {
    std::string err;
    const auto options = parse({"--base-dir", "/b", "--channel", "off", "tcp",
                                "127.0.0.1", "502", "--disabled", "--channel",
                                "spin", "tcp", "127.0.0.1", "503", "--spin"},
                               err);
    if (!options.has_value()) {
        FAIL() << err;
    }
    EXPECT_FALSE(options->config.channels[0].enabled);
    EXPECT_EQ(options->endpoints[1].fault, Fault::SPIN);
}

TEST(DrvTestArgs, TheWorkerLogQueueSize) {
    // 64 records by default; 1 to the logger's limit (D-148).
    std::string err;
    const auto plain =
        parse({"--base-dir", "/b", "--channel", "c", "tcp", "h", "1"}, err);
    if (!plain.has_value()) {
        FAIL() << err;
    }
    EXPECT_EQ(plain->settings.worker_log_queue, DEFAULT_WORKER_LOG_QUEUE);
    const auto small = parse({"--base-dir", "/b", "--worker-log-queue", "1",
                              "--channel", "c", "tcp", "h", "1"},
                             err);
    if (!small.has_value()) {
        FAIL() << err;
    }
    EXPECT_EQ(small->settings.worker_log_queue, 1U);
    EXPECT_FALSE(parse({"--base-dir", "/b", "--worker-log-queue", "0",
                        "--channel", "c", "tcp", "h", "1"},
                       err)
                     .has_value());
    EXPECT_FALSE(parse({"--base-dir", "/b", "--worker-log-queue", "65537",
                        "--channel", "c", "tcp", "h", "1"},
                       err)
                     .has_value());
}

TEST(DrvTestArgs, SignsAndNumbersOutOfRangeAreRefused) {
    std::string err;
    const std::vector<std::vector<const char*>> bad{
        {"--base-dir", "/b", "--tagd-uid", "-1", "--channel", "c", "tcp", "h",
         "1"},
        {"--base-dir", "/b", "--heartbeat-ms", "+5", "--channel", "c", "tcp",
         "h", "1"},
        {"--base-dir", "/b", "--heartbeat-ms", "0", "--channel", "c", "tcp",
         "h", "1"},
        {"--base-dir", "/b", "--channel", "c", "tcp", "h", "65536"},
        {"--base-dir", "/b", "--channel", "c", "tcp", "h", "1", "--device", "d",
         "248"},
        {"--base-dir", "/b", "--channel", "c", "tcp", "h", "1", "--device", "d",
         "1", "--timeout-ms", "-5"},
        {"--channel", "c", "tcp", "h", "1"},  // no --base-dir
        {"--base-dir", "/b"},                 // no channel
    };
    for (const auto& args : bad) {
        EXPECT_FALSE(parse(args, err).has_value()) << args[2];
        EXPECT_NE(err.find("usage:"), std::string::npos);
    }
}

}  // namespace
}  // namespace scada::driver::test
