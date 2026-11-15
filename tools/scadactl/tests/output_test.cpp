// Unit tests for how scadactl writes its output: a stream that fails is an
// error, not a success (an integration test covers pipes and files).

#include "scada/ctl/scadactl.h"

#include <gtest/gtest.h>

#include <ostream>
#include <sstream>
#include <string_view>
#include <vector>

namespace scada::ctl {
namespace {

TEST(Output, AWrittenTextIsASuccess) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_TRUE(write_out(out, err, "line\n"));
    EXPECT_EQ(out.str(), "line\n");
    EXPECT_EQ(err.str(), "");
}

TEST(Output, AStreamThatFailsIsAnError) {
    std::ostream broken(nullptr);  // no buffer: every write fails
    std::ostringstream err;
    EXPECT_FALSE(write_out(broken, err, "line\n"));
    EXPECT_EQ(err.str(), "scadactl: writing the output failed\n");
}

TEST(Output, HelpThatCannotBeWrittenExitsWithAnError) {
    std::ostream broken(nullptr);
    std::ostringstream err;
    const std::vector<std::string_view> args{"--help"};
    EXPECT_EQ(run(args, broken, err), EXIT_ERROR);
    std::ostringstream out;
    EXPECT_EQ(run(args, out, err), EXIT_OK);
}

}  // namespace
}  // namespace scada::ctl
