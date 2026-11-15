// Unit tests for the command line of scadactl.

#include "scada/ctl/arguments.h"

#include "scada/tag/scada_tag.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::ctl {
namespace {

// The arguments `args` give, or default arguments with a failed
// expectation if they are refused.
Arguments parse_ok(std::vector<std::string_view> args) {
    std::string detail;
    std::optional<Arguments> result = parse_arguments(args, detail);
    if (!result.has_value()) {
        ADD_FAILURE() << "refused: " << detail;
        return Arguments{};
    }
    return std::move(*result);
}

// True if `args` are refused with a reason, which goes to `detail_out`.
bool refused(std::vector<std::string_view> args,
             std::string* detail_out = nullptr) {
    std::string detail;
    const std::optional<Arguments> result = parse_arguments(args, detail);
    if (detail_out != nullptr) {
        *detail_out = detail;
    }
    if (!result.has_value()) {
        EXPECT_FALSE(detail.empty());
        return true;
    }
    return false;
}

TEST(Arguments, Help) {
    const Arguments args = parse_ok({"--help"});
    EXPECT_EQ(args.command, Command::HELP);
    // --help wins over anything else, even arguments that are not valid.
    EXPECT_EQ(parse_ok({"tag", "get", "--help"}).command, Command::HELP);
}

TEST(Arguments, Status) {
    const Arguments args = parse_ok({"status"});
    EXPECT_EQ(args.command, Command::STATUS);
    EXPECT_FALSE(args.json);
    EXPECT_TRUE(args.base_dir.empty());
    EXPECT_FALSE(args.tagd_uid.has_value());
    EXPECT_TRUE(refused({"status", "extra"}));
}

TEST(Arguments, TagList) {
    const Arguments all = parse_ok({"tag", "list"});
    EXPECT_EQ(all.command, Command::TAG_LIST);
    EXPECT_TRUE(all.pattern.matches("anything.at.all"));

    const Arguments some = parse_ok({"tag", "list", "plant.*"});
    EXPECT_TRUE(some.pattern.matches("plant.pump1"));
    EXPECT_FALSE(some.pattern.matches("other"));

    EXPECT_TRUE(refused({"tag", "list", "a", "b"}));
    EXPECT_TRUE(refused({"tag", "list", "UPPER"}));
}

TEST(Arguments, TagGet) {
    const Arguments args = parse_ok({"tag", "get", "plant.pump1.speed"});
    EXPECT_EQ(args.command, Command::TAG_GET);
    EXPECT_EQ(args.name, "plant.pump1.speed");
    EXPECT_TRUE(refused({"tag", "get"}));
    EXPECT_TRUE(refused({"tag", "get", "a", "b"}));
    EXPECT_TRUE(refused({"tag", "get", "plant.*"}));
    EXPECT_TRUE(refused({"tag", "get", std::string_view("a\0b", 3)}));
    const std::string long_name(SCADA_TAG_NAME_MAX + 1, 'a');
    EXPECT_TRUE(refused({"tag", "get", long_name}));
}

TEST(Arguments, TagNameThatLooksLikeAnOptionAfterDoubleDash) {
    const Arguments args = parse_ok({"tag", "get", "--", "--odd"});
    EXPECT_EQ(args.name, "--odd");
}

TEST(Arguments, TagWatch) {
    const Arguments args = parse_ok({"tag", "watch", "plant.*"});
    EXPECT_EQ(args.command, Command::TAG_WATCH);
    EXPECT_FALSE(args.count.has_value());
    EXPECT_TRUE(refused({"tag", "watch"}));
    EXPECT_TRUE(refused({"tag", "watch", "a", "b"}));

    const Arguments counted = parse_ok({"tag", "watch", "*", "--count", "5"});
    EXPECT_EQ(counted.count, std::optional<std::uint64_t>{5});
    EXPECT_TRUE(refused({"tag", "watch", "*", "--count", "0"}));
    EXPECT_TRUE(refused({"tag", "watch", "*", "--count", "-1"}));
    EXPECT_TRUE(refused({"tag", "watch", "*", "--count", "5x"}));
    EXPECT_TRUE(refused({"tag", "watch", "*", "--count", "+5"}));
    EXPECT_TRUE(
        refused({"tag", "watch", "*", "--count", "18446744073709551616"}));
    EXPECT_TRUE(refused({"tag", "watch", "*", "--count"}));
    EXPECT_TRUE(refused({"tag", "watch", "*", "--count", "1", "--count", "2"}));
}

TEST(Arguments, CountOnlyWithWatch) {
    EXPECT_TRUE(refused({"tag", "list", "--count", "1"}));
    EXPECT_TRUE(refused({"status", "--count", "1"}));
}

TEST(Arguments, GlobalOptionsAnywhere) {
    const Arguments args = parse_ok({"--json", "tag", "get", "a", "--base-dir",
                                     "/tmp/x", "--tagd-uid", "1000"});
    EXPECT_TRUE(args.json);
    EXPECT_EQ(args.base_dir, "/tmp/x");
    EXPECT_EQ(args.tagd_uid, std::optional<std::uint32_t>{1000});
    EXPECT_TRUE(refused({"--json", "--json", "status"}));
    EXPECT_TRUE(refused({"--base-dir", "a", "--base-dir", "b", "status"}));
    EXPECT_TRUE(refused({"--base-dir", "", "status"}));
    EXPECT_TRUE(refused({"--base-dir"}));
    EXPECT_TRUE(refused({"--tagd-uid", "4294967295", "status"}));
    EXPECT_TRUE(refused({"--tagd-uid", "x", "status"}));
    EXPECT_TRUE(refused({"--tagd-uid", "1", "--tagd-uid", "1", "status"}));
    const Arguments root = parse_ok({"--tagd-uid", "0", "status"});
    EXPECT_EQ(root.tagd_uid, std::optional<std::uint32_t>{0});
}

TEST(Arguments, UnknownWordsAndOptions) {
    EXPECT_TRUE(refused({}));
    EXPECT_TRUE(refused({"stat"}));
    EXPECT_TRUE(refused({"tag"}));
    EXPECT_TRUE(refused({"tag", "set", "a"}));
    EXPECT_TRUE(refused({"--verbose", "status"}));
    // Write commands do not exist (reference 11.6: read-only).
    EXPECT_TRUE(refused({"tag", "write", "a", "1"}));
}

TEST(Arguments, DetailDoesNotEchoControlCharacters) {
    std::string detail;
    EXPECT_TRUE(refused({"\x1b[2J"}, &detail));
    EXPECT_EQ(detail.find('\x1b'), std::string::npos);
    EXPECT_NE(detail.find("\\x1b"), std::string::npos);
}

TEST(Arguments, UsageNamesEveryCommandAndOption) {
    for (const std::string_view word :
         {"status", "tag list", "tag get", "tag watch", "--json", "--count",
          "--base-dir", "--tagd-uid", "--help", "Exit codes"}) {
        EXPECT_NE(USAGE.find(word), std::string_view::npos) << word;
    }
}

}  // namespace
}  // namespace scada::ctl
