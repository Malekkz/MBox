// The work of evaluating expressions (decision D-114; E1 of the review of
// #146): each expression is parsed once for all its evaluations. The
// reviewer's tree makes about 4,000,000 units of work, so it has its own
// executable and time limit.

#include "expand_cases.h"

#include <gtest/gtest.h>

#include <string>

namespace scada::conf {
namespace {

// A template of one `repeat` over `values` values whose block `when` is
// `when`, making one point per value.
std::string repeat_when(int values, const std::string& when) {
    return "template: meter_a\nprotocol: modbus\nrepeat:\n  - for: { n: \"1.." +
           std::to_string(values) + "\" }\n    when: \"" + when +
           "\"\n    points:\n      - { name: \"p{n}\", address: { table: "
           "holding, register: 1, type: uint16 } }\n";
}

// `n < 0 OR n < 0 OR ...` of `terms` terms.
std::string false_terms(int terms) {
    std::string text = "n < 0";
    for (int term = 1; term < terms; ++term) {
        text += " OR n < 0";
    }
    return text;
}

// `count` instances of meter_a.
std::string instances(int count) {
    std::string devices;
    for (int unit = 1; unit <= count; ++unit) {
        devices += "  - { name: ps1.m" + std::to_string(unit) +
                   ", template: meter_a, unit_id: " + std::to_string(unit) +
                   " }\n";
    }
    return channel(devices);
}

// E1 of the review of #146: an expression is parsed once for all its
// evaluations, and typed again only when a name changes type. The
// reviewer's tree, 30 instances of a `repeat` whose `when` has 400 terms,
// has two expressions (the `for` value and the `when`): two parses, where a
// parse per evaluation made 30 x 11 with 10 values.
TEST(Expand, AnExpressionIsParsedOncePerSite) {
    const Expanded small = expand_limited(
        {{"templates/meter_a.yaml", repeat_when(10, false_terms(400))},
         {"drivers/modbus/line1.yaml", instances(30)}},
        {});
    EXPECT_EQ(small.lines, Lines{});
    EXPECT_EQ(small.error_count, 0U);
    EXPECT_EQ(small.counts.parses, 2U);
    EXPECT_EQ(small.counts.typings, 2U);
    // At its full size it passes the expression budget, refused with the
    // budget's own message (G1). The default budget takes seconds in a
    // release build (D-114); here a budget of 4,000,000 nodes reaches it at
    // the first instance.
    const Expanded full = expand_limited(
        {{"templates/meter_a.yaml", repeat_when(65536, false_terms(400))},
         {"drivers/modbus/line1.yaml", instances(30)}},
        {.expression_work = 4'000'000});
    EXPECT_EQ(full.lines,
              (Lines{"drivers/modbus/line1.yaml:7:13: error CF-113: the "
                     "expression nodes evaluated for the tree exceed their "
                     "limit of 4000000\n  from template "
                     "templates/meter_a.yaml:5:11"}));
    EXPECT_EQ(full.counts.parses, 2U);
}

}  // namespace
}  // namespace scada::conf
