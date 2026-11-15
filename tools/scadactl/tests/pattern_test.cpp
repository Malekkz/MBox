// Unit tests for the tag-name patterns of scadactl.

#include "scada/ctl/pattern.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace scada::ctl {
namespace {

TagPattern parse_ok(const std::string& text) {
    std::string detail;
    const std::optional<TagPattern> pattern = TagPattern::parse(text, detail);
    EXPECT_TRUE(pattern.has_value()) << text << ": " << detail;
    return pattern.value_or(TagPattern::all());
}

TEST(TagPattern, LiteralMatchesOnlyItself) {
    const TagPattern pattern = parse_ok("plant.pump1.speed");
    EXPECT_TRUE(pattern.matches("plant.pump1.speed"));
    EXPECT_FALSE(pattern.matches("plant.pump1.speed2"));
    EXPECT_FALSE(pattern.matches("plant.pump1.spee"));
    EXPECT_FALSE(pattern.matches(""));
}

TEST(TagPattern, StarMatchesAnySequenceDotsIncluded) {
    const TagPattern pattern = parse_ok("plant.*.speed");
    EXPECT_TRUE(pattern.matches("plant.pump1.speed"));
    EXPECT_TRUE(pattern.matches("plant.area2.pump1.speed"));
    EXPECT_TRUE(pattern.matches("plant..speed"));
    EXPECT_FALSE(pattern.matches("plant.pump1.speed.raw"));
    EXPECT_FALSE(pattern.matches("other.pump1.speed"));
}

TEST(TagPattern, QuestionMarkMatchesExactlyOne) {
    const TagPattern pattern = parse_ok("pump?");
    EXPECT_TRUE(pattern.matches("pump1"));
    EXPECT_FALSE(pattern.matches("pump"));
    EXPECT_FALSE(pattern.matches("pump12"));
}

TEST(TagPattern, StarsAtBothEndsAndInARow) {
    EXPECT_TRUE(parse_ok("*").matches(""));
    EXPECT_TRUE(parse_ok("*").matches("a.b"));
    EXPECT_TRUE(parse_ok("**").matches("abc"));
    EXPECT_TRUE(parse_ok("*pump*").matches("plant.pump1.speed"));
    EXPECT_FALSE(parse_ok("*pump*").matches("plant.valve1"));
    EXPECT_TRUE(parse_ok("a*b*c").matches("aXbYbZc"));
    EXPECT_FALSE(parse_ok("a*b*c").matches("aXbYbZ"));
}

TEST(TagPattern, BacktrackingWorstCaseTerminates) {
    // Many stars against a long name that does not match: the matcher
    // retries the last star only, so this ends quickly.
    const std::string name(127, 'a');
    EXPECT_FALSE(parse_ok("*a*a*a*a*a*a*a*a*a*a*a*a*a*b").matches(name));
}

TEST(TagPattern, RejectsWhatIsNoPattern) {
    std::string detail;
    EXPECT_FALSE(TagPattern::parse("", detail).has_value());
    EXPECT_FALSE(detail.empty());
    EXPECT_FALSE(TagPattern::parse("Plant", detail).has_value());
    EXPECT_FALSE(TagPattern::parse("a b", detail).has_value());
    EXPECT_FALSE(TagPattern::parse("a/b", detail).has_value());
    EXPECT_FALSE(TagPattern::parse("[ab]", detail).has_value());
    EXPECT_FALSE(TagPattern::parse(std::string("a\0b", 3), detail).has_value());
    EXPECT_FALSE(
        TagPattern::parse(std::string(PATTERN_MAX + 1, 'a'), detail).has_value());
    EXPECT_TRUE(
        TagPattern::parse(std::string(PATTERN_MAX, 'a'), detail).has_value());
}

}  // namespace
}  // namespace scada::ctl
