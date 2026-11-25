// The limits of the expanded tree (contract 1.3; decision D-114; C2 of the
// review of #146): its tags (CF-1001), its nodes and the work of `repeat`
// (CF-113). They make millions of nodes, so they have their own executable
// and time limit, apart from the unit tests of the expander.

#include "expand_cases.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace scada::conf {
namespace {

// C2 of the review of #146: the limits of 1.3 hold for the expanded tree,
// on running totals; passing one stops the expansion.
TEST(Expand, TheLimitsOfTheExpandedTree) {
    const std::string meter =
        "template: meter_a\nprotocol: modbus\n"
        "repeat:\n"
        "  - for: { n: \"1..4\" }\n"
        "    points:\n"
        "      - { name: \"a{n}\", address: { table: holding, register: 1, "
        "type: uint16 } }\n";
    // `limits.max_tags` (CF-1001): 3 devices of 4 tags pass 10 at the third.
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml", meter},
                {"system.yaml",
                 std::string(SYSTEM) + "limits: { max_tags: 10 }\n"},
                {"drivers/modbus/line1.yaml",
                 channel("  - { name: ps1.m1, template: meter_a, unit_id: 1 "
                         "}\n"
                         "  - { name: ps1.m2, template: meter_a, unit_id: 2 "
                         "}\n"
                         "  - { name: ps1.m3, template: meter_a, unit_id: 3 "
                         "}\n"
                         "  - { name: ps1.m4, template: meter_a, unit_id: 4 "
                         "}\n")}}),
        (Lines{"drivers/modbus/line1.yaml:9:13: error CF-1001: 12 tags, the "
               "limit is 10"}));
    // The nodes an instance makes count before step 4: 40,000 points of
    // more than 100 nodes each pass 4,000,000.
    std::string list = "[";
    constexpr int VALUES = 100;
    for (int value = 0; value < VALUES; ++value) {
        list += (value == 0 ? "" : ", ") + std::to_string(value);
    }
    list += "]";
    EXPECT_EQ(expand({{"templates/meter_a.yaml",
                       "template: meter_a\nprotocol: modbus\n"
                       "repeat:\n"
                       "  - for: { n: \"1..40000\" }\n"
                       "    points:\n"
                       "      - { name: \"a{n}\", invalid_values: " +
                           list +
                           ", address: { table: holding, register: 1, type: "
                           "uint16 } }\n"},
                      {"drivers/modbus/line1.yaml", one_device()}}),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-113: the "
                     "expanded tree exceeds its limit of 4000000 nodes\n  from "
                     "template templates/meter_a.yaml:6:17"}));
    // Every `repeat` value evaluated counts toward the work of the tree
    // (with every point made, D3 of the delta review):
    // 62 blocks of 65,536 values pass 4,000,000.
    std::string blocks = "template: meter_a\nprotocol: modbus\nrepeat:\n";
    constexpr int BLOCKS = 62;
    for (int block = 0; block < BLOCKS; ++block) {
        blocks +=
            "  - { for: { n: \"1..65536\" }, when: \"false\", points: "
            "[] }\n";
    }
    EXPECT_EQ(
        expand({{"templates/meter_a.yaml", blocks},
                {"drivers/modbus/line1.yaml", one_device()}}),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-113: the points "
               "and repeat values made for the tree exceed their limit "
               "of 4000000\n  from template templates/meter_a.yaml:65:5"}));
}

// D1 of the delta review of #146: the defaults merged under every point
// count before step 5, so 1,000 values under 50,000 points are refused at
// once, not after 7 GiB.
TEST(Expand, DefaultsUnderEveryPointCount) {
    std::string list = "[";
    constexpr int VALUES = 1000;
    for (int value = 0; value < VALUES; ++value) {
        list += (value == 0 ? "" : ", ") + std::to_string(value);
    }
    list += "]";
    EXPECT_EQ(expand({{"templates/meter_a.yaml",
                       "template: meter_a\nprotocol: modbus\n"
                       "defaults: { invalid_values: { uint16: " +
                           list +
                           " } }\n"
                           "repeat:\n"
                           "  - for: { n: \"1..50000\" }\n"
                           "    points:\n"
                           "      - { name: \"a{n}\", address: { table: "
                           "holding, register: 1, type: uint16 } }\n"},
                      {"drivers/modbus/line1.yaml", one_device()}}),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-113: the "
                     "expanded tree exceeds its limit of 4000000 nodes"}));
    // The count is made before step 5: the device stops there, so its
    // step 6 (an override of a point it does not have, CF-413) is never
    // reached.
    std::string small = "[";
    constexpr int SMALL = 100;
    for (int value = 0; value < SMALL; ++value) {
        small += (value == 0 ? "" : ", ") + std::to_string(value);
    }
    small += "]";
    EXPECT_EQ(
        expand_limited({{"templates/meter_a.yaml",
                         "template: meter_a\nprotocol: modbus\n"
                         "defaults: { invalid_values: { uint16: " +
                             small +
                             " } }\n"
                             "repeat:\n"
                             "  - for: { n: \"1..200\" }\n"
                             "    points:\n"
                             "      - { name: \"a{n}\", address: { table: "
                             "holding, register: 1, type: uint16 } }\n"},
                        {"drivers/modbus/line1.yaml",
                         one_device("    overrides: { nope: { unit: V } }\n")}},
                       {.nodes = 10000})
            .lines,
        Lines{"drivers/modbus/line1.yaml:7:11: error CF-113: the expanded tree "
              "exceeds its limit of 10000 nodes"});
}

