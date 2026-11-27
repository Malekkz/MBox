// The command line of scada-drv-test (drv_test_support.h): every number
// unsigned and in range, `--help`, a disabled channel and its faults.

#include "drv_test_support.h"

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
