// The limits of the expanded tree (contract 1.3; decision D-114; C2 of the
// review of #146): its tags (CF-1001), its nodes and the work of `repeat`
// (CF-113). They make millions of nodes, so they have their own executable
// and time limit, apart from the unit tests of the expander.

#include "expand_cases.h"

#include <gtest/gtest.h>

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
    // Every `repeat` value evaluated counts toward the work of the tree:
    // 62 blocks of 65,536 values pass 4,000,000.
    std::string blocks = "template: meter_a\nprotocol: modbus\nrepeat:\n";
    constexpr int BLOCKS = 62;
    for (int block = 0; block < BLOCKS; ++block) {
        blocks +=
            "  - { for: { n: \"1..65536\" }, when: \"false\", points: "
            "[] }\n";
    }
    EXPECT_EQ(expand({{"templates/meter_a.yaml", blocks},
                      {"drivers/modbus/line1.yaml", one_device()}}),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-113: the repeat "
                     "values of the tree exceed their limit of 4000000\n  from "
                     "template templates/meter_a.yaml:65:5"}));
}

}  // namespace
}  // namespace scada::conf