// The limits on small values (D6 of the delta review of #146), each at its
// edge.
namespace {

// A template of `points` written points p0 to p<points - 1>.
std::string written_points(int points) {
    std::string text = "template: meter_a\nprotocol: modbus\npoints:\n";
    for (int point = 0; point < points; ++point) {
        text += "  - { name: p" + std::to_string(point) +
                ", address: { table: holding, register: 1, type: uint16 } "
                "}\n";
    }
    return text;
}

// `count` instances of meter_a, each with `more` before its closing brace.
std::string instances(int count, std::string_view more = {}) {
    std::string devices;
    for (int unit = 1; unit <= count; ++unit) {
        devices += "  - { name: ps1.m" + std::to_string(unit) +
                   ", template: meter_a, unit_id: " + std::to_string(unit) +
                   std::string(more) + " }\n";
    }
    return channel(devices);
}

// A template of one `repeat` over `values` values whose block `when` is
// `when`, making one point per value.
std::string repeat_when(int values, const std::string& when) {
    return "template: meter_a\nprotocol: modbus\nrepeat:\n  - for: { n: \"1.." +
           std::to_string(values) + "\" }\n    when: \"" + when +
           "\"\n    points:\n      - { name: \"p{n}\", address: { table: "
           "holding, register: 1, type: uint16 } }\n";
}

// meter_a with a SET parameter `s` of `items` values, all given by default,
// and a `repeat` of 10 values whose `when` is `when`.
std::string with_set(int items, const std::string& when) {
    std::string values;
    for (int item = 0; item < items; ++item) {
        values += (item == 0 ? "v" : ", v") + std::to_string(item);
    }
    return "template: meter_a\nprotocol: modbus\nparams: { s: { type: set, "
           "values: [" +
           values + "], default: [" + values +
           "] } }\nrepeat:\n  - for: { n: \"1..10\" }\n    when: \"" + when +
           "\"\n    points:\n      - { name: \"p{n}\", address: { table: "
           "holding, register: 1, type: uint16 } }\n";
}

}  // namespace

// The nodes written: a device's keys count, not only what an instance
// makes (the running total after each device).
TEST(Expand, TheWrittenNodesOfTheTree) {
    std::string enums = "  states: {";
    constexpr int STATES = 60;
    for (int state = 0; state < STATES; ++state) {
        enums += (state == 0 ? " " : ", ") + std::to_string(state) + ": s" +
                 std::to_string(state);
    }
    enums += " }\n";
    const Files files{{"templates/meter_a.yaml",
                       "template: meter_a\nprotocol: modbus\nenums:\n" + enums +
                           "points:\n  - { name: p, address: { table: "
                           "holding, register: 1, type: uint16 } }\n"},
                      {"drivers/modbus/line1.yaml", instances(3)}};
    EXPECT_EQ(expand_limited(files, {.nodes = 400}).lines,
              (Lines{"drivers/modbus/line1.yaml:9:13: error CF-113: the "
                     "expanded tree exceeds its limit of 400 nodes"}));
    EXPECT_EQ(expand_limited(files, {.nodes = 4000}).lines.size(), 3U);
}

// The text, checked after each point: 40 descriptions of 100 bytes pass
// 2,000 bytes within the first device.
TEST(Expand, TheTextOfTheTree) {
    std::string text = "template: meter_a\nprotocol: modbus\npoints:\n";
    const std::string description(100, 'd');
    constexpr int POINTS = 40;
    for (int point = 0; point < POINTS; ++point) {
        text += "  - { name: p" + std::to_string(point) +
                ", description: " + description +
                ", address: { table: holding, register: 1, type: uint16 } }\n";
    }
    const Expanded expanded =
        expand_limited({{"templates/meter_a.yaml", text},
                        {"drivers/modbus/line1.yaml", instances(2)}},
                       {.text_bytes = 2000});
    EXPECT_EQ(expanded.lines,
              (Lines{"drivers/modbus/line1.yaml:7:13: error CF-113: the "
                     "expanded tree exceeds its limit of 2000 bytes of text"}));
    // The writer stops at the point that passes the limit (E4 of the review
    // of #146): a point here is about 130 bytes, the device about 5,200.
    constexpr std::size_t ONE_POINT_MORE = 2000 + 200;
    EXPECT_GT(expanded.counts.written_text, 2000U);
    EXPECT_LE(expanded.counts.written_text, ONE_POINT_MORE);
}

