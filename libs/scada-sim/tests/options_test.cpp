// Unit tests for the command line of scada-sim: what each option sets, what
// is refused, and the tag-set command end to end into a stream.

#include "scada/sim/sim.h"

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/sim/pattern.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/tag_set_json.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scada::sim;
using scada::Result;

Result<Options> parse(std::initializer_list<std::string_view> list,
                      std::string& detail) {
    const std::vector<std::string_view> args(list);
    return parse_options(args, detail);
}

// True if `list` is refused, with a sentence for the user.
bool refused(std::initializer_list<std::string_view> list) {
    std::string detail;
    const Result<Options> options = parse(list, detail);
    return !options.ok() && !detail.empty();
}

int run_with(std::initializer_list<std::string_view> list, std::string& out,
             std::string& err) {
    const std::vector<std::string_view> args(list);
    std::ostringstream out_stream;
    std::ostringstream err_stream;
    const int code = run(args, out_stream, err_stream);
    out = out_stream.str();
    err = err_stream.str();
    return code;
}

TEST(SimOptions, NothingOrHelpIsHelp) {
    std::string detail;
    const Result<Options> none = parse({}, detail);
    ASSERT_TRUE(none.ok());
    EXPECT_EQ(none.value().command, Command::HELP);
    const Result<Options> help = parse({"--help"}, detail);
    ASSERT_TRUE(help.ok());
    EXPECT_EQ(help.value().command, Command::HELP);
    EXPECT_TRUE(refused({"--help", "run"}));
}

// The options `list` gives; the defaults, with a failed expectation, if it
// is refused.
Options parsed(std::initializer_list<std::string_view> list) {
    std::string detail;
    const Result<Options> options = parse(list, detail);
    if (!options.ok()) {
        ADD_FAILURE() << detail;
        return Options{};
    }
    return options.value();
}

