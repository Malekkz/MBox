// One device's identity check as it runs (driver interface contract 3.6;
// decision D-147, ruling items 2 to 4): the verdicts and the identity text.

#include "scada/driver/identity_run.h"

#include "scada/driver/identity.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/tag_value.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::driver {
namespace {

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers): the points, values and times are the test data

using Kind = IdentityVerdict::Kind;

// A check on point 0 that its STRING matches `MODEL-*`, and one on point 1
// that its UINT64 is 3 or 4.
std::vector<IdentityCheck> two_checks() {
    std::vector<IdentityCheck> checks(2);
    checks[0].point = 0;
    checks[0].kind = IdentityCheck::Kind::PATTERN;
    checks[0].pattern = "MODEL-*";
    checks[1].point = 1;
    checks[1].kind = IdentityCheck::Kind::IN;
    checks[1].values = {ExpectedValue{.type = tag::TagType::UINT64, .u = 3},
                        ExpectedValue{.type = tag::TagType::UINT64, .u = 4}};
    return checks;
}

std::string text_of(const IdentityRun& run) {
    std::array<char, TAG_STRING_MAX_BYTES> out{};
    return {out.data(), run.text(out)};
}

TEST(IdentityRun, EveryCheckMatchedIsAMatch) {
    const std::vector<IdentityCheck> checks = two_checks();
    IdentityRun run(checks);
    run.start(100);
    EXPECT_EQ(run.verdict(0).kind, Kind::NONE);  // nothing read yet
    run.sample(0, Outcome::VALUE, true, string_value("MODEL-A"));
    EXPECT_EQ(run.verdict(0).kind, Kind::NONE);  // one check not read
    run.sample(1, Outcome::VALUE, true, uint64_value(4));
    EXPECT_EQ(run.verdict(0).kind, Kind::MATCH);
    EXPECT_EQ(text_of(run), "MODEL-A; 4");
}

TEST(IdentityRun, AWrongValueOrAnAbsentAddressIsAMismatchAtOnce) {
    const std::vector<IdentityCheck> checks = two_checks();
    IdentityRun run(checks);
    run.start(100);
    run.sample(1, Outcome::VALUE, true, uint64_value(7));
    const IdentityVerdict wrong = run.verdict(0);
    EXPECT_EQ(wrong.kind, Kind::MISMATCH);
    EXPECT_EQ(wrong.reason, MismatchReason::VALUE);
    EXPECT_EQ(wrong.check, 1U);
    run.start(100);
    run.sample(0, Outcome::ABSENT, false, TagValue{});
    const IdentityVerdict absent = run.verdict(0);
    EXPECT_EQ(absent.reason, MismatchReason::ABSENT);
    EXPECT_EQ(absent.check, 0U);
}

TEST(IdentityRun, AnUnreadableValueWaitsForTheDeadline) {
    const std::vector<IdentityCheck> checks = two_checks();
    IdentityRun run(checks);
    run.start(100);
    run.sample(0, Outcome::NO_VALUE, false, TagValue{});
    run.sample(1, Outcome::VALUE, true, uint64_value(3));
    EXPECT_EQ(run.verdict(99).kind, Kind::NONE);
    const IdentityVerdict late = run.verdict(100);
    EXPECT_EQ(late.kind, Kind::MISMATCH);
    EXPECT_EQ(late.reason, MismatchReason::UNREADABLE);
    // A value processing refused is unreadable too.
    run.start(100);
    run.sample(0, Outcome::VALUE, false, string_value("MODEL"));
    EXPECT_EQ(run.verdict(200).reason, MismatchReason::UNREADABLE);
}

TEST(IdentityRun, ACommunicationFailureIsNoVerdict) {
    // A time-out, a broken frame or a gateway failure gives COMM_FAIL: the
    // check is read again, whatever the deadline (ruling items 2, 3).
    const std::vector<IdentityCheck> checks = two_checks();
    IdentityRun run(checks);
    run.start(100);
    run.sample(0, Outcome::COMM_FAIL, false, TagValue{});
    run.sample(1, Outcome::VALUE, true, uint64_value(3));
    EXPECT_EQ(run.verdict(1'000).kind, Kind::NONE);
    EXPECT_EQ(text_of(run), "; 3");
}

TEST(IdentityRun, APatternSeesTheCleanedTextAndTheTextShowsIt) {
    std::vector<IdentityCheck> checks(1);
    checks[0].point = 0;
    checks[0].kind = IdentityCheck::Kind::EQUALS;
    checks[0].values = {
        ExpectedValue{.type = tag::TagType::STRING, .text = "A?B"}};
    IdentityRun run(checks);
    run.start(100);
    const std::string raw(
        "A\x01"
        "B",
        3);
    run.sample(0, Outcome::VALUE, true, string_value(raw));
    EXPECT_EQ(run.verdict(0).kind, Kind::MATCH);
    EXPECT_EQ(text_of(run), "A?B");
    std::array<char, 16> field{};
    EXPECT_EQ(std::string_view(field.data(), run.check_text(0, field)), "A?B");
}

TEST(IdentityRun, AMovedRunKeepsItsTexts) {
    const std::vector<IdentityCheck> checks = two_checks();
    IdentityRun first(checks);
    first.start(100);
    first.sample(0, Outcome::VALUE, true, string_value("MODEL-5"));
    first.sample(1, Outcome::VALUE, true, uint64_value(3));
    std::vector<IdentityRun> runs;
    runs.push_back(std::move(first));
    runs.emplace_back(checks);  // the vector grows: the first run moves
    EXPECT_EQ(text_of(runs.front()), "MODEL-5; 3");
    EXPECT_EQ(runs.front().verdict(0).kind, Kind::MATCH);
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

}  // namespace
}  // namespace scada::driver