// `limits.max_tags` at N passes and at N + 1 stops the expansion, so a
// later device's own error is not reported; a point with `tag: false`
// counts its bits only.
TEST(Expand, TheTagsOfTheTree) {
    const std::string meter = written_points(4);
    const auto with_limit = [](int limit) {
        return std::string(SYSTEM) +
               "limits: { max_tags: " + std::to_string(limit) + " }\n";
    };
    EXPECT_EQ(expand({{"templates/meter_a.yaml", meter},
                      {"system.yaml", with_limit(8)},
                      {"drivers/modbus/line1.yaml", instances(2)}})
                  .size(),
              8U);
    EXPECT_EQ(expand({{"templates/meter_a.yaml", meter},
                      {"system.yaml", with_limit(7)},
                      {"drivers/modbus/line1.yaml",
                       channel("  - { name: ps1.m1, template: meter_a, "
                               "unit_id: 1 }\n"
                               "  - { name: ps1.m2, template: meter_a, "
                               "unit_id: 2 }\n"
                               "  - { name: ps1.m3, template: meter_a, "
                               "unit_id: 3, overrides: { nope: { unit: V } } "
                               "}\n")}}),
              (Lines{"drivers/modbus/line1.yaml:8:13: error CF-1001: 8 tags, "
                     "the limit is 7"}));
    EXPECT_EQ(expand({{"templates/meter_a.yaml",
                       "template: meter_a\nprotocol: modbus\nbit_order: lsb0\n"
                       "points:\n"
                       "  - { name: w, tag: false, address: { table: holding, "
                       "register: 1, type: uint16 }, bits: { 0: a, 1: b } }\n"
                       "  - { name: c, address: { table: holding, register: 2, "
                       "type: uint16 } }\n"},
                      {"system.yaml", with_limit(2)},
                      {"drivers/modbus/line1.yaml", one_device()}}),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-1001: 3 tags, "
                     "the limit is 2"}));
}

// D3: every point made counts toward the work, written ones too, kept or
// not: 100 written points at 30 instances that keep one pass 2,000.
TEST(Expand, WrittenPointsAreWork) {
    EXPECT_EQ(
        expand_limited(
            {{"templates/meter_a.yaml", written_points(100)},
             {"drivers/modbus/line1.yaml", instances(30, ", only: [p0]")}},
            {.work = 2000})
            .lines,
        (Lines{"drivers/modbus/line1.yaml:27:13: error CF-113: the points and "
               "repeat values made for the tree exceed their limit of 2000\n  "
               "from template templates/meter_a.yaml:4:13"}));
}

// D2: one cause met at many instances keeps one text, however many
// instances; past the kept limit, an error is counted only.
TEST(Expand, WhatIsKeptToReportErrors) {
    std::string text =
        "template: meter_a\nprotocol: modbus\nparams: { k: { type: int, "
        "default: 0 } }\npoints:\n";
    constexpr int CAUSES = 40;
    for (int point = 0; point < CAUSES; ++point) {
        text += "  - { name: p" + std::to_string(point) +
                ", address: { table: holding, register: \"" +
                std::to_string(point) + " DIV k\", type: uint16 } }\n";
    }
    const Files files{{"templates/meter_a.yaml", text},
                      {"drivers/modbus/line1.yaml", instances(200)}};
    const Expanded all = expand_limited(files, {});
    EXPECT_EQ(all.lines.size(), 40U);
    EXPECT_EQ(all.counts.kept_texts, 40U);
    EXPECT_EQ(all.counts.causes, 40U);
}