TEST(SimOptions, RunTakesTheLayout) {
    const Options options =
        parsed({"run", "--prefix", "load", "--processes", "2", "--owners", "10",
                "--tags", "50000", "--pattern", "check,ramp,sine,step,walk"});
    EXPECT_EQ(options.command, Command::RUN);
    EXPECT_EQ(options.layout.prefix, "load");
    EXPECT_EQ(options.layout.processes, 2U);
    EXPECT_EQ(options.layout.owners, 10U);
    EXPECT_EQ(options.layout.tags, 50'000U);
    ASSERT_EQ(options.layout.patterns.size(), 5U);
    EXPECT_EQ(options.layout.patterns[4], Pattern::WALK);
}

TEST(SimOptions, RunTakesTheRun) {
    const Options options =
        parsed({"run", "--processes", "2", "--owners", "2", "--instance", "1",
                "--rate", "50000", "--rounds", "3", "--notify", "each",
                "--base-dir", "/tmp/x", "--tagd-uid", "123"});
    EXPECT_EQ(options.instance, 1U);
    EXPECT_EQ(effective_rate(options), 50'000U);
    EXPECT_EQ(options.rounds, 3U);
    EXPECT_EQ(options.notify, Notify::EACH);
    EXPECT_EQ(options.base_dir, "/tmp/x");
    EXPECT_EQ(options.attach_tagd_uid, 123U);
}

TEST(SimOptions, RunTakesTheWaveform) {
    const Options options =
        parsed({"run", "--amplitude", "2.5", "--period", "50", "--seed", "9"});
    EXPECT_DOUBLE_EQ(options.waveform.amplitude, 2.5);
    EXPECT_EQ(options.waveform.period, 50U);
    EXPECT_EQ(options.waveform.seed, 9U);
}

TEST(SimOptions, TheDefaultRateIsATenthOfTheTags) {
    std::string detail;
    const Result<Options> parsed = parse({"run", "--tags", "50000"}, detail);
    ASSERT_TRUE(parsed.ok()) << detail;
    EXPECT_EQ(effective_rate(parsed.value()), 5000U);
    const Result<Options> few = parse({"run", "--tags", "5"}, detail);
    ASSERT_TRUE(few.ok()) << detail;
    EXPECT_EQ(effective_rate(few.value()), 1U);
    const Result<Options> fastest = parse({"run", "--rate", "0"}, detail);
    ASSERT_TRUE(fastest.ok()) << detail;
    EXPECT_EQ(effective_rate(fastest.value()), 0U);
}

TEST(SimOptions, TagSetTakesItsOwnOptions) {
    const Options options =
        parsed({"tag-set", "--uid", "1000", "--tagd-uid", "999", "--reader",
                "reader", "--reader", "probe", "--output", "out.json"});
    EXPECT_EQ(options.uid, 1000U);
    EXPECT_EQ(options.tagd_uid, 999U);
    ASSERT_EQ(options.readers.size(), 2U);
    EXPECT_EQ(options.readers[1], "probe");
    EXPECT_EQ(options.output, "out.json");
}

TEST(SimOptions, TagSetTakesTheSettingsOfTheTagSet) {
    const Options options =
        parsed({"tag-set", "--heartbeat-check-ms", "50",
                "--heartbeat-timeout-ms", "1000", "--ring-capacity", "2048"});
    EXPECT_EQ(options.heartbeat_check_ms, 50U);
    EXPECT_EQ(options.heartbeat_timeout_ms, 1000U);
    EXPECT_EQ(options.ring_capacity, 2048U);
}

TEST(SimOptions, RefusesWrongCommandsAndOptions) {
    EXPECT_TRUE(refused({"start"}));
    EXPECT_TRUE(refused({"run", "--uid", "1"}));       // tag-set only
    EXPECT_TRUE(refused({"tag-set", "--rate", "1"}));  // run only
    EXPECT_TRUE(refused({"run", "--rate"}));           // no value
    EXPECT_TRUE(refused({"run", "--rate", "1", "--rate", "2"}));
    EXPECT_TRUE(refused({"run", "--colour", "red"}));
    EXPECT_TRUE(refused({"run", "rate", "1"}));
}

TEST(SimOptions, RefusesValuesOutOfRange) {
    EXPECT_TRUE(refused({"run", "--rate", "100000001"}));
    EXPECT_TRUE(refused({"run", "--rate", "-1"}));
    EXPECT_TRUE(refused({"run", "--rate", "1x"}));
    EXPECT_TRUE(refused({"run", "--rate", ""}));
    EXPECT_TRUE(refused({"run", "--tags", "1000001"}));
    EXPECT_TRUE(refused({"run", "--owners", "1024"}));
    EXPECT_TRUE(refused({"run", "--rounds", "0"}));
    EXPECT_TRUE(refused({"run", "--seconds", "0"}));
    EXPECT_TRUE(refused({"run", "--period", "1"}));
    EXPECT_TRUE(refused({"run", "--amplitude", "0"}));
    EXPECT_TRUE(refused({"run", "--amplitude", "nan"}));
    EXPECT_TRUE(refused({"run", "--amplitude", "inf"}));
    EXPECT_TRUE(refused({"run", "--amplitude", "1e13"}));
    EXPECT_TRUE(refused({"run", "--pattern", "check,"}));
    EXPECT_TRUE(refused({"run", "--pattern", "square"}));
    EXPECT_TRUE(refused({"run", "--notify", "never"}));
    EXPECT_TRUE(refused({"tag-set", "--uid", "4294967295"}));
    EXPECT_TRUE(refused({"tag-set", "--reader", "Reader"}));
    EXPECT_TRUE(refused({"tag-set", "--heartbeat-check-ms", "0"}));
    EXPECT_TRUE(refused({"tag-set", "--ring-capacity", "0"}));
}

TEST(SimOptions, RefusesCombinationsThatDoNotFit) {
    EXPECT_TRUE(refused({"run", "--processes", "2", "--owners", "3"}));
    EXPECT_TRUE(refused(
        {"run", "--processes", "2", "--owners", "2", "--instance", "2"}));
    EXPECT_TRUE(refused({"run", "--rounds", "1", "--seconds", "1"}));
    EXPECT_TRUE(refused({"run", "--owners", "10", "--tags", "9"}));
}

TEST(SimOptions, TagSetWritesAFileThatScadaTagdReads) {
    std::string out;
    std::string err;
    EXPECT_EQ(run_with({"tag-set", "--owners", "2", "--tags", "4", "--uid",
                        "1000", "--reader", "reader"},
                       out, err),
              EXIT_OK)
        << err;
    std::string detail;
    const auto parsed = scada::tag::parse_tag_set_json(out, detail);
    ASSERT_TRUE(parsed.ok()) << detail;
    EXPECT_TRUE(scada::tag::validate(parsed.value(), detail).ok()) << detail;
    EXPECT_EQ(parsed.value().tags.size(), 4U);
}

TEST(SimOptions, HelpAndUsageErrors) {
    std::string out;
    std::string err;
    EXPECT_EQ(run_with({"--help"}, out, err), EXIT_OK);
    EXPECT_NE(out.find("Usage: scada-sim tag-set"), std::string::npos);
    EXPECT_EQ(run_with({"run", "--rate", "x"}, out, err), EXIT_USAGE);
    EXPECT_NE(err.find("--rate"), std::string::npos);
    EXPECT_EQ(
        run_with({"tag-set", "--output", "/nonexistent/dir/file"}, out, err),
        EXIT_ERROR);
}

}  // namespace
