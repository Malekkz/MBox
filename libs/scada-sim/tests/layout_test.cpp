// Unit tests for the layout of a simulation: names, how tags are shared
// out, and which layouts are refused.

#include "scada/sim/layout.h"

#include "scada/common/status.h"
#include "scada/sim/pattern.h"
#include "scada/tag/handle.h"
#include "scada/tag/tag_set.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {

using namespace scada::sim;
using scada::StatusCode;

// The reference load: 50,000 tags across 10 owners.
constexpr std::uint32_t OWNERS = 10;
constexpr std::uint64_t TAGS = 50'000;
// 11 tags across 3 owners: 4, 4 and 3.
constexpr std::uint32_t UNEVEN_OWNERS = 3;
constexpr std::uint64_t UNEVEN_TAGS = 11;

Layout reference_load() {
    return Layout{.prefix = "load",
                  .processes = 1,
                  .owners = OWNERS,
                  .tags = TAGS,
                  .patterns = {Pattern::CHECK}};
}

StatusCode check(const Layout& layout) {
    std::string detail;
    const scada::Status status = check_layout(layout, detail);
    EXPECT_EQ(status.ok(), detail.empty()) << detail;
    return status.code();
}

TEST(Layout, OneProcessWithSeveralOwnerThreads) {
    const Layout layout = reference_load();
    EXPECT_EQ(check(layout), StatusCode::OK);
    EXPECT_EQ(owners_per_process(layout), 10U);
    EXPECT_EQ(process_name(layout, 0), "load");
    EXPECT_EQ(owner_name(layout, 0), "load.w0");
    EXPECT_EQ(owner_name(layout, 9), "load.w9");
    EXPECT_EQ(tags_of_owner(layout, 3), 5000U);
    EXPECT_EQ(tag_name(layout, 9, 4999), "load.w9.t4999");
    EXPECT_EQ(process_of_owner(layout, 9), 0U);
}

TEST(Layout, OneOwnerPerProcessIsNamedLikeTheProcess) {
    Layout layout = reference_load();
    layout.processes = OWNERS;
    EXPECT_EQ(check(layout), StatusCode::OK);
    EXPECT_EQ(process_name(layout, 7), "load7");
    EXPECT_EQ(owner_name(layout, 7), "load7");
    EXPECT_EQ(tag_name(layout, 7, 0), "load7.t0");
    EXPECT_EQ(process_of_owner(layout, 7), 7U);
}

TEST(Layout, SeveralProcessesWithSeveralOwners) {
    Layout layout = reference_load();
    layout.processes = 2;
    EXPECT_EQ(check(layout), StatusCode::OK);
    EXPECT_EQ(owner_name(layout, 4), "load0.w4");
    EXPECT_EQ(owner_name(layout, 5), "load1.w0");
    EXPECT_EQ(process_of_owner(layout, 5), 1U);
}

TEST(Layout, TheFirstOwnersGetTheRemainder) {
    Layout layout = reference_load();
    layout.owners = UNEVEN_OWNERS;
    layout.tags = UNEVEN_TAGS;
    EXPECT_EQ(check(layout), StatusCode::OK);
    EXPECT_EQ(tags_of_owner(layout, 0), 4U);
    EXPECT_EQ(tags_of_owner(layout, 1), 4U);
    EXPECT_EQ(tags_of_owner(layout, 2), 3U);
}

TEST(Layout, PatternsGoToTheTagsInTurn) {
    Layout layout = reference_load();
    layout.patterns = {Pattern::RAMP, Pattern::CHECK, Pattern::WALK};
    EXPECT_EQ(pattern_of(layout, 0), Pattern::RAMP);
    EXPECT_EQ(pattern_of(layout, 1), Pattern::CHECK);
    EXPECT_EQ(pattern_of(layout, 5), Pattern::WALK);
}

TEST(Layout, RefusesWhatTheTagSetCannotHold) {
    Layout layout = reference_load();
    layout.processes = 0;
    EXPECT_EQ(check(layout), StatusCode::INVALID_ARGUMENT);
    layout = reference_load();
    layout.processes = 3;
    EXPECT_EQ(check(layout), StatusCode::INVALID_ARGUMENT);
    layout = reference_load();
    layout.owners = 0;
    EXPECT_EQ(check(layout), StatusCode::INVALID_ARGUMENT);
    layout = reference_load();
    layout.tags = OWNERS - 1;
    EXPECT_EQ(check(layout), StatusCode::INVALID_ARGUMENT);
    layout = reference_load();
    layout.tags = scada::tag::MAX_TAGS + 1;
    EXPECT_EQ(check(layout), StatusCode::OUT_OF_RANGE);
    layout = reference_load();
    layout.owners = scada::tag::MAX_OWNERS + 1;
    EXPECT_EQ(check(layout), StatusCode::OUT_OF_RANGE);
    layout = reference_load();
    layout.patterns.clear();
    EXPECT_EQ(check(layout), StatusCode::INVALID_ARGUMENT);
    layout.patterns.assign(MAX_PATTERNS + 1, Pattern::CHECK);
    EXPECT_EQ(check(layout), StatusCode::INVALID_ARGUMENT);
}

TEST(Layout, RefusesAPrefixThatMakesNoValidName) {
    Layout layout = reference_load();
    layout.prefix = "Load";
    EXPECT_EQ(check(layout), StatusCode::INVALID_ARGUMENT);
    layout.prefix = "";
    EXPECT_EQ(check(layout), StatusCode::INVALID_ARGUMENT);
    // 31 bytes is a valid process name, but the owner `<prefix>.w9` is not.
    layout.prefix = std::string(scada::tag::MAX_SHORT_NAME_LENGTH, 'a');
    EXPECT_EQ(check(layout), StatusCode::INVALID_ARGUMENT);
    // The longest prefix that leaves room for `.w9`.
    layout.prefix = std::string(scada::tag::MAX_SHORT_NAME_LENGTH - 3, 'a');
    EXPECT_EQ(check(layout), StatusCode::OK);
}

}  // namespace