// Past the kept limit, an error is counted only, and so is its text.
TEST(Expand, WhatIsKeptPastTheLimit) {
    std::string text =
        "template: meter_a\nprotocol: modbus\nparams: { k: { type: int, "
        "default: 0 } }\npoints:\n";
    constexpr int CAUSES = 40;
    for (int point = 0; point < CAUSES; ++point) {
        text += "  - { name: p" + std::to_string(point) +
                ", address: { table: holding, register: \"" +
                std::to_string(point) + " DIV k\", type: uint16 } }\n";
    }
    const Files files{{"templates/meter_a.yaml", text},
                      {"drivers/modbus/line1.yaml", instances(200)}};
    const Expanded capped = expand_limited(files, {.kept_errors = 10});
    EXPECT_EQ(capped.lines.size(), 10U);
    EXPECT_EQ(capped.counts.kept_texts, 10U);
    EXPECT_EQ(capped.counts.causes, 10U);
    EXPECT_GE(capped.error_count, 40U);
}

}  // namespace
// The weight of an evaluation in the work budget is its expression's nodes
// (E1 of the review of #146). With 10 values, each node more in `when` costs
// 12: one in each of the 10 evaluations, one in its parse and one in its
// typing. So the budget at which the tree passes moves by 12 per node.
TEST(Expand, AnEvaluationCostsItsNodes) {
    // 65 = the `for` value "1..10" (3 nodes: parsed, typed, evaluated: 9),
    // 10 values, 10 points made, and `n > 0` (3 nodes) parsed and typed (6)
    // and evaluated 10 times (30).
    constexpr std::size_t EDGE = 65;
    constexpr int VALUES = 10;
    const auto passes = [](const std::string& when, std::size_t work) {
        return expand_limited(
                   {{"templates/meter_a.yaml", repeat_when(VALUES, when)},
                    {"drivers/modbus/line1.yaml", instances(1)}},
                   {.work = work})
                   .error_count == 0;
    };
    // `n > 0` has 3 nodes, `n + 0 > 0` 5, `n + 0 + 0 > 0` 7.
    EXPECT_TRUE(passes("n > 0", EDGE));
    EXPECT_FALSE(passes("n > 0", EDGE - 1));
    EXPECT_TRUE(passes("n + 0 > 0", EDGE + 24));
    EXPECT_FALSE(passes("n + 0 > 0", EDGE + 23));
    EXPECT_TRUE(passes("n + 0 + 0 > 0", EDGE + 48));
    EXPECT_FALSE(passes("n + 0 + 0 > 0", EDGE + 47));
}

// E4 of the review of #146: what an instance makes counts with what is
// already written. One instance writes 1,127 nodes; the second, under a
// limit of 1,700, passes it while it makes its points, at the point where
// the two together pass it (template line 56), not only once it is written.
TEST(Expand, WhatIsMadeCountsWithWhatIsWritten) {
    EXPECT_EQ(expand_limited({{"templates/meter_a.yaml", written_points(100)},
                              {"drivers/modbus/line1.yaml", instances(1)}},
                             {.nodes = 1127})
                  .error_count,
              0U);
    EXPECT_EQ(expand_limited({{"templates/meter_a.yaml", written_points(100)},
                              {"drivers/modbus/line1.yaml", instances(2)}},
                             {.nodes = 1700})
                  .lines,
              (Lines{"drivers/modbus/line1.yaml:8:13: error CF-113: the "
                     "expanded tree exceeds its limit of 1700 nodes\n"
                     "  from template templates/meter_a.yaml:56:13"}));
}

// A reference to a parameter costs the value it copies (E1 of the review
// of #146): `'v0' IN s` over 10 values passes at 165 with a set of 10
// items, 65 as for `n > 0` and 10 x 10 for the set, and at 265 with 20.
TEST(Expand, AReferenceCostsItsValue) {
    const auto passes = [](int items, std::size_t work) {
        return expand_limited(
                   {{"templates/meter_a.yaml", with_set(items, "'v0' IN s")},
                    {"drivers/modbus/line1.yaml", instances(1)}},
                   {.work = work})
                   .error_count == 0;
    };
    EXPECT_TRUE(passes(10, 165));
    EXPECT_FALSE(passes(10, 164));
    EXPECT_TRUE(passes(20, 265));
    EXPECT_FALSE(passes(20, 264));
}

// Past the work budget nothing more is counted: one stop is one error,
// not one for each expression evaluated after it.
TEST(Expand, OneStopIsOneError) {
    std::string text =
        "template: meter_a\nprotocol: modbus\nparams: { k: { type: int, "
        "default: 1 } }\npoints:\n";
    constexpr int POINTS = 20;
    for (int point = 0; point < POINTS; ++point) {
        text += "  - { name: p" + std::to_string(point) +
                ", address: { table: holding, register: \"k + " +
                std::to_string(point) + "\", type: uint16 } }\n";
    }
    const Expanded expanded =
        expand_limited({{"templates/meter_a.yaml", text},
                        {"drivers/modbus/line1.yaml", instances(1)}},
                       {.work = 40});
    EXPECT_EQ(expanded.error_count, 1U);
}

}  // namespace scada::conf
